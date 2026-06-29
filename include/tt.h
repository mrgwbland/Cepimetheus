#ifndef TT_H
#define TT_H

#include "board.h"
#include "move.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_PLY_DEPTH 256

typedef uint8_t TranspositionScoreType;
#define TT_SCORE_UPPER 0
#define TT_SCORE_EXACT 1
#define TT_SCORE_LOWER 2

typedef struct
{
    U64 hash;
    Move best_move;
    int16_t score;
    int8_t depth;
    TranspositionScoreType score_type;
} TranspositionEntry;

typedef struct
{
    TranspositionEntry *entries;
    size_t size;
    size_t count;
} TranspositionTable;

bool transposition_table_init(TranspositionTable *table, size_t hash_power);
void transposition_table_destroy(TranspositionTable *table);
const TranspositionEntry *transposition_table_lookup(const TranspositionTable *table, U64 hash);
bool transposition_table_probe(const TranspositionTable *table, U64 hash, int depth, int alpha, int beta, int ply, int *score);
bool transposition_table_probe_exact(const TranspositionTable *table, U64 hash, int depth, int ply, int *score);
void transposition_table_store(TranspositionTable *table, U64 hash, int depth, int score, TranspositionScoreType score_type, Move best_move, int ply);
TranspositionScoreType transposition_score_type(int score, int alpha, int beta);

#endif
