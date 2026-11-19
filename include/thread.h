#ifndef THREAD_H
#define THREAD_H

#include "board.h"
#include "search.h"

struct SearchLimits;
struct SearchOptions;

typedef struct SmpPool SmpPool;

void smp_pool_start(SearchContext *primary,
                    const Board *board,
                    const struct SearchLimits *limits,
                    const struct SearchOptions *options,
                    const RepetitionHistory *history);
void smp_pool_stop_and_join(SearchContext *primary);
void smp_pool_clear(SearchContext *primary);
void smp_pool_destroy(SmpPool *pool);
unsigned long long smp_pool_nodes(const SearchContext *primary);

#endif
