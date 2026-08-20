#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"

typedef struct MoveList {
    Move moves[256];
    int count;
} MoveList;

void movegen_generate_pseudo_legal(Board *board, MoveList *list);
bool movegen_find_legal_move(Board *board, const char *uci_move, Move *out_move);
int find_move_index(const MoveList *list, Move move);

#endif
