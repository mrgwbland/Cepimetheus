#ifndef MOVEPICKER_H
#define MOVEPICKER_H

#include "board.h"
#include "move.h"
#include "movegen.h"
#include "search.h"

#define MAX_ORDERED_MOVES 256

enum {
    STAGE_TT,
    STAGE_GENERATE_NOISY,
    STAGE_PLAY_NOISY,
    STAGE_KILLER_1,
    STAGE_KILLER_2,
    STAGE_GENERATE_QUIET,
    STAGE_PLAY_QUIET,
    STAGE_DONE
};

typedef struct {
    MoveList all_moves;
    bool used[MAX_ORDERED_MOVES];
    Move moves[MAX_ORDERED_MOVES];
    int scores[MAX_ORDERED_MOVES];
    int count;
    int current_idx;
    int stage;
    Move tt_move;
    Move killer1;
    Move killer2;
    const Move *excluded_moves;
    int excluded_move_count;
    bool in_qsearch;
    bool in_check;
    Board *board;
    const SearchContext *context;
    int ply;
} MovePicker;

void movepicker_init(MovePicker *mp,
                     Board *board,
                     const SearchContext *context,
                     int ply,
                     Move tt_move,
                     const Move *excluded_moves,
                     int excluded_move_count,
                     bool in_qsearch);

Move movepicker_next_move(MovePicker *mp);
int estimate_move_score(Board *board, Move move, const SearchContext *context, int ply);
void update_history_entry(int16_t *entry, int delta);
int history_bonus(int depth);

#endif
