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
#define HISTORY_BONUS_CAP 400
#define HISTORY_GRAVITY 512
#define HISTORY_SCALE 16
#define MAX_QUIET_TRACKED 256

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
    int16_t hh_table[2][64][64];
};

static const int piece_values[6] = {
    1000, /* Pawn */
    3000, /* Knight */
    3200, /* Bishop */
    5000, /* Rook */
    9000, /* Queen */
    0    /* King */
};

static inline int history_bonus(int depth)
{
    int b = depth * depth;
    return b < HISTORY_BONUS_CAP ? b : HISTORY_BONUS_CAP;
}

static inline void update_history_entry(int16_t *entry, int delta)
{
    *entry += HISTORY_SCALE * delta - *entry * abs(delta) / HISTORY_GRAVITY;
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
    int flags = move_flags(move);

    /* 1. Promotions */
    if (move_promotion(move) != MOVE_PROMO_NONE)
    {
        static const int promo_bonus[5] = {0, 300, 320, 500, 950};
        int promo = move_promotion(move);
        if (promo >= 0 && promo <= 4)
        {
            return 2000000 + promo_bonus[promo];
        }
    }

    /* 2. Captures - MVV/LVA */
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

        return 1000000 + victim_value * 10 - attacker_value;
    }

    /* 3. Quiet Moves (Killers, History) */
    if (context != NULL && ply >= 0 && ply < MAX_PLY_DEPTH)
    {
        /* First killer */
        if (context->killer_moves[ply][0] != MOVE_NONE && context->killer_moves[ply][0] == move)
        {
            return 90000;
        }

        /* Second killer */
        if (context->killer_moves[ply][1] != MOVE_NONE && context->killer_moves[ply][1] == move)
        {
            return 80000;
        }
    }

    /* Castling */
    if ((flags & MOVE_FLAG_CASTLE) != 0)
    {
        return 100;
    }

    /* History */
    if (context != NULL)
    {
        return context->hh_table[board->side][move_from(move)][move_to(move)];
    }

    return 0;
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

static inline int score_to_tt(int score, int ply)
{
    if (score > MATE_SCORE - MAX_PLY_DEPTH)
    {
        return score + ply;
    }
    if (score < -MATE_SCORE + MAX_PLY_DEPTH)
    {
        return score - ply;
    }
    return score;
}

static inline int score_from_tt(int score, int ply)
{
    if (score > MATE_SCORE - MAX_PLY_DEPTH)
    {
        return score - ply;
    }
    if (score < -MATE_SCORE + MAX_PLY_DEPTH)
    {
        return score + ply;
    }
    return score;
}

static bool transposition_table_probe(const TranspositionTable *table,
                                      U64 hash,
                                      int depth,
                                      int alpha,
                                      int beta,
                                      int ply,
                                      int *score)
{
    const TranspositionEntry *entry = transposition_table_lookup(table, hash);
    if (entry == NULL || entry->depth < depth || score == NULL)
    {
        return false;
    }

    int entry_score = score_from_tt(entry->score, ply);

    switch (entry->score_type)
    {
    case TT_SCORE_EXACT:
        *score = entry_score;
        return true;

    case TT_SCORE_LOWER:
        if (entry_score >= beta)
        {
            *score = entry_score;
            return true;
        }
        break;

    case TT_SCORE_UPPER:
        if (entry_score <= alpha)
        {
            *score = entry_score;
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
                                      Move best_move,
                                      int ply)
{
    if (table == NULL || table->entries == NULL || table->size == 0 || best_move == MOVE_NONE)
    {
        return;
    }

    int tt_score = score_to_tt(score, ply);

    TranspositionEntry *entry = &table->entries[transposition_table_index(table, hash)];
    if (entry->hash == hash)
    {
        if (!transposition_entry_should_replace_same_hash(entry, depth, tt_score, score_type))
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
    entry->score = tt_score;
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

enum {
    STAGE_TT,
    STAGE_GENERATE_NOISY,
    STAGE_PLAY_NOISY,
    STAGE_KILLER_1,
    STAGE_KILLER_2,
    STAGE_GENERATE_QUIET,
    STAGE_PLAY_QUIET,
    STAGE_DONE
};

typedef struct {
    MoveList all_moves;
    bool used[MAX_ORDERED_MOVES];
    Move moves[MAX_ORDERED_MOVES];
    int scores[MAX_ORDERED_MOVES];
    int count;
    int current_idx;
    int stage;
    Move tt_move;
    Move killer1;
    Move killer2;
    const Move *excluded_moves;
    int excluded_move_count;
    bool in_qsearch;
    bool in_check;
    Board *board;
    const SearchContext *context;
    int ply;
} MovePicker;

static void movepicker_init(MovePicker *mp,
                            Board *board,
                            const SearchContext *context,
                            int ply,
                            Move tt_move,
                            const Move *excluded_moves,
                            int excluded_move_count,
                            bool in_qsearch)
{
    mp->board = board;
    mp->context = context;
    mp->ply = ply;
    mp->tt_move = tt_move;
    
    mp->killer1 = MOVE_NONE;
    mp->killer2 = MOVE_NONE;
    if (context != NULL && ply >= 0 && ply < MAX_PLY_DEPTH)
    {
        mp->killer1 = context->killer_moves[ply][0];
        mp->killer2 = context->killer_moves[ply][1];
    }
    
    mp->excluded_moves = excluded_moves;
    mp->excluded_move_count = excluded_move_count;
    mp->in_qsearch = in_qsearch;
    mp->in_check = board_is_in_check(board, board->side);
    
    mp->stage = STAGE_TT;
    mp->count = 0;
    mp->current_idx = 0;
    memset(mp->used, 0, sizeof(mp->used));
    
    movegen_generate_pseudo_legal(board, &mp->all_moves);
}

static Move movepicker_next_move(MovePicker *mp)
{
    while (mp->stage != STAGE_DONE)
    {
        switch (mp->stage)
        {
            case STAGE_TT:
                mp->stage = STAGE_GENERATE_NOISY;
                if (mp->tt_move != MOVE_NONE &&
                    !move_is_excluded(mp->tt_move, mp->excluded_moves, mp->excluded_move_count))
                {
                    int idx = find_move_index(&mp->all_moves, mp->tt_move);
                    if (idx >= 0)
                    {
                        mp->used[idx] = true;
                        return mp->tt_move;
                    }
                }
                break;

            case STAGE_GENERATE_NOISY:
                mp->count = 0;
                mp->current_idx = 0;
                for (int i = 0; i < mp->all_moves.count && mp->count < MAX_ORDERED_MOVES; ++i)
                {
                    Move m = mp->all_moves.moves[i];
                    if (mp->used[i]) continue;
                    if (move_is_excluded(m, mp->excluded_moves, mp->excluded_move_count)) continue;
                    
                    if (move_iscapture(m) || move_promotion(m) != MOVE_PROMO_NONE)
                    {
                        mp->moves[mp->count] = m;
                        mp->scores[mp->count] = estimate_move_score(mp->board, m, mp->context, mp->ply);
                        mp->used[i] = true;
                        mp->count++;
                    }
                }
                
                mp->stage = STAGE_PLAY_NOISY;
                break;

            case STAGE_PLAY_NOISY:
                if (mp->current_idx < mp->count)
                {
                    int best_idx = mp->current_idx;
                    for (int i = mp->current_idx + 1; i < mp->count; ++i)
                    {
                        if (mp->scores[i] > mp->scores[best_idx])
                        {
                            best_idx = i;
                        }
                    }
                    Move best_move = mp->moves[best_idx];
                    int best_score = mp->scores[best_idx];
                    
                    mp->moves[best_idx] = mp->moves[mp->current_idx];
                    mp->scores[best_idx] = mp->scores[mp->current_idx];
                    mp->moves[mp->current_idx] = best_move;
                    mp->scores[mp->current_idx] = best_score;
                    
                    mp->current_idx++;
                    return best_move;
                }
                
                if (mp->in_qsearch && !mp->in_check)
                {
                    mp->stage = STAGE_DONE;
                }
                else
                {
                    mp->stage = STAGE_KILLER_1;
                }
                break;

            case STAGE_KILLER_1:
                mp->stage = STAGE_KILLER_2;
                if (mp->killer1 != MOVE_NONE && mp->killer1 != mp->tt_move &&
                    !move_is_excluded(mp->killer1, mp->excluded_moves, mp->excluded_move_count))
                {
                    int idx = find_move_index(&mp->all_moves, mp->killer1);
                    if (idx >= 0 && !mp->used[idx])
                    {
                        if (!move_iscapture(mp->killer1) && move_promotion(mp->killer1) == MOVE_PROMO_NONE)
                        {
                            mp->used[idx] = true;
                            return mp->killer1;
                        }
                    }
                }
                break;

            case STAGE_KILLER_2:
                mp->stage = STAGE_GENERATE_QUIET;
                if (mp->killer2 != MOVE_NONE && mp->killer2 != mp->tt_move && mp->killer2 != mp->killer1 &&
                    !move_is_excluded(mp->killer2, mp->excluded_moves, mp->excluded_move_count))
                {
                    int idx = find_move_index(&mp->all_moves, mp->killer2);
                    if (idx >= 0 && !mp->used[idx])
                    {
                        if (!move_iscapture(mp->killer2) && move_promotion(mp->killer2) == MOVE_PROMO_NONE)
                        {
                            mp->used[idx] = true;
                            return mp->killer2;
                        }
                    }
                }
                break;

            case STAGE_GENERATE_QUIET:
                mp->count = 0;
                mp->current_idx = 0;
                for (int i = 0; i < mp->all_moves.count && mp->count < MAX_ORDERED_MOVES; ++i)
                {
                    Move m = mp->all_moves.moves[i];
                    if (mp->used[i]) continue;
                    if (move_is_excluded(m, mp->excluded_moves, mp->excluded_move_count)) continue;
                    
                    mp->moves[mp->count] = m;
                    mp->scores[mp->count] = estimate_move_score(mp->board, m, mp->context, mp->ply);
                    mp->used[i] = true;
                    mp->count++;
                }
                
                mp->stage = STAGE_PLAY_QUIET;
                break;

            case STAGE_PLAY_QUIET:
                if (mp->current_idx < mp->count)
                {
                    int best_idx = mp->current_idx;
                    for (int i = mp->current_idx + 1; i < mp->count; ++i)
                    {
                        if (mp->scores[i] > mp->scores[best_idx])
                        {
                            best_idx = i;
                        }
                    }
                    Move best_move = mp->moves[best_idx];
                    int best_score = mp->scores[best_idx];
                    
                    mp->moves[best_idx] = mp->moves[mp->current_idx];
                    mp->scores[best_idx] = mp->scores[mp->current_idx];
                    mp->moves[mp->current_idx] = best_move;
                    mp->scores[mp->current_idx] = best_score;
                    
                    mp->current_idx++;
                    return best_move;
                }
                mp->stage = STAGE_DONE;
                break;
        }
    }
    return MOVE_NONE;
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
        if (transposition_table_probe(&context->table, board_position_key(board), 0, alpha, beta, ply, &tt_score))
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

    MovePicker picker;
    // Pass NULL as context to disable killer move sorting in qsearch
    movepicker_init(&picker, board, NULL, ply, MOVE_NONE, NULL, 0, true);

    bool has_legal_move = false;
    Move move;

    while ((move = movepicker_next_move(&picker)) != MOVE_NONE)
    {
        if (search_should_stop(control))
        {
            break;
        }

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
        TranspositionScoreType score_type = transposition_score_type(alpha, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board_position_key(board), 0, alpha, score_type, best_move, ply);
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
        if (transposition_table_probe(&context->table, board_position_key(board), depth, alpha, beta, ply, &tt_score))
        {
            result.score = tt_score;

            int pv_depth = 0;
            Undo undos[MAX_PV_MOVES];
            U64 pv_hashes[MAX_PV_MOVES];
            while (pv_depth < depth && result.pv_length < MAX_PV_MOVES)
            {
                U64 hash = board_position_key(board);
                
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

    /* Null-move pruning */
    if (depth >= 3 &&// NMP not done near leaves as tree is already small
        beta < MATE_SCORE - MAX_PLY_DEPTH &&// Not done in mating sequences
        !board_is_in_check(board, board->side) && // In check passing is illegal
        has_sufficient_nmp_material(board)) //Not done in endgames to avoid zugzwang issues
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

    Move tt_move = MOVE_NONE;
    if (context != NULL)
    {
        const TranspositionEntry *entry = transposition_table_lookup(&context->table, board_position_key(board));
        if (entry != NULL)
        {
            tt_move = entry->best_move;
        }
    }

    MovePicker picker;
    movepicker_init(&picker, board, context, ply, tt_move, NULL, 0, false);

    bool has_legal_move = false;
    Move quiet_searched[MAX_QUIET_TRACKED];
    int quiet_searched_count = 0;

    Move move;
    while ((move = movepicker_next_move(&picker)) != MOVE_NONE)
    {
        if (search_should_stop(control))
        {
            break;
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

    EvalTerminalState terminal_state = eval_terminal_state(board, has_legal_move);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        result.score = eval_terminal_score(terminal_state, ply);
        return result;
    }

    if (result.move != MOVE_NONE && context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board_position_key(board), depth, result.score, score_type, result.move, ply);
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
    SearchResult result = {0, MOVE_NONE, {0}, 0, false};
    const int alpha_orig = alpha;
    const int beta_orig = beta;
    stats->hashfull = 0;
    /* `context` holds the transposition table and killer moves. */

    Move tt_move = MOVE_NONE;
    if (context != NULL)
    {
        const TranspositionEntry *entry = transposition_table_lookup(&context->table, board_position_key(board));
        if (entry != NULL)
        {
            tt_move = entry->best_move;
        }
    }

    MovePicker picker;
    movepicker_init(&picker, board, context, 0, tt_move, excluded_moves, excluded_move_count, false);

    if (board_is_draw(board, history, lichess_draw_rules))
    {
        result.score = 0;
        for (int i = 0; i < picker.all_moves.count; ++i)
        {
            if (move_is_excluded(picker.all_moves.moves[i], excluded_moves, excluded_move_count))
            {
                continue;
            }
            result.move = picker.all_moves.moves[i];
            result.pv[0] = picker.all_moves.moves[i];
            result.pv_length = 1;
            break;
        }
        return result;
    }

    EvalTerminalState terminal_state = eval_terminal_state(board, picker.all_moves.count);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        result.score = eval_terminal_score(terminal_state, 0);
        return result;
    }

    if (picker.all_moves.count == 1) // Despite this technically being pseudo-legal, because this is root, if there's only one move, it's the only legal move (if no moves then it's mate or stalemate).
    {
        if (!move_is_excluded(picker.all_moves.moves[0], excluded_moves, excluded_move_count))
        {
            result.move = picker.all_moves.moves[0];
            result.pv[0] = picker.all_moves.moves[0];
            result.pv_length = 1;
            result.forced_root_move = (control != NULL && control->allow_forced_root_move);
        }
        return result;
    }

    Move move;
    int move_index = 0;
    while ((move = movepicker_next_move(&picker)) != MOVE_NONE)
    {
        if (search_should_stop(control))
        {
            break;
        }

        Undo undo;
        int curr_index = move_index++;

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
    stats->hashfull = (int)((context->table.count * 1000) / context->table.size);
    if (result.move != MOVE_NONE && context != NULL)
    {
        TranspositionScoreType score_type = transposition_score_type(result.score, alpha_orig, beta_orig);
        transposition_table_store(&context->table, board_position_key(board), depth, result.score, score_type, result.move, 0);
    }

    return result;
}
