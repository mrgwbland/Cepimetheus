#ifndef EVAL_HELPERS_H
#define EVAL_HELPERS_H

#include "board.h"
#include "movegen.h"

extern const U64 file_masks[8];

// Helper functions for evaluation module
void count_pawns_per_file(U64 pawns, int pawns_per_file[8]);
U64 mark_passed_pawns(const Board *board, int side);

static inline int rank_of(int square) { return square >> 3; }
static inline int file_of(int square) { return square & 7; }

#endif
