#include "eval.h"
#include "eval_helpers.h"
#include "movegen.h"
#include <stddef.h>
#include <stdbool.h>

int piece_values_mg[6] = {
    1000, /* Pawn */
    2945, /* Knight */
    3040, /* Bishop */
    3155, /* Rook */
    9870, /* Queen */
    0    /* King */
};

int piece_values_eg[6] = {
    1000, /* Pawn */
    2945, /* Knight */
    3040, /* Bishop */
    5845, /* Rook */
    9870, /* Queen */
    0    /* King */
};

int eval_parameters_mg[13] = {
    83,  // TEMPO_BONUS
    396, // PAWN_BLOCKING_PENALTY
    10,  // KNIGHT_PAWN_COUNT_PENALTY
    40,  // PAWN_SHIELD_PENALTY
    132, // DOUBLED_PAWN_PENALTY
    78,  // ISOLATED_PAWN_PENALTY
    74,  // KNIGHT_MOBILITY_BONUS
    78,  // BISHOP_MOBILITY_BONUS
    100, // ROOK_CONTROL_BONUS
    190, // ROOK_OPEN_FILE_BONUS
    24,  // QUEEN_MOBILITY_BONUS
    1,   // KING_EXPOSURE_PENALTY
    97   // KING_CORNER_DISTANCE_BONUS
};

int eval_parameters_eg[13] = {
    83,  // TEMPO_BONUS
    396, // PAWN_BLOCKING_PENALTY
    10,  // KNIGHT_PAWN_COUNT_PENALTY
    40,  // PAWN_SHIELD_PENALTY
    132, // DOUBLED_PAWN_PENALTY
    78,  // ISOLATED_PAWN_PENALTY
    74,  // KNIGHT_MOBILITY_BONUS
    78,  // BISHOP_MOBILITY_BONUS
    100, // ROOK_CONTROL_BONUS
    190, // ROOK_OPEN_FILE_BONUS
    24,  // QUEEN_MOBILITY_BONUS
    1,   // KING_EXPOSURE_PENALTY
    97   // KING_CORNER_DISTANCE_BONUS
};

int passed_pawn_rank_bonus_mg[6] = {
    0, //Rank 2
    0, //Rank 3
    54, //Rank 4
    248, //Rank 5
    621, //Rank 6
    1003 //Rank 7
};

int passed_pawn_rank_bonus_eg[6] = {
    0, //Rank 2
    0, //Rank 3
    54, //Rank 4
    248, //Rank 5
    621, //Rank 6
    1003 //Rank 7
};

int phalanx_pawn_rank_bonus_mg[6] = {
    0, //Rank 2
    0, //Rank 3
    54, //Rank 4
    248, //Rank 5
    621, //Rank 6
    1003 //Rank 7
};

int phalanx_pawn_rank_bonus_eg[6] = {
    0, //Rank 2
    0, //Rank 3
    54, //Rank 4
    248, //Rank 5
    621, //Rank 6
    1003 //Rank 7
};

int king_ring_penalty_mg[14] = {
    0, 20, 0, 50,
    43, 44, 160, 120,
    112, 187, 176, 295,
    450, 694
};

/* Macros redirect the existing engine code to array*/
#define TEMPO_BONUS_MG eval_parameters_mg[0]
#define TEMPO_BONUS_EG eval_parameters_eg[0]
#define PAWN_BLOCKING_PENALTY_MG eval_parameters_mg[1]
#define PAWN_BLOCKING_PENALTY_EG eval_parameters_eg[1]
#define KNIGHT_PAWN_COUNT_PENALTY_MG eval_parameters_mg[2]
#define KNIGHT_PAWN_COUNT_PENALTY_EG eval_parameters_eg[2]
#define PAWN_SHIELD_PENALTY_MG eval_parameters_mg[3]
#define PAWN_SHIELD_PENALTY_EG eval_parameters_eg[3]
#define DOUBLED_PAWN_PENALTY_MG eval_parameters_mg[4]
#define DOUBLED_PAWN_PENALTY_EG eval_parameters_eg[4]
#define ISOLATED_PAWN_PENALTY_MG eval_parameters_mg[5]
#define ISOLATED_PAWN_PENALTY_EG eval_parameters_eg[5]
#define KNIGHT_MOBILITY_BONUS_MG eval_parameters_mg[6]
#define KNIGHT_MOBILITY_BONUS_EG eval_parameters_eg[6]
#define BISHOP_MOBILITY_BONUS_MG eval_parameters_mg[7]
#define BISHOP_MOBILITY_BONUS_EG eval_parameters_eg[7]
#define ROOK_CONTROL_BONUS_MG eval_parameters_mg[8]
#define ROOK_CONTROL_BONUS_EG eval_parameters_eg[8]
#define ROOK_OPEN_FILE_BONUS_MG eval_parameters_mg[9]
#define ROOK_OPEN_FILE_BONUS_EG eval_parameters_eg[9]
#define QUEEN_MOBILITY_BONUS_MG eval_parameters_mg[10]
#define QUEEN_MOBILITY_BONUS_EG eval_parameters_eg[10]
#define KING_EXPOSURE_PENALTY_MG eval_parameters_mg[11]
#define KING_EXPOSURE_PENALTY_EG eval_parameters_eg[11]
#define KING_CORNER_DISTANCE_BONUS_MG eval_parameters_mg[12]
#define KING_CORNER_DISTANCE_BONUS_EG eval_parameters_eg[12]

typedef struct
{
    int mg;
    int eg;
} Score;

static inline Score make_score(int mg, int eg)
{
    Score s = {mg, eg};
    return s;
}



// Evaluats MG and EG logic distinctly, without passing phase weight
static Score evaluate_piece(int piece,
                            int square,
                            U64 passed_pawns,
                            U64 own_pawns,
                            const int white_pawns_per_file[8],
                            const int black_pawns_per_file[8],
                            U64 all_pieces,
                            U64 all_pawns,
                            U64 white_central_blocked_mask,
                            U64 black_central_blocked_mask,
                            int knight_open_position_penalty_mg,
                            int knight_open_position_penalty_eg)
{
    int side = board_piece_color(piece);
    int type = board_piece_type(piece);
    int file = file_of(square);
    int rank = rank_of(square);
    bool is_white = (side == WHITE);

    // Base material applies MG and EG values
    Score s = make_score(piece_values_mg[type], piece_values_eg[type]);

    switch (type)
    {
    case WHITE_PAWN:
    {
        int pawn_rank = is_white ? rank : (7 - rank);
        if (passed_pawns & (1ULL << square))
        {
            /* Passed pawns are further rewarded for advancement. */
            s.mg += passed_pawn_rank_bonus_mg[pawn_rank - 1];
            s.eg += passed_pawn_rank_bonus_eg[pawn_rank - 1];
        }

        // --- Phalanx / Connected Pawn Bonus ---
        U64 connected_mask = 0;
        if (file > 0)
        {
            connected_mask |= (1ULL << (square - 1));
            if (rank > 0) connected_mask |= (1ULL << (square - 9));
            if (rank < 7) connected_mask |= (1ULL << (square + 7));
        }
        if (file < 7)
        {
            connected_mask |= (1ULL << (square + 1));
            if (rank > 0) connected_mask |= (1ULL << (square - 7));
            if (rank < 7) connected_mask |= (1ULL << (square + 9));
        }

        if (own_pawns & connected_mask)
        {
            s.mg += phalanx_pawn_rank_bonus_mg[pawn_rank - 1];
            s.eg += phalanx_pawn_rank_bonus_eg[pawn_rank - 1];
        }

        const int *pawns_per_file = is_white ? white_pawns_per_file : black_pawns_per_file;
        int this_file_count = pawns_per_file[file];

        if (this_file_count > 1)
        {
            /* Doubled pawn penalty */
            s.mg -= DOUBLED_PAWN_PENALTY_MG * (this_file_count - 1);
            s.eg -= DOUBLED_PAWN_PENALTY_EG * (this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right)
        {
            /* Isolated pawn penalty */
            s.mg -= ISOLATED_PAWN_PENALTY_MG;
            s.eg -= ISOLATED_PAWN_PENALTY_EG;
        }
        break;
    }
    case WHITE_KNIGHT:
    {
        // Knights reduce in value as pawns leave the board
        s.mg -= knight_open_position_penalty_mg;
        s.eg -= knight_open_position_penalty_eg;

        // Score knights based on mobility
        int mobility = __builtin_popcountll(bitboard_knight_attacks(square));
        s.mg += KNIGHT_MOBILITY_BONUS_MG * mobility;
        s.eg += KNIGHT_MOBILITY_BONUS_EG * mobility;

        U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
        if (blocked_mask & (1ULL << square))
        {
            s.mg -= PAWN_BLOCKING_PENALTY_MG;
            s.eg -= PAWN_BLOCKING_PENALTY_EG;
        }
        break;
    }
    case WHITE_BISHOP:
    {
        /* Reward bishops with mobility through pawn occupancy only. */
        int mobility = __builtin_popcountll(bitboard_bishop_attacks(square, all_pawns));
        s.mg += BISHOP_MOBILITY_BONUS_MG * mobility;
        s.eg += BISHOP_MOBILITY_BONUS_EG * mobility;

        U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
        if (blocked_mask & (1ULL << square))
        {
            s.mg -= PAWN_BLOCKING_PENALTY_MG;
            s.eg -= PAWN_BLOCKING_PENALTY_EG;
        }
        break;
    }
    case WHITE_ROOK:
    {
        /* Reward squares controlled. */
        int control = __builtin_popcountll(bitboard_rook_attacks(square, all_pieces));
        s.mg += ROOK_CONTROL_BONUS_MG * control;
        s.eg += ROOK_CONTROL_BONUS_EG * control;

        U64 file_mask = file_masks[file];
        /* Open file bonus: + points if no pawns on the file. */
        if (__builtin_popcountll(all_pawns & file_mask) == 0)
        {
            s.mg += ROOK_OPEN_FILE_BONUS_MG;
            s.eg += ROOK_OPEN_FILE_BONUS_EG;
        }
        break;
    }
    case WHITE_QUEEN:
    {
        int mobility = __builtin_popcountll(bitboard_queen_attacks(square, all_pieces));
        s.mg += QUEEN_MOBILITY_BONUS_MG * mobility;
        s.eg += QUEEN_MOBILITY_BONUS_EG * mobility;
        break;
    }
    case WHITE_KING:
    {
        /* In opening/middlegame, king safety is important. */
        int attacks_all = __builtin_popcountll(bitboard_queen_attacks(square, all_pieces));
        int attacks_pawns = __builtin_popcountll(bitboard_queen_attacks(square, all_pawns));

        s.mg -= KING_EXPOSURE_PENALTY_MG * attacks_all;
        s.eg -= KING_EXPOSURE_PENALTY_EG * attacks_all;
        s.mg -= PAWN_SHIELD_PENALTY_MG * attacks_pawns;
        s.eg -= PAWN_SHIELD_PENALTY_EG * attacks_pawns;

        /* Calculate Manhattan distance to closest corner. */
        int distance_a1 = file + rank;
        int distance_h1 = (7 - file) + rank;
        int distance_a8 = file + (7 - rank);
        int distance_h8 = (7 - file) + (7 - rank);

        int corner_distance = distance_a1;
        if (distance_h1 < corner_distance)
            corner_distance = distance_h1;
        if (distance_a8 < corner_distance)
            corner_distance = distance_a8;
        if (distance_h8 < corner_distance)
            corner_distance = distance_h8;

        /* In endgames favour activity; in middlegames favour safety. */
        s.mg -= corner_distance * KING_CORNER_DISTANCE_BONUS_MG;
        s.eg += corner_distance * KING_CORNER_DISTANCE_BONUS_EG; // Mallus becomes a bonus
        break;
    }
    default:
        break;
    }
    return s;
}


int evaluate_position(Board *board, const RepetitionHistory *history, int ply, const MoveList *list, bool lichess_draw_rules)
{
    if (board == NULL || list == NULL)
        return 0;
    if (board_is_draw(board, history, lichess_draw_rules))
        return 0;

    EvalTerminalState terminal_state = eval_terminal_state(board, list->count);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        return eval_terminal_score(terminal_state, ply);
    }

    Score white_score = make_score(0, 0);
    Score black_score = make_score(0, 0);

    U64 white_pawns = board->pieces[WHITE_PAWN];
    U64 black_pawns = board->pieces[BLACK_PAWN];
    U64 all_pawns = white_pawns | black_pawns;

    int white_pawns_per_file[8];
    int black_pawns_per_file[8];
    count_pawns_per_file(white_pawns, white_pawns_per_file);
    count_pawns_per_file(black_pawns, black_pawns_per_file);

    U64 central_files = file_masks[2] | file_masks[3] | file_masks[4]; // C, D, E files
    U64 white_central_blocked_mask = (white_pawns & central_files) << 8;
    U64 black_central_blocked_mask = (black_pawns & central_files) >> 8;

    U64 white_passed_pawns = mark_passed_pawns(board, WHITE);
    U64 black_passed_pawns = mark_passed_pawns(board, BLACK);

    U64 all_pieces = board->occupancy[BOTH];

    int knight_open_position_penalty_mg = KNIGHT_PAWN_COUNT_PENALTY_MG * (16 - __builtin_popcountll(all_pawns));
    int knight_open_position_penalty_eg = KNIGHT_PAWN_COUNT_PENALTY_EG * (16 - __builtin_popcountll(all_pawns));

    for (int piece = 0; piece < PIECE_NB; ++piece)
    {
        U64 bb = board->pieces[piece];
        while (bb)
        {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            U64 passed = (side == WHITE) ? white_passed_pawns : black_passed_pawns;
            U64 own_pawns = (side == WHITE) ? white_pawns : black_pawns;

            Score value = evaluate_piece(piece, square, passed, own_pawns, white_pawns_per_file, black_pawns_per_file, all_pieces, all_pawns, white_central_blocked_mask, black_central_blocked_mask, knight_open_position_penalty_mg, knight_open_position_penalty_eg);

            if (side == WHITE)
            {
                white_score.mg += value.mg;
                white_score.eg += value.eg;
            }
            else
            {
                black_score.mg += value.mg;
                black_score.eg += value.eg;
            }
        }
    }

    int white_king_ring_attackers = count_king_ring_attackers(board, WHITE, all_pieces);
    int black_king_ring_attackers = count_king_ring_attackers(board, BLACK, all_pieces);

    if (white_king_ring_attackers >= 14)
        white_king_ring_attackers = 13;
    if (black_king_ring_attackers >= 14)
        black_king_ring_attackers = 13;
    
    // King ring penalty is essentially 0 for endgames
    white_score.mg -= king_ring_penalty_mg[white_king_ring_attackers];
    black_score.mg -= king_ring_penalty_mg[black_king_ring_attackers];

    int mg_total = white_score.mg - black_score.mg;
    int eg_total = white_score.eg - black_score.eg;

    /* Unless the position is zugzwang, having a move is often better. Zugzwang more likely in endgames */
    if (board->side == WHITE)
    {
        mg_total += TEMPO_BONUS_MG;
        eg_total += TEMPO_BONUS_EG;
    }
    else
    {
        mg_total -= TEMPO_BONUS_MG;
        eg_total -= TEMPO_BONUS_EG;
    }

    /* Phase Interpolation */
    int phase = get_endgame_weight(board); // 0 (MG) to 1000 (EG)
    int final_score = ((1000 - phase) * mg_total + phase * eg_total) / 1000;

    return board->side == WHITE ? final_score : -final_score;
}

/* ==============================================================================
 * PYTHON BRIDGE INTERFACE
 * ============================================================================== */
int evaluate_position_with_weights(const char *fen, int *weights)
{
    // 1. Overwrite global evaluation weights in RAM using a unified offset
    int offset = 0;
    
    for (int i = 0; i < 6; ++i) {
        piece_values_mg[i] = weights[offset++];
    }
    for (int i = 0; i < 6; ++i) {
        piece_values_eg[i] = weights[offset++];
    }
    
    for (int i = 0; i < 13; ++i) {
        eval_parameters_mg[i] = weights[offset++];
    }
    for (int i = 0; i < 13; ++i) {
        eval_parameters_eg[i] = weights[offset++];
    }

    for (int i = 0; i < 6; ++i) {
        passed_pawn_rank_bonus_mg[i] = weights[offset++];
    }
    for (int i = 0; i < 6; ++i) {
        passed_pawn_rank_bonus_eg[i] = weights[offset++];
    }

    for (int i = 0; i < 6; ++i) {
        phalanx_pawn_rank_bonus_mg[i] = weights[offset++];
    }
    for (int i = 0; i < 6; ++i) {
        phalanx_pawn_rank_bonus_eg[i] = weights[offset++];
    }

    for (int i = 0; i < 14; ++i) {
        king_ring_penalty_mg[i] = weights[offset++];
    }

    // 2. Initialise the board architecture and parse FEN
    Board board;
    board_init(&board); 

    if (!board_set_fen(&board, fen))
    {
        return 0;
    }

    // 3. Generate a pseudo-legal move-list
    MoveList list;
    movegen_generate_pseudo_legal(&board, &list);

    // 4. Calculate relative score from your internal function
    int relative_score = evaluate_position(&board, NULL, 0, &list, false);

    // 5. Convert perspective (White-centric)
    int final_score = relative_score;
    if (board.side == BLACK)
    {
        final_score = -final_score;
    }

    return final_score;
}