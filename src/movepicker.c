#include "movepicker.h"
#include "see.h"
#include <stdlib.h>
#include <string.h>

int history_bonus_cap = 366;
int history_gravity = 415;
int history_scale = 23;
int order_knight_promo = 331;
int order_bishop_promo = 313;
int order_rook_promo = 530;
int order_queen_promo = 844;
int order_victim_mult = 15;
int order_killer1 = 88833;
int order_killer2 = 75863;
int order_castle = 145;

int history_bonus(int depth)
{
    int b = depth * depth;
    return b < history_bonus_cap ? b : history_bonus_cap;
}

void update_history_entry(int16_t *entry, int delta)
{
    int grav = history_gravity > 0 ? history_gravity : 1;
    *entry += history_scale * delta - *entry * abs(delta) / grav;
}

static bool is_good_noisy(const Board *board, Move move)
{
    if (move_promotion(move) != MOVE_PROMO_NONE)
    {
        return true;
    }

    int flags = move_flags(move);
    int attacker_piece = board_piece_at(board, move_from(move));
    int attacker_value = piece_values[board_piece_type(attacker_piece)];
    int victim_value;

    if ((flags & MOVE_FLAG_EN_PASSANT) != 0)
    {
        victim_value = piece_values[WHITE_PAWN];
    }
    else
    {
        int victim_piece = board_piece_at(board, move_to(move));
        victim_value = piece_values[board_piece_type(victim_piece)];
    }

    if (victim_value >= attacker_value)
    {
        return true;
    }

    return see_ge(board, move, 0);
}

/* Estimate move score for move ordering. This is intentionally cheap. */
int estimate_move_score(const Board *board, Move move, const SearchContext *context, int ply)
{
    int flags = move_flags(move);

    /* 1. Promotions */
    if (move_promotion(move) != MOVE_PROMO_NONE)
    {
        const int promo_bonus[5] = {0, order_knight_promo, order_bishop_promo, order_rook_promo, order_queen_promo};
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

        return 1000000 + victim_value * order_victim_mult - attacker_value;
    }

    /* 3. Quiet Moves (Killers, History) */
    if (context != NULL && ply >= 0 && ply < MAX_PLY_DEPTH)
    {
        /* First killer */
        if (context->killer_moves[ply][0] != MOVE_NONE && context->killer_moves[ply][0] == move)
        {
            return order_killer1;
        }

        /* Second killer */
        if (context->killer_moves[ply][1] != MOVE_NONE && context->killer_moves[ply][1] == move)
        {
            return order_killer2;
        }
    }

    /* Castling */
    if ((flags & MOVE_FLAG_CASTLE) != 0)
    {
        return order_castle;
    }

    /* History */
    if (context != NULL)
    {
        return context->hh_table[board->side][move_from(move)][move_to(move)];
    }

    return 0;
}

void movepicker_init(MovePicker *mp,
                     const Board *board,
                     const SearchContext *context,
                     const SearchStack *ss,
                     Move tt_move,
                     const Move *excluded_moves,
                     int excluded_move_count,
                     bool in_qsearch)
{
    mp->board = board;
    mp->context = context;
    mp->ply = (ss != NULL) ? ss->ply : 0;
    mp->tt_move = tt_move;
    
    mp->killer1 = MOVE_NONE;
    mp->killer2 = MOVE_NONE;
    if (context != NULL && mp->ply >= 0 && mp->ply < MAX_PLY_DEPTH)
    {
        mp->killer1 = context->killer_moves[mp->ply][0];
        mp->killer2 = context->killer_moves[mp->ply][1];
    }
    
    mp->counter1 = MOVE_NONE;
    mp->counter2 = MOVE_NONE;
    Move previous_move = (ss != NULL) ? (ss - 1)->move : MOVE_NONE;
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
    mp->bad_noisy_count = 0;
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
                    !move_is_in_list(mp->tt_move, mp->excluded_moves, mp->excluded_move_count) &&
                    board_is_move_pseudo_legal(mp->board, mp->tt_move))
                {
                    return mp->tt_move;
                }
                /* fallthrough */

            case STAGE_GENERATE_NOISY: {
                Move raw_noisy[MAX_ORDERED_MOVES];
                int raw_count = movegen_generate_noisy(mp->board, raw_noisy);
                mp->count = 0;
                mp->bad_noisy_count = 0;
                for (int i = 0; i < raw_count; ++i)
                {
                    Move m = raw_noisy[i];
                    if (m == mp->tt_move ||
                        move_is_in_list(m, mp->excluded_moves, mp->excluded_move_count))
                    {
                        continue;
                    }
                    if (is_good_noisy(mp->board, m))
                    {
                        mp->moves[mp->count] = m;
                        mp->scores[mp->count] = estimate_move_score(mp->board, m, mp->context, mp->ply);
                        mp->count++;
                    }
                    else if (mp->bad_noisy_count < 64)
                    {
                        mp->bad_noisy[mp->bad_noisy_count] = m;
                        mp->bad_scores[mp->bad_noisy_count] = estimate_move_score(mp->board, m, mp->context, mp->ply);
                        mp->bad_noisy_count++;
                    }
                }
                mp->current_idx = 0;
                mp->stage = STAGE_PLAY_NOISY;
                /* fallthrough */
            }

            case STAGE_PLAY_NOISY:
                while (mp->current_idx < mp->count)
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
                    return MOVE_NONE;
                }

                mp->stage = STAGE_KILLER_1;
                /* fallthrough */

            case STAGE_KILLER_1:
                mp->stage = STAGE_KILLER_2;
                if (mp->killer1 != MOVE_NONE && mp->killer1 != mp->tt_move &&
                    !move_iscapture(mp->killer1) && move_promotion(mp->killer1) == MOVE_PROMO_NONE &&
                    !move_is_in_list(mp->killer1, mp->excluded_moves, mp->excluded_move_count) &&
                    board_is_move_pseudo_legal(mp->board, mp->killer1))
                {
                    return mp->killer1;
                }
                /* fallthrough */

            case STAGE_KILLER_2:
                mp->stage = STAGE_COUNTER_1;
                if (mp->killer2 != MOVE_NONE && mp->killer2 != mp->tt_move && mp->killer2 != mp->killer1 &&
                    !move_iscapture(mp->killer2) && move_promotion(mp->killer2) == MOVE_PROMO_NONE &&
                    !move_is_in_list(mp->killer2, mp->excluded_moves, mp->excluded_move_count) &&
                    board_is_move_pseudo_legal(mp->board, mp->killer2))
                {
                    return mp->killer2;
                }
                /* fallthrough */

            case STAGE_COUNTER_1:
                mp->stage = STAGE_COUNTER_2;
                if (mp->counter1 != MOVE_NONE && mp->counter1 != mp->tt_move &&
                    mp->counter1 != mp->killer1 && mp->counter1 != mp->killer2 &&
                    !move_iscapture(mp->counter1) && move_promotion(mp->counter1) == MOVE_PROMO_NONE &&
                    !move_is_in_list(mp->counter1, mp->excluded_moves, mp->excluded_move_count) &&
                    board_is_move_pseudo_legal(mp->board, mp->counter1))
                {
                    return mp->counter1;
                }
                /* fallthrough */

            case STAGE_COUNTER_2:
                mp->stage = STAGE_GENERATE_QUIET;
                if (mp->counter2 != MOVE_NONE && mp->counter2 != mp->tt_move &&
                    mp->counter2 != mp->killer1 && mp->counter2 != mp->killer2 &&
                    mp->counter2 != mp->counter1 &&
                    !move_iscapture(mp->counter2) && move_promotion(mp->counter2) == MOVE_PROMO_NONE &&
                    !move_is_in_list(mp->counter2, mp->excluded_moves, mp->excluded_move_count) &&
                    board_is_move_pseudo_legal(mp->board, mp->counter2))
                {
                    return mp->counter2;
                }
                /* fallthrough */

            case STAGE_GENERATE_QUIET: {
                Move raw_quiets[MAX_ORDERED_MOVES];
                int raw_count = movegen_generate_quiet(mp->board, raw_quiets);
                mp->count = 0;
                for (int i = 0; i < raw_count; ++i)
                {
                    Move m = raw_quiets[i];
                    if (m == mp->tt_move || m == mp->killer1 || m == mp->killer2 ||
                        m == mp->counter1 || m == mp->counter2 ||
                        move_is_in_list(m, mp->excluded_moves, mp->excluded_move_count))
                    {
                        continue;
                    }
                    mp->moves[mp->count] = m;
                    mp->scores[mp->count] = estimate_move_score(mp->board, m, mp->context, mp->ply);
                    mp->count++;
                }
                mp->current_idx = 0;
                mp->stage = STAGE_PLAY_QUIET;
                /* fallthrough */
            }

            case STAGE_PLAY_QUIET:
                while (mp->current_idx < mp->count)
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
                
                mp->stage = STAGE_GENERATE_BAD_NOISY;
                /* fallthrough */

            case STAGE_GENERATE_BAD_NOISY:
                mp->current_idx = 0;
                mp->stage = STAGE_PLAY_BAD_NOISY;
                /* fallthrough */

            case STAGE_PLAY_BAD_NOISY:
                while (mp->current_idx < mp->bad_noisy_count)
                {
                    int best_idx = mp->current_idx;
                    for (int i = mp->current_idx + 1; i < mp->bad_noisy_count; ++i)
                    {
                        if (mp->bad_scores[i] > mp->bad_scores[best_idx])
                        {
                            best_idx = i;
                        }
                    }
                    Move best_move = mp->bad_noisy[best_idx];
                    int best_score = mp->bad_scores[best_idx];
                    
                    mp->bad_noisy[best_idx] = mp->bad_noisy[mp->current_idx];
                    mp->bad_scores[best_idx] = mp->bad_scores[mp->current_idx];
                    mp->bad_noisy[mp->current_idx] = best_move;
                    mp->bad_scores[mp->current_idx] = best_score;
                    mp->current_idx++;

                    return best_move;
                }

                mp->stage = STAGE_DONE;
                break;

            case STAGE_DONE:
                return MOVE_NONE;
        }
    }
    return MOVE_NONE;
}
