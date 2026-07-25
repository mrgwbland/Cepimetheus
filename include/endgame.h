#ifndef ENDGAME_H
#define ENDGAME_H

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

#define ENDGAME_TABLE_SIZE 4096
#define SCALE_NORMAL 256
#define SCALE_DRAW 0

typedef int (*EndgameFunc)(const Board *board, int strong_side);
typedef int (*EndgameScaleFunc)(const Board *board, int strong_side);

typedef struct {
    uint64_t key;
    EndgameFunc eval_fn;
    EndgameScaleFunc scale_fn;
    int strong_side;
    bool is_valid;
} EndgameEntry;

void endgame_init(void);
uint64_t board_material_key(const Board *board);
const EndgameEntry *endgame_probe(const Board *board);
bool is_ocb(const Board *board);
bool is_material_draw(const Board *board);

#endif // ENDGAME_H
