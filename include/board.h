#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "bitboard.h"
#include "move.h"

enum {
    WHITE = 0,
    BLACK = 1,
    BOTH = 2
};

enum {
    WHITE_PAWN,
    WHITE_KNIGHT,
    WHITE_BISHOP,
    WHITE_ROOK,
    WHITE_QUEEN,
    WHITE_KING,
    BLACK_PAWN,
    BLACK_KNIGHT,
    BLACK_BISHOP,
    BLACK_ROOK,
    BLACK_QUEEN,
    BLACK_KING,
    PIECE_NB
};

enum {
    CASTLE_WHITE_KING = 1 << 0,
    CASTLE_WHITE_QUEEN = 1 << 1,
    CASTLE_BLACK_KING = 1 << 2,
    CASTLE_BLACK_QUEEN = 1 << 3
};

enum {
    REPETITION_HISTORY_MAX = 1024
};

typedef struct Board {
    int squares[64];
    U64 pieces[PIECE_NB];
    U64 occupancy[3];
    int side;
    int castling_rights;
    int ep_square;
    int halfmove_clock;
    int fullmove_number;
    int king_square[2];
    U64 hash;
} Board;

typedef struct Undo {
    U64 hash;
    int castling_rights;
    int ep_square;
    int halfmove_clock;
    int captured_piece;
    Move move;
} Undo;

typedef struct RepetitionHistory {
    U64 keys[REPETITION_HISTORY_MAX];
    int count;
} RepetitionHistory;

void board_init(Board *board);
void board_set_startpos(Board *board);
bool board_set_fen(Board *board, const char *fen);
void board_clear(Board *board);
void repetition_history_init(RepetitionHistory *history);
bool repetition_history_push(RepetitionHistory *history, U64 key);
U64 board_position_key(const Board *board);
bool board_is_draw(const Board *board, const RepetitionHistory *history, int ply, bool lichess_draw_rules);
bool board_make_move(Board *board, Move move, Undo *undo);
void board_make_null_move(Board *board, Undo *undo);
void board_unmake_move(Board *board, const Undo *undo);
bool board_is_square_attacked(const Board *board, int square, int attacker_side);
bool board_is_square_attacked_with_occupancy(const Board *board, int square, int attacker_side, U64 occupancy);
bool board_is_in_check(const Board *board, int side);
U64 board_pinned_mask(const Board *board, int side);
U64 board_checkers(const Board *board, int side);
bool board_is_move_legal(const Board *board, Move move, U64 pinned_mask, U64 checkers);
int board_piece_at(const Board *board, int square);
int board_piece_color(int piece);
int board_piece_type(int piece);
int board_parse_square(const char *text);
void board_square_to_string(int square, char buffer[3]);

#endif
