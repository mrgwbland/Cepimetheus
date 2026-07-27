#ifndef SEE_H
#define SEE_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "move.h"
#include "movepicker.h"

U64 board_attackers_to(const Board *board, int square, U64 occupancy);
bool see_ge(const Board *board, Move move, int threshold);

#endif
