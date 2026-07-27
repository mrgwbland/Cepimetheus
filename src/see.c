#include "see.h"

#include "bitboard.h"
#include "board.h"
#include "move.h"
#include "movepicker.h"

U64 board_attackers_to(const Board *board, int square, U64 occupancy) {
    if (square < 0 || square >= 64) {
        return 0ULL;
    }

    U64 attackers = 0ULL;

    /* Pawns */
    attackers |= bitboard_pawn_attacks(BLACK, square) & board->pieces[WHITE_PAWN];
    attackers |= bitboard_pawn_attacks(WHITE, square) & board->pieces[BLACK_PAWN];

    /* Knights */
    attackers |= bitboard_knight_attacks(square) & (board->pieces[WHITE_KNIGHT] | board->pieces[BLACK_KNIGHT]);

    /* Kings */
    attackers |= bitboard_king_attacks(square) & (board->pieces[WHITE_KING] | board->pieces[BLACK_KING]);

    /* Bishops & Queens */
    U64 bishops = board->pieces[WHITE_BISHOP] | board->pieces[BLACK_BISHOP]
                | board->pieces[WHITE_QUEEN]  | board->pieces[BLACK_QUEEN];
    attackers |= bitboard_bishop_attacks(square, occupancy) & bishops;

    /* Rooks & Queens */
    U64 rooks = board->pieces[WHITE_ROOK] | board->pieces[BLACK_ROOK]
              | board->pieces[WHITE_QUEEN] | board->pieces[BLACK_QUEEN];
    attackers |= bitboard_rook_attacks(square, occupancy) & rooks;

    return attackers;
}

// Check if the capture is greater than or equal to the threshold
bool see_ge(const Board *board, Move move, int threshold) {
    int flags = move_flags(move);

    if (flags & MOVE_FLAG_CASTLE) {
        return 0 >= threshold;
    }

    int from = move_from(move);
    int to = move_to(move);

    int mover_piece = board_piece_at(board, from);
    if (mover_piece < 0) {
        return false;
    }
    int mover_type = board_piece_type(mover_piece);
    int stm = board->side;

    int victim_value = 0;
    if (flags & MOVE_FLAG_EN_PASSANT) {
        victim_value = piece_values[0]; /* Pawn */
    } else {
        int target_piece = board_piece_at(board, to);
        if (target_piece >= 0) {
            victim_value = piece_values[board_piece_type(target_piece)];
        }
    }

    if (move_promotion(move) != MOVE_PROMO_NONE) {
        victim_value += piece_values[move_promotion(move)] - piece_values[0];
    }

    int balance = victim_value - threshold;

    /* Fail-low cutoff */
    if (balance < 0) {
        return false;
    }

    int attacker_value = piece_values[mover_type];

    /* Fail-high cutoff */
    if (attacker_value - balance <= 0) {
        return true;
    }

    U64 occupancy = board->occupancy[BOTH];
    occupancy ^= (1ULL << from);
    if (flags & MOVE_FLAG_EN_PASSANT) {
        int ep_cap_sq = stm == WHITE ? to - 8 : to + 8;
        occupancy ^= (1ULL << ep_cap_sq);
    }
    occupancy |= (1ULL << to);

    U64 attackers = board_attackers_to(board, to, occupancy);

    U64 diagonal_sliders = board->pieces[WHITE_BISHOP] | board->pieces[BLACK_BISHOP]
                         | board->pieces[WHITE_QUEEN]  | board->pieces[BLACK_QUEEN];
    U64 straight_sliders = board->pieces[WHITE_ROOK]   | board->pieces[BLACK_ROOK]
                         | board->pieces[WHITE_QUEEN]  | board->pieces[BLACK_QUEEN];

    int curr_stm = stm ^ 1;

    while (1) {
        attackers &= occupancy;
        U64 my_attackers = attackers & board->occupancy[curr_stm];

        if (!my_attackers) {
            break;
        }

        int next_victim_type = -1;
        U64 least_attacker_bb = 0ULL;

        /* Find least valuable attacker */
        for (int pt = 0; pt < 6; ++pt) {
            int p = curr_stm == WHITE ? pt : pt + 6;
            U64 subset = my_attackers & board->pieces[p];
            if (subset) {
                next_victim_type = pt;
                least_attacker_bb = subset & -subset;
                break;
            }
        }

        if (next_victim_type < 0) {
            break;
        }

        /* If King captures, check if opponent still has attackers guarding `to` square */
        if (next_victim_type == 5 /* KING */) {
            if (attackers & board->occupancy[curr_stm ^ 1]) {
                curr_stm ^= 1;
            }
            break;
        }

        balance = attacker_value - balance;
        attacker_value = piece_values[next_victim_type];

        if (balance < 0) {
            break;
        }

        /* Remove used attacker from occupancy */
        occupancy ^= least_attacker_bb;

        /* Update X-ray attacks */
        if (next_victim_type == 0 || next_victim_type == 2 || next_victim_type == 4) {
            attackers |= bitboard_bishop_attacks(to, occupancy) & diagonal_sliders;
        }
        if (next_victim_type == 3 || next_victim_type == 4) {
            attackers |= bitboard_rook_attacks(to, occupancy) & straight_sliders;
        }

        curr_stm ^= 1;
    }

    return curr_stm != stm;
}
