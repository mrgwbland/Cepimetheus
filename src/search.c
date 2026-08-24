#include "search.h"
#include "search_helpers.h"

#include "eval.h"
#include "movegen.h"
#include "movepicker.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int quiescence(Board *board,
                      int alpha,
                      int beta,
                      RepetitionHistory *history,
                      SearchStats *stats,
                      SearchStack *ss,
                      int qply,
                      SearchContext *context,
                      SearchControl *control,
                      bool lichess_draw_rules)
{
    int ply = ss->ply;
    ++stats->nodes;
    if (ply > stats->seldepth)
    {
        stats->seldepth = ply;
    }

    if (board_is_draw(board, history, ply, lichess_draw_rules))
    {
        return 0;
    }

    bool in_check = board_is_in_check(board, board->side);
    Move best_move = MOVE_NONE;
    int stand_pat = -32000;

    if (!in_check)
    {
        stand_pat = evaluate_position(board);
        ss->static_eval = stand_pat;

        if (stand_pat >= beta)
        {
            return beta;
        }

        if (stand_pat > alpha)
        {
            alpha = stand_pat;
        }
    }

    MovePicker picker;
    // Pass NULL as context to disable killer move sorting in qsearch
    movepicker_init(&picker, board, NULL, ss, MOVE_NONE, NULL, 0, true);

    bool has_legal_move = false;
    Move move;

    while ((move = movepicker_next_move(&picker)) != MOVE_NONE)
    {
        if (search_should_stop(control, stats->nodes))
        {
            break;
        }

        // Delta Pruning
        if (!in_check)
        {
            int captured_val = get_captured_piece_value(board, move);
            int promo = move_promotion(move);
            int gain = captured_val;
            if (promo != MOVE_PROMO_NONE)
            {
                gain += piece_values[promo] - 1000;
            }

            if (stand_pat + gain + qs_delta_margin < alpha)
            {
                continue;
            }
        }

        Undo undo;

        if (!board_make_move(board, move, &undo))
        {
            continue;
        }

        has_legal_move = true;

        U64 key = board->hash;
        if (!repetition_history_push(history, key))
        {
            board_unmake_move(board, &undo);
            continue;
        }

        (ss + 1)->ply = ss->ply + 1;
        (ss + 1)->move = move;
        (ss + 1)->static_eval = -32000;

        int score = -quiescence(board, -beta, -alpha, history, stats, ss + 1, qply + 1, context, control, lichess_draw_rules);

        if (best_move == MOVE_NONE || score > alpha)
        {
            best_move = move;
        }

        --history->count;

        board_unmake_move(board, &undo);

        if (score >= beta)
        {
            return beta;
        }

        if (score > alpha)
        {
            alpha = score;
        }
    }

    if (!has_legal_move)
    {
        if (!in_check && board_has_any_legal_move(board))
        {
            return alpha;
        }

        EvalTerminalState terminal_state = eval_terminal_state(board, false);
        if (terminal_state != EVAL_TERMINAL_NONE)
        {
            return eval_terminal_score(terminal_state, ply);
        }
    }

    if (context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(alpha, alpha, beta);
        transposition_table_store(&context->table, board->hash, 0, alpha, score_type, best_move, ply);
    }

    return alpha;
}

/* Alpha-beta pruned negamax search. */
static SearchResult negamax(Board *board,
                            int depth,
                            int alpha,
                            int beta,
                            RepetitionHistory *history,
                            SearchStats *stats,
                            SearchStack *ss,
                            SearchContext *context,
                            SearchControl *control,
                            bool lichess_draw_rules)
{
    int ply = ss->ply;
    SearchResult result = {0, MOVE_NONE, {0}, 0};
    // Original bounds to determine cutoff types
    const int alpha_orig = alpha;
    const int beta_orig = beta;

    // Determine PV node implicitly by checking if the search window width is greater than 1.
    const bool pv_node = (beta - alpha) > 1;

    // Time constraint met
    if (search_should_stop(control, stats->nodes))
    {
        result.score = alpha;
        return result;
    }

    ++stats->nodes;
    if (ply > stats->seldepth)
    {
        stats->seldepth = ply;
    }

    if (board_is_draw(board, history, ply, lichess_draw_rules))
    {
        result.score = 0;
        return result;
    }

    int tt_score = 0;
    if (context != NULL)
    {
        // (Using TT in PV can cause the info PV to be truncated but this is neccessary to maximise search speed)
        bool tt_cutoff = pv_node
            ? transposition_table_probe_exact(&context->table, board->hash, depth, ply, &tt_score)// On PV nodes, only allow EXACT cutoffs — bound scores from null-window searches would return imprecise values and corrupt the PV.
            : transposition_table_probe(&context->table, board->hash, depth, alpha, beta, ply, &tt_score);// On non-PV nodes, allow all TT cutoffs (EXACT, LOWER, UPPER).

        bool is_repeated = false;
        // Check if the current position is a repeated position in the real game history
        if (history != NULL && history->count > 1)
        {
            U64 current_key = board->hash;
            int start = 0;
            int history_limit = history->count - 1;
            int halfmove_limit = history->count - 1 - board->halfmove_clock;
            if (halfmove_limit > start) {
                start = halfmove_limit;
            }
            for (int i = start; i < history_limit; ++i) {
                if (history->keys[i] == current_key) {
                    is_repeated = true;
                    break;
                }
            }
        }
        if (tt_cutoff && !is_repeated)
        {
            result.score = tt_score;

            int pv_depth = 0;
            Undo undos[MAX_PV_MOVES];
            U64 pv_hashes[MAX_PV_MOVES];
            /* Build a temporary history that includes the current path so that
             * board_is_draw() can detect threefold repetitions inside the PV. */
            RepetitionHistory pv_history = *history;
            while (pv_depth < depth && result.pv_length < MAX_PV_MOVES)
            {
                U64 hash = board->hash;

                /* Stop if the current position is already a draw. */
                if (board_is_draw(board, &pv_history, ply + pv_depth, lichess_draw_rules))
                {
                    break;
                }

                // Avoid infinite loops if there is a cycle in the TT
                bool cycle = false;
                for (int h = 0; h < pv_depth; ++h)
                {
                    if (pv_hashes[h] == hash)
                    {
                        cycle = true;
                        break;
                    }
                }
                if (cycle)
                {
                    break;
                }

                pv_hashes[pv_depth] = hash;
                const TranspositionEntry *entry = transposition_table_lookup(&context->table, hash);
                if (entry == NULL || entry->best_move == MOVE_NONE)
                {
                    break;
                }

                if (!board_is_move_pseudo_legal(board, entry->best_move))
                {
                    break;
                }

                if (!board_make_move(board, entry->best_move, &undos[pv_depth]))
                {
                    break;
                }

                /* Record the new position in the temporary history so that
                 * subsequent iterations can detect draws through it. */
                U64 new_hash = board->hash;
                if (!repetition_history_push(&pv_history, new_hash))
                {
                    /* History full — roll back this move and stop. */
                    board_unmake_move(board, &undos[pv_depth]);
                    break;
                }

                result.pv[result.pv_length++] = entry->best_move;
                pv_depth++;
            }
            // Now unmake all the moves we made
            for (int j = pv_depth - 1; j >= 0; --j)
            {
                board_unmake_move(board, &undos[j]);
            }

            if (result.pv_length > 0)
            {
                result.move = result.pv[0];
            }

            return result;
        }
    }        

    // At leaf node, quiescence search
    if (depth <= 0)
    {
        result.score = quiescence(board, alpha, beta, history, stats, ss, 0, context, control, lichess_draw_rules);
        return result;
    }

    Move previous_move = (ss - 1)->move;

    /* Null-move pruning */
    if (!pv_node && // Disallowed in PV nodes- PV must be a legal continuation
        previous_move != MOVE_NONE && //Disallow null move immediately after a null move
        depth >= nmp_min_depth &&// NMP not done near leaves as tree is already small, and NMP has overhead
        beta < MATE_SCORE - MAX_PLY_DEPTH &&// Not done in mating sequences
        !board_is_in_check(board, board->side) && // In check passing is illegal
        has_sufficient_nmp_material(board)) //Not done in endgames to avoid zugzwang issues
    {
        int scale = nmp_depth_scale > 0 ? nmp_depth_scale : 1;
        int reduction = nmp_base_reduction + depth / scale;
        int child_depth = depth - 1 - reduction;
        if (child_depth < 0) child_depth = 0;

        Undo undo;
        board_make_null_move(board, &undo);

        (ss + 1)->ply = ss->ply + 1;
        (ss + 1)->move = MOVE_NONE;
        (ss + 1)->static_eval = -32000;

        SearchResult null_child = negamax(board,
                                          child_depth,
                                          -beta,
                                          -beta + 1,
                                          history,
                                          stats,
                                          ss + 1,
                                          context,
                                          control,
                                          lichess_draw_rules);
        int null_score = -null_child.score;

        board_unmake_move(board, &undo);

        if (null_score >= beta)
        {
            result.score = beta;
            return result;
        }
    }

    bool in_check = board_is_in_check(board, board->side);      
    bool futility_prune = false; 
    if (in_check)
    {
        depth++; // Check extension to ensure forcing lines are fully explored
    }
    else // The following pruning doesn't occur when in check so we can reuse this is_in_check to save compute
    {
        int static_eval = evaluate_position(board);
        ss->static_eval = static_eval;
        // Reverse Futility Pruning: At relatively shallow non-PV nodes, if the static eval exceeds beta by a depth-dependent margin, prune the entire node.
        if (!pv_node && depth <= rfp_max_depth
            && abs(static_eval) < MATE_SCORE - MAX_PLY_DEPTH // Don't prune in mating sequences
            && static_eval - rfp_margin * depth > beta)
        {
            result.score = static_eval;
            return result;
        }                    
        // Futility Pruning: At depth 1, if static evaluation plus a safety margin is still less than alpha, prune all remaining quiet moves
        if (depth == 1 && abs(alpha) < MATE_SCORE - MAX_PLY_DEPTH)
        {
            if (static_eval + futility_margin < alpha)
            {
                futility_prune = true;
            }
        }
    }  

    // Extract the best move from the transposition table if it exists for move ordering
    Move tt_move = MOVE_NONE;
    if (context != NULL)
    {
        const TranspositionEntry *entry = transposition_table_lookup(&context->table, board->hash);
        if (entry != NULL)
        {
            tt_move = entry->best_move;
        }
        else if (depth >= 4 && !pv_node) // Internal Iterative Reductions, if no TT move then move ordering will be worse so reduce depth to save time
        {
            depth--;
        }
    }

    MovePicker picker;
    movepicker_init(&picker, board, context, ss, tt_move, NULL, 0, false);

    bool has_legal_move = false;
    Move quiet_searched[MAX_QUIET_TRACKED];
    int quiet_searched_count = 0;
    int legal_moves_searched = 0;

    Move move;

    // Main move loop
    while ((move = movepicker_next_move(&picker)) != MOVE_NONE)
    {
        if (search_should_stop(control, stats->nodes))
        {
            break;
        }

        bool is_quiet = !move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE;

        if (futility_prune)
        {
            if (is_quiet && !move_ischeck(board, move))
            {
                continue;
            }
        }

        // Late Move Pruning (LMP) - Quiet only pruning when remaining depth < 11
        if (depth < 11 && !in_check && is_quiet && !pv_node)
        {
            if (quiet_searched_count >= lmp_quiet_limits[depth])
            {
                continue;
            }
        }

        Undo undo;

        if (!board_make_move(board, move, &undo))
        {
            continue; // Illegal move, skip
        }

        has_legal_move = true;

        /* Track searched quiet moves for history malus on cutoff. */
        if (!move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE
            && quiet_searched_count < MAX_QUIET_TRACKED)
        {
            quiet_searched[quiet_searched_count++] = move;
        }

        U64 key = board->hash;
        if (!repetition_history_push(history, key))
        {
            board_unmake_move(board, &undo);
            continue;
        }

        (ss + 1)->ply = ss->ply + 1;
        (ss + 1)->move = move;
        (ss + 1)->static_eval = -32000;

        SearchResult child;
        if (legal_moves_searched == 0)
        {
            // First move is searched with the full window
            child = negamax(board, depth - 1, -beta, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
        }
        else
        {
            // Subsequent moves use a null window
            // Late Move Reductions (LMR)
            int r = 0;
            if (depth >= lmr_min_depth && !move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE)
            {
                int d = depth > 63 ? 63 : depth;
                int m = legal_moves_searched > 255 ? 255 : legal_moves_searched;
                r = LMR[d][m];
            }

            if (r > 0)
            {
                child = negamax(board, depth - 1 - r, -alpha - 1, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
                int score = -child.score;
                if (score > alpha) //If it fails high do a full search
                {
                    child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
                }
            }
            else
            {
                child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
            }
            int score = -child.score;

            // If the null-window search fails high, we must re-search with the full window.
            // Bypassing the re-search is safe if score >= beta, as it will trigger a cutoff anyway.
            if (score > alpha && pv_node && score < beta)
            {
                child = negamax(board, depth - 1, -beta, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
            }
        }
        int score = -child.score;
        legal_moves_searched++;

        --history->count;

        board_unmake_move(board, &undo);

        if (score > result.score || result.move == MOVE_NONE)
        {
            result.score = score;
            result.move = move;
            result.pv[0] = move;
            result.pv_length = 1;
            for (int j = 0; j < child.pv_length && result.pv_length < MAX_PV_MOVES; ++j)
            {
                result.pv[result.pv_length++] = child.pv[j];
            }
        }

        if (score > alpha)
        {
            alpha = score;
        }

        if (alpha >= beta)
        {
            /* record killer and update history if quiet move */
            if (context != NULL && !move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE)
            {
                if (ply >= 0 && ply < MAX_PLY_DEPTH)
                {
                    if (context->killer_moves[ply][0] != move)
                    {
                        context->killer_moves[ply][1] = context->killer_moves[ply][0];
                        context->killer_moves[ply][0] = move;
                    }
                }


                if (previous_move != MOVE_NONE)
                {
                    int prev_from = move_from(previous_move);
                    int prev_to = move_to(previous_move);
                    if (context->counter_moves[prev_from][prev_to][0] != move)
                    {
                        context->counter_moves[prev_from][prev_to][1] = context->counter_moves[prev_from][prev_to][0];
                        context->counter_moves[prev_from][prev_to][0] = move;
                    }
                }

                int bonus = history_bonus(depth);
                int side = board->side;

                /* Bonus for the cutoff move. */
                update_history_entry(&context->hh_table[side][move_from(move)][move_to(move)], bonus);

                /* Malus for all quiet moves searched before the cutoff. */
                for (int q = 0; q < quiet_searched_count; ++q)
                {
                    Move qm = quiet_searched[q];
                    if (qm != move)
                    {
                        update_history_entry(&context->hh_table[side][move_from(qm)][move_to(qm)], -bonus);
                    }
                }
            }

            break;
        }
    }

    if (!has_legal_move)
    {
        if (!in_check && board_has_any_legal_move(board))
        {
            result.score = alpha;
            return result;
        }

        EvalTerminalState terminal_state = eval_terminal_state(board, false);
        if (terminal_state != EVAL_TERMINAL_NONE)
        {
            result.score = eval_terminal_score(terminal_state, ply);
            return result;
        }
    }

    if (context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board->hash, depth, result.score, score_type, result.move, ply);
    }

    return result;
}

SearchResult search_root(Board *board,
                         int depth,
                         int alpha,
                         int beta,
                         RepetitionHistory *history,
                         SearchStats *stats,
                         SearchContext *context,
                         SearchControl *control,
                         SearchMoveInfoCallback on_move_info,
                         void *user_data,
                         bool lichess_draw_rules,
                         const Move *excluded_moves,
                         int excluded_move_count,
                         const Move *search_moves,
                         int search_move_count)
{
    init_lmr();
    SearchResult result = {0, MOVE_NONE, {0}, 0};
    const int alpha_orig = alpha;
    const int beta_orig = beta;
    stats->hashfull = 0;

    SearchStack stack[MAX_PLY_DEPTH + STACK_OFFSET + 4];
    memset(stack, 0, sizeof(stack));
    SearchStack *ss = stack + STACK_OFFSET;
    ss->ply = 0;
    ss->move = MOVE_NONE;
    ss->static_eval = -32000;


    Move tt_move = MOVE_NONE;
    if (context != NULL)
    {
        const TranspositionEntry *entry = transposition_table_lookup(&context->table, board->hash);
        if (entry != NULL)
        {
            tt_move = entry->best_move;
        }
    }

    MovePicker picker;
    movepicker_init(&picker, board, context, ss, tt_move, excluded_moves, excluded_move_count, false);

    if (board_is_draw(board, history, 0, lichess_draw_rules))
    {
        result.score = 0;
        MoveList root_list;
        movegen_generate_pseudo_legal(board, &root_list);
        for (int i = 0; i < root_list.count; ++i)
        {
            Move m = root_list.moves[i];
            if (move_is_in_list(m, excluded_moves, excluded_move_count))
            {
                continue;
            }
            if (search_moves != NULL && search_move_count > 0 && !move_is_in_list(m, search_moves, search_move_count))
            {
                continue;
            }
            Undo undo;
            if (board_make_move(board, m, &undo))
            {
                board_unmake_move(board, &undo);
                result.move = m;
                result.pv[0] = m;
                result.pv_length = 1;
                break;
            }
        }
        return result;
    }

    Move move;
    int move_index = 0;
    int legal_moves_searched = 0;
    while ((move = movepicker_next_move(&picker)) != MOVE_NONE)
    {
        if (search_should_stop(control, stats->nodes))
        {
            break;
        }

        if (search_moves != NULL && search_move_count > 0 && !move_is_in_list(move, search_moves, search_move_count))
        {
            continue;
        }

        Undo undo;
        int curr_index = move_index++;

        if (!board_make_move(board, move, &undo))
        {
            continue;
        }

        unsigned long long nodes_before = stats->nodes;

        U64 key = board->hash;
        if (!repetition_history_push(history, key))
        {
            board_unmake_move(board, &undo);
            continue;
        }

        (ss + 1)->ply = 1;
        (ss + 1)->move = move;
        (ss + 1)->static_eval = -32000;


        SearchResult child;
        if (legal_moves_searched == 0)
        {
            // First move is searched with the full window
            child = negamax(board, depth - 1, -beta, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
        }
        else
        {
            // Subsequent moves use a null window
            // Late Move Reductions (LMR)
            int r = 0;
            if (depth >= lmr_min_depth && !move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE)
            {
                int d = depth > 63 ? 63 : depth;
                int m = legal_moves_searched > 255 ? 255 : legal_moves_searched;
                r = LMR[d][m];
            }

            if (r > 0)
            {
                child = negamax(board, depth - 1 - r, -alpha - 1, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
                int score = -child.score;
                if (score > alpha)
                {
                    child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
                }
            }
            else
            {
                child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
            }
            int score = -child.score;

            // If the null-window search fails high, re-search with the full window.
            // If score >= beta, skip the re-search: the aspiration window loop will
            // widen the bounds and re-search the position from scratch.
            if (score > alpha && score < beta)
            {
                child = negamax(board, depth - 1, -beta, -alpha, history, stats, ss + 1, context, control, lichess_draw_rules);
            }
        }
        int score = -child.score;
        legal_moves_searched++;

        --history->count;

        board_unmake_move(board, &undo);

        unsigned long long nodes_spent = stats->nodes - nodes_before;
        if (context != NULL)
        {
            bool found = false;
            for (int i = 0; i < context->root_moves.count; ++i)
            {
                if (context->root_moves.entries[i].move == move)
                {
                    context->root_moves.entries[i].nodes += nodes_spent;
                    found = true;
                    break;
                }
            }
            if (!found && context->root_moves.count < MAX_QUIET_TRACKED)
            {
                context->root_moves.entries[context->root_moves.count].move = move;
                context->root_moves.entries[context->root_moves.count].nodes = nodes_spent;
                context->root_moves.count++;
            }
        }

        if (on_move_info != NULL)
        {
            on_move_info(depth, curr_index + 1, move, score, user_data);
        }

        if (score > result.score || result.move == MOVE_NONE)
        {
            result.score = score;
            result.move = move;
            result.pv[0] = move;
            result.pv_length = 1;
            for (int j = 0; j < child.pv_length && result.pv_length < MAX_PV_MOVES; ++j)
            {
                result.pv[result.pv_length++] = child.pv[j];
            }
        }

        if (score > alpha)
        {
            alpha = score;
        }

        if (alpha >= beta)
        {
            break;
        }
    }
    // hashfull of 0 means empty TT, 1000 means full TT
    stats->hashfull = (int)((context->table.count * 1000) / (context->table.size * 4));
    if (legal_moves_searched == 0)
    {
        EvalTerminalState terminal_state = eval_terminal_state(board, false);
        if (terminal_state != EVAL_TERMINAL_NONE)
        {
            result.score = eval_terminal_score(terminal_state, 0);
            return result;
        }
    }

    if (context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board->hash, depth, result.score, score_type, result.move, 0);
    }

    return result;
}
