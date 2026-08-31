#ifndef TT_H
#define TT_H

#include "board.h"
#include "eval.h"
#include "move.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_PLY_DEPTH 256

typedef uint8_t TranspositionScoreType;
#define TT_SCORE_UPPER 0
#define TT_SCORE_EXACT 1
#define TT_SCORE_LOWER 2

#define TT_BOUND_MASK 0x03
#define TT_GEN_MASK   0xFC
#define TT_GEN_INC    0x04 // TT increments by 4 because 2 bits are used for score type, so this can be thought of as an increment of 1
#define TT_GEN_CYCLE  (256 + TT_GEN_INC)

typedef struct
{
    U64 hash; // 8 bytes
    Move best_move; // 4 bytes
    int16_t score; // 2 bytes
    int8_t depth; // 1 byte
    uint8_t gen_bound; // 1 byte (first 6 bits = generation, last 2 bits = score_type)
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
    uint8_t generation;
} TranspositionTable;

static inline TranspositionScoreType tt_entry_bound(const TranspositionEntry *entry)
{
    return (TranspositionScoreType)(entry->gen_bound & TT_BOUND_MASK);
}

static inline uint8_t tt_entry_gen(const TranspositionEntry *entry)
{
    return (uint8_t)(entry->gen_bound & TT_GEN_MASK);
}

// 
static inline int score_to_tt(int score, int ply)
{
    if (score > MATE_SCORE - MAX_PLY_DEPTH)
    {
        return score + ply;
    }
    if (score < -MATE_SCORE + MAX_PLY_DEPTH)
    {
        return score - ply;
    }
    return score;
}

static inline int score_from_tt(int score, int ply)
{
    if (score > MATE_SCORE - MAX_PLY_DEPTH)
    {
        return score - ply;
    }
    if (score < -MATE_SCORE + MAX_PLY_DEPTH)
    {
        return score + ply;
    }
    return score;
}

bool transposition_table_init(TranspositionTable *table, size_t hash_power);
void transposition_table_destroy(TranspositionTable *table);
void transposition_table_clear(TranspositionTable *table);
void transposition_table_new_search(TranspositionTable *table);
const TranspositionEntry *transposition_table_lookup(const TranspositionTable *table, U64 hash);
bool transposition_table_probe(const TranspositionTable *table, U64 hash, int depth, int alpha, int beta, int ply, int *score);
bool transposition_table_probe_exact(const TranspositionTable *table, U64 hash, int depth, int ply, int *score);
void transposition_table_store(TranspositionTable *table, U64 hash, int depth, int score, TranspositionScoreType score_type, Move best_move, int ply);
TranspositionScoreType transposition_score_type(int score, int alpha, int beta);

#endif
