#ifndef THINK_H
#define THINK_H

#include "movegen.h"

typedef struct SearchOptions {
    int overhead_ms;
    int multipv;
    bool lichess_draw_rules;
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
} SearchLimits;

Move think(Board *board,
           const SearchLimits *limits,
           const SearchOptions *options,
           const RepetitionHistory *history,
           volatile bool *stop_signal);

#endif
