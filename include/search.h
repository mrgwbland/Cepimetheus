#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include <stddef.h>

#define MAX_PV_MOVES 128

typedef struct {
    int score;
    Move move;
    Move pv[MAX_PV_MOVES];
    int pv_length;
    bool forced_root_move;
} SearchResult;

typedef struct {
    unsigned long long nodes;
    int seldepth;
    int hashfull;
} SearchStats;

typedef struct {
    bool hard_time_limited;
    long long hard_stop_time_ms;
    bool allow_forced_root_move;
    bool stop;
    volatile bool *external_stop;
} SearchControl;

typedef struct SearchContext SearchContext;

typedef void (*SearchMoveInfoCallback)(int depth,
                                       int move_number,
                                       Move move,
                                       int score,
                                       void *user_data);

SearchContext *search_context_create(size_t hash_power);
void search_context_destroy(SearchContext *context);

SearchResult search_root(Board *board,
                         int depth,
                         int alpha,
                         int beta,
                         RepetitionHistory *history,
                         SearchStats *stats,
                         SearchContext *context,
                         SearchControl *control,
                         SearchMoveInfoCallback on_move_info,
                         void *user_data,
                         bool lichess_draw_rules,
                         const Move *excluded_moves,
                         int excluded_move_count);

#endif
