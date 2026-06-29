#include "movepicker.h"
#include <stdlib.h>
#include <string.h>

#define HISTORY_BONUS_CAP 400
#define HISTORY_GRAVITY 512
#define HISTORY_SCALE 16

static const int piece_values[6] = {
    1000, /* Pawn */
    3000, /* Knight */
    3200, /* Bishop */
    5000, /* Rook */
    9000, /* Queen */
    0    /* King */
};

int history_bonus(int depth)
{
    int b = depth * depth;
    return b < HISTORY_BONUS_CAP ? b : HISTORY_BONUS_CAP;
}

void update_history_entry(int16_t *entry, int delta)
{
    *entry += HISTORY_SCALE * delta - *entry * abs(delta) / HISTORY_GRAVITY;
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

/* Estimate move score for move ordering. This is intentionally cheap. */
int estimate_move_score(Board *board, Move move, const SearchContext *context, int ply)
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

void movepicker_init(MovePicker *mp,
                     Board *board,
                     const SearchContext *context,
                     int ply,
                     Move previous_move,
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
    
    mp->counter1 = MOVE_NONE;
    mp->counter2 = MOVE_NONE;
    if (context != NULL && previous_move != MOVE_NONE)
    {
        int prev_from = move_from(previous_move);
        int prev_to = move_to(previous_move);
        mp->counter1 = context->counter_moves[prev_from][prev_to][0];
        mp->counter2 = context->counter_moves[prev_from][prev_to][1];
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

Move movepicker_next_move(MovePicker *mp)
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
                mp->stage = STAGE_COUNTER_1;
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

            case STAGE_COUNTER_1:
                mp->stage = STAGE_COUNTER_2;
                if (mp->counter1 != MOVE_NONE && mp->counter1 != mp->tt_move &&
                    mp->counter1 != mp->killer1 && mp->counter1 != mp->killer2 &&
                    !move_is_excluded(mp->counter1, mp->excluded_moves, mp->excluded_move_count))
                {
                    int idx = find_move_index(&mp->all_moves, mp->counter1);
                    if (idx >= 0 && !mp->used[idx])
                    {
                        if (!move_iscapture(mp->counter1) && move_promotion(mp->counter1) == MOVE_PROMO_NONE)
                        {
                            mp->used[idx] = true;
                            return mp->counter1;
                        }
                    }
                }
                break;

            case STAGE_COUNTER_2:
                mp->stage = STAGE_GENERATE_QUIET;
                if (mp->counter2 != MOVE_NONE && mp->counter2 != mp->tt_move &&
                    mp->counter2 != mp->killer1 && mp->counter2 != mp->killer2 &&
                    mp->counter2 != mp->counter1 &&
                    !move_is_excluded(mp->counter2, mp->excluded_moves, mp->excluded_move_count))
                {
                    int idx = find_move_index(&mp->all_moves, mp->counter2);
                    if (idx >= 0 && !mp->used[idx])
                    {
                        if (!move_iscapture(mp->counter2) && move_promotion(mp->counter2) == MOVE_PROMO_NONE)
                        {
                            mp->used[idx] = true;
                            return mp->counter2;
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
