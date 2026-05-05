#include "movegen.h"

#include <stdbool.h>
#include <string.h>

static void add_legal_move(Board *board, MoveList *list, Move move) {
    if (list->count >= (int)(sizeof(list->moves) / sizeof(list->moves[0]))) {
        return;
    }
    Undo undo;
    if (board_make_move(board, move, &undo)) {
        list->moves[list->count++] = move;
        board_unmake_move(board, &undo);
    }
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
                add_legal_move(board, list, move_make(from, one, MOVE_PROMO_KNIGHT, 0));
                add_legal_move(board, list, move_make(from, one, MOVE_PROMO_BISHOP, 0));
                add_legal_move(board, list, move_make(from, one, MOVE_PROMO_ROOK, 0));
                add_legal_move(board, list, move_make(from, one, MOVE_PROMO_QUEEN, 0));
            } else {
                add_legal_move(board, list, move_make(from, one, MOVE_PROMO_NONE, 0));
                if (rank == start_rank) {
                    int two = from + step * 2;
                    if (two >= 0 && two < 64 && !(board->occupancy[BOTH] & bitboard_square(two))) {
                        add_legal_move(board, list, move_make(from, two, MOVE_PROMO_NONE, MOVE_FLAG_DOUBLE_PAWN));
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
                add_legal_move(board, list, move_make(from, target, MOVE_PROMO_KNIGHT, flags));
                add_legal_move(board, list, move_make(from, target, MOVE_PROMO_BISHOP, flags));
                add_legal_move(board, list, move_make(from, target, MOVE_PROMO_ROOK, flags));
                add_legal_move(board, list, move_make(from, target, MOVE_PROMO_QUEEN, flags));
            } else {
                add_legal_move(board, list, move_make(from, target, MOVE_PROMO_NONE, flags | MOVE_FLAG_CAPTURE));
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
            add_legal_move(board, list, move_make(from, target, MOVE_PROMO_NONE, flags));
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
            add_legal_move(board, list, move_make(from, target, MOVE_PROMO_NONE, flags));
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
            add_legal_move(board, list, move_make(from, target, MOVE_PROMO_NONE, flags));
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
            add_legal_move(board, list, move_make(from, target, MOVE_PROMO_NONE, flags));
        }
    }
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
        add_legal_move(board, list, move_make(from, target, MOVE_PROMO_NONE, flags));
    }

    int enemy = side ^ 1;
    if (side == WHITE && from == 4 && !board_is_in_check(board, WHITE)) {
        if ((board->castling_rights & CASTLE_WHITE_KING) != 0 &&
            board_piece_at(board, 5) < 0 && board_piece_at(board, 6) < 0 &&
            board_piece_at(board, 7) == WHITE_ROOK &&
            !board_is_square_attacked(board, 5, enemy) &&
            !board_is_square_attacked(board, 6, enemy)) {
            add_legal_move(board, list, move_make(4, 6, MOVE_PROMO_NONE, MOVE_FLAG_CASTLE));
        }
        if ((board->castling_rights & CASTLE_WHITE_QUEEN) != 0 &&
            board_piece_at(board, 1) < 0 && board_piece_at(board, 2) < 0 && board_piece_at(board, 3) < 0 &&
            board_piece_at(board, 0) == WHITE_ROOK &&
            !board_is_square_attacked(board, 3, enemy) &&
            !board_is_square_attacked(board, 2, enemy)) {
            add_legal_move(board, list, move_make(4, 2, MOVE_PROMO_NONE, MOVE_FLAG_CASTLE));
        }
    } else if (side == BLACK && from == 60 && !board_is_in_check(board, BLACK)) {
        if ((board->castling_rights & CASTLE_BLACK_KING) != 0 &&
            board_piece_at(board, 61) < 0 && board_piece_at(board, 62) < 0 &&
            board_piece_at(board, 63) == BLACK_ROOK &&
            !board_is_square_attacked(board, 61, enemy) &&
            !board_is_square_attacked(board, 62, enemy)) {
            add_legal_move(board, list, move_make(60, 62, MOVE_PROMO_NONE, MOVE_FLAG_CASTLE));
        }
        if ((board->castling_rights & CASTLE_BLACK_QUEEN) != 0 &&
            board_piece_at(board, 57) < 0 && board_piece_at(board, 58) < 0 && board_piece_at(board, 59) < 0 &&
            board_piece_at(board, 56) == BLACK_ROOK &&
            !board_is_square_attacked(board, 59, enemy) &&
            !board_is_square_attacked(board, 58, enemy)) {
            add_legal_move(board, list, move_make(60, 58, MOVE_PROMO_NONE, MOVE_FLAG_CASTLE));
        }
    }
}

void movegen_generate_legal(Board *board, MoveList *list) {
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
    movegen_generate_legal(board, &list);
    char buffer[6];
    for (int i = 0; i < list.count; ++i) {
        move_to_string(list.moves[i], buffer);
        if (strcmp(buffer, uci_move) == 0) {
            *out_move = list.moves[i];
            return true;
        }
    }

    return false;
}
