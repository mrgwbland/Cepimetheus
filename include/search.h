#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "tt.h"
#include <stddef.h>
#include <stdlib.h>

#define MAX_PV_MOVES 128
#define MAX_QUIET_TRACKED 256
#define STACK_OFFSET 6 // Offset to allow negative checks at root

#define PAWN_CORR_HIST_SIZE (1 << 14)
//When the entry is small, the decay is near zero, so  the entry grows fast with delta
//When the entry grows large enough, the decay term equals the scaling, preventing growth
static inline void gravity_update_entry(int16_t *entry, int delta, int scale, int gravity)
{
    int grav = gravity > 0 ? gravity : 1;
    int val = *entry;
    val += scale * delta - val * abs(delta) / grav;
    if (val > 32767) val = 32767;
    else if (val < -32767) val = -32767;
    *entry = (int16_t)val;
}

typedef int16_t ContHistEntry[12][64];

// Information for each ply in the search stack
typedef struct SearchStack {
    int ply;
    int static_eval;
    Move move;
    Move excluded_move;
    ContHistEntry *cont_entry;
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
    // For a ply the move that caused the most recent beta cutoff, and the second most recent move that caused a beta cutoff
    Move killer_moves[MAX_PLY_DEPTH][2];
    // Functions like a per move killer instead of a per ply killer, stores two moves for each piece/to pair
    Move counter_moves[12][64][2]; // [piece][to][slot]
    // Scores how good a move is based on performance across the tree
    int16_t hh_table[12][64]; // [piece][to]
    // Continuation history table: [prev_piece][prev_to][curr_piece][curr_to]
    int16_t cont_hist[12][64][12][64];
    // Ordered list of root moves for root ordering
    RootMoveTable root_moves;
    // Pawn structure correction history table: [side_to_move][pawn_hash % PAWN_CORR_HIST_SIZE]
    int16_t pawn_corr_hist[2][PAWN_CORR_HIST_SIZE];
    int nmp_min_ply;
};

typedef void (*SearchMoveInfoCallback)(int depth,
                                       int move_number,
                                       Move move,
                                       void *user_data);

void init_lmr(void);
void reinit_lmr(void);
void init_lmp(void);
void reinit_lmp(void);

#ifdef SPSA_TUNING
#define TUNE_VAR int
#else
#define TUNE_VAR const int
#endif

// Search parameters 
extern TUNE_VAR futility_margin;
extern TUNE_VAR futility_max_depth;
extern TUNE_VAR rfp_margin;
extern TUNE_VAR rfp_improving_margin;
extern TUNE_VAR rfp_max_depth;
extern TUNE_VAR nmp_min_depth;
extern TUNE_VAR nmp_verification_min_depth;
extern TUNE_VAR nmp_base_reduction;
extern TUNE_VAR nmp_reduction;
extern TUNE_VAR nmp_depth_scale;
extern TUNE_VAR nmp_min_pieces;
extern TUNE_VAR qs_delta_margin;
extern TUNE_VAR lmr_min_depth;
extern TUNE_VAR lmr_offset;
extern TUNE_VAR lmr_divisor;
extern TUNE_VAR lmr_move_multiplier;
extern TUNE_VAR lmp_base;
extern TUNE_VAR lmp_multiplier;
extern TUNE_VAR lmp_max_depth;
extern int lmp_quiet_limits[33];
extern TUNE_VAR se_min_depth;
extern TUNE_VAR se_depth_margin;
extern TUNE_VAR se_margin;
extern TUNE_VAR se_depth_scale;
extern TUNE_VAR se_negative_extension;
extern TUNE_VAR iir_min_depth;
extern TUNE_VAR iir_reduction;
extern TUNE_VAR check_extension;

// Move picker & history parameters 
extern TUNE_VAR history_bonus_cap;
extern TUNE_VAR history_gravity;
extern TUNE_VAR history_scale;
extern TUNE_VAR order_knight_promo;
extern TUNE_VAR order_bishop_promo;
extern TUNE_VAR order_rook_promo;
extern TUNE_VAR order_queen_promo;
extern TUNE_VAR order_victim_mult;
extern TUNE_VAR order_castle;

// Continuation history parameters 
extern TUNE_VAR conthist_bonus_cap;
extern TUNE_VAR conthist_1_gravity;
extern TUNE_VAR conthist_1_scale;
extern TUNE_VAR conthist_2_gravity;
extern TUNE_VAR conthist_2_scale;
extern TUNE_VAR conthist_4_gravity;
extern TUNE_VAR conthist_4_scale;

// Correction history parameters 
extern TUNE_VAR pawn_corr_gravity;
extern TUNE_VAR pawn_corr_max_depth;
extern TUNE_VAR pawn_corr_max_bonus;

int pawn_corr_score(const SearchContext *context, const Board *board);
void pawn_corr_update(SearchContext *context, const Board *board, int depth, int diff);

// Aspiration window parameters 
extern TUNE_VAR asp_min_depth;
extern TUNE_VAR asp_initial_delta;
extern TUNE_VAR asp_growth_factor;

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
