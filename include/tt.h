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
    U64 hash; // 8 bytes
    Move best_move; // 4 bytes
    int16_t score; // 2 bytes
    int8_t depth; // 1 byte
    TranspositionScoreType score_type; // 1 byte
} TranspositionEntry; // 16 bytes

typedef struct
{
    TranspositionEntry entries[4];
} TranspositionBucket; // 64 bytes (aligned to cache lines)

typedef struct
{
    TranspositionBucket *buckets;
    size_t size;
    size_t count;
} TranspositionTable;

bool transposition_table_init(TranspositionTable *table, size_t hash_power);
void transposition_table_destroy(TranspositionTable *table);
void transposition_table_clear(TranspositionTable *table);
const TranspositionEntry *transposition_table_lookup(const TranspositionTable *table, U64 hash);
bool transposition_table_probe(const TranspositionTable *table, U64 hash, int depth, int alpha, int beta, int ply, int *score);
bool transposition_table_probe_exact(const TranspositionTable *table, U64 hash, int depth, int ply, int *score);
void transposition_table_store(TranspositionTable *table, U64 hash, int depth, int score, TranspositionScoreType score_type, Move best_move, int ply);
TranspositionScoreType transposition_score_type(int score, int alpha, int beta);

#endif
