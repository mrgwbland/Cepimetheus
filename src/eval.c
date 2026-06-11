#include "eval.h"

#include "movegen.h"
#include <stddef.h>


int piece_values[6] = {
    1000, /* Pawn */
    3090, /* Knight */
    3095, /* Bishop */
    2705, /* Rook */
    10080, /* Queen */
    0    /* King */
};

int eval_parameters[15] = {
    51, // TEMPO_BONUS
    0, // UNUSED
    15, // KNIGHT_PAWN_COUNT_PENALTY
    104, // PAWN_SHIELD_PENALTY
    137, // DOUBLED_PAWN_PENALTY
    68, // ISOLATED_PAWN_PENALTY
    62, // KNIGHT_MOBILITY_BONUS
    76, // BISHOP_MOBILITY_BONUS
    177, // ROOK_CONTROL_BONUS
    436, // ROOK_OPEN_FILE_BONUS
    2810, // ENDGAME_ROOK_BONUS
    24, // QUEEN_MOBILITY_BONUS
    22, // KING_EXPOSURE_PENALTY
    73, // KING_CORNER_DISTANCE_BONUS
    0 // UNUSED  
};
int passed_pawn_rank_bonus[6] = {
    1, //Rank 2
    1, //Rank 3
    8, //Rank 4
    174, //Rank 5
    555, //Rank 6
    893 //Rank 7
};
int king_ring_penalty[14] = {
    20, 5, 90, 138,
    171, 83, 80, 121,
    319, 475, 1656, 2627,
    1315, 2216
};

/* Macros redirect the existing engine code to array*/
#define TEMPO_BONUS                    eval_parameters[0]
#define UNUSED                         eval_parameters[1]
#define KNIGHT_PAWN_COUNT_PENALTY      eval_parameters[2]
#define PAWN_SHIELD_PENALTY            eval_parameters[3]
#define DOUBLED_PAWN_PENALTY           eval_parameters[4]
#define ISOLATED_PAWN_PENALTY          eval_parameters[5]
#define KNIGHT_MOBILITY_BONUS          eval_parameters[6]
#define BISHOP_MOBILITY_BONUS          eval_parameters[7]
#define ROOK_CONTROL_BONUS             eval_parameters[8]
#define ROOK_OPEN_FILE_BONUS           eval_parameters[9]
#define ENDGAME_ROOK_BONUS             eval_parameters[10]
#define QUEEN_MOBILITY_BONUS           eval_parameters[11]
#define KING_EXPOSURE_PENALTY          eval_parameters[12]
#define KING_CORNER_DISTANCE_BONUS     eval_parameters[13]

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

static int rank_of(int square)
{
    return square >> 3;
}

static int file_of(int square)
{
    return square & 7;
}

static int count_attackers_on_square(const Board *board, int square, int attacker_side, U64 all_pieces)
{
    int attackers = 0;

    int file = file_of(square);
    int rank = rank_of(square);

    if (attacker_side == WHITE)
    {
        if (file > 0 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 9))))
        {
            ++attackers;
        }
        if (file < 7 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 7))))
        {
            ++attackers;
        }
    }
    else
    {
        if (file > 0 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 7))))
        {
            ++attackers;
        }
        if (file < 7 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 9))))
        {
            ++attackers;
        }
    }

    U64 knights = (attacker_side == WHITE) ? board->pieces[WHITE_KNIGHT] : board->pieces[BLACK_KNIGHT];
    attackers += __builtin_popcountll(bitboard_knight_attacks(square) & knights);

    U64 kings = (attacker_side == WHITE) ? board->pieces[WHITE_KING] : board->pieces[BLACK_KING];
    attackers += __builtin_popcountll(bitboard_king_attacks(square) & kings);

    U64 bishops_and_queens = (attacker_side == WHITE)
                                 ? (board->pieces[WHITE_BISHOP] | board->pieces[WHITE_QUEEN])
                                 : (board->pieces[BLACK_BISHOP] | board->pieces[BLACK_QUEEN]);
    attackers += __builtin_popcountll(bitboard_bishop_attacks(square, all_pieces) & bishops_and_queens);

    U64 rooks_and_queens = (attacker_side == WHITE)
                               ? (board->pieces[WHITE_ROOK] | board->pieces[WHITE_QUEEN])
                               : (board->pieces[BLACK_ROOK] | board->pieces[BLACK_QUEEN]);
    attackers += __builtin_popcountll(bitboard_rook_attacks(square, all_pieces) & rooks_and_queens);

    return attackers;
}

static int count_king_ring_attackers(const Board *board, int king_side, U64 all_pieces)
{
    int king_square = board->king_square[king_side];
    if (king_square < 0 || king_square >= 64)
    {
        return 0;
    }

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
    for (int i = 0; i < 8; ++i)
    {
        pawns_per_file[i] = 0;
    }

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

static int evaluate_piece(int piece,
                          int square,
                          int endgame_weight,
                          U64 passed_pawns,
                          const int white_pawns_per_file[8],
                          const int black_pawns_per_file[8],
                          U64 all_pieces,
                          U64 all_pawns,
                          int knight_open_position_penalty)
{
    int side = board_piece_color(piece);
    int type = board_piece_type(piece);
    int file = file_of(square);
    int rank = rank_of(square);
    bool is_white = (side == WHITE);
    int piece_value = piece_values[type];

    switch (type)
    {
    case WHITE_PAWN:
    {
        int pawn_rank = is_white ? rank : (7 - rank);
        if (passed_pawns & (1ULL << square))
        {
            /* Passed pawns are further rewarded for advancement. */
            piece_value += (int)(((long long)endgame_weight * passed_pawn_rank_bonus[pawn_rank - 1]) / 1000);
        }

        const int *pawns_per_file = is_white ? white_pawns_per_file : black_pawns_per_file;
        int this_file_count = pawns_per_file[file];

        if (this_file_count > 1)
        {
            /* Doubled pawn penalty: - points for each same-color pawn on the file. */
            piece_value -= DOUBLED_PAWN_PENALTY * (this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right)
        {
            /* Isolated pawn penalty: - points if no same-color pawns on adjacent files. */
            piece_value -= ISOLATED_PAWN_PENALTY; //Consider experimenting with this being proportional to the piece value in the future
        }
        break;
    }
    case WHITE_KNIGHT:
        //Knights reduce in value as pawns leave the board
        piece_value -= knight_open_position_penalty;
        // Score knights based on mobility
        piece_value += KNIGHT_MOBILITY_BONUS * __builtin_popcountll(bitboard_knight_attacks(square));
        break;
    case WHITE_BISHOP:
    {
        /* Reward bishops with mobility through pawn occupancy only.
        This is because a bishop on g2 with a knight on f3 is still good whereas if there was a pawn on f3 it would be blocked*/
        piece_value += BISHOP_MOBILITY_BONUS * __builtin_popcountll(bitboard_bishop_attacks(square, all_pawns));
        break;
    }
    case WHITE_ROOK:
        /* Reward squares controlled. */
        piece_value += (int)(((long long)(1000 - endgame_weight) * ROOK_CONTROL_BONUS * __builtin_popcountll(bitboard_rook_attacks(square, all_pieces))) / 1000);

        U64 file_mask = file_masks[file];
        /* Open file bonus: + points if no pawns on the file. */
        if (__builtin_popcountll(all_pawns & file_mask) == 0)
        {
            piece_value += (int)(((long long)(1000 - endgame_weight) * ROOK_OPEN_FILE_BONUS) / 1000);
        }
        /* Rooks are better in the endgame. */
        piece_value += (int)(((long long)endgame_weight * ENDGAME_ROOK_BONUS) / 1000);
        break;
    case WHITE_QUEEN:
        piece_value += QUEEN_MOBILITY_BONUS * __builtin_popcountll(bitboard_queen_attacks(square, all_pieces));
        break;
    case WHITE_KING:
    {
        /* If nothing prior, it is a king. */
        /* In opening/middlegame, king safety is important. */
        piece_value -= (int)(((long long)(1000 - endgame_weight) * KING_EXPOSURE_PENALTY * __builtin_popcountll(bitboard_queen_attacks(square, all_pieces))) / 1000);
        piece_value -= (int)(((long long)(1000 - endgame_weight) * PAWN_SHIELD_PENALTY * __builtin_popcountll(bitboard_queen_attacks(square, all_pawns))) / 1000);

        /* Calculate Manhattan distance to closest corner. */
        int distance_a1 = file + rank;
        int distance_h1 = (7 - file) + rank;
        int distance_a8 = file + (7 - rank);
        int distance_h8 = (7 - file) + (7 - rank);
        int corner_distance = distance_a1;
        if (distance_h1 < corner_distance)
        {
            corner_distance = distance_h1;
        }
        if (distance_a8 < corner_distance)
        {
            corner_distance = distance_a8;
        }
        if (distance_h8 < corner_distance)
        {
            corner_distance = distance_h8;
        }

        /* In endgames favour activity; in middlegames favour safety. */
        piece_value += (int)(((long long)(2 * endgame_weight - 1000) * corner_distance * KING_CORNER_DISTANCE_BONUS) / 1000);
        break;
    }
    default:
        break;
    }
    return piece_value;
}

//I'm not counting pawns to determine endgame because you can have many pawns left in an endgame
int get_endgame_weight(const Board *board)
{
    if (board == NULL)
    {
        return 0;
    }

    int total_piece_value = 0;
    int initial_piece_value = 4*piece_values[WHITE_KNIGHT] + 4*piece_values[WHITE_BISHOP] + 4*piece_values[WHITE_ROOK] + 2*piece_values[WHITE_QUEEN];
    for (int i = WHITE_KNIGHT; i < WHITE_KING; i++)
    {
        total_piece_value += __builtin_popcountll(board->pieces[i]) * piece_values[i];
    }
    for (int i = BLACK_KNIGHT; i < BLACK_KING; i++)
    {
        total_piece_value += __builtin_popcountll(board->pieces[i]) * piece_values[i];
    }

    return 1000 - (int)(((long long)(total_piece_value + 1) * 1000) / (initial_piece_value + 1));// +1 to avoid division by zero, and the 1- to invert the ratio so that it goes from 0 (opening) to 1 (endgame)
}

EvalTerminalState eval_terminal_state(const Board *board, bool has_legal_move)
{
    if (board == NULL || has_legal_move == true)
    {
        return EVAL_TERMINAL_NONE;
    }

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
    {
        return 0;
    }

    if (board_is_draw(board, history, lichess_draw_rules))
    {
        return 0;
    }

    EvalTerminalState terminal_state = eval_terminal_state(board, list->count);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        return eval_terminal_score(terminal_state, ply);
    }

    int side_to_move = board->side;

    /* Determine if the position is an endgame.
    Endgame eval is different from opening/middlegame eval, so we need to know which phase we're in. */
    int endgame_weight = get_endgame_weight(board);

    U64 white_pawns = board->pieces[WHITE_PAWN];
    U64 black_pawns = board->pieces[BLACK_PAWN];
    U64 all_pawns = white_pawns | black_pawns;

    int white_pawns_per_file[8];
    int black_pawns_per_file[8];
    count_pawns_per_file(white_pawns, white_pawns_per_file);
    count_pawns_per_file(black_pawns, black_pawns_per_file);

    U64 white_passed_pawns = mark_passed_pawns(board, WHITE);
    U64 black_passed_pawns = mark_passed_pawns(board, BLACK);

    U64 all_pieces = board->occupancy[BOTH];

    int white_score = 0;
    int black_score = 0;

    int knight_open_position_penalty = KNIGHT_PAWN_COUNT_PENALTY * (16 - __builtin_popcountll(all_pawns));

    for (int piece = 0; piece < PIECE_NB; ++piece)
    {
        U64 bb = board->pieces[piece];
        while (bb)
        {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            U64 passed = (side == WHITE) ? white_passed_pawns : black_passed_pawns;
            int value = evaluate_piece(piece,
                                       square,
                                       endgame_weight,
                                       passed,
                                       white_pawns_per_file,
                                       black_pawns_per_file,
                                       all_pieces,
                                       all_pawns,
                                       knight_open_position_penalty);

            if (side == WHITE)
            {
                white_score += value;
            }
            else
            {
                black_score += value;
            }
        }
    }
    int tempo_bonus = 0;
    if (endgame_weight < 600)
    {

        /* Unless the position is zugzwang, having a move is often better. */
        tempo_bonus = TEMPO_BONUS;
    }
    int white_king_ring_attackers = count_king_ring_attackers(board, WHITE, all_pieces);
    int black_king_ring_attackers = count_king_ring_attackers(board, BLACK, all_pieces);

    if (white_king_ring_attackers >= 14)
    {
        white_king_ring_attackers = 13;
    }
    if (black_king_ring_attackers >= 14)
    {
        black_king_ring_attackers = 13;
    }
    white_score -= king_ring_penalty[white_king_ring_attackers];
    black_score -= king_ring_penalty[black_king_ring_attackers];

    if (side_to_move == WHITE)
    {
        return white_score - black_score + tempo_bonus;
    }

    return black_score - white_score + tempo_bonus;
}

/* ==============================================================================
 * PYTHON BRIDGE INTERFACE (DO NOT REMOVE)
 * ============================================================================== */
int evaluate_position_with_weights(const char* fen, int* weights) {
    // 1. Overwrite global evaluation weights in RAM
    for (int i = 0; i < 6; ++i) {
        piece_values[i] = weights[i];
    }
    for (int i = 0; i < 15; ++i) {
        eval_parameters[i] = weights[6 + i];
    }

    for (int i = 0; i < 6; ++i) {
        passed_pawn_rank_bonus[i] = weights[6 + 15 + i];
    }

    for (int i = 0; i < 14; ++i) {
        king_ring_penalty[i] = weights[6 + 15 + 6 + i];
    }
    // 2. Initialize the board architecture and parse FEN
    Board board;
    board_init(&board); // Crucial: sets up bitboard tables and clears fields
    
    if (!board_set_fen(&board, fen)) {
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
    if (board.side == BLACK) {
        final_score = -final_score;
    }

    return final_score;
}