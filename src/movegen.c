#include "movegen.h"

#include <stdbool.h>
#include <string.h>

static int try_add_castle_move(const Board *board, Move *moves, int count, int side, int castle_flag, int king_to, int rook_idx) {
    if ((board->castling_rights & castle_flag) == 0) {
        return count;
    }
    if (board_is_in_check(board, side)) {
        return count;
    }

    int king_from = board->king_square[side];
    int rook_from = board->castling_rook_square[rook_idx];
    int rook_to = (side == WHITE) ? (king_to == 6 ? 5 : 3) : (king_to == 62 ? 61 : 59);

    U64 king_crossing = bitboard_in_between_mask(king_from, king_to) | (1ULL << king_to);
    U64 rook_crossing = bitboard_in_between_mask(rook_from, rook_to) | (1ULL << rook_to);
    U64 between = king_crossing | rook_crossing;

    U64 occ_check = board->occupancy[BOTH] ^ (1ULL << king_from) ^ (1ULL << rook_from);
    if (occ_check & between) {
        return count;
    }

    int enemy = side ^ 1;
    U64 check_sqs = king_crossing;
    while (check_sqs) {
        int sq = bitboard_pop_lsb(&check_sqs);
        if (board_is_square_attacked_with_occupancy(board, sq, enemy, board->occupancy[BOTH])) {
            return count;
        }
    }

    moves[count++] = move_make(king_from, king_to, MOVE_PROMO_NONE, MOVE_FLAG_CASTLE);
    return count;
}

int movegen_generate_noisy(const Board *board, Move *moves) {
    if (board == NULL || moves == NULL) {
        return 0;
    }

    int count = 0;
    int side = board->side;
    int enemy_side = side ^ 1;
    U64 enemy = board->occupancy[enemy_side];
    int step = side == WHITE ? 8 : -8;
    int promo_rank = side == WHITE ? 6 : 1;

    // 1. Pawns: promos and captures
    U64 pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    while (pawns) {
        int from = bitboard_pop_lsb(&pawns);
        int rank = from >> 3;
        int one = from + step;

        if (rank == promo_rank && one >= 0 && one < 64 && !(board->occupancy[BOTH] & (1ULL << one))) {
            moves[count++] = move_make(from, one, MOVE_PROMO_KNIGHT, 0);
            moves[count++] = move_make(from, one, MOVE_PROMO_BISHOP, 0);
            moves[count++] = move_make(from, one, MOVE_PROMO_ROOK, 0);
            moves[count++] = move_make(from, one, MOVE_PROMO_QUEEN, 0);
        }

        const int capture_offsets[2] = {step + 1, step - 1};
        for (int i = 0; i < 2; ++i) {
            int target = from + capture_offsets[i];
            if (target < 0 || target >= 64) {
                continue;
            }
            int target_file = target & 7;
            if ((i == 0 && target_file == 0) || (i == 1 && target_file == 7)) {
                continue;
            }
            int flags = (board_piece_at(board, target) >= 0 ? MOVE_FLAG_CAPTURE : 0);
            bool is_ep = (board->ep_square == target && board->ep_square >= 0);
            if (is_ep) {
                flags |= MOVE_FLAG_CAPTURE | MOVE_FLAG_EN_PASSANT;
            } else if (!(enemy & (1ULL << target))) {
                continue;
            }
            if (rank == promo_rank) {
                moves[count++] = move_make(from, target, MOVE_PROMO_KNIGHT, flags);
                moves[count++] = move_make(from, target, MOVE_PROMO_BISHOP, flags);
                moves[count++] = move_make(from, target, MOVE_PROMO_ROOK, flags);
                moves[count++] = move_make(from, target, MOVE_PROMO_QUEEN, flags);
            } else {
                moves[count++] = move_make(from, target, MOVE_PROMO_NONE, flags | MOVE_FLAG_CAPTURE);
            }
        }
    }

    // 2. Knights
    U64 knights = board->pieces[side == WHITE ? WHITE_KNIGHT : BLACK_KNIGHT];
    while (knights) {
        int from = bitboard_pop_lsb(&knights);
        U64 targets = bitboard_knight_attacks(from) & enemy;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, MOVE_FLAG_CAPTURE);
        }
    }

    // 3. Bishops
    U64 bishops = board->pieces[side == WHITE ? WHITE_BISHOP : BLACK_BISHOP];
    while (bishops) {
        int from = bitboard_pop_lsb(&bishops);
        U64 targets = bitboard_bishop_attacks(from, board->occupancy[BOTH]) & enemy;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, MOVE_FLAG_CAPTURE);
        }
    }

    // 4. Rooks
    U64 rooks = board->pieces[side == WHITE ? WHITE_ROOK : BLACK_ROOK];
    while (rooks) {
        int from = bitboard_pop_lsb(&rooks);
        U64 targets = bitboard_rook_attacks(from, board->occupancy[BOTH]) & enemy;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, MOVE_FLAG_CAPTURE);
        }
    }

    // 5. Queens
    U64 queens = board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
    while (queens) {
        int from = bitboard_pop_lsb(&queens);
        U64 targets = bitboard_queen_attacks(from, board->occupancy[BOTH]) & enemy;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, MOVE_FLAG_CAPTURE);
        }
    }

    // 6. King
    U64 king = board->pieces[side == WHITE ? WHITE_KING : BLACK_KING];
    if (king) {
        int from = bitboard_pop_lsb(&king);
        U64 targets = bitboard_king_attacks(from) & enemy;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, MOVE_FLAG_CAPTURE);
        }
    }

    return count;
}

int movegen_generate_quiet(const Board *board, Move *moves) {
    if (board == NULL || moves == NULL) {
        return 0;
    }

    int count = 0;
    int side = board->side;
    U64 empty = ~board->occupancy[BOTH];
    int step = side == WHITE ? 8 : -8;
    int start_rank = side == WHITE ? 1 : 6;
    int promo_rank = side == WHITE ? 6 : 1;

    // 1. Pawns: quiet pushes
    U64 pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    while (pawns) {
        int from = bitboard_pop_lsb(&pawns);
        int rank = from >> 3;
        int one = from + step;

        if (rank != promo_rank && one >= 0 && one < 64 && (empty & (1ULL << one))) {
            moves[count++] = move_make(from, one, MOVE_PROMO_NONE, 0);
            if (rank == start_rank) {
                int two = from + step * 2;
                if (two >= 0 && two < 64 && (empty & (1ULL << two))) {
                    moves[count++] = move_make(from, two, MOVE_PROMO_NONE, MOVE_FLAG_DOUBLE_PAWN);
                }
            }
        }
    }

    // 2. Knights
    U64 knights = board->pieces[side == WHITE ? WHITE_KNIGHT : BLACK_KNIGHT];
    while (knights) {
        int from = bitboard_pop_lsb(&knights);
        U64 targets = bitboard_knight_attacks(from) & empty;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, 0);
        }
    }

    // 3. Bishops
    U64 bishops = board->pieces[side == WHITE ? WHITE_BISHOP : BLACK_BISHOP];
    while (bishops) {
        int from = bitboard_pop_lsb(&bishops);
        U64 targets = bitboard_bishop_attacks(from, board->occupancy[BOTH]) & empty;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, 0);
        }
    }

    // 4. Rooks
    U64 rooks = board->pieces[side == WHITE ? WHITE_ROOK : BLACK_ROOK];
    while (rooks) {
        int from = bitboard_pop_lsb(&rooks);
        U64 targets = bitboard_rook_attacks(from, board->occupancy[BOTH]) & empty;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, 0);
        }
    }

    // 5. Queens
    U64 queens = board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
    while (queens) {
        int from = bitboard_pop_lsb(&queens);
        U64 targets = bitboard_queen_attacks(from, board->occupancy[BOTH]) & empty;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, 0);
        }
    }

    // 6. King
    U64 king = board->pieces[side == WHITE ? WHITE_KING : BLACK_KING];
    if (king) {
        int from = bitboard_pop_lsb(&king);
        U64 targets = bitboard_king_attacks(from) & empty;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            moves[count++] = move_make(from, target, MOVE_PROMO_NONE, 0);
        }

        if (side == WHITE) {
            count = try_add_castle_move(board, moves, count, WHITE, CASTLE_WHITE_KING, 6, 0);
            count = try_add_castle_move(board, moves, count, WHITE, CASTLE_WHITE_QUEEN, 2, 1);
        } else {
            count = try_add_castle_move(board, moves, count, BLACK, CASTLE_BLACK_KING, 62, 2);
            count = try_add_castle_move(board, moves, count, BLACK, CASTLE_BLACK_QUEEN, 58, 3);
        }
    }

    return count;
}

void movegen_generate_pseudo_legal(const Board *board, MoveList *list) {
    list->count = 0;
    if (board == NULL) {
        return;
    }
    int noisy_count = movegen_generate_noisy(board, list->moves);
    int quiet_count = movegen_generate_quiet(board, list->moves + noisy_count);
    list->count = noisy_count + quiet_count;
}

bool movegen_find_legal_move(Board *board, const char *uci_move, Move *out_move) {
    if (board == NULL || uci_move == NULL || out_move == NULL) {
        return false;
    }

    MoveList list;
    movegen_generate_pseudo_legal(board, &list);
    char buffer[6];
    for (int i = 0; i < list.count; ++i) {
        move_to_string(list.moves[i], board, buffer);
        if (strcmp(buffer, uci_move) == 0) {
            *out_move = list.moves[i];
            return true;
        }
    }

    if (strlen(uci_move) >= 4) {
        int from = board_parse_square(uci_move);
        int to = board_parse_square(uci_move + 2);
        if (from >= 0 && to >= 0) {
            for (int i = 0; i < list.count; ++i) {
                Move m = list.moves[i];
                if (move_from(m) == from && (move_flags(m) & MOVE_FLAG_CASTLE)) {
                    int k_to = move_to(m);
                    int r_idx = (board->side == WHITE) ? (k_to == 6 ? 0 : 1) : (k_to == 62 ? 2 : 3);
                    int r_sq = board->castling_rook_square[r_idx];
                    if (to == k_to || to == r_sq) {
                        *out_move = m;
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

int find_move_index(const MoveList *list, Move move) {
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
