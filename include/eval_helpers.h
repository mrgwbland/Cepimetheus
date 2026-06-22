#ifndef EVAL_HELPERS_H
#define EVAL_HELPERS_H

#include "board.h"
#include "movegen.h"

extern const U64 file_masks[8];

// Helper functions for evaluation module
int count_attackers_on_square(const Board *board, int square, int attacker_side, U64 all_pieces);
int count_king_ring_attackers(const Board *board, int king_side, U64 all_pieces);
void count_pawns_per_file(U64 pawns, int pawns_per_file[8]);
U64 mark_passed_pawns(const Board *board, int side);

static inline int rank_of(int square) { return square >> 3; }
static inline int file_of(int square) { return square & 7; }

#endif
