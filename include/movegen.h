#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"

typedef struct MoveList {
    Move moves[256];
    int count;
} MoveList;

int movegen_generate_noisy(const Board *board, Move *moves);
int movegen_generate_quiet(const Board *board, Move *moves);
void movegen_generate_pseudo_legal(const Board *board, MoveList *list);
bool movegen_find_legal_move(Board *board, const char *uci_move, Move *out_move);
int find_move_index(const MoveList *list, Move move);

#endif
