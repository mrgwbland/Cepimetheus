#ifndef THINK_H
#define THINK_H

#include "movegen.h"
#include "search.h"
#include <stddef.h>


typedef struct SearchOptions {
    int overhead_ms;
    int multipv;
    int hash_power;
    bool lichess_draw_rules;
    bool display_currmove;
} SearchOptions;

typedef struct SearchLimits {
    int depth;
    int movetime_ms;
    int wtime_ms;
    int btime_ms;
    int winc_ms;
    int binc_ms;
    int movestogo;
    bool has_clock_time;
    bool infinite;
    Move search_moves[256];
    int search_move_count;
    bool has_search_moves;
} SearchLimits;

Move think(Board *board,
           const SearchLimits *limits,
           const SearchOptions *options,
           const RepetitionHistory *history,
           volatile bool *stop_signal,
           unsigned long long *out_nodes,
           SearchResult *out_result);

void get_score_string(int score, char *buffer, size_t size);
long long current_time_ms(void);

#endif
