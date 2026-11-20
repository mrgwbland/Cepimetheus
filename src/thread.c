#include "thread.h"
#include "think.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    Board board;
    RepetitionHistory history;
    SearchLimits limits;
    SearchOptions options;
    SearchContext *context;
    volatile bool *stop_signal;
} SmpWorkerArg;

// Pool of worker threads owned by the primary thread
struct SmpPool
{
    int worker_count; // threads - 1
    int active_count;
    pthread_t *threads;
    SearchContext **contexts;
    SmpWorkerArg *args;
    volatile bool stop;
};

// Thread entry point, runs iterative deepening via think()
static void *smp_worker_entry(void *raw_arg)
{
    SmpWorkerArg *arg = (SmpWorkerArg *)raw_arg;
    think(&arg->board,
          &arg->limits,
          &arg->options,
          &arg->history,
          arg->stop_signal,
          NULL,
          NULL,
          NULL,
          arg->context);
    return NULL;
}

static bool smp_pool_resize(SmpPool *pool, TranspositionTable *table, int desired_workers)
{
    if (pool->worker_count == desired_workers)
    {
        return true;
    }

    for (int i = 0; i < pool->worker_count; ++i)
    {
        search_context_destroy(pool->contexts[i]);
    }
    free(pool->threads);
    free(pool->contexts);
    free(pool->args);

    pool->worker_count = 0;
    pool->active_count = 0;
    pool->threads = NULL;
    pool->contexts = NULL;
    pool->args = NULL;

    if (desired_workers <= 0)
    {
        return true;
    }

    pool->threads = calloc((size_t)desired_workers, sizeof(*pool->threads));
    pool->contexts = calloc((size_t)desired_workers, sizeof(*pool->contexts));
    pool->args = calloc((size_t)desired_workers, sizeof(*pool->args));
    if (pool->threads == NULL || pool->contexts == NULL || pool->args == NULL)
    {
        free(pool->threads);
        free(pool->contexts);
        free(pool->args);
        pool->threads = NULL;
        pool->contexts = NULL;
        pool->args = NULL;
        return false;
    }

    for (int i = 0; i < desired_workers; ++i)
    {
        pool->contexts[i] = search_context_create(0, table, i + 1);
        if (pool->contexts[i] == NULL)
        {
            for (int j = 0; j < i; ++j)
            {
                search_context_destroy(pool->contexts[j]);
            }
            free(pool->threads);
            free(pool->contexts);
            free(pool->args);
            pool->threads = NULL;
            pool->contexts = NULL;
            pool->args = NULL;
            return false;
        }
    }

    pool->worker_count = desired_workers;
    return true;
}

// Spawn (options->threads - 1) worker threads to search alongside the primary thread
void smp_pool_start(SearchContext *primary,
                    const Board *board,
                    const SearchLimits *limits,
                    const SearchOptions *options,
                    const RepetitionHistory *history)
{
    if (primary == NULL || primary->thread_index != 0 || options == NULL || options->threads <= 1)
    {
        return;
    }

    int desired_workers = options->threads - 1;
    if (primary->smp_pool == NULL)
    {
        primary->smp_pool = calloc(1, sizeof(*primary->smp_pool));
        if (primary->smp_pool == NULL)
        {
            return;
        }
    }

    SmpPool *pool = primary->smp_pool;
    if (!smp_pool_resize(pool, primary->table, desired_workers))
    {
        return;
    }

    pool->stop = false;
    pool->active_count = 0;

    // Workers ignore limits and run until depth cap or primary's stop signal
    SearchLimits worker_limits = {0};
    if (limits != NULL)
    {
        worker_limits = *limits;
    }
    worker_limits.has_clock_time = false;
    worker_limits.movetime_ms = 0;
    worker_limits.infinite = false;
    worker_limits.ponder = false;

    SearchOptions worker_options = *options;
    worker_options.silent = true;
    worker_options.display_currmove = false;
    worker_options.threads = 1;

    for (int i = 0; i < pool->worker_count; ++i)
    {
        atomic_store_explicit(&pool->contexts[i]->thread_nodes, 0ULL, memory_order_relaxed);
        pool->args[i].board = *board;
        if (history != NULL)
        {
            pool->args[i].history = *history;
        }
        else
        {
            repetition_history_init(&pool->args[i].history);
        }
        pool->args[i].limits = worker_limits;
        pool->args[i].options = worker_options;
        pool->args[i].context = pool->contexts[i];
        pool->args[i].stop_signal = &pool->stop;

        if (pthread_create(&pool->threads[i], NULL, smp_worker_entry, &pool->args[i]) != 0)
        {
            break;
        }
        pool->active_count++;
    }
}

void smp_pool_stop_and_join(SearchContext *primary)
{
    if (primary == NULL || primary->thread_index != 0 || primary->smp_pool == NULL)
    {
        return;
    }

    SmpPool *pool = primary->smp_pool;
    pool->stop = true;

    for (int i = 0; i < pool->active_count; ++i)
    {
        pthread_join(pool->threads[i], NULL);
    }
    pool->active_count = 0;
}

void smp_pool_clear(SearchContext *primary)
{
    if (primary == NULL || primary->thread_index != 0 || primary->smp_pool == NULL)
    {
        return;
    }

    SmpPool *pool = primary->smp_pool;
    for (int i = 0; i < pool->worker_count; ++i)
    {
        search_context_clear(pool->contexts[i]);
    }
}

void smp_pool_destroy(SmpPool *pool)
{
    if (pool == NULL)
    {
        return;
    }

    pool->stop = true;
    for (int i = 0; i < pool->active_count; ++i)
    {
        pthread_join(pool->threads[i], NULL);
    }
    for (int i = 0; i < pool->worker_count; ++i)
    {
        search_context_destroy(pool->contexts[i]);
    }
    free(pool->threads);
    free(pool->contexts);
    free(pool->args);
    free(pool);
}

unsigned long long smp_pool_nodes(const SearchContext *primary)
{
    if (primary == NULL || primary->thread_index != 0 || primary->smp_pool == NULL)
    {
        return 0ULL;
    }

    const SmpPool *pool = primary->smp_pool;
    unsigned long long total = 0ULL;
    for (int i = 0; i < pool->worker_count; ++i)
    {
        total += atomic_load_explicit(&pool->contexts[i]->thread_nodes, memory_order_relaxed);
    }
    return total;
}
