#ifndef SEARCH_HELPERS_H
#define SEARCH_HELPERS_H

#include "board.h"
#include "move.h"
#include "movegen.h"
#include "search.h"
#include <stdbool.h>

extern int LMR[64][256];

bool search_should_stop(SearchControl *control, long long nodes);
bool has_sufficient_nmp_material(const Board *board);
int get_captured_piece_value(const Board *board, Move move);
bool has_any_legal_move(Board *board, const MoveList *list);
bool board_has_any_legal_move(Board *board);

#endif
