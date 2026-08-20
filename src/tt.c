#include "tt.h"
#include "eval.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <malloc.h>
#endif

#define MAX_TRANSPOSITION_TABLE_POWER 30

// Create TT, return true if successful
bool transposition_table_init(TranspositionTable *table, size_t hash_power)
{
    if (hash_power > MAX_TRANSPOSITION_TABLE_POWER)
    {
        hash_power = MAX_TRANSPOSITION_TABLE_POWER;
    }
    size_t size_power = (hash_power >= 2) ? (hash_power - 2) : 0;
    table->size = (size_t)1 << size_power;

    size_t bytes = table->size * sizeof(TranspositionBucket);
#if defined(_WIN32)
    table->buckets = (TranspositionBucket *)_aligned_malloc(bytes, 64);
    int ret = (table->buckets == NULL) ? -1 : 0;
#else
    int ret = posix_memalign((void **)&table->buckets, 64, bytes);
#endif
    if (ret != 0)
    {
        table->buckets = NULL;
        table->size = 0;
        table->count = 0;
        return false;
    }

    memset(table->buckets, 0, bytes);
    table->count = 0;
    return true;
}

// Clear TT
void transposition_table_destroy(TranspositionTable *table)
{
    if (table != NULL && table->buckets != NULL)
    {
#if defined(_WIN32)
        _aligned_free(table->buckets);
#else
        free(table->buckets);
#endif
        table->buckets = NULL;
        table->size = 0;
        table->count = 0;
    }
}

// Get the index of the bucket for a given hash
static size_t transposition_table_index(const TranspositionTable *table, U64 hash)
{
    return (size_t)(hash & (table->size - 1U));
}

const TranspositionEntry *transposition_table_lookup(const TranspositionTable *table, U64 hash)
{
    if (table == NULL || table->buckets == NULL || table->size == 0)
    {
        return NULL;
    }

    const TranspositionBucket *bucket = &table->buckets[transposition_table_index(table, hash)];
    for (int i = 0; i < 4; i++)
    {
        if (bucket->entries[i].hash == hash)
        {
            return &bucket->entries[i];
        }
    }

    return NULL;
}

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

// Test whether a position exists and can cause a cutoff
bool transposition_table_probe(const TranspositionTable *table,
                               U64 hash,
                               int depth,
                               int alpha,
                               int beta,
                               int ply,
                               int *score)
{
    const TranspositionEntry *entry = transposition_table_lookup(table, hash);
    if (entry == NULL || entry->depth < depth || score == NULL)
    {
        return false;
    }

    int entry_score = score_from_tt(entry->score, ply);

    switch (entry->score_type)
    {
    case TT_SCORE_EXACT:
        *score = entry_score;
        return true;

    case TT_SCORE_LOWER:
        if (entry_score >= beta)
        {
            *score = entry_score;
            return true;
        }
        break;

    case TT_SCORE_UPPER:
        if (entry_score <= alpha)
        {
            *score = entry_score;
            return true;
        }
        break;
    }

    return false;
}

// Test whether a position exists and can cause a cutoff, but only allow exact values
bool transposition_table_probe_exact(const TranspositionTable *table,
                                     U64 hash,
                                     int depth,
                                     int ply,
                                     int *score)
{
    const TranspositionEntry *entry = transposition_table_lookup(table, hash);
    if (entry == NULL || entry->depth < depth || score == NULL)
    {
        return false;
    }

    if (entry->score_type == TT_SCORE_EXACT)
    {
        *score = score_from_tt(entry->score, ply);
        return true;
    }

    return false;
}

TranspositionScoreType transposition_score_type(int score, int alpha, int beta)
{
    if (score <= alpha)
    {
        return TT_SCORE_UPPER;
    }

    if (score >= beta)
    {
        return TT_SCORE_LOWER;
    }

    return TT_SCORE_EXACT;
}

static bool transposition_entry_should_replace_same_hash(const TranspositionEntry *entry,
                                                         int depth,
                                                         int score,
                                                         TranspositionScoreType score_type)
{
    if (depth > entry->depth)
    {
        return true;
    }

    if (depth < entry->depth)
    {
        return false;
    }

    if (score_type == TT_SCORE_EXACT)
    {
        return entry->score_type != TT_SCORE_EXACT;
    }

    if (entry->score_type == TT_SCORE_EXACT)
    {
        return false;
    }

    if (score_type != entry->score_type)
    {
        return false;
    }

    if (score_type == TT_SCORE_LOWER)
    {
        return score > entry->score;
    }

    if (score_type == TT_SCORE_UPPER)
    {
        return score < entry->score;
    }

    return false;
}

void transposition_table_store(TranspositionTable *table,
                               U64 hash,
                               int depth,
                               int score,
                               TranspositionScoreType score_type,
                               Move best_move,
                               int ply)
{
    if (table == NULL || table->buckets == NULL || table->size == 0 || best_move == MOVE_NONE)
    {
        return;
    }

    int tt_score = score_to_tt(score, ply);

    TranspositionBucket *bucket = &table->buckets[transposition_table_index(table, hash)];
    TranspositionEntry *entry = NULL;

    // 1. Check if hash already exists in this bucket
    for (int i = 0; i < 4; i++)
    {
        if (bucket->entries[i].hash == hash)
        {
            entry = &bucket->entries[i];
            break;
        }
    }

    if (entry != NULL)
    {
        if (!transposition_entry_should_replace_same_hash(entry, depth, tt_score, score_type))
        {
            return;
        }
    }
    else
    {
        // 2. Look for an empty slot
        for (int i = 0; i < 4; i++)
        {
            if (bucket->entries[i].hash == 0)
            {
                entry = &bucket->entries[i];
                table->count++;
                break;
            }
        }

        // 3. If no empty slot, find the slot with the smallest depth (least valuable)
        if (entry == NULL)
        {
            entry = &bucket->entries[0];
            for (int i = 1; i < 4; i++)
            {
                if (bucket->entries[i].depth < entry->depth)
                {
                    entry = &bucket->entries[i];
                }
            }
            // Evict if the new search depth is strictly greater
            if (depth <= entry->depth)
            {
                return;
            }
        }
    }

    entry->hash = hash;
    entry->depth = depth;
    entry->score = tt_score;
    entry->score_type = score_type;
    entry->best_move = best_move;
}
