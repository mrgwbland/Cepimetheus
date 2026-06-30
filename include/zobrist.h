#ifndef ZOBRIST_H
#define ZOBRIST_H

#include <stdint.h>
#include "board.h"

extern uint64_t ZOBRIST_PIECES[PIECE_NB][64];
extern uint64_t ZOBRIST_EP_KEYS[8];
extern uint64_t ZOBRIST_CASTLE_KEYS[16];
extern uint64_t ZOBRIST_SIDE_KEY;

void zobrist_init(void);
uint64_t zobrist_hash_full(const Board *board);

#endif
