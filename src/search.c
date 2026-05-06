#include "../include/search.h"

#include "eval.h"
#include "movegen.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define TRANSPOSITION_TABLE_SIZE (1U << 20) /* 1 million entries, this must be a power of 2 */
#define MAX_ORDERED_MOVES 256

typedef struct {
    Move move;
    int score;
} ScoredMove;

typedef struct {
    Move move;
    float score;
    bool searched;
} RankedMove;

typedef enum {
    TT_SCORE_UPPER,
    TT_SCORE_EXACT,
    TT_SCORE_LOWER,
} TranspositionScoreType;

typedef struct {
    bool valid;
    U64 hash;
    int depth;
    float score;
    TranspositionScoreType score_type;
    int move_count;
    Move moves[MAX_ORDERED_MOVES];
} TranspositionEntry;

typedef struct {
    TranspositionEntry *entries;
    size_t size;
} TranspositionTable;

struct SearchContext {
    TranspositionTable table;
};

static long long current_time_ms(void) {
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + (long long)now.tv_usec / 1000LL;
}

static bool search_should_stop(SearchControl *control) {
    if (control == NULL) {
        return false;
    }

    if (control->external_stop != NULL && *control->external_stop) {
        control->stop = true;
        return true;
    }

    if (control->stop) {
        return true;
    }

    if (!control->hard_time_limited) {
        return false;
    }

    if (current_time_ms() >= control->hard_stop_time_ms) {
        control->stop = true;
        return true;
    }

    return false;
}

/* Estimate move score for move ordering. This is intentionally cheap. */
static int estimate_move_score(Board *board, Move move) {
    int score = 0;
    int flags = move_flags(move);

    /* Captures - MVV/LVA (Most Valuable Victim / Least Valuable Aggressor). */
    if (move_iscapture(board, move)) {
        int attacker_piece = board_piece_at(board, move_from(move));
        int attacker_type = board_piece_type(attacker_piece);

        int victim_piece = board_piece_at(board, move_to(move));
        int victim_type;

        if ((flags & MOVE_FLAG_EN_PASSANT) != 0) {
            victim_type = WHITE_PAWN; /* type 0 */
        } else {
            victim_type = board_piece_type(victim_piece);
        }

        if (attacker_type >= 0 && attacker_type < 6 && victim_type >= 0 && victim_type < 6) {
            score += 10000 + piece_values[victim_type] * 10 - piece_values[attacker_type];
        }
    }

    /* Promotions. */
    if (move_promotion(move) != MOVE_PROMO_NONE) {
        static const int promo_bonus[5] = {0, 300, 320, 500, 950};
        int promo = move_promotion(move);
        if (promo >= 0 && promo <= 4) {
            score += 20000 + promo_bonus[promo];
        }
    }

    /* Castling. */
    if ((flags & MOVE_FLAG_CASTLE) != 0) {
        score += 100;
    }

    return score;
}

/* Compare function for qsort - sort in descending order of score. */
static int compare_scored_moves(const void *a, const void *b) {
    const ScoredMove *move_a = (const ScoredMove *)a;
    const ScoredMove *move_b = (const ScoredMove *)b;
    return move_b->score - move_a->score;
}

static int compare_ranked_moves(const void *a, const void *b) {
    const RankedMove *move_a = (const RankedMove *)a;
    const RankedMove *move_b = (const RankedMove *)b;

    if (move_a->score < move_b->score) {
        return 1;
    }

    if (move_a->score > move_b->score) {
        return -1;
    }

    return 0;
}

static size_t transposition_table_index(const TranspositionTable *table, U64 hash) {
    return (size_t)(hash & (table->size - 1U));
}

static const TranspositionEntry *transposition_table_lookup(const TranspositionTable *table, U64 hash) {
    if (table == NULL || table->entries == NULL || table->size == 0) {
        return NULL;
    }

    const TranspositionEntry *entry = &table->entries[transposition_table_index(table, hash)];
    if (!entry->valid || entry->hash != hash) {
        return NULL;
    }

    return entry;
}

static bool transposition_table_probe(const TranspositionTable *table,
                                      U64 hash,
                                      int depth,
                                      float alpha,
                                      float beta,
                                      float *score) {
    const TranspositionEntry *entry = transposition_table_lookup(table, hash);
    if (entry == NULL || entry->depth < depth || score == NULL) {
        return false;
    }

    switch (entry->score_type) {
        case TT_SCORE_EXACT:
            *score = entry->score;
            return true;

        case TT_SCORE_LOWER:
            if (entry->score >= beta) {
                *score = entry->score;
                return true;
            }
            break;

        case TT_SCORE_UPPER:
            if (entry->score <= alpha) {
                *score = entry->score;
                return true;
            }
            break;
    }

    return false;
}

static TranspositionScoreType transposition_score_type(float score, float alpha, float beta) {
    if (score <= alpha) {
        return TT_SCORE_UPPER;
    }

    if (score >= beta) {
        return TT_SCORE_LOWER;
    }

    return TT_SCORE_EXACT;
}

static bool transposition_entry_should_replace_same_hash(const TranspositionEntry *entry,
                                                         int depth,
                                                         float score,
                                                         TranspositionScoreType score_type) {
    if (!entry->valid) {
        return true;
    }

    if (depth > entry->depth) {
        return true;
    }

    if (depth < entry->depth) {
        return false;
    }

    if (score_type == TT_SCORE_EXACT) {
        return entry->score_type != TT_SCORE_EXACT;
    }

    if (entry->score_type == TT_SCORE_EXACT) {
        return false;
    }

    if (score_type != entry->score_type) {
        return false;
    }

    if (score_type == TT_SCORE_LOWER) {
        return score > entry->score;
    }

    if (score_type == TT_SCORE_UPPER) {
        return score < entry->score;
    }

    return false;
}

static void transposition_table_store(TranspositionTable *table,
                                      U64 hash,
                                      int depth,
                                      float score,
                                      TranspositionScoreType score_type,
                                      const Move *moves,
                                      int move_count) {
    if (table == NULL || table->entries == NULL || table->size == 0 || moves == NULL || move_count <= 0) {
        return;
    }

    if (move_count > MAX_ORDERED_MOVES) {
        move_count = MAX_ORDERED_MOVES;
    }

    TranspositionEntry *entry = &table->entries[transposition_table_index(table, hash)];
    if (entry->valid && entry->hash == hash) {
        if (!transposition_entry_should_replace_same_hash(entry, depth, score, score_type)) {
            return;
        }
    } else if (entry->valid && entry->hash != hash && depth <= entry->depth) {
        return;
    }

    entry->valid = true;
    entry->hash = hash;
    entry->depth = depth;
    entry->score = score;
    entry->score_type = score_type;
    entry->move_count = move_count;
    memcpy(entry->moves, moves, (size_t)move_count * sizeof(Move));
}

static int find_move_index(const MoveList *list, Move move) {
    if (list == NULL) {
        return -1;
    }

    for (int i = 0; i < list->count; ++i) {
        if (list->moves[i] == move) {
            return i;
        }
    }

    return -1;
}

static int build_ordered_moves(Board *board,
                               const MoveList *list,
                               const TranspositionTable *table,
                               Move ordered_moves[MAX_ORDERED_MOVES]) {
    if (board == NULL || list == NULL || ordered_moves == NULL || list->count <= 0) {
        return 0;
    }

    U64 hash = board_position_key(board);
    const TranspositionEntry *entry = transposition_table_lookup(table, hash);

    if (entry == NULL) {
        ScoredMove scored_moves[MAX_ORDERED_MOVES];
        int scored_count = 0;
        for (int i = 0; i < list->count && i < MAX_ORDERED_MOVES; ++i) {
            scored_moves[scored_count].move = list->moves[i];
            scored_moves[scored_count].score = estimate_move_score(board, list->moves[i]);
            ++scored_count;
        }

        qsort(scored_moves, (size_t)scored_count, sizeof(ScoredMove), compare_scored_moves);
        for (int i = 0; i < scored_count; ++i) {
            ordered_moves[i] = scored_moves[i].move;
        }

        return scored_count;
    }

    bool used[MAX_ORDERED_MOVES] = {false};
    int ordered_count = 0;

    for (int i = 0; i < entry->move_count && ordered_count < list->count; ++i) {
        Move move = entry->moves[i];
        int index = find_move_index(list, move);
        if (index >= 0 && !used[index]) {
            ordered_moves[ordered_count++] = move;
            used[index] = true;
        }
    }

    ScoredMove fallback_moves[MAX_ORDERED_MOVES];
    int fallback_count = 0;
    for (int i = 0; i < list->count && fallback_count < MAX_ORDERED_MOVES; ++i) {
        if (used[i]) {
            continue;
        }

        fallback_moves[fallback_count].move = list->moves[i];
        fallback_moves[fallback_count].score = estimate_move_score(board, list->moves[i]);
        ++fallback_count;
    }

    qsort(fallback_moves, (size_t)fallback_count, sizeof(ScoredMove), compare_scored_moves);
    for (int i = 0; i < fallback_count && ordered_count < MAX_ORDERED_MOVES; ++i) {
        ordered_moves[ordered_count++] = fallback_moves[i].move;
    }

    return ordered_count;
}

static int finalize_move_order(const RankedMove *ranked_moves, int move_count, Move final_order[MAX_ORDERED_MOVES]) {
    if (ranked_moves == NULL || final_order == NULL || move_count <= 0) {
        return 0;
    }

    RankedMove searched_moves[MAX_ORDERED_MOVES];
    int searched_count = 0;

    for (int i = 0; i < move_count && i < MAX_ORDERED_MOVES; ++i) {
        if (ranked_moves[i].searched) {
            searched_moves[searched_count++] = ranked_moves[i];
        }
    }

    qsort(searched_moves, (size_t)searched_count, sizeof(RankedMove), compare_ranked_moves);

    int final_count = 0;
    for (int i = 0; i < searched_count && final_count < MAX_ORDERED_MOVES; ++i) {
        final_order[final_count++] = searched_moves[i].move;
    }

    for (int i = 0; i < move_count && final_count < MAX_ORDERED_MOVES; ++i) {
        if (!ranked_moves[i].searched) {
            final_order[final_count++] = ranked_moves[i].move;
        }
    }

    return final_count;
}

/* Quiescence search: only explores captures and checks. */
static float quiescence(Board *board,
                        float alpha,
                        float beta,
                        RepetitionHistory *history,
                        SearchStats *stats,
                        int ply,
                        TranspositionTable *table,
                        SearchControl *control) {
    const int qsearch_max_ply = 16;
    const int qsearch_forced_only_move_ply_limit = 12;
    const int qsearch_noncapture_check_ply_limit = 6;
    const float alpha_orig = alpha;
    const float beta_orig = beta;

    if (search_should_stop(control)) {
        return evaluate_position(board, history, ply);
    }

    ++stats->nodes;
    if (ply > stats->seldepth) {
        stats->seldepth = ply;
    }

    if (board_is_draw(board, history)) {
        return 0.0f;
    }

    float tt_score = 0.0f;
    if (transposition_table_probe(table, board_position_key(board), 0, alpha, beta, &tt_score)) {
        return tt_score;
    }

    if (ply >= qsearch_max_ply) {
        return evaluate_position(board, history, ply);
    }

    bool in_check = board_is_in_check(board, board->side);

    MoveList list;
    movegen_generate_legal(board, &list);

    EvalTerminalState terminal_state = eval_terminal_state(board, list.count);
    if (terminal_state != EVAL_TERMINAL_NONE) {
        return eval_terminal_score(terminal_state, ply);
    }

    if (!in_check) {
        /* Stand-pat is only valid when side to move can choose to pass tactical action. */
        float stand_pat = evaluate_position(board, history, ply);

        if (stand_pat >= beta) {
            return beta;
        }

        if (stand_pat > alpha) {
            alpha = stand_pat;
        }
    }

    bool allow_forced_only_move = (!in_check &&
                                   list.count == 1 &&
                                   ply < qsearch_forced_only_move_ply_limit);
    bool allow_noncapture_checks = (!in_check &&
                                    !allow_forced_only_move &&
                                    ply < qsearch_noncapture_check_ply_limit);

    /* Filter moves first: collect captures and relevant checks before ordering. */
    Move filtered_moves[MAX_ORDERED_MOVES];
    int filtered_count = 0;

    for (int i = 0; i < list.count && filtered_count < MAX_ORDERED_MOVES; ++i) {
        Move move = list.moves[i];
        bool is_capture = move_iscapture(board, move);

        if (is_capture) {
            filtered_moves[filtered_count++] = move;
        } else if (in_check || allow_forced_only_move) {
            /* When in check or forced single move, accept all legal moves. */
            filtered_moves[filtered_count++] = move;
        } else if (allow_noncapture_checks) {
            /* Pre-compute check status for quiet moves only at shallow plies. */
            if (move_ischeck(board, move)) {
                filtered_moves[filtered_count++] = move;
            }
        }
    }

    /* Build ordered moves from the already-filtered set via direct scoring. */
    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = 0;

    if (filtered_count > 0) {
        ScoredMove scored_moves[MAX_ORDERED_MOVES];
        int scored_count = 0;
        for (int i = 0; i < filtered_count && i < MAX_ORDERED_MOVES; ++i) {
            scored_moves[scored_count].move = filtered_moves[i];
            scored_moves[scored_count].score = estimate_move_score(board, filtered_moves[i]);
            ++scored_count;
        }

        qsort(scored_moves, (size_t)scored_count, sizeof(ScoredMove), compare_scored_moves);
        for (int i = 0; i < scored_count; ++i) {
            ordered_moves[i] = scored_moves[i].move;
        }

        ordered_count = scored_count;
    }

    RankedMove ranked_moves[MAX_ORDERED_MOVES] = {0};
    for (int i = 0; i < ordered_count; ++i) {
        ranked_moves[i].move = ordered_moves[i];
        ranked_moves[i].searched = false;
    }

    for (int i = 0; i < ordered_count; ++i) {
        if (search_should_stop(control)) {
            break;
        }

        Move move = ranked_moves[i].move;

        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        float score = -quiescence(board, -beta, -alpha, history, stats, ply + 1, table, control);

        ranked_moves[i].score = score;
        ranked_moves[i].searched = true;

        --history->count;

        board_unmake_move(board, &undo);

        if (score >= beta) {
            return beta;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    Move final_order[MAX_ORDERED_MOVES];
    int final_count = finalize_move_order(ranked_moves, ordered_count, final_order);
    if (final_count > 0) {
        TranspositionScoreType score_type = transposition_score_type(alpha, alpha_orig, beta_orig);
        transposition_table_store(table, board_position_key(board), 0, alpha, score_type, final_order, final_count);
    }

    return alpha;
}

/* Alpha-beta pruned negamax search. */
static SearchResult negamax(Board *board,
                            int depth,
                            float alpha,
                            float beta,
                            RepetitionHistory *history,
                            SearchStats *stats,
                            int ply,
                            TranspositionTable *table,
                            SearchControl *control) {
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0, false};
    const float alpha_orig = alpha;
    const float beta_orig = beta;

    if (search_should_stop(control)) {
        result.score = evaluate_position(board, history, ply);
        return result;
    }

    ++stats->nodes;

    if (board_is_draw(board, history)) {
        result.score = 0.0f;
        return result;
    }

    float tt_score = 0.0f;
    if (transposition_table_probe(table, board_position_key(board), depth, alpha, beta, &tt_score)) {
        result.score = tt_score;
        return result;
    }

    /* Null-move pruning, avoided in endgames to avoid zugzwang issues. */
    if (depth >= 3 &&
        beta < 10000.0f &&
        !board_is_in_check(board, board->side) &&
        !eval_is_endgame_position(board)) {
        const int reduction = 2;
        Undo undo;
        undo.snapshot = *board;

        if (board->side == BLACK) {
            ++board->fullmove_number;
        }
        board->side ^= 1;
        board->ep_square = -1;
        ++board->halfmove_clock;

        SearchResult null_child = negamax(board,
                                          depth - 1 - reduction,
                                          -beta,
                                          -beta + 1.0f,
                                          history,
                                          stats,
                                          ply + 1,
                                          table,
                                          control);
        float null_score = -null_child.score;

        board_unmake_move(board, &undo);

        if (null_score >= beta) {
            result.score = beta;
            return result;
        }
    }

    if (depth == 0) {
        result.score = quiescence(board, alpha, beta, history, stats, ply, table, control);
        return result;
    }

    MoveList list;
    movegen_generate_legal(board, &list);

    EvalTerminalState terminal_state = eval_terminal_state(board, list.count);
    if (terminal_state != EVAL_TERMINAL_NONE) {
        result.score = eval_terminal_score(terminal_state, ply);
        return result;
    }

    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = build_ordered_moves(board, &list, table, ordered_moves);

    RankedMove ranked_moves[MAX_ORDERED_MOVES] = {0};
    for (int i = 0; i < ordered_count; ++i) {
        ranked_moves[i].move = ordered_moves[i];
        ranked_moves[i].searched = false;
    }

    for (int i = 0; i < ordered_count; ++i) {
        if (search_should_stop(control)) {
            break;
        }

        Move move = ordered_moves[i];
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, ply + 1, table, control);
        float score = -child.score;

        ranked_moves[i].score = score;
        ranked_moves[i].searched = true;

        --history->count;

        board_unmake_move(board, &undo);

        if (score > result.score || result.move == MOVE_NONE) {
            result.score = score;
            result.move = move;
            result.pv[0] = move;
            result.pv_length = 1;
            for (int j = 0; j < child.pv_length && result.pv_length < MAX_PV_MOVES; ++j) {
                result.pv[result.pv_length++] = child.pv[j];
            }
        }

        if (score > alpha) {
            alpha = score;
        }

        if (alpha >= beta) {
            break;
        }
    }

    Move final_order[MAX_ORDERED_MOVES];
    int final_count = finalize_move_order(ranked_moves, ordered_count, final_order);
    if (final_count > 0) {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(table, board_position_key(board), depth, result.score, score_type, final_order, final_count);
    }

    return result;
}

SearchContext *search_context_create(void) {
    SearchContext *context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }

    context->table.size = TRANSPOSITION_TABLE_SIZE;
    context->table.entries = calloc(context->table.size, sizeof(*context->table.entries));
    return context;
}

void search_context_destroy(SearchContext *context) {
    if (context == NULL) {
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
                         void *user_data) {
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0, false};
    float alpha = -FLT_MAX;
    float beta = FLT_MAX;
    const float alpha_orig = alpha;
    const float beta_orig = beta;

    TranspositionTable *table = NULL;
    if (context != NULL && context->table.entries != NULL) {
        table = &context->table;
    }

    MoveList list;
    movegen_generate_legal(board, &list);

    if (board_is_draw(board, history)) {
        result.score = 0.0f;
        if (list.count > 0) {
            result.move = list.moves[0];
            result.pv[0] = list.moves[0];
            result.pv_length = 1;
        }
        return result;
    }

    EvalTerminalState terminal_state = eval_terminal_state(board, list.count);
    if (terminal_state != EVAL_TERMINAL_NONE) {
        result.score = eval_terminal_score(terminal_state, 0);
        return result;
    }

    if (list.count == 1) {
        result.move = list.moves[0];
        result.pv[0] = list.moves[0];
        result.pv_length = 1;
        result.forced_root_move = (control != NULL && control->allow_forced_root_move);
        return result;
    }

    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = build_ordered_moves(board, &list, table, ordered_moves);

    RankedMove ranked_moves[MAX_ORDERED_MOVES] = {0};
    for (int i = 0; i < ordered_count; ++i) {
        ranked_moves[i].move = ordered_moves[i];
        ranked_moves[i].searched = false;
    }

    for (int i = 0; i < ordered_count; ++i) {
        if (search_should_stop(control)) {
            break;
        }

        Move move = ordered_moves[i];
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, 1, table, control);
        float score = -child.score;

        ranked_moves[i].score = score;
        ranked_moves[i].searched = true;

        --history->count;

        board_unmake_move(board, &undo);

        if (on_move_info != NULL) {
            on_move_info(depth, i + 1, move, score, user_data);
        }

        if (score > result.score || result.move == MOVE_NONE) {
            result.score = score;
            result.move = move;
            result.pv[0] = move;
            result.pv_length = 1;
            for (int j = 0; j < child.pv_length && result.pv_length < MAX_PV_MOVES; ++j) {
                result.pv[result.pv_length++] = child.pv[j];
            }
        }

        if (score > alpha) {
            alpha = score;
        }

        if (alpha >= beta) {
            break;
        }
    }

    Move final_order[MAX_ORDERED_MOVES];
    int final_count = finalize_move_order(ranked_moves, ordered_count, final_order);
    if (final_count > 0) {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(table, board_position_key(board), depth, result.score, score_type, final_order, final_count);
    }

    return result;
}
