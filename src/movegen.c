#include "movegen.h"

#include <stdbool.h>
#include <string.h>

static void add_pseudo_legal_move(MoveList *list, Move move) {
    if (list->count >= (int)(sizeof(list->moves) / sizeof(list->moves[0]))) {
        return;
    }
    list->moves[list->count++] = move;
}

static int move_capture_flag(const Board *board, int target) {
    return board_piece_at(board, target) >= 0 ? MOVE_FLAG_CAPTURE : 0;
}

static void generate_pawn_moves(Board *board, MoveList *list, int side) {
    U64 pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    U64 enemy = board->occupancy[side ^ 1];
    int step = side == WHITE ? 8 : -8;
    int start_rank = side == WHITE ? 1 : 6;
    int promo_rank = side == WHITE ? 6 : 1;

    while (pawns) {
        int from = bitboard_pop_lsb(&pawns);
        int rank = from >> 3;
        int one = from + step;
        if (one >= 0 && one < 64 && !(board->occupancy[BOTH] & bitboard_square(one))) {
            if (rank == promo_rank) {
                add_pseudo_legal_move(list, move_make(from, one, MOVE_PROMO_KNIGHT, 0));
                add_pseudo_legal_move(list, move_make(from, one, MOVE_PROMO_BISHOP, 0));
                add_pseudo_legal_move(list, move_make(from, one, MOVE_PROMO_ROOK, 0));
                add_pseudo_legal_move(list, move_make(from, one, MOVE_PROMO_QUEEN, 0));
            } else {
                add_pseudo_legal_move(list, move_make(from, one, MOVE_PROMO_NONE, 0));
                if (rank == start_rank) {
                    int two = from + step * 2;
                    if (two >= 0 && two < 64 && !(board->occupancy[BOTH] & bitboard_square(two))) {
                        add_pseudo_legal_move(list, move_make(from, two, MOVE_PROMO_NONE, MOVE_FLAG_DOUBLE_PAWN));
                    }
                }
            }
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
            int flags = move_capture_flag(board, target);
            bool is_ep = board->ep_square == target;
            if (is_ep) {
                flags |= MOVE_FLAG_CAPTURE | MOVE_FLAG_EN_PASSANT;
            } else if (!(enemy & bitboard_square(target))) {
                continue;
            }
            if (rank == promo_rank) {
                add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_KNIGHT, flags));
                add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_BISHOP, flags));
                add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_ROOK, flags));
                add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_QUEEN, flags));
            } else {
                add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_NONE, flags | MOVE_FLAG_CAPTURE));
            }
        }
    }
}

static void generate_knight_moves(Board *board, MoveList *list, int side) {
    U64 pieces = board->pieces[side == WHITE ? WHITE_KNIGHT : BLACK_KNIGHT];
    U64 own = board->occupancy[side];
    while (pieces) {
        int from = bitboard_pop_lsb(&pieces);
        U64 targets = bitboard_knight_attacks(from) & ~own;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            int flags = board_piece_at(board, target) >= 0 ? MOVE_FLAG_CAPTURE : 0;
            add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_NONE, flags));
        }
    }
}

static void generate_bishop_moves(Board *board, MoveList *list, int side) {
    U64 pieces = board->pieces[side == WHITE ? WHITE_BISHOP : BLACK_BISHOP];
    U64 own = board->occupancy[side];
    while (pieces) {
        int from = bitboard_pop_lsb(&pieces);
        U64 targets = bitboard_bishop_attacks(from, board->occupancy[BOTH]) & ~own;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            int flags = board_piece_at(board, target) >= 0 ? MOVE_FLAG_CAPTURE : 0;
            add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_NONE, flags));
        }
    }
}

static void generate_rook_moves(Board *board, MoveList *list, int side) {
    U64 pieces = board->pieces[side == WHITE ? WHITE_ROOK : BLACK_ROOK];
    U64 own = board->occupancy[side];
    while (pieces) {
        int from = bitboard_pop_lsb(&pieces);
        U64 targets = bitboard_rook_attacks(from, board->occupancy[BOTH]) & ~own;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            int flags = board_piece_at(board, target) >= 0 ? MOVE_FLAG_CAPTURE : 0;
            add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_NONE, flags));
        }
    }
}

static void generate_queen_moves(Board *board, MoveList *list, int side) {
    U64 pieces = board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
    U64 own = board->occupancy[side];
    while (pieces) {
        int from = bitboard_pop_lsb(&pieces);
        U64 targets = bitboard_queen_attacks(from, board->occupancy[BOTH]) & ~own;
        while (targets) {
            int target = bitboard_pop_lsb(&targets);
            int flags = board_piece_at(board, target) >= 0 ? MOVE_FLAG_CAPTURE : 0;
            add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_NONE, flags));
        }
    }
}

static void try_add_castle_move(Board *board, MoveList *list, int side, int castle_flag, int king_to, int rook_idx) {
    if ((board->castling_rights & castle_flag) == 0) {
        return;
    }
    if (board_is_in_check(board, side)) {
        return;
    }

    int king_from = board->king_square[side];
    int rook_from = board->castling_rook_square[rook_idx];
    int rook_to = (side == WHITE) ? (king_to == 6 ? 5 : 3) : (king_to == 62 ? 61 : 59);

    U64 king_crossing = bitboard_in_between_mask(king_from, king_to) | (1ULL << king_to);
    U64 rook_crossing = bitboard_in_between_mask(rook_from, rook_to) | (1ULL << rook_to);
    U64 between = king_crossing | rook_crossing;

    U64 occ_check = board->occupancy[BOTH] ^ (1ULL << king_from) ^ (1ULL << rook_from);
    if (occ_check & between) {
        return;
    }

    int enemy = side ^ 1;
    U64 check_sqs = king_crossing;
    while (check_sqs) {
        int sq = bitboard_pop_lsb(&check_sqs);
        if (board_is_square_attacked_with_occupancy(board, sq, enemy, board->occupancy[BOTH])) {
            return;
        }
    }

    add_pseudo_legal_move(list, move_make(king_from, king_to, MOVE_PROMO_NONE, MOVE_FLAG_CASTLE));
}

static void generate_king_moves(Board *board, MoveList *list, int side) {
    U64 king = board->pieces[side == WHITE ? WHITE_KING : BLACK_KING];
    U64 own = board->occupancy[side];
    if (!king) {
        return;
    }

    int from = bitboard_pop_lsb(&king);
    U64 targets = bitboard_king_attacks(from) & ~own;
    while (targets) {
        int target = bitboard_pop_lsb(&targets);
        int flags = board_piece_at(board, target) >= 0 ? MOVE_FLAG_CAPTURE : 0;
        add_pseudo_legal_move(list, move_make(from, target, MOVE_PROMO_NONE, flags));
    }

    if (side == WHITE) {
        try_add_castle_move(board, list, WHITE, CASTLE_WHITE_KING, 6, 0);
        try_add_castle_move(board, list, WHITE, CASTLE_WHITE_QUEEN, 2, 1);
    } else {
        try_add_castle_move(board, list, BLACK, CASTLE_BLACK_KING, 62, 2);
        try_add_castle_move(board, list, BLACK, CASTLE_BLACK_QUEEN, 58, 3);
    }
}

void movegen_generate_pseudo_legal(Board *board, MoveList *list) {
    list->count = 0;
    if (board == NULL) {
        return;
    }

    int side = board->side;
    generate_pawn_moves(board, list, side);
    generate_knight_moves(board, list, side);
    generate_bishop_moves(board, list, side);
    generate_rook_moves(board, list, side);
    generate_queen_moves(board, list, side);
    generate_king_moves(board, list, side);
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

