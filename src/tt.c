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

// Reset all entries in the TT without freeing memory
void transposition_table_clear(TranspositionTable *table)
{
    if (table != NULL && table->buckets != NULL && table->size > 0)
    {
        size_t bytes = table->size * sizeof(TranspositionBucket);
        memset(table->buckets, 0, bytes);
        table->count = 0;
        table->generation = 0;
    }
}

// Increment search generation counter for age-based replacement
void transposition_table_new_search(TranspositionTable *table)
{
    if (table != NULL)
    {
        table->generation += TT_GEN_INC;
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
    TranspositionScoreType score_type = tt_entry_bound(entry);

    switch (score_type)
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

    if (tt_entry_bound(entry) == TT_SCORE_EXACT)
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

static int tt_entry_quality(const TranspositionEntry *entry, uint8_t current_gen)
{
    int age_diff = (TT_GEN_CYCLE + current_gen - entry->gen_bound) & TT_GEN_MASK;
    return (int)entry->depth - age_diff;
}

void transposition_table_store(TranspositionTable *table,
                               U64 hash,
                               int depth,
                               int score,
                               TranspositionScoreType score_type,
                               Move best_move,
                               int ply)
{
    if (table == NULL || table->buckets == NULL || table->size == 0)
    {
        return;
    }

    int tt_score = score_to_tt(score, ply);
    TranspositionBucket *bucket = &table->buckets[transposition_table_index(table, hash)];

    // Find the target slot

    TranspositionEntry *target = NULL;
    bool same_pos = false;

    // (a) Look for a hash match first
    for (int i = 0; i < 4; i++)
    {
        if (bucket->entries[i].hash == hash)
        {
            target = &bucket->entries[i];
            same_pos = true;
            break;
        }
    }

    // (b) If no match, look for an empty slot
    if (target == NULL)
    {
        for (int i = 0; i < 4; i++)
        {
            if (bucket->entries[i].hash == 0)
            {
                target = &bucket->entries[i];
                table->count++;
                break;
            }
        }
    }

    // (c) If no empty slot either, evict the lowest-quality entry
    // We never don't store a value, as the current search tree is almost certainly more relevant
    if (target == NULL)
    {
        target = &bucket->entries[0];
        int worst_quality = tt_entry_quality(target, table->generation);

        for (int i = 1; i < 4; i++)
        {
            int quality = tt_entry_quality(&bucket->entries[i], table->generation);
            if (quality < worst_quality)
            {
                worst_quality = quality;
                target = &bucket->entries[i];
            }
        }
    }

    // Overwrite protection for same position
    if (same_pos)
    {
        // Allow the overwrite if any of these are true:
        bool is_better = ((score_type == TT_SCORE_EXACT && (target->gen_bound & TT_BOUND_MASK) != TT_SCORE_EXACT) || (score_type == TT_SCORE_EXACT && depth > target->depth)); // The new result has a better EXACT score (most valuable bound type)
        bool is_stale = (tt_entry_gen(target) != table->generation); // The existing entry is from a previous search (stale generation)
        bool is_deeper = (depth >= (int)target->depth); // The new depth is deeper than the existing depth

        if (!is_better && !is_stale && !is_deeper) // Replacement rejected
        {            
            if (best_move != MOVE_NONE && target->best_move == MOVE_NONE) // Despite rejection, store a best move if we have one and one is not already present
            {
                target->best_move = best_move;
            }
            return;
        }
    }

    // Store best move if we have one or if we're overwriting (even move=none should be stored when overwriting)
    if (best_move != MOVE_NONE || !same_pos)
    {
        target->best_move = best_move;
    }

    target->hash = hash;
    target->depth = (int8_t)depth;
    target->score = (int16_t)tt_score;
    target->gen_bound = (uint8_t)(table->generation | (score_type & TT_BOUND_MASK));
}
