#include "../include/search.h"

#include "eval.h"
#include "movegen.h"
#include "movepicker.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define FUTILITY_MARGIN 1500
#define RFP_MARGIN 850
#define RFP_MAX_DEPTH 8

static int LMR[64][256];
static bool lmr_initialised = false;

void init_lmr(void)
{
    if (lmr_initialised)
    {
        return;
    }

    for (int depth = 0; depth < 64; ++depth)
    {
        for (int moves = 0; moves < 256; ++moves)
        {
            if (depth == 0 || moves == 0)
            {
                LMR[depth][moves] = 0;
            }
            else
            {
                int r = (int)(((log(depth) * log(2*moves)) / 2) -0.25); //Tune at some point
                LMR[depth][moves] = r > 0 ? r : 0;
            }
        }
    }
    lmr_initialised = true;
}

static long long current_time_ms(void)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0)
    {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + (long long)now.tv_usec / 1000LL;
}

static bool has_sufficient_nmp_material(const Board *board)
{
    int side = board->side;
    int start = (side == WHITE) ? WHITE_KNIGHT : BLACK_KNIGHT;
    int end = (side == WHITE) ? WHITE_KING : BLACK_KING;
    int piece_count = 0;

    for (int i = start; i < end; ++i)
    {
        U64 bb = board->pieces[i];
        while (bb)
        {
            bitboard_pop_lsb(&bb);
            piece_count++;
            if (piece_count >= 3)
            {
                return true;
            }
        }
    }

    return false;
}

static bool search_should_stop(SearchControl *control, long long nodes)
{
    if (control == NULL)
    {
        return false;
    }
    
    if (control->external_stop != NULL && *control->external_stop)
    {
        control->stop = true;
        return true;
    }

    if (control->stop)
    {
        return true;
    }

    if (!control->hard_time_limited)
    {
        return false;
    }
    if ((nodes & 1023) == 0)
    {
        if (current_time_ms() >= control->hard_stop_time_ms)
        {
            control->stop = true;
            return true;
        }
    }

    return false;
}

static int find_move_index(const MoveList *list, Move move)
{
    if (list == NULL)
    {
        return -1;
    }

    for (int i = 0; i < list->count; ++i)
    {
        if (list->moves[i] == move)
        {
            return i;
        }
    }

    return -1;
}

static bool move_is_excluded(Move move, const Move *excluded_moves, int excluded_move_count)
{
    if (excluded_moves == NULL || excluded_move_count <= 0)
    {
        return false;
    }

    for (int i = 0; i < excluded_move_count; ++i)
    {
        if (excluded_moves[i] == move)
        {
            return true;
        }
    }

    return false;
}

/* Returns true if any pseudo-legal move is legal in the current position. */
static bool has_any_legal_move(Board *board, const MoveList *list)
{
    if (board == NULL || list == NULL)
    {
        return false;
    }

    for (int i = 0; i < list->count; ++i)
    {
        Undo undo;
        if (board_make_move(board, list->moves[i], &undo))
        {
            board_unmake_move(board, &undo);
            return true;
        }
    }

    return false;
}

static int get_captured_piece_value(const Board *board, Move move)
{
    int flags = move_flags(move);
    if (flags & MOVE_FLAG_EN_PASSANT)
    {
        return 1000; // Pawn value
    }

    int target_piece = board_piece_at(board, move_to(move));
    if (target_piece >= 0)
    {
        int type = board_piece_type(target_piece);
        if (type >= 0 && type < 6)
        {
            return piece_values[type];
        }
    }

    return 0;
}

static int quiescence(Board *board,
                      int alpha,
                      int beta,
                      RepetitionHistory *history,
                      SearchStats *stats,
                      int ply,
                      int qply,
                      SearchContext *context,
                      SearchControl *control,
                      bool lichess_draw_rules)
{
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
    movepicker_init(&picker, board, NULL, ply, MOVE_NONE, MOVE_NONE, NULL, 0, true);

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

            const int SAFETY_MARGIN = 3000;//Found optimal to the nearest 1000 in 10s+0.1s
            if (stand_pat + gain + SAFETY_MARGIN < alpha)
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

        int score = -quiescence(board, -beta, -alpha, history, stats, ply + 1, qply + 1, context, control, lichess_draw_rules);

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
        if (!in_check && has_any_legal_move(board, &picker.all_moves))
        {
            return alpha;
        }

        EvalTerminalState terminal_state = eval_terminal_state(board, false);
        if (terminal_state != EVAL_TERMINAL_NONE)
        {
            return eval_terminal_score(terminal_state, ply);
        }
    }

    if (best_move != MOVE_NONE && context != NULL)
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
                            int ply,
                            Move previous_move,
                            SearchContext *context,
                            SearchControl *control,
                            bool lichess_draw_rules)
{
    SearchResult result = {0, MOVE_NONE, {0}, 0, false};
    const int alpha_orig = alpha;
    const int beta_orig = beta;

    // Determine PV node implicitly by checking if the search window width is greater than 1.
    const bool pv_node = (beta - alpha) > 1;

    if (search_should_stop(control, stats->nodes))
    {
        MoveList eval_list;
        movegen_generate_pseudo_legal(board, &eval_list);
        result.score = evaluate_position(board);
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
        // On non-PV nodes, allow all TT cutoffs (EXACT, LOWER, UPPER).
        // On PV nodes, only allow EXACT cutoffs — bound scores from null-window searches would return imprecise values and corrupt the PV.
        // Using TT in PV can cause the info PV to be truncated but this is neccessary to maximise search speed
        bool tt_cutoff = pv_node
            ? transposition_table_probe_exact(&context->table, board->hash, depth, ply, &tt_score)
            : transposition_table_probe(&context->table, board->hash, depth, alpha, beta, ply, &tt_score);

        bool is_repeated = false;
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

                MoveList list;
                movegen_generate_pseudo_legal(board, &list);
                if (find_move_index(&list, entry->best_move) < 0)
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

    if (depth <= 0)
    {
        result.score = quiescence(board, alpha, beta, history, stats, ply, 0, context, control, lichess_draw_rules);
        return result;
    }

    /* Null-move pruning */
    if (!pv_node &&
        depth >= 3 &&// NMP not done near leaves as tree is already small, and NMP has overhead
        beta < MATE_SCORE - MAX_PLY_DEPTH &&// Not done in mating sequences
        !board_is_in_check(board, board->side) && // In check passing is illegal
        has_sufficient_nmp_material(board)) //Not done in endgames to avoid zugzwang issues
    {
        const int reduction = 2;
        Undo undo;
        board_make_null_move(board, &undo);

        SearchResult null_child = negamax(board,
                                          depth - 1 - reduction,
                                          -beta,
                                          -beta + 1,
                                          history,
                                          stats,
                                          ply + 1,
                                          MOVE_NONE,
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
    if (in_check)
    {
        depth++;
    }

    int static_eval = evaluate_position(board);

    // Reverse Futility Pruning: At realtively shallow non-PV nodes, if the static eval exceeds beta by a depth-dependent margin, prune the entire node.
    if (!pv_node && !in_check && depth <= RFP_MAX_DEPTH
        && abs(static_eval) < MATE_SCORE - MAX_PLY_DEPTH // Don't prune in mating sequences
        && static_eval - RFP_MARGIN * depth > beta)
    {
        result.score = static_eval;
        return result;
    }

    // Futility Pruning: At depth 1, if static evaluation plus a safety margin is still less than alpha, prune all remaining quiet moves
    bool futility_prune = false;
    if (depth == 1 && !in_check && abs(alpha) < MATE_SCORE - MAX_PLY_DEPTH)
    {
        if (static_eval + FUTILITY_MARGIN < alpha)
        {
            futility_prune = true;
        }
    }

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
    movepicker_init(&picker, board, context, ply, previous_move, tt_move, NULL, 0, false);

    bool has_legal_move = false;
    Move quiet_searched[MAX_QUIET_TRACKED];
    int quiet_searched_count = 0;
    int legal_moves_searched = 0;

    Move move;
    while ((move = movepicker_next_move(&picker)) != MOVE_NONE)
    {
        if (search_should_stop(control, stats->nodes))
        {
            break;
        }

        if (futility_prune)
        {
            bool is_quiet = !move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE;
            if (is_quiet && !move_ischeck(board, move))
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

        SearchResult child;
        if (legal_moves_searched == 0)
        {
            // First move is searched with the full window
            child = negamax(board, depth - 1, -beta, -alpha, history, stats, ply + 1, move, context, control, lichess_draw_rules);
        }
        else
        {
            // Subsequent moves use a null window
            // Late Move Reductions (LMR)
            int r = 0;
            if (depth >= 3 && !move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE)
            {
                int d = depth > 63 ? 63 : depth;
                int m = legal_moves_searched > 255 ? 255 : legal_moves_searched;
                r = LMR[d][m];
            }

            if (r > 0)
            {
                child = negamax(board, depth - 1 - r, -alpha - 1, -alpha, history, stats, ply + 1, move, context, control, lichess_draw_rules);
                int score = -child.score;
                if (score > alpha)
                {
                    child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, ply + 1, move, context, control, lichess_draw_rules);
                }
            }
            else
            {
                child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, ply + 1, move, context, control, lichess_draw_rules);
            }
            int score = -child.score;

            // If the null-window search fails high, we must re-search with the full window.
            // Bypassing the re-search is safe if score >= beta, as it will trigger a cutoff anyway.
            if (score > alpha && pv_node && score < beta)
            {
                child = negamax(board, depth - 1, -beta, -alpha, history, stats, ply + 1, move, context, control, lichess_draw_rules);
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
        if (!in_check && has_any_legal_move(board, &picker.all_moves))
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

    if (result.move != MOVE_NONE && context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board->hash, depth, result.score, score_type, result.move, ply);
    }

    return result;
}

SearchContext *search_context_create(size_t hash_power)
{
    SearchContext *context = calloc(1, sizeof(*context));
    if (context == NULL)
    {
        return NULL;
    }

    for (int p = 0; p < MAX_PLY_DEPTH; ++p)
    {
        context->killer_moves[p][0] = MOVE_NONE;
        context->killer_moves[p][1] = MOVE_NONE;
    }
    for (int f = 0; f < 64; ++f)
    {
        for (int t = 0; t < 64; ++t)
        {
            context->counter_moves[f][t][0] = MOVE_NONE;
            context->counter_moves[f][t][1] = MOVE_NONE;
        }
    }

    if (!transposition_table_init(&context->table, hash_power))
    {
        free(context);
        return NULL;
    }

    return context;
}

void search_context_destroy(SearchContext *context)
{
    if (context == NULL)
    {
        return;
    }

    transposition_table_destroy(&context->table);
    free(context);
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
                         int excluded_move_count)
{
    init_lmr();
    SearchResult result = {0, MOVE_NONE, {0}, 0, false};
    const int alpha_orig = alpha;
    const int beta_orig = beta;
    stats->hashfull = 0;
    /* `context` holds the transposition table and killer moves. */

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
    movepicker_init(&picker, board, context, 0, MOVE_NONE, tt_move, excluded_moves, excluded_move_count, false);

    if (board_is_draw(board, history, 0, lichess_draw_rules))
    {
        result.score = 0;
        for (int i = 0; i < picker.all_moves.count; ++i)
        {
            Move m = picker.all_moves.moves[i];
            if (move_is_excluded(m, excluded_moves, excluded_move_count))
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

    EvalTerminalState terminal_state = eval_terminal_state(board, picker.all_moves.count);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        result.score = eval_terminal_score(terminal_state, 0);
        return result;
    }

    // Count legal moves to detect forced moves.
    // This is only done at the root, so the cost of testing legality is negligible.
    if (control != NULL && control->allow_forced_root_move)
    {
        int legal_count = 0;
        Move only_legal_move = MOVE_NONE;
        for (int i = 0; i < picker.all_moves.count; ++i)
        {
            Move m = picker.all_moves.moves[i];
            if (move_is_excluded(m, excluded_moves, excluded_move_count))
            {
                continue;
            }
            Undo undo;
            if (board_make_move(board, m, &undo))
            {
                board_unmake_move(board, &undo);
                legal_count++;
                only_legal_move = m;
                if (legal_count > 1)
                {
                    break; // No need to count further
                }
            }
        }
        if (legal_count == 1)
        {
            result.move = only_legal_move;
            result.pv[0] = only_legal_move;
            result.pv_length = 1;
            result.forced_root_move = true;
            return result;
        }
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

        Undo undo;
        int curr_index = move_index++;

        if (!board_make_move(board, move, &undo))
        {
            continue;
        }

        U64 key = board->hash;
        if (!repetition_history_push(history, key))
        {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child;
        if (legal_moves_searched == 0)
        {
            // First move is searched with the full window
            child = negamax(board, depth - 1, -beta, -alpha, history, stats, 1, move, context, control, lichess_draw_rules);
        }
        else
        {
            // Subsequent moves use a null window
            // Late Move Reductions (LMR)
            int r = 0;
            if (depth >= 3 && !move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE)
            {
                int d = depth > 63 ? 63 : depth;
                int m = legal_moves_searched > 255 ? 255 : legal_moves_searched;
                r = LMR[d][m];
            }

            if (r > 0)
            {
                child = negamax(board, depth - 1 - r, -alpha - 1, -alpha, history, stats, 1, move, context, control, lichess_draw_rules);
                int score = -child.score;
                if (score > alpha)
                {
                    child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, 1, move, context, control, lichess_draw_rules);
                }
            }
            else
            {
                child = negamax(board, depth - 1, -alpha - 1, -alpha, history, stats, 1, move, context, control, lichess_draw_rules);
            }
            int score = -child.score;

            // If the null-window search fails high, re-search with the full window.
            // If score >= beta, skip the re-search: the aspiration window loop will
            // widen the bounds and re-search the position from scratch.
            if (score > alpha && score < beta)
            {
                child = negamax(board, depth - 1, -beta, -alpha, history, stats, 1, move, context, control, lichess_draw_rules);
            }
        }
        int score = -child.score;
        legal_moves_searched++;

        --history->count;

        board_unmake_move(board, &undo);

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
    if (result.move != MOVE_NONE && context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board->hash, depth, result.score, score_type, result.move, 0);
    }

    return result;
}
