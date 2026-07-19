#include "eval.h"
#include "eval_helpers.h"
#include "movegen.h"
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

int piece_values_mg[6] = {
    1000, 2370, 2495, 3255, 10800, 0
};

int piece_values_eg[6] = {
    1000, 3945, 3345, 5855, 8890, 0
};

int eval_parameters_mg[18] = {
    123, 326, 0, 28, 138, 14, 65, 81, 67, 199, 15, 6, 111, 98, 423, 97, 0, 19
};

int eval_parameters_eg[18] = {
    85, 0, 100, 21, 125, 88, 101, 58, 21, 0, 44, 0, 92, 67, 959, 40, 41, 1087
};

int passed_pawn_rank_bonus_mg[6] = {
    0, 0, 80, 412, 930, 1502
};

int passed_pawn_rank_bonus_eg[6] = {
    0, 0, 108, 300, 546, 959
};

int phalanx_pawn_rank_bonus_mg[6] = {
    0, 17, 82, 200, 527, 1218
};

int phalanx_pawn_rank_bonus_eg[6] = {
    0, 0, 0, 9, 293, 238
};

int piece_attack_weights_mg[5] = {
    11, 75, 27, 31, 45
};

int piece_attack_weights_eg[5] = {
    0, 1, 2, 1, 13
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
#define HANGING_PIECE_STM_PENALTY_MG eval_parameters_mg[13]
#define HANGING_PIECE_STM_PENALTY_EG eval_parameters_eg[13]
#define HANGING_PIECE_NSTM_PENALTY_MG eval_parameters_mg[14]
#define HANGING_PIECE_NSTM_PENALTY_EG eval_parameters_eg[14]
#define PASSED_PAWN_FRIENDLY_KING_PROXIMITY_MG eval_parameters_mg[15]
#define PASSED_PAWN_FRIENDLY_KING_PROXIMITY_EG eval_parameters_eg[15]
#define PASSED_PAWN_ENEMY_KING_PROXIMITY_MG eval_parameters_mg[16]
#define PASSED_PAWN_ENEMY_KING_PROXIMITY_EG eval_parameters_eg[16]
#define BISHOP_PAIR_BONUS_MG eval_parameters_mg[17]
#define BISHOP_PAIR_BONUS_EG eval_parameters_eg[17]

static inline int manhattan_distance(int sq1, int sq2)
{
    return abs(file_of(sq1) - file_of(sq2)) + abs(rank_of(sq1) - rank_of(sq2));
}

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



typedef struct
{
    U64 key;
    int mg;
    int eg;
} PawnEntry;

#define PAWN_TABLE_SIZE 32768
static PawnEntry pawn_table[PAWN_TABLE_SIZE];

static inline U64 pawn_mix_u64(U64 value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

static inline U64 pawn_hash_combine(U64 hash, U64 value) {
    hash ^= pawn_mix_u64(value + 0x9e3779b97f4a7c15ULL);
    hash *= 1099511628211ULL;
    return hash;
}

static inline U64 board_pawn_key(U64 white_pawns, U64 black_pawns) {
    U64 key = 1469598103934665603ULL;
    key = pawn_hash_combine(key, white_pawns);
    key = pawn_hash_combine(key, black_pawns);
    return key;
}

static int king_corner_pst[64];
static bool eval_initialized = false;

void init_eval(void)
{
    for (int sq = 0; sq < 64; ++sq)
    {
        int distance_a1 = manhattan_distance(sq, 0);
        int distance_h1 = manhattan_distance(sq, 7);
        int distance_a8 = manhattan_distance(sq, 56);
        int distance_h8 = manhattan_distance(sq, 63);

        int corner_distance = distance_a1;
        if (distance_h1 < corner_distance)
            corner_distance = distance_h1;
        if (distance_a8 < corner_distance)
            corner_distance = distance_a8;
        if (distance_h8 < corner_distance)
            corner_distance = distance_h8;

        king_corner_pst[sq] = corner_distance;
    }
    memset(pawn_table, 0, sizeof(pawn_table));
    eval_initialized = true;
}

// Evaluats MG and EG logic distinctly, without passing phase weight
static Score evaluate_piece(const Board *board,
                            int piece,
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
                            int knight_open_position_penalty_eg,
                            U64 enemy_king_ring,
                            int *king_ring_attackers_mg,
                            int *king_ring_attackers_eg)
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

            // King proximity adjustments using Manhattan distance
            int friendly_king = board->king_square[side];
            int enemy_king = board->king_square[!side];

            int friendly_dist = manhattan_distance(square, friendly_king);
            int enemy_dist = manhattan_distance(square, enemy_king);

            s.mg -= PASSED_PAWN_FRIENDLY_KING_PROXIMITY_MG * friendly_dist;
            s.eg -= PASSED_PAWN_FRIENDLY_KING_PROXIMITY_EG * friendly_dist;

            s.mg += PASSED_PAWN_ENEMY_KING_PROXIMITY_MG * enemy_dist;
            s.eg += PASSED_PAWN_ENEMY_KING_PROXIMITY_EG * enemy_dist;
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
        U64 attacks = bitboard_knight_attacks(square);
        int mobility = __builtin_popcountll(attacks);
        s.mg += KNIGHT_MOBILITY_BONUS_MG * mobility;
        s.eg += KNIGHT_MOBILITY_BONUS_EG * mobility;

        U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
        if (blocked_mask & (1ULL << square))
        {
            s.mg -= PAWN_BLOCKING_PENALTY_MG;
            s.eg -= PAWN_BLOCKING_PENALTY_EG;
        }

        if (king_ring_attackers_mg && king_ring_attackers_eg) {
            int attacks_count = __builtin_popcountll(attacks & enemy_king_ring);
            *king_ring_attackers_mg += piece_attack_weights_mg[type] * attacks_count;
            *king_ring_attackers_eg += piece_attack_weights_eg[type] * attacks_count;
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

        if (king_ring_attackers_mg && king_ring_attackers_eg) {
            U64 attacks = bitboard_bishop_attacks(square, all_pieces);
            int direct_count = __builtin_popcountll(attacks & enemy_king_ring);
            *king_ring_attackers_mg += piece_attack_weights_mg[type] * direct_count;
            *king_ring_attackers_eg += piece_attack_weights_eg[type] * direct_count;

            // X-ray attacks
            U64 own_bishops_queens = board->pieces[side == WHITE ? WHITE_BISHOP : BLACK_BISHOP] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_bishops_queens = board->pieces[side == WHITE ? BLACK_BISHOP : WHITE_BISHOP] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_diagonals = attacks & (own_bishops_queens | enemy_bishops_queens);
            if (frontline_diagonals) {
                U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                U64 xray_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                int xray_count = __builtin_popcountll(xray_attacks & enemy_king_ring);
                *king_ring_attackers_mg += piece_attack_weights_mg[type] * xray_count;
                *king_ring_attackers_eg += piece_attack_weights_eg[type] * xray_count;
            }
        }
        break;
    }
    case WHITE_ROOK:
    {
        /* Reward squares controlled. */
        U64 attacks = bitboard_rook_attacks(square, all_pieces);
        int control = __builtin_popcountll(attacks);
        s.mg += ROOK_CONTROL_BONUS_MG * control;
        s.eg += ROOK_CONTROL_BONUS_EG * control;

        U64 file_mask = file_masks[file];
        /* Open file bonus: + points if no pawns on the file. */
        if (__builtin_popcountll(all_pawns & file_mask) == 0)
        {
            s.mg += ROOK_OPEN_FILE_BONUS_MG;
            s.eg += ROOK_OPEN_FILE_BONUS_EG;
        }

        if (king_ring_attackers_mg && king_ring_attackers_eg) {
            int direct_count = __builtin_popcountll(attacks & enemy_king_ring);
            *king_ring_attackers_mg += piece_attack_weights_mg[type] * direct_count;
            *king_ring_attackers_eg += piece_attack_weights_eg[type] * direct_count;

            // X-ray attacks
            U64 own_rooks_queens = board->pieces[side == WHITE ? WHITE_ROOK : BLACK_ROOK] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_rooks_queens = board->pieces[side == WHITE ? BLACK_ROOK : WHITE_ROOK] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_orthogonals = attacks & (own_rooks_queens | enemy_rooks_queens);
            if (frontline_orthogonals) {
                U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                U64 xray_attacks = bitboard_rook_attacks(square, xray_occupancy);
                int xray_count = __builtin_popcountll(xray_attacks & enemy_king_ring);
                *king_ring_attackers_mg += piece_attack_weights_mg[type] * xray_count;
                *king_ring_attackers_eg += piece_attack_weights_eg[type] * xray_count;
            }
        }
        break;
    }
    case WHITE_QUEEN:
    {
        U64 bishop_atk = bitboard_bishop_attacks(square, all_pieces);
        U64 rook_atk = bitboard_rook_attacks(square, all_pieces);
        U64 attacks = bishop_atk | rook_atk;
        int mobility = __builtin_popcountll(attacks);
        s.mg += QUEEN_MOBILITY_BONUS_MG * mobility;
        s.eg += QUEEN_MOBILITY_BONUS_EG * mobility;

        if (king_ring_attackers_mg && king_ring_attackers_eg) {
            // Diagonal direct & X-ray attacks
            int bishop_direct = __builtin_popcountll(bishop_atk & enemy_king_ring);
            *king_ring_attackers_mg += piece_attack_weights_mg[type] * bishop_direct;
            *king_ring_attackers_eg += piece_attack_weights_eg[type] * bishop_direct;
            U64 own_bishops_queens = board->pieces[side == WHITE ? WHITE_BISHOP : BLACK_BISHOP] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_bishops_queens = board->pieces[side == WHITE ? BLACK_BISHOP : WHITE_BISHOP] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_diagonals = bishop_atk & (own_bishops_queens | enemy_bishops_queens);
            if (frontline_diagonals) {
                U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                U64 xray_bishop_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                int bishop_xray = __builtin_popcountll(xray_bishop_attacks & enemy_king_ring);
                *king_ring_attackers_mg += piece_attack_weights_mg[type] * bishop_xray;
                *king_ring_attackers_eg += piece_attack_weights_eg[type] * bishop_xray;
            }

            // Orthogonal direct & X-ray attacks
            int rook_direct = __builtin_popcountll(rook_atk & enemy_king_ring);
            *king_ring_attackers_mg += piece_attack_weights_mg[type] * rook_direct;
            *king_ring_attackers_eg += piece_attack_weights_eg[type] * rook_direct;
            U64 own_rooks_queens = board->pieces[side == WHITE ? WHITE_ROOK : BLACK_ROOK] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_rooks_queens = board->pieces[side == WHITE ? BLACK_ROOK : WHITE_ROOK] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_orthogonals = rook_atk & (own_rooks_queens | enemy_rooks_queens);
            if (frontline_orthogonals) {
                U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                U64 xray_rook_attacks = bitboard_rook_attacks(square, xray_occupancy);
                int rook_xray = __builtin_popcountll(xray_rook_attacks & enemy_king_ring);
                *king_ring_attackers_mg += piece_attack_weights_mg[type] * rook_xray;
                *king_ring_attackers_eg += piece_attack_weights_eg[type] * rook_xray;
            }
        }
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

        /* Use precalculated King corner distance PST */
        s.mg -= king_corner_pst[square] * KING_CORNER_DISTANCE_BONUS_MG;
        s.eg += king_corner_pst[square] * KING_CORNER_DISTANCE_BONUS_EG;
        break;
    }
    default:
        break;
    }
    return s;
}


static Score evaluate_pawn_structure(const Board *board,
                                     U64 white_pawns,
                                     U64 black_pawns,
                                     U64 white_passed_pawns,
                                     U64 black_passed_pawns,
                                     const int white_pawns_per_file[8],
                                     const int black_pawns_per_file[8])
{
    Score s = make_score(0, 0);

    // Evaluate White Pawns
    U64 wp = white_pawns;
    while (wp)
    {
        int square = bitboard_pop_lsb(&wp);
        Score val = evaluate_piece(board, WHITE_PAWN, square, white_passed_pawns, white_pawns, white_pawns_per_file, black_pawns_per_file, 0, 0, 0, 0, 0, 0, 0, NULL, NULL);
        s.mg += val.mg;
        s.eg += val.eg;
    }

    // Evaluate Black Pawns
    U64 bp = black_pawns;
    while (bp)
    {
        int square = bitboard_pop_lsb(&bp);
        Score val = evaluate_piece(board, BLACK_PAWN, square, black_passed_pawns, black_pawns, white_pawns_per_file, black_pawns_per_file, 0, 0, 0, 0, 0, 0, 0, NULL, NULL);
        s.mg -= val.mg;
        s.eg -= val.eg;
    }

    return s;
}


static void calculate_hanging_piece_penalties(const Board *board, int *white_penalty, int *black_penalty)
{
    *white_penalty = 0;
    *black_penalty = 0;

    U64 wp = board->pieces[WHITE_PAWN];
    U64 wn = board->pieces[WHITE_KNIGHT];
    U64 wb = board->pieces[WHITE_BISHOP];
    U64 wr = board->pieces[WHITE_ROOK];
    U64 wq = board->pieces[WHITE_QUEEN];

    U64 bp = board->pieces[BLACK_PAWN];
    U64 bn = board->pieces[BLACK_KNIGHT];
    U64 bb = board->pieces[BLACK_BISHOP];
    U64 br = board->pieces[BLACK_ROOK];
    U64 bq = board->pieces[BLACK_QUEEN];

    U64 occupancy = board->occupancy[BOTH];

    // --- White's hanging pieces (Black is attacker) ---
    U64 white_targets = wn | wb | wr | wq;
    U64 black_attackers = bp | bn | bb | br;

    if (white_targets && black_attackers) {
        int penalty = 0;
        if (bp) {
            U64 temp = wn;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                U64 pawns = bitboard_pawn_attacks(WHITE, sq) & bp;
                penalty += __builtin_popcountll(pawns) * 2;
            }
            temp = wb;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                U64 pawns = bitboard_pawn_attacks(WHITE, sq) & bp;
                penalty += __builtin_popcountll(pawns) * 2;
            }
        }
        if (bp || bn || bb) {
            U64 temp = wr;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                if (bp) {
                    U64 pawns = bitboard_pawn_attacks(WHITE, sq) & bp;
                    penalty += __builtin_popcountll(pawns) * 4;
                }
                U64 kb = 0;
                if (bn) kb |= bitboard_knight_attacks(sq) & bn;
                if (bb) kb |= bitboard_bishop_attacks(sq, occupancy) & bb;
                if (kb) penalty += __builtin_popcountll(kb) * 2;
            }
        }
        U64 temp = wq;
        while (temp) {
            int sq = bitboard_pop_lsb(&temp);
            if (bp) {
                U64 pawns = bitboard_pawn_attacks(WHITE, sq) & bp;
                penalty += __builtin_popcountll(pawns) * 8;
            }
            U64 kb = 0;
            if (bn) kb |= bitboard_knight_attacks(sq) & bn;
            if (bb) kb |= bitboard_bishop_attacks(sq, occupancy) & bb;
            if (kb) penalty += __builtin_popcountll(kb) * 6;
            if (br) {
                U64 rooks = bitboard_rook_attacks(sq, occupancy) & br;
                penalty += __builtin_popcountll(rooks) * 4;
            }
        }
        *white_penalty = penalty;
    }

    // --- Black's hanging pieces (White is attacker) ---
    U64 black_targets = bn | bb | br | bq;
    U64 white_attackers = wp | wn | wb | wr;

    if (black_targets && white_attackers) {
        int penalty = 0;
        if (wp) {
            U64 temp = bn;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                U64 pawns = bitboard_pawn_attacks(BLACK, sq) & wp;
                penalty += __builtin_popcountll(pawns) * 2;
            }
            temp = bb;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                U64 pawns = bitboard_pawn_attacks(BLACK, sq) & wp;
                penalty += __builtin_popcountll(pawns) * 2;
            }
        }
        if (wp || wn || wb) {
            U64 temp = br;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                if (wp) {
                    U64 pawns = bitboard_pawn_attacks(BLACK, sq) & wp;
                    penalty += __builtin_popcountll(pawns) * 4;
                }
                U64 kb = 0;
                if (wn) kb |= bitboard_knight_attacks(sq) & wn;
                if (wb) kb |= bitboard_bishop_attacks(sq, occupancy) & wb;
                if (kb) penalty += __builtin_popcountll(kb) * 2;
            }
        }
        U64 temp = bq;
        while (temp) {
            int sq = bitboard_pop_lsb(&temp);
            if (wp) {
                U64 pawns = bitboard_pawn_attacks(BLACK, sq) & wp;
                penalty += __builtin_popcountll(pawns) * 8;
            }
            U64 kb = 0;
            if (wn) kb |= bitboard_knight_attacks(sq) & wn;
            if (wb) kb |= bitboard_bishop_attacks(sq, occupancy) & wb;
            if (kb) penalty += __builtin_popcountll(kb) * 6;
            if (wr) {
                U64 rooks = bitboard_rook_attacks(sq, occupancy) & wr;
                penalty += __builtin_popcountll(rooks) * 4;
            }
        }
        *black_penalty = penalty;
    }
}


int evaluate_position(Board *board)
{
    if (!eval_initialized)
    {
        init_eval();
    }

    if (board == NULL)
        return 0;

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

    // Probe pawn structure cache
    U64 pawn_key = board_pawn_key(white_pawns, black_pawns);
    int pawn_idx = (int)(pawn_key % PAWN_TABLE_SIZE);
    Score pawn_score;
    if (pawn_table[pawn_idx].key == pawn_key)
    {
        pawn_score.mg = pawn_table[pawn_idx].mg;
        pawn_score.eg = pawn_table[pawn_idx].eg;
    }
    else
    {
        pawn_score = evaluate_pawn_structure(board, white_pawns, black_pawns, white_passed_pawns, black_passed_pawns, white_pawns_per_file, black_pawns_per_file);
        pawn_table[pawn_idx].key = pawn_key;
        pawn_table[pawn_idx].mg = pawn_score.mg;
        pawn_table[pawn_idx].eg = pawn_score.eg;
    }

    U64 white_king_ring = bitboard_king_attacks(board->king_square[WHITE]);
    U64 black_king_ring = bitboard_king_attacks(board->king_square[BLACK]);

    int white_king_ring_attackers_mg =
        piece_attack_weights_mg[0] * (
        __builtin_popcountll(((black_pawns & ~file_masks[0]) >> 9) & white_king_ring) +
        __builtin_popcountll(((black_pawns & ~file_masks[7]) >> 7) & white_king_ring));

    int white_king_ring_attackers_eg =
        piece_attack_weights_eg[0] * (
        __builtin_popcountll(((black_pawns & ~file_masks[0]) >> 9) & white_king_ring) +
        __builtin_popcountll(((black_pawns & ~file_masks[7]) >> 7) & white_king_ring));

    int black_king_ring_attackers_mg =
        piece_attack_weights_mg[0] * (
        __builtin_popcountll(((white_pawns & ~file_masks[0]) << 7) & black_king_ring) +
        __builtin_popcountll(((white_pawns & ~file_masks[7]) << 9) & black_king_ring));

    int black_king_ring_attackers_eg =
        piece_attack_weights_eg[0] * (
        __builtin_popcountll(((white_pawns & ~file_masks[0]) << 7) & black_king_ring) +
        __builtin_popcountll(((white_pawns & ~file_masks[7]) << 9) & black_king_ring));

    for (int piece = 0; piece < PIECE_NB; ++piece)
    {
        if (piece == WHITE_PAWN || piece == BLACK_PAWN)
            continue;

        U64 bb = board->pieces[piece];
        while (bb)
        {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            U64 passed = (side == WHITE) ? white_passed_pawns : black_passed_pawns;
            U64 own_pawns = (side == WHITE) ? white_pawns : black_pawns;
            U64 enemy_king_ring = (side == WHITE) ? black_king_ring : white_king_ring;
            int *king_ring_attackers_mg = (side == WHITE) ? &black_king_ring_attackers_mg : &white_king_ring_attackers_mg;
            int *king_ring_attackers_eg = (side == WHITE) ? &black_king_ring_attackers_eg : &white_king_ring_attackers_eg;

            Score value = evaluate_piece(board, piece, square, passed, own_pawns, white_pawns_per_file, black_pawns_per_file, all_pieces, all_pawns, white_central_blocked_mask, black_central_blocked_mask, knight_open_position_penalty_mg, knight_open_position_penalty_eg, enemy_king_ring, king_ring_attackers_mg, king_ring_attackers_eg);

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

    white_score.mg -= (white_king_ring_attackers_mg * white_king_ring_attackers_mg) >> 6; // Quadratic penalty
    black_score.mg -= (black_king_ring_attackers_mg * black_king_ring_attackers_mg) >> 6;

    white_score.eg -= (white_king_ring_attackers_eg * white_king_ring_attackers_eg) >> 6;
    black_score.eg -= (black_king_ring_attackers_eg * black_king_ring_attackers_eg) >> 6;

    int mg_total = white_score.mg - black_score.mg + pawn_score.mg;
    int eg_total = white_score.eg - black_score.eg + pawn_score.eg;

    // Apply Hanging Piece Penalty
    int white_hanging_penalty_sum = 0;
    int black_hanging_penalty_sum = 0;
    calculate_hanging_piece_penalties(board, &white_hanging_penalty_sum, &black_hanging_penalty_sum);

    int white_penalty_mg = 0;
    int white_penalty_eg = 0;
    int black_penalty_mg = 0;
    int black_penalty_eg = 0;

    if (board->side == WHITE)
    {
        white_penalty_mg = HANGING_PIECE_STM_PENALTY_MG * white_hanging_penalty_sum;
        white_penalty_eg = HANGING_PIECE_STM_PENALTY_EG * white_hanging_penalty_sum;
        black_penalty_mg = HANGING_PIECE_NSTM_PENALTY_MG * black_hanging_penalty_sum;
        black_penalty_eg = HANGING_PIECE_NSTM_PENALTY_EG * black_hanging_penalty_sum;
    }
    else
    {
        black_penalty_mg = HANGING_PIECE_STM_PENALTY_MG * black_hanging_penalty_sum;
        black_penalty_eg = HANGING_PIECE_STM_PENALTY_EG * black_hanging_penalty_sum;
        white_penalty_mg = HANGING_PIECE_NSTM_PENALTY_MG * white_hanging_penalty_sum;
        white_penalty_eg = HANGING_PIECE_NSTM_PENALTY_EG * white_hanging_penalty_sum;
    }

    mg_total = mg_total - white_penalty_mg + black_penalty_mg;
    eg_total = eg_total - white_penalty_eg + black_penalty_eg;

    // Bishop Pair Bonus
    U64 white_bishops = board->pieces[WHITE_BISHOP];
    int has_white_bishop_pair = (white_bishops & (white_bishops - 1)) != 0;
    mg_total += has_white_bishop_pair * BISHOP_PAIR_BONUS_MG;
    eg_total += has_white_bishop_pair * BISHOP_PAIR_BONUS_EG;

    U64 black_bishops = board->pieces[BLACK_BISHOP];
    int has_black_bishop_pair = (black_bishops & (black_bishops - 1)) != 0;
    mg_total -= has_black_bishop_pair * BISHOP_PAIR_BONUS_MG;
    eg_total -= has_black_bishop_pair * BISHOP_PAIR_BONUS_EG;

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

    // Cap evaluation to avoid overlap with mate scores
    if (final_score > 30000)
    {
        final_score = 30000;
    }
    else if (final_score < -30000)
    {
        final_score = -30000;
    }

    return board->side == WHITE ? final_score : -final_score;
}

/* ==============================================================================
 * PYTHON BRIDGE INTERFACE
 * ============================================================================== */
int evaluate_position_with_weights(const char *fen, int *weights)
{
    // 1. Overwrite global evaluation weights in RAM using a unified offset
    bool weights_changed = false;
    int offset = 0;
    
    for (int i = 0; i < 6; ++i) {
        if (piece_values_mg[i] != weights[offset]) {
            piece_values_mg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    for (int i = 0; i < 6; ++i) {
        if (piece_values_eg[i] != weights[offset]) {
            piece_values_eg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    
    for (int i = 0; i < 18; ++i) {
        if (eval_parameters_mg[i] != weights[offset]) {
            eval_parameters_mg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    for (int i = 0; i < 18; ++i) {
        if (eval_parameters_eg[i] != weights[offset]) {
            eval_parameters_eg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }

    for (int i = 0; i < 6; ++i) {
        if (passed_pawn_rank_bonus_mg[i] != weights[offset]) {
            passed_pawn_rank_bonus_mg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    for (int i = 0; i < 6; ++i) {
        if (passed_pawn_rank_bonus_eg[i] != weights[offset]) {
            passed_pawn_rank_bonus_eg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }

    for (int i = 0; i < 6; ++i) {
        if (phalanx_pawn_rank_bonus_mg[i] != weights[offset]) {
            phalanx_pawn_rank_bonus_mg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    for (int i = 0; i < 6; ++i) {
        if (phalanx_pawn_rank_bonus_eg[i] != weights[offset]) {
            phalanx_pawn_rank_bonus_eg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }

    for (int i = 0; i < 5; ++i) {
        if (piece_attack_weights_mg[i] != weights[offset]) {
            piece_attack_weights_mg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    for (int i = 0; i < 5; ++i) {
        if (piece_attack_weights_eg[i] != weights[offset]) {
            piece_attack_weights_eg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }

    if (weights_changed || !eval_initialized)
    {
        init_eval();
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
    int relative_score = evaluate_position(&board);

    // 5. Convert perspective (White-centric)
    int final_score = relative_score;
    if (board.side == BLACK)
    {
        final_score = -final_score;
    }

    return final_score;
}