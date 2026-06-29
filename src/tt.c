#include "tt.h"
#include "eval.h"
#include <stdlib.h>
#include <string.h>

#define MAX_TRANSPOSITION_TABLE_POWER 30

bool transposition_table_init(TranspositionTable *table, size_t hash_power)
{
    if (hash_power > MAX_TRANSPOSITION_TABLE_POWER)
    {
        hash_power = MAX_TRANSPOSITION_TABLE_POWER;
    }
    table->size = (size_t)1 << hash_power;
    table->entries = calloc(table->size, sizeof(*table->entries));
    table->count = 0;
    return table->entries != NULL;
}

void transposition_table_destroy(TranspositionTable *table)
{
    if (table != NULL)
    {
        free(table->entries);
        table->entries = NULL;
        table->size = 0;
        table->count = 0;
    }
}

static size_t transposition_table_index(const TranspositionTable *table, U64 hash)
{
    return (size_t)(hash & (table->size - 1U));
}

const TranspositionEntry *transposition_table_lookup(const TranspositionTable *table, U64 hash)
{
    if (table == NULL || table->entries == NULL || table->size == 0)
    {
        return NULL;
    }

    const TranspositionEntry *entry = &table->entries[transposition_table_index(table, hash)];
    if (entry->hash == 0 || entry->hash != hash)
    {
        return NULL;
    }

    return entry;
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
    if (table == NULL || table->entries == NULL || table->size == 0 || best_move == MOVE_NONE)
    {
        return;
    }

    int tt_score = score_to_tt(score, ply);

    TranspositionEntry *entry = &table->entries[transposition_table_index(table, hash)];
    if (entry->hash == hash)
    {
        if (!transposition_entry_should_replace_same_hash(entry, depth, tt_score, score_type))
        {
            return;
        }
    }
    else if (entry->hash != 0 && depth <= entry->depth)
    {
        return;
    }

    if (entry->hash == 0)
    {
        table->count++;
    }

    entry->hash = hash;
    entry->depth = depth;
    entry->score = tt_score;
    entry->score_type = score_type;
    entry->best_move = best_move;
}
