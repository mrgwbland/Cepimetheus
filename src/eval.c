#include "eval.h"

#include "movegen.h"
#include <stddef.h>

#define MATE_SCORE 100000.0f

const int piece_values[6] = {
    100, /* Pawn */
    300, /* Knight */
    350, /* Bishop */
    450, /* Rook */
    950, /* Queen */
    0    /* King */
};

enum EvalParams
{
    ENDGAME_THRESHOLD = 1500,
    TEMPO_BONUS = 20,
    CENTRE_CONTROL_BONUS = 3,
    KING_RING_PENALTY = 2,
    ENDGAME_PAWN_ADVANCEMENT_BONUS = 2,
    PASSED_PAWN_BONUS = 4,
    DOUBLED_PAWN_PENALTY = 5,
    ISOLATED_PAWN_PENALTY = 10,
    KNIGHT_MOBILITY_BONUS = 3,
    BISHOP_MOBILITY_BONUS = 1,
    ROOK_CONTROL_BONUS = 1,
    ROOK_OPEN_FILE_BONUS = 50,
    ENDGAME_ROOK_BONUS = 100,
    QUEEN_MOBILITY_BONUS = 1,
    KING_CORNER_DISTANCE_BONUS = 3
};
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

/* Count the number of set bits in a 64-bit bitboard. */
/* Brian Kernighan’s Algorithm */
static int popcount_u64(U64 bb)
{
    int count = 0;

    /* Repeatedly remove the least-significant set bit until empty. */
    while (bb)
    {
        bb &= (bb - 1); // Don't use bitboard_pop_lsb here since we just want to count, not get the index of the lsb.
        count++;
    }

    return count;
}

static int rank_of(int square)
{
    return square >> 3;
}

static int file_of(int square)
{
    return square & 7;
}

static int count_attackers_on_square(const Board *board, int square, int attacker_side)
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
    attackers += popcount_u64(bitboard_knight_attacks(square) & knights);

    U64 kings = (attacker_side == WHITE) ? board->pieces[WHITE_KING] : board->pieces[BLACK_KING];
    attackers += popcount_u64(bitboard_king_attacks(square) & kings);

    U64 bishops_and_queens = (attacker_side == WHITE)
                                 ? (board->pieces[WHITE_BISHOP] | board->pieces[WHITE_QUEEN])
                                 : (board->pieces[BLACK_BISHOP] | board->pieces[BLACK_QUEEN]);
    attackers += popcount_u64(bitboard_bishop_attacks(square, board->occupancy[BOTH]) & bishops_and_queens);

    U64 rooks_and_queens = (attacker_side == WHITE)
                               ? (board->pieces[WHITE_ROOK] | board->pieces[WHITE_QUEEN])
                               : (board->pieces[BLACK_ROOK] | board->pieces[BLACK_QUEEN]);
    attackers += popcount_u64(bitboard_rook_attacks(square, board->occupancy[BOTH]) & rooks_and_queens);

    return attackers;
}

static int count_king_ring_attackers(const Board *board, int king_side)
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
        attackers += count_attackers_on_square(board, square, attacker_side);
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
        pawns_per_file[f] = popcount_u64(pawns & file_masks[f]);
    }
}

/* Get mask for all ranks ahead of given rank (for white pawns: rank+1 to 7) */
static U64 get_ranks_ahead_white(int rank)
{
    if (rank >= 7)
    {
        return 0;
    }

    return 0xFFFFFFFFFFFFFFFFULL << ((rank + 1) * 8);
}

/* Get mask for all ranks ahead of given rank (for black pawns: 0 to rank-1) */
static U64 get_ranks_ahead_black(int rank)
{
    if (rank <= 0)
    {
        return 0;
    }

    return (1ULL << (rank * 8)) - 1;
}

static void mark_passed_pawns(const Board *board, int side, bool passed_pawns[64])
{
    for (int i = 0; i < 64; ++i)
    {
        passed_pawns[i] = false;
    }

    U64 own_pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    U64 enemy_pawns = board->pieces[side == WHITE ? BLACK_PAWN : WHITE_PAWN];

    U64 bb = own_pawns;
    while (bb)
    {
        int square = bitboard_pop_lsb(&bb);
        int file = file_of(square);
        int rank = rank_of(square);

        /* Create mask for current file and adjacent files. */
        U64 check_files = 0;
        if (file > 0)
        {
            check_files |= file_masks[file - 1];
        }
        check_files |= file_masks[file];
        if (file < 7)
        {
            check_files |= file_masks[file + 1];
        }

        /* Get mask for ranks ahead of this pawn. */
        U64 ranks_ahead = (side == WHITE) ? get_ranks_ahead_white(rank) : get_ranks_ahead_black(rank);

        /* Check if any enemy pawns exist in the check region. */
        if ((enemy_pawns & check_files & ranks_ahead) == 0)
        {
            passed_pawns[square] = true;
        }
    }
}

static float evaluate_piece(const Board *board,
                            int piece,
                            int square,
                            bool endgame,
                            const bool passed_pawns[64],
                            const int white_pawns_per_file[8],
                            const int black_pawns_per_file[8])
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
        if (endgame)
        {
            /* Reward advanced pawns in the endgame. */
            piece_value += ENDGAME_PAWN_ADVANCEMENT_BONUS * (float)pawn_rank;
            if (passed_pawns[square])
            {
                /* Passed pawns are further rewarded for advancement. */
                piece_value += PASSED_PAWN_BONUS * (float)pawn_rank;
            }
        }

        const int *pawns_per_file = is_white ? white_pawns_per_file : black_pawns_per_file;
        int this_file_count = pawns_per_file[file];

        if (this_file_count > 1)
        {
            /* Doubled pawn penalty: - points for each same-color pawn on the file. */
            piece_value -= DOUBLED_PAWN_PENALTY * (float)(this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right)
        {
            /* Isolated pawn penalty: - points if no same-color pawns on adjacent files. */
            piece_value -= ISOLATED_PAWN_PENALTY;
        }
        break;
    }
    case WHITE_KNIGHT:
        /* Knights on the rim are grim. */
        piece_value += KNIGHT_MOBILITY_BONUS * (float)popcount_u64(bitboard_knight_attacks(square));
        break;
    case WHITE_BISHOP:
    {
        /* Reward bishops with mobility through pawn occupancy only.
        This is because a bishop on g2 with a knight on f3 is still good whereas if there was a pawn on f3 it would be blocked*/
        U64 pawn_occupancy = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
        piece_value += BISHOP_MOBILITY_BONUS * (float)popcount_u64(bitboard_bishop_attacks(square, pawn_occupancy));
        break;
    }
    case WHITE_ROOK:
        if (!endgame)
        {
            /* Reward squares controlled. */
            piece_value += ROOK_CONTROL_BONUS * (float)popcount_u64(bitboard_rook_attacks(square, board->occupancy[BOTH]));

            U64 all_pawns = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
            U64 file_mask = file_masks[file];
            /* Open file bonus: + points if no pawns on the file. */
            if (popcount_u64(all_pawns & file_mask) == 0)
            {
                piece_value += ROOK_OPEN_FILE_BONUS;
            }
        }
        else
        {
            /* Rooks are better in the endgame. */
            piece_value += ENDGAME_ROOK_BONUS;
        }
        break;
    case WHITE_QUEEN:
        piece_value += QUEEN_MOBILITY_BONUS * (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
        break;
    case WHITE_KING:
    {
        /* If nothing prior, it is a king. */
        if (!endgame)
        {
            /* In opening/middlegame, king safety is important. */
            piece_value -= (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
        }

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

        /* In endgames favor activity; in middlegames favor safety. */
        if (endgame)
        {
            piece_value += (float)(corner_distance * KING_CORNER_DISTANCE_BONUS);
        }
        else
        {
            piece_value -= (float)(corner_distance * KING_CORNER_DISTANCE_BONUS);
        }
        break;
    }
    default:
        break;
    }
    return piece_value;
}

bool eval_is_endgame_position(const Board *board)
{
    if (board == NULL)
    {
        return false;
    }

    int total_piece_value = 0;
    for (int i = 0; i < WHITE_KING; i++)
    {
        total_piece_value += popcount_u64(board->pieces[i]) * piece_values[i];
    }

    return total_piece_value <= ENDGAME_THRESHOLD;
}

EvalTerminalState eval_terminal_state(const Board *board, int legal_move_count)
{
    if (board == NULL || legal_move_count > 0)
    {
        return EVAL_TERMINAL_NONE;
    }

    return board_is_in_check(board, board->side) ? EVAL_TERMINAL_CHECKMATE : EVAL_TERMINAL_STALEMATE;
}

float eval_terminal_score(EvalTerminalState terminal_state, int ply)
{
    switch (terminal_state)
    {
    case EVAL_TERMINAL_CHECKMATE:
        return -MATE_SCORE + (float)ply;
    case EVAL_TERMINAL_STALEMATE:
    case EVAL_TERMINAL_NONE:
    default:
        return 0.0f;
    }
}

float evaluate_position(Board *board, const RepetitionHistory *history, int ply)
{
    if (board == NULL)
    {
        return 0.0f;
    }

    if (board_is_draw(board, history))
    {
        return 0.0f;
    }

    MoveList list;
    movegen_generate_legal(board, &list);

    EvalTerminalState terminal_state = eval_terminal_state(board, list.count);
    if (terminal_state != EVAL_TERMINAL_NONE)
    {
        return eval_terminal_score(terminal_state, ply);
    }

    int side_to_move = board->side;

    /* Determine if the position is an endgame.
    Endgame eval is different from opening/middlegame eval, so we need to know which phase we're in. */
    bool endgame = eval_is_endgame_position(board);

    int white_pawns_per_file[8];
    int black_pawns_per_file[8];
    count_pawns_per_file(board->pieces[WHITE_PAWN], white_pawns_per_file);
    count_pawns_per_file(board->pieces[BLACK_PAWN], black_pawns_per_file);

    bool white_passed_pawns[64];
    bool black_passed_pawns[64];
    mark_passed_pawns(board, WHITE, white_passed_pawns);
    mark_passed_pawns(board, BLACK, black_passed_pawns);

    float white_score = 0.0f;
    float black_score = 0.0f;

    for (int piece = 0; piece < PIECE_NB; ++piece)
    {
        U64 bb = board->pieces[piece];
        while (bb)
        {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            const bool *passed = (side == WHITE) ? white_passed_pawns : black_passed_pawns;
            float value = evaluate_piece(board,
                                         piece,
                                         square,
                                         endgame,
                                         passed,
                                         white_pawns_per_file,
                                         black_pawns_per_file);

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
    float tempo_bonus = 0.0f;
    if (!endgame)
    {

        /* Unless the position is zugzwang, having a move is often better. */
        tempo_bonus = TEMPO_BONUS;
        /* Centre control. not a big deal in endgames */
        static const int center_squares[4] = {27, 28, 35, 36}; /* d4, e4, d5, e5 */
        for (int i = 0; i < 4; ++i)
        {
            int square = center_squares[i];
            white_score += CENTRE_CONTROL_BONUS * (float)count_attackers_on_square(board, square, WHITE);
            black_score += CENTRE_CONTROL_BONUS * (float)count_attackers_on_square(board, square, BLACK);
        }
    }

    white_score -= KING_RING_PENALTY * (float)count_king_ring_attackers(board, WHITE);
    black_score -= KING_RING_PENALTY * (float)count_king_ring_attackers(board, BLACK);

    if (side_to_move == WHITE)
    {
        return white_score - black_score + tempo_bonus;
    }

    return black_score - white_score + tempo_bonus;
}
