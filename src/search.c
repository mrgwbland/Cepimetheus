#include "../include/search.h"

#include "eval.h"
#include "movegen.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define MAX_TRANSPOSITION_TABLE_POWER 30
#define MAX_ORDERED_MOVES 256
#define MAX_PLY_DEPTH 256

typedef struct
{
    Move move;
    int score;
} ScoredMove;

typedef struct
{
    Move move;
    int score;
    bool searched;
} RankedMove;

typedef uint8_t TranspositionScoreType;
#define TT_SCORE_UPPER 0
#define TT_SCORE_EXACT 1
#define TT_SCORE_LOWER 2

typedef struct
{
    U64 hash;
    Move best_move;
    int16_t score;
    int8_t depth;
    TranspositionScoreType score_type;
} TranspositionEntry; // 16 bytes per entry

typedef struct
{
    TranspositionEntry *entries;
    size_t size;
    size_t count;
} TranspositionTable;

struct SearchContext
{
    TranspositionTable table;
    Move killer_moves[MAX_PLY_DEPTH][2];
};

static long long current_time_ms(void)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0)
    {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + (long long)now.tv_usec / 1000LL;
}

static bool search_should_stop(SearchControl *control)
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

    if (current_time_ms() >= control->hard_stop_time_ms)
    {
        control->stop = true;
        return true;
    }

    return false;
}

/* Estimate move score for move ordering. This is intentionally cheap. */
static int estimate_move_score(Board *board, Move move, const SearchContext *context, int ply)
{

    /* Killer moves: prefer quiet moves that previously caused beta cutoffs. */
    if (context != NULL && ply >= 0 && ply < MAX_PLY_DEPTH)
    {
        /* First killer (most recent) slightly preferred. */
        if (context->killer_moves[ply][0] != MOVE_NONE && context->killer_moves[ply][0] == move)
        {
            return 5000;
        }

        if (context->killer_moves[ply][1] != MOVE_NONE && context->killer_moves[ply][1] == move)
        {
            return 4000;
        }
    }

    int flags = move_flags(move);

    /* Castling. */
    if ((flags & MOVE_FLAG_CASTLE) != 0)
    {
        return 100;
    }

    int score = 0;

    /* Captures - MVV/LVA (Most Valuable Victim / Least Valuable Aggressor). */
    if (move_iscapture(move))
    {
        int attacker_piece = board_piece_at(board, move_from(move));
        int attacker_value = piece_values[board_piece_type(attacker_piece)];

        int victim_piece = board_piece_at(board, move_to(move));
        int victim_value;

        if ((flags & MOVE_FLAG_EN_PASSANT) != 0)
        {
            victim_value = piece_values[WHITE_PAWN];
        }
        else
        {
            victim_value = piece_values[board_piece_type(victim_piece)];
        }

        score += 10000 + victim_value * 10 - attacker_value;
    }

    /* Promotions. */
    if (move_promotion(move) != MOVE_PROMO_NONE)
    {
        static const int promo_bonus[5] = {0, 300, 320, 500, 950};
        int promo = move_promotion(move);
        if (promo >= 0 && promo <= 4)
        {
            score += 20000 + promo_bonus[promo];
        }
    }

    return score;
}

/* Compare function for sort - sort in descending order of score. */
static int compare_scored_moves(const void *a, const void *b)
{
    const ScoredMove *move_a = (const ScoredMove *)a;
    const ScoredMove *move_b = (const ScoredMove *)b;
    return move_b->score - move_a->score;
}

static void insertion_sort_scored_moves(ScoredMove *moves, int count)
{
    for (int i = 1; i < count; ++i)
    {
        ScoredMove key = moves[i];
        int j = i - 1;
        while (j >= 0 && compare_scored_moves(&moves[j], &key) > 0)
        {
            moves[j + 1] = moves[j];
            --j;
        }
        moves[j + 1] = key;
    }
}

static size_t transposition_table_index(const TranspositionTable *table, U64 hash)
{
    return (size_t)(hash & (table->size - 1U));
}

static const TranspositionEntry *transposition_table_lookup(const TranspositionTable *table, U64 hash)
{
    if (table == NULL || table->entries == NULL || table->size == 0)
    {
        return NULL;
    }

    const TranspositionEntry *entry = &table->entries[transposition_table_index(table, hash)];
    if (entry->hash == 0 || entry->hash != hash)
    {
        return NULL;
    }

    return entry;
}

static bool transposition_table_probe(const TranspositionTable *table,
                                      U64 hash,
                                      int depth,
                                      int alpha,
                                      int beta,
                                      int *score)
{
    const TranspositionEntry *entry = transposition_table_lookup(table, hash);
    if (entry == NULL || entry->depth < depth || score == NULL)
    {
        return false;
    }

    switch (entry->score_type)
    {
    case TT_SCORE_EXACT:
        *score = entry->score;
        return true;

    case TT_SCORE_LOWER:
        if (entry->score >= beta)
        {
            *score = entry->score;
            return true;
        }
        break;

    case TT_SCORE_UPPER:
        if (entry->score <= alpha)
        {
            *score = entry->score;
            return true;
        }
        break;
    }

    return false;
}

static TranspositionScoreType transposition_score_type(int score, int alpha, int beta)
{
    if (score <= alpha)
    {
        return TT_SCORE_UPPER;
    }

    if (score >= beta)
    {
        return TT_SCORE_LOWER;
    }

    return TT_SCORE_EXACT;
}

static bool transposition_entry_should_replace_same_hash(const TranspositionEntry *entry,
                                                         int depth,
                                                         int score,
                                                         TranspositionScoreType score_type)
{
    if (depth > entry->depth)
    {
        return true;
    }

    if (depth < entry->depth)
    {
        return false;
    }

    if (score_type == TT_SCORE_EXACT)
    {
        return entry->score_type != TT_SCORE_EXACT;
    }

    if (entry->score_type == TT_SCORE_EXACT)
    {
        return false;
    }

    if (score_type != entry->score_type)
    {
        return false;
    }

    if (score_type == TT_SCORE_LOWER)
    {
        return score > entry->score;
    }

    if (score_type == TT_SCORE_UPPER)
    {
        return score < entry->score;
    }

    return false;
}

static void transposition_table_store(TranspositionTable *table,
                                      U64 hash,
                                      int depth,
                                      int score,
                                      TranspositionScoreType score_type,
                                      Move best_move)
{
    if (table == NULL || table->entries == NULL || table->size == 0 || best_move == MOVE_NONE)
    {
        return;
    }

    TranspositionEntry *entry = &table->entries[transposition_table_index(table, hash)];
    if (entry->hash == hash)
    {
        if (!transposition_entry_should_replace_same_hash(entry, depth, score, score_type))
        {
            return;
        }
    }
    else if (entry->hash != 0 && depth <= entry->depth)
    {
        return;
    }

    if (entry->hash == 0)
    {
        table->count++;
    }

    entry->hash = hash;
    entry->depth = depth;
    entry->score = score;
    entry->score_type = score_type;
    entry->best_move = best_move;
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

static int build_ordered_moves(Board *board,
                               const MoveList *list,
                               const SearchContext *context,
                               int ply,
                               const Move *excluded_moves,
                               int excluded_move_count,
                               Move ordered_moves[MAX_ORDERED_MOVES])
{
    if (board == NULL || list == NULL || ordered_moves == NULL || list->count <= 0)
    {
        return 0;
    }

    U64 hash = board_position_key(board);
    const TranspositionEntry *entry = NULL;
    if (context != NULL)
    {
        entry = transposition_table_lookup(&context->table, hash);
    }

    if (entry == NULL)
    {
        ScoredMove scored_moves[MAX_ORDERED_MOVES];
        int scored_count = 0;
        for (int i = 0; i < list->count && i < MAX_ORDERED_MOVES; ++i)
        {
            if (move_is_excluded(list->moves[i], excluded_moves, excluded_move_count))
            {
                continue;
            }
            scored_moves[scored_count].move = list->moves[i];
            scored_moves[scored_count].score = estimate_move_score(board, list->moves[i], context, ply);
            ++scored_count;
        }

        insertion_sort_scored_moves(scored_moves, scored_count);
        for (int i = 0; i < scored_count; ++i)
        {
            ordered_moves[i] = scored_moves[i].move;
        }

        return scored_count;
    }

    bool used[MAX_ORDERED_MOVES] = {false};
    int ordered_count = 0;

    if (!move_is_excluded(entry->best_move, excluded_moves, excluded_move_count))
    {
        int index = find_move_index(list, entry->best_move);
        if (index >= 0)
        {
            ordered_moves[ordered_count++] = entry->best_move;
            used[index] = true;
        }
    }

    ScoredMove fallback_moves[MAX_ORDERED_MOVES];
    int fallback_count = 0;
    for (int i = 0; i < list->count && fallback_count < MAX_ORDERED_MOVES; ++i)
    {
        if (used[i])
        {
            continue;
        }

        if (move_is_excluded(list->moves[i], excluded_moves, excluded_move_count))
        {
            continue;
        }

        fallback_moves[fallback_count].move = list->moves[i];
        fallback_moves[fallback_count].score = estimate_move_score(board, list->moves[i], context, ply);
        ++fallback_count;
    }

    insertion_sort_scored_moves(fallback_moves, fallback_count);
    for (int i = 0; i < fallback_count && ordered_count < MAX_ORDERED_MOVES; ++i)
    {
        ordered_moves[ordered_count++] = fallback_moves[i].move;
    }

    return ordered_count;
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

/* Quiescence search: only explores captures, checks and single legal move positions. */
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
    const int qsearch_max_depth = 16;
    const int alpha_orig = alpha;
    const int beta_orig = beta;

    if (search_should_stop(control))
    {
        MoveList dummy_list = {.count = 1};
        return evaluate_position(board, history, ply, &dummy_list, lichess_draw_rules);
    }

    ++stats->nodes;
    if (ply > stats->seldepth)
    {
        stats->seldepth = ply;
    }

    if (board_is_draw(board, history, lichess_draw_rules))
    {
        return 0;
    }

    int tt_score = 0;
    if (context != NULL)
    {
        if (transposition_table_probe(&context->table, board_position_key(board), 0, alpha, beta, &tt_score))
        {
            return tt_score;
        }
    }

    if (qply >= qsearch_max_depth)
    {
        MoveList dummy_list = {.count = 1};
        return evaluate_position(board, history, ply, &dummy_list, lichess_draw_rules);
    }

    bool in_check = board_is_in_check(board, board->side);
    Move best_move = MOVE_NONE;

    if (!in_check)
    {
        /* Stand-pat is only valid when side to move can choose to pass tactical action. */
        MoveList dummy_list = {.count = 1};
        int stand_pat = evaluate_position(board, history, ply, &dummy_list, lichess_draw_rules);

        if (stand_pat >= beta)
        {
            return beta;
        }

        if (stand_pat > alpha)
        {
            alpha = stand_pat;
        }
    }

    MoveList list;
    movegen_generate_pseudo_legal(board, &list);

    /* Streamlined scoring and sorting directly from list.moves */
    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = 0;

    ScoredMove scored_moves[MAX_ORDERED_MOVES];
    int scored_count = 0;

    for (int i = 0; i < list.count && scored_count < MAX_ORDERED_MOVES; ++i)
    {
        Move move = list.moves[i];
        if (in_check || move_iscapture(move))
        {
            scored_moves[scored_count].move = move;
            // Pass NULL as context to disable killer move sorting in qsearch
            scored_moves[scored_count].score = estimate_move_score(board, move, NULL, ply);
            scored_count++;
        }
    }

    if (scored_count > 0)
    {
        insertion_sort_scored_moves(scored_moves, scored_count);
        for (int i = 0; i < scored_count; ++i)
        {
            ordered_moves[i] = scored_moves[i].move;
        }
        ordered_count = scored_count;
    }

    bool has_legal_move = false;

    for (int i = 0; i < ordered_count; ++i)
    {
        if (search_should_stop(control))
        {
            break;
        }

        Move move = ordered_moves[i];

        Undo undo;

        if (!board_make_move(board, move, &undo))
        {
            continue;
        }

        has_legal_move = true;

        U64 key = board_position_key(board);
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
        if (!in_check && has_any_legal_move(board, &list))
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
        TranspositionScoreType score_type = transposition_score_type(alpha, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board_position_key(board), 0, alpha, score_type, best_move);
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
                            SearchContext *context,
                            SearchControl *control,
                            bool lichess_draw_rules)
{
    SearchResult result = {0, MOVE_NONE, {0}, 0, false};
    const int alpha_orig = alpha;
    const int beta_orig = beta;

    if (search_should_stop(control))
    {
        MoveList eval_list;
        movegen_generate_pseudo_legal(board, &eval_list);
        result.score = evaluate_position(board, history, ply, &eval_list, lichess_draw_rules);
        return result;
    }

    ++stats->nodes;
    if (ply > stats->seldepth)
    {
        stats->seldepth = ply;
    }

    if (board_is_draw(board, history, lichess_draw_rules))
    {
        result.score = 0;
        return result;
    }

    int tt_score = 0;
    if (context != NULL)
    {
        if (transposition_table_probe(&context->table, board_position_key(board), depth, alpha, beta, &tt_score))
        {
            result.score = tt_score;
            return result;
        }
    }

    /* Null-move pruning, avoided in endgames to avoid zugzwang issues. */
    if (depth >= 3 &&
        beta < 10000 &&
        !board_is_in_check(board, board->side) &&
        get_endgame_weight(board) < 600)
    {
        const int reduction = 2;
        Undo undo;
        undo.snapshot = *board;

        if (board->side == BLACK)
        {
            ++board->fullmove_number;
        }
        board->side ^= 1;
        board->ep_square = -1;
        ++board->halfmove_clock;

        SearchResult null_child = negamax(board,
                                          depth - 1 - reduction,
                                          -beta,
                                          -beta + 1,
                                          history,
                                          stats,
                                          ply + 1,
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

    if (depth == 0)
    {
        result.score = quiescence(board, alpha, beta, history, stats, ply, 0, context, control, lichess_draw_rules);
        return result;
    }

    MoveList list;
    movegen_generate_pseudo_legal(board, &list);

    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = build_ordered_moves(board, &list, context, ply, NULL, 0, ordered_moves);

    bool has_legal_move = false;

    for (int i = 0; i < ordered_count; ++i)
    {
        if (search_should_stop(control))
        {
            break;
        }

        Move move = ordered_moves[i];
        Undo undo;

        if (!board_make_move(board, move, &undo))
        {
            continue; // Illegal move, skip
        }

        has_legal_move = true;

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key))
        {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, ply + 1, context, control, lichess_draw_rules);
        int score = -child.score;

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
            /* record killer if quiet move */
            if (context != NULL)
            {
                if (!move_iscapture(move) && move_promotion(move) == MOVE_PROMO_NONE)
                {
                    if (ply >= 0 && ply < MAX_PLY_DEPTH)
                    {
                        if (context->killer_moves[ply][0] != move)
                        {
                            context->killer_moves[ply][1] = context->killer_moves[ply][0];
                            context->killer_moves[ply][0] = move;
                        }
                    }
                }
            }

            break;
        }
    }

    EvalTerminalState terminal_state = eval_terminal_state(board, has_legal_move);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        result.score = eval_terminal_score(terminal_state, ply);
        return result;
    }

    if (result.move != MOVE_NONE && context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board_position_key(board), depth, result.score, score_type, result.move);
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

    if (hash_power > MAX_TRANSPOSITION_TABLE_POWER)
    {
        hash_power = MAX_TRANSPOSITION_TABLE_POWER;
    }

    context->table.size = (size_t)1 << hash_power;
    context->table.entries = calloc(context->table.size, sizeof(*context->table.entries));
    if (context->table.entries == NULL)
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

    free(context->table.entries);
    free(context);
}

SearchResult search_root(Board *board,
                         int depth,
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
    SearchResult result = {0, MOVE_NONE, {0}, 0, false};
    int alpha = -MATE_SCORE;
    int beta = MATE_SCORE;
    const int alpha_orig = alpha;
    const int beta_orig = beta;
    stats->hashfull = 0;
    /* `context` holds the transposition table and killer moves. */

    MoveList list;
    movegen_generate_pseudo_legal(board, &list);

    if (board_is_draw(board, history, lichess_draw_rules))
    {
        result.score = 0;
        for (int i = 0; i < list.count; ++i)
        {
            if (move_is_excluded(list.moves[i], excluded_moves, excluded_move_count))
            {
                continue;
            }
            result.move = list.moves[i];
            result.pv[0] = list.moves[i];
            result.pv_length = 1;
            break;
        }
        return result;
    }

    EvalTerminalState terminal_state = eval_terminal_state(board, list.count);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        result.score = eval_terminal_score(terminal_state, 0);
        return result;
    }

    if (list.count == 1)
    {
        if (!move_is_excluded(list.moves[0], excluded_moves, excluded_move_count))
        {
            result.move = list.moves[0];
            result.pv[0] = list.moves[0];
            result.pv_length = 1;
            result.forced_root_move = (control != NULL && control->allow_forced_root_move);
        }
        return result;
    }

    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = build_ordered_moves(board, &list, context, 0, excluded_moves, excluded_move_count, ordered_moves);

    for (int i = 0; i < ordered_count; ++i)
    {
        if (search_should_stop(control))
        {
            break;
        }

        Move move = ordered_moves[i];
        Undo undo;

        if (!board_make_move(board, move, &undo))
        {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key))
        {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, 1, context, control, lichess_draw_rules);
        int score = -child.score;

        --history->count;

        board_unmake_move(board, &undo);

        if (on_move_info != NULL)
        {
            on_move_info(depth, i + 1, move, score, user_data);
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
    stats->hashfull = (int)((context->table.count * 1000) / context->table.size);
    if (result.move != MOVE_NONE && context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board_position_key(board), depth, result.score, score_type, result.move);
    }

    return result;
}
