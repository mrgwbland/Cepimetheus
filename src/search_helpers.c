#include "search_helpers.h"
#include "eval.h"
#include "movegen.h"
#include "movepicker.h"
#include "think.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

int futility_margin = 1509;
int rfp_margin = 969;
int rfp_max_depth = 7;
int nmp_min_depth = 2;
int nmp_base_reduction = 2;
int nmp_reduction = 2;
int nmp_depth_scale = 4;
int nmp_min_pieces = 1;
int qs_delta_margin = 3249;
int lmr_min_depth = 1;
int lmr_offset = -32;
int lmr_divisor = 189;
int lmr_move_multiplier = 234;

int lmp_base = 300;
int lmp_multiplier = 80;
int lmp_quiet_limits[11];
static bool lmp_initialised = false;

void reinit_lmp(void)
{
    double base = (double)lmp_base / 100.0;
    double mult = (double)lmp_multiplier / 100.0;
    for (int depth = 0; depth < 11; ++depth)
    {
        lmp_quiet_limits[depth] = (int)(base + mult * depth * depth);
    }
}

void init_lmp(void)
{
    if (lmp_initialised)
    {
        return;
    }
    reinit_lmp();
    lmp_initialised = true;
}

int LMR[64][256];
static bool lmr_initialised = false;

void reinit_lmr(void)
{
    double move_mult = (double)lmr_move_multiplier / 100.0;
    double div_factor = (double)lmr_divisor / 100.0;
    if (div_factor == 0.0) div_factor = 0.01;
    double offset = (double)lmr_offset / 100.0;

    for (int depth = 0; depth < 64; ++depth)
    {
        for (int moves = 0; moves < 256; ++moves)
        {
            if (depth == 0 || moves == 0)
            {
                LMR[depth][moves] = 0;
            }
            else
            {
                int r = (int)(((log(depth) * log(move_mult * moves)) / div_factor) + offset);
                LMR[depth][moves] = r > 0 ? r : 0;
            }
        }
    }
    lmr_initialised = true;
}

void init_lmr(void)
{
    if (lmr_initialised)
    {
        return;
    }
    reinit_lmr();
    init_lmp();
}

bool has_sufficient_nmp_material(const Board *board)
{
    int side = board->side;
    int start = (side == WHITE) ? WHITE_KNIGHT : BLACK_KNIGHT;
    int end = (side == WHITE) ? WHITE_KING : BLACK_KING;
    int piece_count = 0;

    for (int i = start; i < end; ++i)
    {
        U64 bb = board->pieces[i];
        while (bb)
        {
            bitboard_pop_lsb(&bb);
            piece_count++;
            if (piece_count >= nmp_min_pieces)
            {
                return true;
            }
        }
    }

    return false;
}

bool search_should_stop(SearchControl *control, long long nodes)
{
    if (control == NULL)
    {
        return false;
    }
    
    if (control->external_stop != NULL && *control->external_stop)
    {
        control->stop = true;
        return true;
    }

    if (control->stop)
    {
        return true;
    }

    if (!control->hard_time_limited)
    {
        return false;
    }
    if ((nodes & 1023) == 0)
    {
        if (current_time_ms() >= control->hard_stop_time_ms)
        {
            control->stop = true;
            return true;
        }
    }

    return false;
}

bool has_any_legal_move(Board *board, const MoveList *list)
{
    if (board == NULL || list == NULL)
    {
        return false;
    }

    for (int i = 0; i < list->count; ++i)
    {
        if (board_is_move_legal(board, list->moves[i], board->pinned_mask, board->checkers))
        {
            return true;
        }
    }

    return false;
}

int get_captured_piece_value(const Board *board, Move move)
{
    int flags = move_flags(move);
    if (flags & MOVE_FLAG_EN_PASSANT)
    {
        return 1000; // Pawn value
    }

    int target_piece = board_piece_at(board, move_to(move));
    if (target_piece >= 0)
    {
        int type = board_piece_type(target_piece);
        if (type >= 0 && type < 6)
        {
            return piece_values[type];
        }
    }

    return 0;
}

SearchContext *search_context_create(size_t hash_power)
{
    SearchContext *context = calloc(1, sizeof(*context));
    if (context == NULL)
    {
        return NULL;
    }

    for (int p = 0; p < MAX_PLY_DEPTH; ++p)
    {
        context->killer_moves[p][0] = MOVE_NONE;
        context->killer_moves[p][1] = MOVE_NONE;
    }
    for (int f = 0; f < 64; ++f)
    {
        for (int t = 0; t < 64; ++t)
        {
            context->counter_moves[f][t][0] = MOVE_NONE;
            context->counter_moves[f][t][1] = MOVE_NONE;
        }
    }

    if (!transposition_table_init(&context->table, hash_power))
    {
        free(context);
        return NULL;
    }

    return context;
}

void search_context_destroy(SearchContext *context)
{
    if (context == NULL)
    {
        return;
    }

    transposition_table_destroy(&context->table);
    free(context);
}
