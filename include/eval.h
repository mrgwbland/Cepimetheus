#ifndef EVAL_H
#define EVAL_H

#include "board.h"

bool eval_is_endgame_position(const Board *board);
extern const int piece_values[6];
float evaluate_position(Board *board, const RepetitionHistory *history, int ply);

#endif
