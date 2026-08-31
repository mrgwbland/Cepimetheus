#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "tt.h"
#include <stddef.h>

#define MAX_PV_MOVES 128
#define MAX_QUIET_TRACKED 256
#define STACK_OFFSET 4 // Offset to allow negative checks at root

// Information for each ply in the search stack
typedef struct SearchStack {
    int ply;
    int static_eval;
    Move move;
} SearchStack;

typedef struct {
    int score;
    Move move;
    Move pv[MAX_PV_MOVES];
    int pv_length;
} SearchResult;

typedef struct {
    unsigned long long nodes;
    int seldepth;
    int hashfull;
} SearchStats;

typedef struct {
    bool hard_time_limited;
    long long hard_stop_time_ms;
    bool stop;
    volatile bool *external_stop;
} SearchControl;

typedef struct {
    Move move;
    int score;
    int previous_score;
    unsigned long long nodes;
    Move pv[MAX_PV_MOVES];
    int pv_length;
} RootMoveEntry;

typedef struct {
    RootMoveEntry entries[MAX_QUIET_TRACKED];
    int count;
} RootMoveTable;

typedef struct SearchContext SearchContext;

struct SearchContext
{
    TranspositionTable table;
    Move killer_moves[MAX_PLY_DEPTH][2];
    Move counter_moves[64][64][2];
    int16_t hh_table[2][64][64];
    RootMoveTable root_moves;
};

typedef void (*SearchMoveInfoCallback)(int depth,
                                       int move_number,
                                       Move move,
                                       int score,
                                       void *user_data);

void init_lmr(void);
void reinit_lmr(void);
void init_lmp(void);
void reinit_lmp(void);

/* Search parameters */
extern int futility_margin;
extern int rfp_margin;
extern int rfp_max_depth;
extern int nmp_min_depth;
extern int nmp_base_reduction;
extern int nmp_reduction;
extern int nmp_depth_scale;
extern int nmp_min_pieces;
extern int qs_delta_margin;
extern int lmr_min_depth;
extern int lmr_offset;
extern int lmr_divisor;
extern int lmr_move_multiplier;
extern int lmp_base;
extern int lmp_multiplier;
extern int lmp_quiet_limits[11];

/* Move picker & history parameters */
extern int history_bonus_cap;
extern int history_gravity;
extern int history_scale;
extern int order_knight_promo;
extern int order_bishop_promo;
extern int order_rook_promo;
extern int order_queen_promo;
extern int order_victim_mult;
extern int order_killer1;
extern int order_killer2;
extern int order_castle;

/* Aspiration window parameters */
extern int asp_min_depth;
extern int asp_initial_delta;
extern int asp_growth_factor;

SearchContext *search_context_create(size_t hash_power);
void search_context_destroy(SearchContext *context);
void search_context_clear(SearchContext *context);
void search_context_reset_search(SearchContext *context);
bool search_context_resize(SearchContext *context, size_t hash_power);

void root_moves_init(RootMoveTable *table, Board *board, const Move *search_moves, int search_move_count, const SearchContext *context);
void root_moves_seed_iteration(RootMoveTable *table);
void root_moves_sort(RootMoveTable *table, int start_index);

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
                         int excluded_move_count,
                         const Move *search_moves,
                         int search_move_count);

#endif
