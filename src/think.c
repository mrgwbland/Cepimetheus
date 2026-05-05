#include "think.h"
#include "../include/search.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static int score_to_cp(float score) {
    if (score >= 0.0f) {
        return (int)(score + 0.5f);
    }

    return (int)(score - 0.5f);
}

static void print_move_info(int depth, int move_number, Move move, float score) {
    char move_buffer[6];
    move_to_string(move, move_buffer);
    printf("info depth %d currmove %s currmovenumber %d score cp %d\n",
           depth,
           move_buffer,
           move_number,
           score_to_cp(score));
    fflush(stdout);
}

static void print_move_info_callback(int depth,
                                     int move_number,
                                     Move move,
                                     float score,
                                     void *user_data) {
    (void)user_data;
    print_move_info(depth, move_number, move, score);
}

static long long current_time_ms(void) {
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + (long long)now.tv_usec / 1000LL;
}

static unsigned long long compute_nps(unsigned long long nodes, long long elapsed_ms) {
    if (elapsed_ms <= 0) {
        return nodes * 1000ULL;
    }

    return (nodes * 1000ULL) / (unsigned long long)elapsed_ms;
}

static void print_depth_info(int depth, const SearchResult *result, const SearchStats *stats, long long elapsed_ms) {
    unsigned long long nps = compute_nps(stats->nodes, elapsed_ms);
    printf("info depth %d seldepth %d score cp %d nodes %llu nps %llu time %lld",
           depth,
           stats->seldepth,
           score_to_cp(result->score),
           stats->nodes,
           nps,
           elapsed_ms);

    if (result->pv_length > 0) {
        printf(" pv");
        for (int i = 0; i < result->pv_length; ++i) {
            char move_buffer[6];
            move_to_string(result->pv[i], move_buffer);
            printf(" %s", move_buffer);
        }
    }

    printf("\n");
    fflush(stdout);
}

static int clamp_time_budget(int budget_ms) {
    if (budget_ms < 10) {
        return 10;
    }

    return budget_ms;
}

static bool compute_clock_budget(const Board *board,
                                 const SearchLimits *limits,
                                 const SearchOptions *options,
                                 int *soft_budget_ms,
                                 int *hard_budget_ms) {
    if (board == NULL || limits == NULL || soft_budget_ms == NULL || hard_budget_ms == NULL) {
        return false;
    }

    int overhead_ms = 50;
    if (options != NULL) {
        overhead_ms = options->overhead_ms;
    }

    int base_ms = 0;
    int increment_ms = 0;

    if (board->side == WHITE) {
        base_ms = limits->wtime_ms;
        increment_ms = limits->winc_ms;
    } else {
        base_ms = limits->btime_ms;
        increment_ms = limits->binc_ms;
    }

    int total_ms = base_ms + increment_ms;
    int soft_ms = (total_ms / 30) - overhead_ms;
    int hard_ms = (total_ms / 10) - overhead_ms;

    if (hard_ms < soft_ms) {
        hard_ms = soft_ms;
    }

    *soft_budget_ms = clamp_time_budget(soft_ms);
    *hard_budget_ms = clamp_time_budget(hard_ms);
    return true;
}


Move think(Board *board,
           const SearchLimits *limits,
           const SearchOptions *options,
           const RepetitionHistory *history,
           volatile bool *stop_signal) {
    if (board == NULL) {
        return MOVE_NONE;
    }

    int target_depth = 4; //Default if no limits provided
    int movetime_ms = 0;
    int soft_time_limit_ms = 0;
    int hard_time_limit_ms = 0;
    int overhead_ms = 50;
    bool depth_explicitly_set = false;
    bool time_limited = false;
    bool infinite_search = false;
    const int max_iterative_depth = 64;

    if (options != NULL) {
        overhead_ms = options->overhead_ms;
    }

    if (limits != NULL) {
        if (limits->depth > 0) {
            target_depth = limits->depth;
            depth_explicitly_set = true;
        }
        if (limits->movetime_ms > 0) {
            movetime_ms = limits->movetime_ms;
        } else if (limits->has_clock_time && compute_clock_budget(board, limits, options, &soft_time_limit_ms, &hard_time_limit_ms)) {
            time_limited = true;
        }

        if (limits->infinite) {
            infinite_search = true;
        }
    }

    if (movetime_ms > 0 && !depth_explicitly_set) {
        /* In pure movetime mode, deepen until the time budget is consumed. */
        target_depth = max_iterative_depth;
    }

    if (movetime_ms > 0) {
        soft_time_limit_ms = movetime_ms - overhead_ms;
        hard_time_limit_ms = soft_time_limit_ms;
        soft_time_limit_ms = clamp_time_budget(soft_time_limit_ms);
        hard_time_limit_ms = clamp_time_budget(hard_time_limit_ms);
        time_limited = true;
    } else if (time_limited && !depth_explicitly_set) {
        target_depth = max_iterative_depth;
    }

    RepetitionHistory search_history;
    repetition_history_init(&search_history);
    if (history != NULL) {
        search_history = *history;
    }

    long long start_time_ms = current_time_ms();

    SearchControl control = {0};
    control.external_stop = stop_signal;
    if (time_limited) {
        control.hard_time_limited = true;
        control.hard_stop_time_ms = start_time_ms + (long long)hard_time_limit_ms;
    }

    SearchContext *search_context = search_context_create();

    SearchResult best_result = {0.0f, MOVE_NONE, {0}, 0, false};
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0, false};

    /* Iterative deepening: search depths 1 through target_depth. */
    int depth_limit = target_depth;
    if (infinite_search && !depth_explicitly_set && !time_limited && movetime_ms <= 0) {
        depth_limit = INT_MAX;
    }

    for (int depth = 1; depth <= depth_limit; ++depth) {
        if (stop_signal != NULL && *stop_signal) {
            break;
        }

        if (time_limited) {
            long long elapsed_before_depth_ms = current_time_ms() - start_time_ms;
            if (elapsed_before_depth_ms < 0) {
                elapsed_before_depth_ms = 0;
            }

            if (elapsed_before_depth_ms >= soft_time_limit_ms && best_result.move != MOVE_NONE) {
                break;
            }
        }

        SearchStats stats = {0ULL, depth};
        control.stop = false;

        result = search_root(board,
                             depth,
                             &search_history,
                             &stats,
                             search_context,
                             &control,
                             print_move_info_callback,
                             NULL);

        if (control.stop) {
            break;
        }

        long long elapsed_ms = current_time_ms() - start_time_ms;
        if (elapsed_ms < 0) {
            elapsed_ms = 0;
        }

        print_depth_info(depth, &result, &stats, elapsed_ms);

        /* Update best result if we have a valid move */
        if (result.move != MOVE_NONE) {
            best_result = result;
        }

        // If the root search resulted in only one legal move then we can stop searching immediately, no matter the time limits, as we won't find a better move by searching deeper
        if (result.forced_root_move) {
            break;
        }

        /* Check if we should stop searching */
        if (time_limited && elapsed_ms >= soft_time_limit_ms) {
            break;
        }
    }

    if (best_result.move == MOVE_NONE) {
        search_context_destroy(search_context);
        return MOVE_NONE;
    }

    search_context_destroy(search_context);
    return best_result.move;
}