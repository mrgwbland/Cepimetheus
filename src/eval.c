#include "eval.h"
#include "movegen.h"
#include <stddef.h>
#include <stdbool.h>

int piece_values[6] = {
    1000, /* Pawn */
    2970, /* Knight */
    3070, /* Bishop */
    3180, /* Rook */
    9920, /* Queen */
    0    /* King */
};

int eval_parameters[15] = {
    88, // TEMPO_BONUS
    386, // PAWN_BLOCKING_PENALTY
    10, // KNIGHT_PAWN_COUNT_PENALTY
    38, // PAWN_SHIELD_PENALTY
    133, // DOUBLED_PAWN_PENALTY
    74, // ISOLATED_PAWN_PENALTY
    75, // KNIGHT_MOBILITY_BONUS
    78, // BISHOP_MOBILITY_BONUS
    97, // ROOK_CONTROL_BONUS
    191, // ROOK_OPEN_FILE_BONUS
    2705, // ENDGAME_ROOK_BONUS
    25, // QUEEN_MOBILITY_BONUS
    4, // KING_EXPOSURE_PENALTY
    88, // KING_CORNER_DISTANCE_BONUS
    1245 // PAWNS_VS_ONE_PIECE_BONUS 
};

int passed_pawn_rank_bonus[6] = {
    0, //Rank 2
    0, //Rank 3
    67, //Rank 4
    238, //Rank 5
    637, //Rank 6
    988 //Rank 7
};

int king_ring_penalty[14] = {
    771, 771, 755, 826,
    790, 751, 919, 844,
    825, 921, 924, 1029,
    1202, 1520
}; // Mildly overfitted, but much better after addition of x ray attacks, could benefit from some sort of smoothing

/* Macros redirect the existing engine code to array*/
#define TEMPO_BONUS eval_parameters[0]
#define PAWN_BLOCKING_PENALTY eval_parameters[1]
#define KNIGHT_PAWN_COUNT_PENALTY eval_parameters[2]
#define PAWN_SHIELD_PENALTY eval_parameters[3]
#define DOUBLED_PAWN_PENALTY eval_parameters[4]
#define ISOLATED_PAWN_PENALTY eval_parameters[5]
#define KNIGHT_MOBILITY_BONUS eval_parameters[6]
#define BISHOP_MOBILITY_BONUS eval_parameters[7]
#define ROOK_CONTROL_BONUS eval_parameters[8]
#define ROOK_OPEN_FILE_BONUS eval_parameters[9]
#define ENDGAME_ROOK_BONUS eval_parameters[10]
#define QUEEN_MOBILITY_BONUS eval_parameters[11]
#define KING_EXPOSURE_PENALTY eval_parameters[12]
#define KING_CORNER_DISTANCE_BONUS eval_parameters[13]
#define PAWNS_VS_ONE_PIECE_BONUS eval_parameters[14]

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

/* File masks - one per file (A-H) */
static const U64 file_masks[8] = {
    0x0101010101010101ULL, /* A-file */
    0x0202020202020202ULL, /* B-file */
    0x0404040404040404ULL, /* C-file */
    0x0808080808080808ULL, /* D-file */
    0x1010101010101010ULL, /* E-file */
    0x2020202020202020ULL, /* F-file */
    0x4040404040404040ULL, /* G-file */
    0x8080808080808080ULL  /* H-file */
};

static int rank_of(int square) { return square >> 3; }
static int file_of(int square) { return square & 7; }

static int count_attackers_on_square(const Board *board, int square, int attacker_side, U64 all_pieces)
{
    int attackers = 0;
    int file = file_of(square);
    int rank = rank_of(square);

    if (attacker_side == WHITE)
    {
        if (file > 0 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 9))))
            ++attackers;
        if (file < 7 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 7))))
            ++attackers;
    }
    else
    {
        if (file > 0 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 7))))
            ++attackers;
        if (file < 7 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 9))))
            ++attackers;
    }

    U64 knights = (attacker_side == WHITE) ? board->pieces[WHITE_KNIGHT] : board->pieces[BLACK_KNIGHT];
    attackers += __builtin_popcountll(bitboard_knight_attacks(square) & knights);

    U64 kings = (attacker_side == WHITE) ? board->pieces[WHITE_KING] : board->pieces[BLACK_KING];
    attackers += __builtin_popcountll(bitboard_king_attacks(square) & kings);

    // --- Diagonal Sliders & Batteries ---
    U64 white_bishops_queens = board->pieces[WHITE_BISHOP] | board->pieces[WHITE_QUEEN];
    U64 black_bishops_queens = board->pieces[BLACK_BISHOP] | board->pieces[BLACK_QUEEN];
    U64 bishops_and_queens = (attacker_side == WHITE) ? white_bishops_queens : black_bishops_queens;

    U64 bishop_attacks = bitboard_bishop_attacks(square, all_pieces);
    attackers += __builtin_popcountll(bishop_attacks & bishops_and_queens);

    // X-Ray: Remove frontline diagonal attackers from occupancy to find the battery behind them
    U64 frontline_diagonals = bishop_attacks & (white_bishops_queens | black_bishops_queens);
    if (frontline_diagonals)
    {
        U64 xray_occupancy = all_pieces ^ frontline_diagonals;
        U64 xray_bishop_attacks = bitboard_bishop_attacks(square, xray_occupancy);
        attackers += __builtin_popcountll(xray_bishop_attacks & bishops_and_queens);
    }

    // --- Orthogonal Sliders & Batteries ---
    U64 white_rooks_queens = board->pieces[WHITE_ROOK] | board->pieces[WHITE_QUEEN];
    U64 black_rooks_queens = board->pieces[BLACK_ROOK] | board->pieces[BLACK_QUEEN];
    U64 rooks_and_queens = (attacker_side == WHITE) ? white_rooks_queens : black_rooks_queens;

    U64 rook_attacks = bitboard_rook_attacks(square, all_pieces);
    attackers += __builtin_popcountll(rook_attacks & rooks_and_queens);

    // X-Ray: Remove frontline orthogonal attackers from occupancy to find the battery behind them
    U64 frontline_orthogonals = rook_attacks & (white_rooks_queens | black_rooks_queens);
    if (frontline_orthogonals)
    {
        U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
        U64 xray_rook_attacks = bitboard_rook_attacks(square, xray_occupancy);
        attackers += __builtin_popcountll(xray_rook_attacks & rooks_and_queens);
    }

    return attackers;
}

static int count_king_ring_attackers(const Board *board, int king_side, U64 all_pieces)
{
    int king_square = board->king_square[king_side];
    if (king_square < 0 || king_square >= 64)
        return 0;

    int attacker_side = (king_side == WHITE) ? BLACK : WHITE;
    U64 ring = bitboard_king_attacks(king_square);
    int attackers = 0;

    U64 bb = ring;
    while (bb)
    {
        int square = bitboard_pop_lsb(&bb);
        attackers += count_attackers_on_square(board, square, attacker_side, all_pieces);
    }
    return attackers;
}

static void count_pawns_per_file(U64 pawns, int pawns_per_file[8])
{
    for (int f = 0; f < 8; ++f)
    {
        pawns_per_file[f] = __builtin_popcountll(pawns & file_masks[f]);
    }
}

static U64 mark_passed_pawns(const Board *board, int side)
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

// Evaluats MG and EG logic distinctly, without passing phase weight
static Score evaluate_piece(int piece,
                            int square,
                            U64 passed_pawns,
                            const int white_pawns_per_file[8],
                            const int black_pawns_per_file[8],
                            U64 all_pieces,
                            U64 all_pawns,
                            U64 white_central_blocked_mask,
                            U64 black_central_blocked_mask,
                            int knight_open_position_penalty)
{
    int side = board_piece_color(piece);
    int type = board_piece_type(piece);
    int file = file_of(square);
    int rank = rank_of(square);
    bool is_white = (side == WHITE);

    // Base material applies universally to both phases
    Score s = make_score(piece_values[type], piece_values[type]);

    switch (type)
    {
    case WHITE_PAWN:
    {
        int pawn_rank = is_white ? rank : (7 - rank);
        if (passed_pawns & (1ULL << square))
        {
            /* Passed pawns are further rewarded for advancement. */
            s.eg += passed_pawn_rank_bonus[pawn_rank - 1];
        }

        const int *pawns_per_file = is_white ? white_pawns_per_file : black_pawns_per_file;
        int this_file_count = pawns_per_file[file];

        if (this_file_count > 1)
        {
            /* Doubled pawn penalty: - points for each same-color pawn on the file. */
            s.mg -= DOUBLED_PAWN_PENALTY * (this_file_count - 1);
            s.eg -= DOUBLED_PAWN_PENALTY * (this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right)
        {
            /* Isolated pawn penalty: - points if no same-color pawns on adjacent files. */
            // Consider experimenting with this being proportional to the piece value in the future
            s.mg -= ISOLATED_PAWN_PENALTY;
            s.eg -= ISOLATED_PAWN_PENALTY;
        }
        break;
    }
    case WHITE_KNIGHT:
    {
        // Knights reduce in value as pawns leave the board
        s.mg -= knight_open_position_penalty;
        s.eg -= knight_open_position_penalty;

        // Score knights based on mobility
        int mobility = __builtin_popcountll(bitboard_knight_attacks(square));
        s.mg += KNIGHT_MOBILITY_BONUS * mobility;
        s.eg += KNIGHT_MOBILITY_BONUS * mobility;

        U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
        if (blocked_mask & (1ULL << square))
        {
            s.mg -= PAWN_BLOCKING_PENALTY; // Only heavily penalized in MG
        }
        break;
    }
    case WHITE_BISHOP:
    {
        /* Reward bishops with mobility through pawn occupancy only.
        This is because a bishop on g2 with a knight on f3 is still good whereas if there was a pawn on f3 it would be blocked*/
        int mobility = __builtin_popcountll(bitboard_bishop_attacks(square, all_pawns));
        s.mg += BISHOP_MOBILITY_BONUS * mobility;
        s.eg += BISHOP_MOBILITY_BONUS * mobility;

        U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
        if (blocked_mask & (1ULL << square))
        {
            s.mg -= PAWN_BLOCKING_PENALTY; // Only heavily penalized in MG
        }
        break;
    }
    case WHITE_ROOK:
    {
        /* Reward squares controlled. */
        int control = __builtin_popcountll(bitboard_rook_attacks(square, all_pieces));
        s.mg += ROOK_CONTROL_BONUS * control;

        U64 file_mask = file_masks[file];
        /* Open file bonus: + points if no pawns on the file. */
        if (__builtin_popcountll(all_pawns & file_mask) == 0)
        {
            s.mg += ROOK_OPEN_FILE_BONUS;
        }

        /* Rooks are better in the endgame. */
        s.eg += ENDGAME_ROOK_BONUS; // Only rewarded in EG
        break;
    }
    case WHITE_QUEEN:
    {
        int mobility = __builtin_popcountll(bitboard_queen_attacks(square, all_pieces));
        s.mg += QUEEN_MOBILITY_BONUS * mobility;
        s.eg += QUEEN_MOBILITY_BONUS * mobility;
        break;
    }
    case WHITE_KING:
    {
        /* In opening/middlegame, king safety is important. */
        int attacks_all = __builtin_popcountll(bitboard_queen_attacks(square, all_pieces));
        int attacks_pawns = __builtin_popcountll(bitboard_queen_attacks(square, all_pawns));

        s.mg -= KING_EXPOSURE_PENALTY * attacks_all;
        s.mg -= PAWN_SHIELD_PENALTY * attacks_pawns;

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
        s.mg -= corner_distance * KING_CORNER_DISTANCE_BONUS;
        s.eg += corner_distance * KING_CORNER_DISTANCE_BONUS;
        break;
    }
    default:
        break;
    }
    return s;
}
// I'm not counting pawns to determine endgame because you can have many pawns left in an endgame
int get_endgame_weight(const Board *board)
{
    if (board == NULL)
        return 0;

    int total_piece_value = 0;
    int initial_piece_value = 4 * piece_values[WHITE_KNIGHT] + 4 * piece_values[WHITE_BISHOP] + 4 * piece_values[WHITE_ROOK] + 2 * piece_values[WHITE_QUEEN];

    for (int i = WHITE_KNIGHT; i < WHITE_KING; i++)
    {
        total_piece_value += __builtin_popcountll(board->pieces[i]) * piece_values[i];
    }
    for (int i = BLACK_KNIGHT; i < BLACK_KING; i++)
    {
        total_piece_value += __builtin_popcountll(board->pieces[i]) * piece_values[i - 6];
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

    // Define the specific rank masks for 6th and 7th ranks
    const U64 white_67_ranks = 0x00FFFF0000000000ULL; // Rank 6 and Rank 7
    const U64 black_67_ranks = 0x0000000000FFFF00ULL; // Rank 3 and Rank 2

    // Count opponent minor/major pieces to check for the overload condition
    int white_opp_pieces = __builtin_popcountll(board->pieces[BLACK_KNIGHT] | board->pieces[BLACK_BISHOP] | board->pieces[BLACK_ROOK]);
    int black_opp_pieces = __builtin_popcountll(board->pieces[WHITE_KNIGHT] | board->pieces[WHITE_BISHOP] | board->pieces[WHITE_ROOK]);

    bool white_has_no_queen = (board->pieces[BLACK_QUEEN] == 0);
    bool black_has_no_queen = (board->pieces[WHITE_QUEEN] == 0);

    // Evaluate White's Connected Pawns vs 1 None Queen
    if (white_has_no_queen && white_opp_pieces <= 1)
    {
        U64 white_67_connected_pawns = white_passed_pawns & white_67_ranks;

        // Isolate pawns that aren't on the edges to prevent wrapping errors
        U64 non_a_file = white_67_connected_pawns & ~file_masks[0];
        U64 non_h_file = white_67_connected_pawns & ~file_masks[7];

        // Generate a bitboard of all possible neighbor squares relative to our pawns
        U64 adjacent_targets =
            (non_a_file >> 1) | (non_a_file << 7) | (non_a_file >> 9) |        // Left, Up-Left, Down-Left
            (non_h_file << 1) | (non_h_file << 9) | (non_h_file >> 7) |        // Right, Up-Right, Down-Right
            (white_67_connected_pawns << 8) | (white_67_connected_pawns >> 8); // Straight Up, Straight Down

        // If any of our connected pawns step on an adjacent target square created by another connected pawn

        if (white_67_connected_pawns & adjacent_targets)
        {
            white_score.eg += PAWNS_VS_ONE_PIECE_BONUS;
        }
    }

    // Evaluate Black's Connected Pawns vs 1 None Queen
    if (black_has_no_queen && black_opp_pieces <= 1)
    {
        U64 black_67_connected_pawns = black_passed_pawns & black_67_ranks;
        U64 non_a_file = black_67_connected_pawns & ~file_masks[0];
        U64 non_h_file = black_67_connected_pawns & ~file_masks[7];

        // Same directional lookups apply to Black because adjacency is symmetric
        U64 adjacent_targets =
            (non_a_file >> 1) | (non_a_file << 7) | (non_a_file >> 9) |
            (non_h_file << 1) | (non_h_file << 9) | (non_h_file >> 7) |
            (black_67_connected_pawns << 8) | (black_67_connected_pawns >> 8);

        if (black_67_connected_pawns & adjacent_targets)
        {
            black_score.eg += PAWNS_VS_ONE_PIECE_BONUS;
        }
    }

    int knight_open_position_penalty = KNIGHT_PAWN_COUNT_PENALTY * (16 - __builtin_popcountll(all_pawns));

    for (int piece = 0; piece < PIECE_NB; ++piece)
    {
        U64 bb = board->pieces[piece];
        while (bb)
        {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            U64 passed = (side == WHITE) ? white_passed_pawns : black_passed_pawns;

            Score value = evaluate_piece(piece, square, passed, white_pawns_per_file, black_pawns_per_file, all_pieces, all_pawns, white_central_blocked_mask, black_central_blocked_mask, knight_open_position_penalty);

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

    // Generally, King Safety is solely an MG consideration.
    // Mapped strictly to preserve your prior unscaled mathematics.
    white_score.mg -= king_ring_penalty[white_king_ring_attackers];
    white_score.eg -= king_ring_penalty[white_king_ring_attackers];
    black_score.mg -= king_ring_penalty[black_king_ring_attackers];
    black_score.eg -= king_ring_penalty[black_king_ring_attackers];

    int mg_total = white_score.mg - black_score.mg;
    int eg_total = white_score.eg - black_score.eg;

    /* Unless the position is zugzwang, having a move is often better. Zugzwang more likely in endgames */
    if (board->side == WHITE)
    {
        mg_total += TEMPO_BONUS;
        eg_total += TEMPO_BONUS;
    }
    else
    {
        mg_total -= TEMPO_BONUS;
        eg_total -= TEMPO_BONUS;
    }

    /* --- NEW: Phase Interpolation strictly performed at the very end --- */
    int phase = get_endgame_weight(board); // 0 (MG) to 1000 (EG)
    int final_score = ((1000 - phase) * mg_total + phase * eg_total) / 1000;

    return board->side == WHITE ? final_score : -final_score;
}

/* ==============================================================================
 * PYTHON BRIDGE INTERFACE (DO NOT REMOVE)
 * ============================================================================== */
int evaluate_position_with_weights(const char *fen, int *weights)
{
    // 1. Overwrite global evaluation weights in RAM
    for (int i = 0; i < 6; ++i)
    {
        piece_values[i] = weights[i];
    }
    for (int i = 0; i < 15; ++i)
    {
        eval_parameters[i] = weights[6 + i];
    }

    for (int i = 0; i < 6; ++i)
    {
        passed_pawn_rank_bonus[i] = weights[6 + 15 + i];
    }

    for (int i = 0; i < 14; ++i)
    {
        king_ring_penalty[i] = weights[6 + 15 + 6 + i];
    }
    // 2. Initialize the board architecture and parse FEN
    Board board;
    board_init(&board); // Crucial: sets up bitboard tables and clears fields

    if (!board_set_fen(&board, fen))
    {
        return 0; // Guard against corrupted input strings
    }

    // 3. Generate a pseudo-legal move-list so the engine can accurately check for terminal states
    MoveList list;
    movegen_generate_pseudo_legal(&board, &list);

    // 4. Calculate relative score from your internal function
    int relative_score = evaluate_position(&board, NULL, 0, &list, false);

    // 5. Convert perspective: Your engine evaluates relative to side-to-move.
    // Stockfish datasets use White-Centric absolute scoring.
    // If it's Black's turn, we invert the evaluation score to match Stockfish.
    int final_score = relative_score;
    if (board.side == BLACK)
    {
        final_score = -final_score;
    }

    return final_score;
}