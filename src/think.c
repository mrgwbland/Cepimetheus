#include "think.h"
#include "../include/search.h"
#include "../include/eval.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static int score_to_cp(int score)
{
    if (score >= 0)
    {
        return score / 10;
    }

    return score / 10;
}

void get_score_string(int score, char *buffer, size_t size)
{
    int abs_score = score >= 0 ? score : -score;
    if (abs_score > MATE_SCORE - 256)
    {
        int depth = (MATE_SCORE - abs_score + 1) / 2;
        if (score < 0)
        {
            snprintf(buffer, size, "mate -%d", depth);
        }
        else
        {
            snprintf(buffer, size, "mate %d", depth);
        }
    }
    else
    {
        snprintf(buffer, size, "cp %d", score_to_cp(score));
    }
}

static void print_move_info(int depth, int move_number, Move move, int score)
{
    char move_buffer[6];
    move_to_string(move, move_buffer);
    char score_buffer[32];
    get_score_string(score, score_buffer, sizeof(score_buffer));
    printf("info depth %d currmove %s currmovenumber %d score %s\n",
           depth,
           move_buffer,
           move_number,
           score_buffer);
    fflush(stdout);
}

static void print_move_info_callback(int depth,
                                     int move_number,
                                     Move move,
                                     int score,
                                     void *user_data)
{
    (void)user_data;
    print_move_info(depth, move_number, move, score);
}

static long long current_time_ms(void)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0)
    {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + (long long)now.tv_usec / 1000LL;
}

static unsigned long long compute_nps(unsigned long long nodes, long long elapsed_ms)
{
    if (elapsed_ms <= 0)
    {
        return nodes * 1000ULL;
    }

    return (nodes * 1000ULL) / (unsigned long long)elapsed_ms;
}

static void print_depth_info(int depth, int multipv, const SearchResult *result, const SearchStats *stats, long long elapsed_ms)
{
    unsigned long long nps = compute_nps(stats->nodes, elapsed_ms);
    char score_buffer[32];
    get_score_string(result->score, score_buffer, sizeof(score_buffer));
    printf("info depth %d multipv %d seldepth %d score %s nodes %llu nps %llu time %lld hashfull %d",
           depth,
           multipv,
           stats->seldepth,
           score_buffer,
           stats->nodes,
           nps,
           elapsed_ms,
           stats->hashfull);

    if (result->pv_length > 0)
    {
        printf(" pv");
        for (int i = 0; i < result->pv_length; ++i)
        {
            char move_buffer[6];
            move_to_string(result->pv[i], move_buffer);
            printf(" %s", move_buffer);
        }
    }

    printf("\n");
    fflush(stdout);
}

static int clamp_time_budget(int budget_ms)
{
    if (budget_ms < 10)
    {
        return 10;
    }

    return budget_ms;
}

static bool compute_clock_budget(const Board *board,
                                 const SearchLimits *limits,
                                 const SearchOptions *options,
                                 int *soft_budget_ms,
                                 int *hard_budget_ms)
{
    if (board == NULL || limits == NULL || soft_budget_ms == NULL || hard_budget_ms == NULL)
    {
        return false;
    }

    int overhead_ms = 50;
    if (options != NULL)
    {
        overhead_ms = options->overhead_ms;
    }

    int base_ms = 0;
    int increment_ms = 0;

    if (board->side == WHITE)
    {
        base_ms = limits->wtime_ms;
        increment_ms = limits->winc_ms;
    }
    else
    {
        base_ms = limits->btime_ms;
        increment_ms = limits->binc_ms;
    }

    int total_ms = base_ms + increment_ms;
    int soft_ms = (total_ms / 30) - overhead_ms;
    int hard_ms = (total_ms / 5) - overhead_ms;

    if (hard_ms < soft_ms)
    {
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
           volatile bool *stop_signal)
{
    if (board == NULL)
    {
        return MOVE_NONE;
    }

    int target_depth = 4; // Default if no limits provided
    int movetime_ms = 0;
    int soft_time_limit_ms = 0;
    int hard_time_limit_ms = 0;
    int overhead_ms = 50;
    int multipv = 1;
    bool depth_explicitly_set = false;
    bool time_limited = false;
    bool infinite_search = false;
    const int max_iterative_depth = 64;

    if (options != NULL)
    {
        overhead_ms = options->overhead_ms;
        if (options->multipv > 0)
        {
            multipv = options->multipv;
            if (multipv > 256)
            {
                multipv = 256;
            }
        }
    }

    if (limits != NULL)
    {
        if (limits->depth > 0)
        {
            target_depth = limits->depth;
            depth_explicitly_set = true;
        }
        if (limits->movetime_ms > 0)
        {
            movetime_ms = limits->movetime_ms;
        }
        else if (limits->has_clock_time && compute_clock_budget(board, limits, options, &soft_time_limit_ms, &hard_time_limit_ms))
        {
            time_limited = true;
        }

        if (limits->infinite)
        {
            infinite_search = true;
        }
    }

    if (movetime_ms > 0 && !depth_explicitly_set)
    {
        /* In pure movetime mode, deepen until the time budget is consumed. */
        target_depth = max_iterative_depth;
    }

    if (movetime_ms > 0)
    {
        soft_time_limit_ms = movetime_ms - overhead_ms;
        hard_time_limit_ms = soft_time_limit_ms;
        soft_time_limit_ms = clamp_time_budget(soft_time_limit_ms);
        hard_time_limit_ms = clamp_time_budget(hard_time_limit_ms);
        time_limited = true;
    }
    else if (time_limited && !depth_explicitly_set)
    {
        target_depth = max_iterative_depth;
    }

    RepetitionHistory search_history;
    repetition_history_init(&search_history);
    if (history != NULL)
    {
        search_history = *history;
    }

    long long start_time_ms = current_time_ms();

    SearchControl control = {0};
    control.external_stop = stop_signal;
    if (time_limited)
    {
        control.hard_time_limited = true;
        control.hard_stop_time_ms = start_time_ms + (long long)hard_time_limit_ms;
    }
    // Only return single moves instantly if playing on clock time
    control.allow_forced_root_move = (time_limited && movetime_ms <= 0 && !depth_explicitly_set && !infinite_search);

    size_t hash_power = 20;
    if (options != NULL && options->hash_power >= 0)
    {
        hash_power = (size_t)options->hash_power;
    }

    SearchContext *search_context = search_context_create(hash_power);

    SearchResult best_result = {0, MOVE_NONE, {0}, 0, false};

    /* Iterative deepening: search depths 1 through target_depth. */
    int depth_limit = target_depth;
    if (infinite_search && !depth_explicitly_set && !time_limited && movetime_ms <= 0)
    {
        depth_limit = INT_MAX;
    }

    for (int depth = 1; depth <= depth_limit; ++depth)
    {
        if (stop_signal != NULL && *stop_signal)
        {
            break;
        }

        if (time_limited)
        {
            long long elapsed_before_depth_ms = current_time_ms() - start_time_ms;
            if (elapsed_before_depth_ms < 0)
            {
                elapsed_before_depth_ms = 0;
            }

            if (elapsed_before_depth_ms >= soft_time_limit_ms && best_result.move != MOVE_NONE)
            {
                break;
            }
        }

        bool lichess_draw_rules = (options != NULL) ? options->lichess_draw_rules : false;
        Move excluded_root_moves[256] = {0};
        int excluded_root_move_count = 0;
        SearchResult depth_best_result = {0, MOVE_NONE, {0}, 0, false};

        for (int multipv_index = 0; multipv_index < multipv; ++multipv_index)
        {
            if (stop_signal != NULL && *stop_signal)
            {
                break;
            }

            SearchStats stats = {0ULL, 0, 0};
            control.stop = false;

            SearchResult result;
            if (depth >= 5 && best_result.move != MOVE_NONE && multipv_index == 0) // Use aspiration window search around the best score from the previous depth
            {
                int delta = 250; //0.25 pawn equivalent
                int alpha = best_result.score - delta;
                int beta = best_result.score + delta;

                if (alpha < -MATE_SCORE)
                    alpha = -MATE_SCORE;
                if (beta > MATE_SCORE)
                    beta = MATE_SCORE;

                while (1)
                {
                    result = search_root(board,
                                         depth,
                                         alpha,
                                         beta,
                                         &search_history,
                                         &stats,
                                         search_context,
                                         &control,
                                         (options != NULL && options->display_currmove) ? print_move_info_callback : NULL,
                                         NULL,
                                         lichess_draw_rules,
                                         excluded_root_moves,
                                         excluded_root_move_count);

                    if (control.stop || result.move == MOVE_NONE)
                    {
                        break;
                    }

                    if (result.score <= alpha && alpha > -MATE_SCORE)
                    {
                        beta = (alpha + beta) / 2;
                        alpha = alpha - delta;
                        if (alpha < -MATE_SCORE)
                            alpha = -MATE_SCORE;
                    }
                    else if (result.score >= beta && beta < MATE_SCORE)
                    {
                        // Future optimisation: consider fail-high depth reduction here
                        beta = beta + delta;
                        if (beta > MATE_SCORE)
                            beta = MATE_SCORE;
                    }
                    else
                    {
                        break;
                    }

                    delta *=1.5; // Increase the window size for the next iteration
                }
            }
            else
            {
                result = search_root(board,
                                     depth,
                                     -MATE_SCORE,
                                     MATE_SCORE,
                                     &search_history,
                                     &stats,
                                     search_context,
                                     &control,
                                     (options != NULL && options->display_currmove) ? print_move_info_callback : NULL,
                                     NULL,
                                     lichess_draw_rules,
                                     excluded_root_moves,
                                     excluded_root_move_count);
            }

            if (control.stop)
            {
                break;
            }

            if (result.move == MOVE_NONE)
            {
                break;
            }

            long long elapsed_ms = current_time_ms() - start_time_ms;
            if (elapsed_ms < 0)
            {
                elapsed_ms = 0;
            }

            print_depth_info(depth, multipv_index + 1, &result, &stats, elapsed_ms);

            if (multipv_index == 0)
            {
                depth_best_result = result;
            }

            if (excluded_root_move_count < 256)
            {
                excluded_root_moves[excluded_root_move_count++] = result.move;
            }

            if (control.allow_forced_root_move && result.forced_root_move)
            {
                break;
            }

            if (time_limited && elapsed_ms >= soft_time_limit_ms)
            {
                break;
            }
        }

        if (depth_best_result.move != MOVE_NONE)
        {
            best_result = depth_best_result;
        }

        long long elapsed_ms = current_time_ms() - start_time_ms;
        if (elapsed_ms < 0)
        {
            elapsed_ms = 0;
        }

        if (time_limited && elapsed_ms >= soft_time_limit_ms)
        {
            break;
        }
    }

    if (best_result.move == MOVE_NONE)
    {
        search_context_destroy(search_context);
        return MOVE_NONE;
    }

    search_context_destroy(search_context);
    return best_result.move;
}