#include "eval.h"
#include "eval_helpers.h"
#include <stddef.h>
#include <stdbool.h>

extern int piece_values_mg[6];

/* File masks - one per file (A-H) */
const U64 file_masks[8] = {
    0x0101010101010101ULL, /* A-file */
    0x0202020202020202ULL, /* B-file */
    0x0404040404040404ULL, /* C-file */
    0x0808080808080808ULL, /* D-file */
    0x1010101010101010ULL, /* E-file */
    0x2020202020202020ULL, /* F-file */
    0x4040404040404040ULL, /* G-file */
    0x8080808080808080ULL  /* H-file */
};



void count_pawns_per_file(U64 pawns, int pawns_per_file[8])
{
    for (int f = 0; f < 8; ++f)
    {
        pawns_per_file[f] = __builtin_popcountll(pawns & file_masks[f]);
    }
}

U64 mark_passed_pawns(const Board *board, int side)
{
    U64 own_pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    U64 enemy_pawns = board->pieces[side == WHITE ? BLACK_PAWN : WHITE_PAWN];
    U64 passed_pawns = 0;

    U64 bb = own_pawns;
    while (bb)
    {
        int square = bitboard_pop_lsb(&bb);

        /* Check if any enemy pawns exist in the precomputed block mask. */
        if ((enemy_pawns & bitboard_passed_pawn_mask(side, square)) == 0)
        {
            passed_pawns |= 1ULL << square;
        }
    }
    return passed_pawns;
}

// I'm not counting pawns to determine endgame because you can have many pawns left in an endgame
int get_endgame_weight(const Board *board)
{
    if (board == NULL)
        return 0;

    int total_piece_value = 0;
    int initial_piece_value = 4 * piece_values_mg[WHITE_KNIGHT] + 4 * piece_values_mg[WHITE_BISHOP] + 4 * piece_values_mg[WHITE_ROOK] + 2 * piece_values_mg[WHITE_QUEEN];

    for (int i = WHITE_KNIGHT; i < WHITE_KING; i++)
    {
        total_piece_value += __builtin_popcountll(board->pieces[i]) * piece_values_mg[i];
    }
    for (int i = BLACK_KNIGHT; i < BLACK_KING; i++)
    {
        total_piece_value += __builtin_popcountll(board->pieces[i]) * piece_values_mg[i - 6];
    }

    int raw_weight = 1000 - (int)(((long long)(total_piece_value + 1) * 1000) / (initial_piece_value + 1));

    if (raw_weight < 0)
        return 0;
    if (raw_weight > 1000)
        return 1000;

    return raw_weight; // 0 = Pure MG, 1000 = Pure EG
}

EvalTerminalState eval_terminal_state(const Board *board, bool has_legal_move)
{
    if (board == NULL || has_legal_move == true)
        return EVAL_TERMINAL_NONE;
    return board_is_in_check(board, board->side) ? EVAL_TERMINAL_CHECKMATE : EVAL_TERMINAL_STALEMATE;
}

int eval_terminal_score(EvalTerminalState terminal_state, int ply)
{
    switch (terminal_state)
    {
    case EVAL_TERMINAL_CHECKMATE:
        return -MATE_SCORE + ply;
    case EVAL_TERMINAL_STALEMATE:
    case EVAL_TERMINAL_NONE:
    default:
        return 0;
    }
}
