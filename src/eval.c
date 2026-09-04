#include "eval.h"
#include "eval_helpers.h"
#include "endgame.h"
#include "movegen.h"
#include "zobrist.h"
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <omp.h>

int piece_values_mg[6] = {
    1000, 2240, 2545, 3195, 7430, 0
};

int piece_values_eg[6] = {
    1000, 3875, 3430, 6540, 11920, 0
};

int eval_parameters_mg[24] = {
    192, 243, 0, 19, 152, 0, 69, 70, 56, 275, 15, 10, 153, 77, 508, 73, 0, 22, 309, 183, 454, 0, 35, 42
};

int eval_parameters_eg[24] = {
    109, 0, 86, 36, 54, 133, 92, 72, 33, 6, 56, 0, 199, 79, 1019, 51, 37, 850, 123, 0, 114, 166, 0, 20
};

int passed_pawn_rank_bonus_mg[6] = {
    0, 0, 0, 379, 600, 569
};

int passed_pawn_rank_bonus_eg[6] = {
    0, 1, 377, 594, 1016, 1944
};

int phalanx_pawn_rank_bonus_mg[6] = {
    0, 23, 40, 130, 531, 1106
};

int phalanx_pawn_rank_bonus_eg[6] = {
    0, 2, 0, 107, 287, 505
};

int piece_attack_weights_mg[5] = {
    38, 68, 27, 28, 48
};

int piece_attack_weights_eg[5] = {
    0, 0, 13, 21, 47
};

int piece_defense_weights_mg[5] = {
    0, 24, 11, 0, 0
};

int piece_defense_weights_eg[5] = {
    761, 73, 73, 0, 896
};

// Macros for parameters
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
#define KNIGHT_OUTPOST_BONUS_MG eval_parameters_mg[18]
#define KNIGHT_OUTPOST_BONUS_EG eval_parameters_eg[18]
#define BISHOP_OUTPOST_BONUS_MG eval_parameters_mg[19]
#define BISHOP_OUTPOST_BONUS_EG eval_parameters_eg[19]
#define OCB_SCALE_MG eval_parameters_mg[20]
#define OCB_SCALE_EG eval_parameters_eg[20]
#define PASSED_PAWN_SAFE_PATH_MG eval_parameters_mg[21]
#define PASSED_PAWN_SAFE_PATH_EG eval_parameters_eg[21]
#define SPACE_BONUS_MG eval_parameters_mg[22]
#define SPACE_BONUS_EG eval_parameters_eg[22]
#define OUTPOST_MOVE_BONUS_MG eval_parameters_mg[23]
#define OUTPOST_MOVE_BONUS_EG eval_parameters_eg[23]

static inline int manhattan_distance(int sq1, int sq2)
{
    return abs(file_of(sq1) - file_of(sq2)) + abs(rank_of(sq1) - rank_of(sq2));
}

/* Central 3 files on the opposite side of the king (files D,E,F for queenside king; C,D,E for kingside king) */
static const U64 king_sq_central_files[64] = {
    // Rank 1 (a1-d1: Queenside -> D,E,F; e1-h1: Kingside -> C,D,E)
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL,
    // Rank 2
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL,
    // Rank 3
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL,
    // Rank 4
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL,
    // Rank 5
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL,
    // Rank 6
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL,
    // Rank 7
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL,
    // Rank 8
    0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL, 0x3838383838383838ULL,
    0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL, 0x1C1C1C1C1C1C1C1CULL
};

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
    U64 white_passed_pawns;
    U64 black_passed_pawns;
    U64 white_outposts;
    U64 black_outposts;
    int eval_version;
} PawnEntry;

#define PAWN_TABLE_SIZE 32768
static _Thread_local PawnEntry pawn_table[PAWN_TABLE_SIZE];
static int current_eval_version = 1;

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
static U64 outpost_mask[2][64];
static bool outpost_eligible[2][64];
static bool eval_initialised = false;

static const U64 ranks_gt[8] = {
    0xFFFFFFFFFFFFFF00ULL,
    0xFFFFFFFFFFFF0000ULL,
    0xFFFFFFFFFF000000ULL,
    0xFFFFFFFF00000000ULL,
    0xFFFFFF0000000000ULL,
    0xFFFF000000000000ULL,
    0xFF00000000000000ULL,
    0x0000000000000000ULL
};

static const U64 ranks_lt[8] = {
    0x0000000000000000ULL,
    0x00000000000000FFULL,
    0x000000000000FFFFULL,
    0x0000000000FFFFFFULL,
    0x00000000FFFFFFFFULL,
    0x000000FFFFFFFFFFULL,
    0x0000FFFFFFFFFFFFULL,
    0x00FFFFFFFFFFFFFFULL
};

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

    for (int side = 0; side < 2; ++side)
    {
        for (int sq = 0; sq < 64; ++sq)
        {
            int rank = rank_of(sq);
            int file = file_of(sq);
            int rel_rank = (side == WHITE) ? rank : (7 - rank);

            if (rel_rank >= 3 && rel_rank <= 5)
            {
                outpost_eligible[side][sq] = true;
                U64 adj_files = 0;
                if (file > 0) adj_files |= file_masks[file - 1];
                if (file < 7) adj_files |= file_masks[file + 1];

                if (side == WHITE)
                {
                    outpost_mask[WHITE][sq] = adj_files & ranks_gt[rank];
                }
                else
                {
                    outpost_mask[BLACK][sq] = adj_files & ranks_lt[rank];
                }
            }
            else
            {
                outpost_eligible[side][sq] = false;
                outpost_mask[side][sq] = 0ULL;
            }
        }
    }

    current_eval_version++;
    endgame_init();
    update_endgame_weight_reciprocal();
    eval_initialised = true;
}

static inline U64 compute_outposts(U64 enemy_pawns, int side)
{
    U64 outposts = 0ULL;
    int start_rank = (side == WHITE) ? 3 : 2;
    int end_rank = (side == WHITE) ? 5 : 4;
    for (int r = start_rank; r <= end_rank; ++r)
    {
        for (int f = 0; f < 8; ++f)
        {
            int sq = (r << 3) | f;
            if (outpost_eligible[side][sq] && (enemy_pawns & outpost_mask[side][sq]) == 0)
            {
                outposts |= (1ULL << sq);
            }
        }
    }
    return outposts;
}

static inline bool is_outpost_square(const Board *board, int square, int side)
{
    if (!outpost_eligible[side][square])
    {
        return false;
    }

    U64 enemy_pawns = (side == WHITE) ? board->pieces[BLACK_PAWN] : board->pieces[WHITE_PAWN];
    return (enemy_pawns & outpost_mask[side][square]) == 0;
}

// Evaluates MG and EG logic distinctly, without passing phase weight
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
                            int *king_ring_attackers_eg,
                            U64 own_king_ring,
                            int *king_ring_defenders_mg,
                            int *king_ring_defenders_eg,
                            bool is_hanging,
                            U64 outposts)
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
            // Passed pawns get a rank based bonus
            s.mg += passed_pawn_rank_bonus_mg[pawn_rank - 1];
            s.eg += passed_pawn_rank_bonus_eg[pawn_rank - 1];
        }

        // Phalanx pawns get a rank based bonus
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
            // Doubled pawn penalty
            s.mg -= DOUBLED_PAWN_PENALTY_MG * (this_file_count - 1);
            s.eg -= DOUBLED_PAWN_PENALTY_EG * (this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right)
        {
            // Isolated pawn penalty
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

        if (outposts & (1ULL << square))
        {
            s.mg += KNIGHT_OUTPOST_BONUS_MG;
            s.eg += KNIGHT_OUTPOST_BONUS_EG;
        }

        U64 legal_moves = attacks & ~board->occupancy[side];
        int outpost_moves = __builtin_popcountll(legal_moves & outposts);
        s.mg += OUTPOST_MOVE_BONUS_MG * outpost_moves;
        s.eg += OUTPOST_MOVE_BONUS_EG * outpost_moves;

        if (king_ring_attackers_mg && king_ring_attackers_eg) {
            int attacks_count = __builtin_popcountll(attacks & enemy_king_ring);
            *king_ring_attackers_mg += piece_attack_weights_mg[type] * attacks_count;
            *king_ring_attackers_eg += piece_attack_weights_eg[type] * attacks_count;
        }

        if (king_ring_defenders_mg && king_ring_defenders_eg && !is_hanging) {
            int defenses_count = __builtin_popcountll(attacks & own_king_ring);
            *king_ring_defenders_mg += piece_defense_weights_mg[type] * defenses_count;
            *king_ring_defenders_eg += piece_defense_weights_eg[type] * defenses_count;
        }
        break;
    }
    case WHITE_BISHOP:
    {
        // Reward bishops with mobility through pawn occupancy only
        int mobility = __builtin_popcountll(bitboard_bishop_attacks(square, all_pawns));
        s.mg += BISHOP_MOBILITY_BONUS_MG * mobility;
        s.eg += BISHOP_MOBILITY_BONUS_EG * mobility;

        U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
        if (blocked_mask & (1ULL << square))
        {
            s.mg -= PAWN_BLOCKING_PENALTY_MG;
            s.eg -= PAWN_BLOCKING_PENALTY_EG;
        }

        if (outposts & (1ULL << square))
        {
            s.mg += BISHOP_OUTPOST_BONUS_MG;
            s.eg += BISHOP_OUTPOST_BONUS_EG;
        }

        U64 attacks = bitboard_bishop_attacks(square, all_pieces);
        U64 legal_moves = attacks & ~board->occupancy[side];
        int outpost_moves = __builtin_popcountll(legal_moves & outposts);
        s.mg += OUTPOST_MOVE_BONUS_MG * outpost_moves;
        s.eg += OUTPOST_MOVE_BONUS_EG * outpost_moves;

        if (king_ring_attackers_mg && king_ring_attackers_eg) {
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

        if (king_ring_defenders_mg && king_ring_defenders_eg && !is_hanging) {
            int direct_count = __builtin_popcountll(attacks & own_king_ring);
            *king_ring_defenders_mg += piece_defense_weights_mg[type] * direct_count;
            *king_ring_defenders_eg += piece_defense_weights_eg[type] * direct_count;

            // X-ray attacks
            U64 own_bishops_queens = board->pieces[side == WHITE ? WHITE_BISHOP : BLACK_BISHOP] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_bishops_queens = board->pieces[side == WHITE ? BLACK_BISHOP : WHITE_BISHOP] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_diagonals = attacks & (own_bishops_queens | enemy_bishops_queens);
            if (frontline_diagonals) {
                U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                U64 xray_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                int xray_count = __builtin_popcountll(xray_attacks & own_king_ring);
                *king_ring_defenders_mg += piece_defense_weights_mg[type] * xray_count;
                *king_ring_defenders_eg += piece_defense_weights_eg[type] * xray_count;
            }
        }
        break;
    }
    case WHITE_ROOK:
    {
        // Reward squares controlled
        U64 attacks = bitboard_rook_attacks(square, all_pieces);
        int control = __builtin_popcountll(attacks);
        s.mg += ROOK_CONTROL_BONUS_MG * control;
        s.eg += ROOK_CONTROL_BONUS_EG * control;

        U64 file_mask = file_masks[file];
        // Open file bonus: + points if no pawns on the file
        	if ((all_pawns & file_mask) == 0)
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

        if (king_ring_defenders_mg && king_ring_defenders_eg && !is_hanging) {
            int direct_count = __builtin_popcountll(attacks & own_king_ring);
            *king_ring_defenders_mg += piece_defense_weights_mg[type] * direct_count;
            *king_ring_defenders_eg += piece_defense_weights_eg[type] * direct_count;

            // X-ray attacks
            U64 own_rooks_queens = board->pieces[side == WHITE ? WHITE_ROOK : BLACK_ROOK] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_rooks_queens = board->pieces[side == WHITE ? BLACK_ROOK : WHITE_ROOK] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_orthogonals = attacks & (own_rooks_queens | enemy_rooks_queens);
            if (frontline_orthogonals) {
                U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                U64 xray_attacks = bitboard_rook_attacks(square, xray_occupancy);
                int xray_count = __builtin_popcountll(xray_attacks & own_king_ring);
                *king_ring_defenders_mg += piece_defense_weights_mg[type] * xray_count;
                *king_ring_defenders_eg += piece_defense_weights_eg[type] * xray_count;
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

        if (king_ring_defenders_mg && king_ring_defenders_eg && !is_hanging) {
            // Diagonal direct & X-ray attacks
            int bishop_direct = __builtin_popcountll(bishop_atk & own_king_ring);
            *king_ring_defenders_mg += piece_defense_weights_mg[type] * bishop_direct;
            *king_ring_defenders_eg += piece_defense_weights_eg[type] * bishop_direct;
            U64 own_bishops_queens = board->pieces[side == WHITE ? WHITE_BISHOP : BLACK_BISHOP] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_bishops_queens = board->pieces[side == WHITE ? BLACK_BISHOP : WHITE_BISHOP] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_diagonals = bishop_atk & (own_bishops_queens | enemy_bishops_queens);
            if (frontline_diagonals) {
                U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                U64 xray_bishop_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                int bishop_xray = __builtin_popcountll(xray_bishop_attacks & own_king_ring);
                *king_ring_defenders_mg += piece_defense_weights_mg[type] * bishop_xray;
                *king_ring_defenders_eg += piece_defense_weights_eg[type] * bishop_xray;
            }

            // Orthogonal direct & X-ray attacks
            int rook_direct = __builtin_popcountll(rook_atk & own_king_ring);
            *king_ring_defenders_mg += piece_defense_weights_mg[type] * rook_direct;
            *king_ring_defenders_eg += piece_defense_weights_eg[type] * rook_direct;
            U64 own_rooks_queens = board->pieces[side == WHITE ? WHITE_ROOK : BLACK_ROOK] | board->pieces[side == WHITE ? WHITE_QUEEN : BLACK_QUEEN];
            U64 enemy_rooks_queens = board->pieces[side == WHITE ? BLACK_ROOK : WHITE_ROOK] | board->pieces[side == WHITE ? BLACK_QUEEN : WHITE_QUEEN];
            U64 frontline_orthogonals = rook_atk & (own_rooks_queens | enemy_rooks_queens);
            if (frontline_orthogonals) {
                U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                U64 xray_rook_attacks = bitboard_rook_attacks(square, xray_occupancy);
                int rook_xray = __builtin_popcountll(xray_rook_attacks & own_king_ring);
                *king_ring_defenders_mg += piece_defense_weights_mg[type] * rook_xray;
                *king_ring_defenders_eg += piece_defense_weights_eg[type] * rook_xray;
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
        Score val = evaluate_piece(board, WHITE_PAWN, square, white_passed_pawns, white_pawns, white_pawns_per_file, black_pawns_per_file, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL, NULL, false, 0ULL);
        s.mg += val.mg;
        s.eg += val.eg;
    }

    // Evaluate Black Pawns
    U64 bp = black_pawns;
    while (bp)
    {
        int square = bitboard_pop_lsb(&bp);
        Score val = evaluate_piece(board, BLACK_PAWN, square, black_passed_pawns, black_pawns, white_pawns_per_file, black_pawns_per_file, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL, NULL, false, 0ULL);
        s.mg -= val.mg;
        s.eg -= val.eg;
    }

    return s;
}


static void calculate_hanging_piece_penalties(const Board *board,
                                              int *white_penalty,
                                              int *black_penalty,
                                              U64 *white_hanging_mask,
                                              U64 *black_hanging_mask)
{
    *white_penalty = 0;
    *black_penalty = 0;
    *white_hanging_mask = 0ULL;
    *black_hanging_mask = 0ULL;

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
                if (pawns) {
                    *white_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(pawns) * 2;
                }
            }
            temp = wb;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                U64 pawns = bitboard_pawn_attacks(WHITE, sq) & bp;
                if (pawns) {
                    *white_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(pawns) * 2;
                }
            }
        }
        if (bp || bn || bb) {
            U64 temp = wr;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                if (bp) {
                    U64 pawns = bitboard_pawn_attacks(WHITE, sq) & bp;
                    if (pawns) {
                        *white_hanging_mask |= (1ULL << sq);
                        penalty += __builtin_popcountll(pawns) * 4;
                    }
                }
                U64 kb = 0;
                if (bn) kb |= bitboard_knight_attacks(sq) & bn;
                if (bb) kb |= bitboard_bishop_attacks(sq, occupancy) & bb;
                if (kb) {
                    *white_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(kb) * 2;
                }
            }
        }
        U64 temp = wq;
        while (temp) {
            int sq = bitboard_pop_lsb(&temp);
            if (bp) {
                U64 pawns = bitboard_pawn_attacks(WHITE, sq) & bp;
                if (pawns) {
                    *white_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(pawns) * 8;
                }
            }
            U64 kb = 0;
            if (bn) kb |= bitboard_knight_attacks(sq) & bn;
            if (bb) kb |= bitboard_bishop_attacks(sq, occupancy) & bb;
            if (kb) {
                *white_hanging_mask |= (1ULL << sq);
                penalty += __builtin_popcountll(kb) * 6;
            }
            if (br) {
                U64 rooks = bitboard_rook_attacks(sq, occupancy) & br;
                if (rooks) {
                    *white_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(rooks) * 4;
                }
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
                if (pawns) {
                    *black_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(pawns) * 2;
                }
            }
            temp = bb;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                U64 pawns = bitboard_pawn_attacks(BLACK, sq) & wp;
                if (pawns) {
                    *black_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(pawns) * 2;
                }
            }
        }
        if (wp || wn || wb) {
            U64 temp = br;
            while (temp) {
                int sq = bitboard_pop_lsb(&temp);
                if (wp) {
                    U64 pawns = bitboard_pawn_attacks(BLACK, sq) & wp;
                    if (pawns) {
                        *black_hanging_mask |= (1ULL << sq);
                        penalty += __builtin_popcountll(pawns) * 4;
                    }
                }
                U64 kb = 0;
                if (wn) kb |= bitboard_knight_attacks(sq) & wn;
                if (wb) kb |= bitboard_bishop_attacks(sq, occupancy) & wb;
                if (kb) {
                    *black_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(kb) * 2;
                }
            }
        }
        U64 temp = bq;
        while (temp) {
            int sq = bitboard_pop_lsb(&temp);
            if (wp) {
                U64 pawns = bitboard_pawn_attacks(BLACK, sq) & wp;
                if (pawns) {
                    *black_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(pawns) * 8;
                }
            }
            U64 kb = 0;
            if (wn) kb |= bitboard_knight_attacks(sq) & wn;
            if (wb) kb |= bitboard_bishop_attacks(sq, occupancy) & wb;
            if (kb) {
                *black_hanging_mask |= (1ULL << sq);
                penalty += __builtin_popcountll(kb) * 6;
            }
            if (wr) {
                U64 rooks = bitboard_rook_attacks(sq, occupancy) & wr;
                if (rooks) {
                    *black_hanging_mask |= (1ULL << sq);
                    penalty += __builtin_popcountll(rooks) * 4;
                }
            }
        }
        *black_penalty = penalty;
    }
}

// Reward every square that is behind a friendly pawn, not attacked by an enemy pawn, C/D/E/F files, 2/3/4 respective rank
static int evaluate_space_advantage(const Board *board, int side, U64 enemy_pawns)
{
    U64 own_pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    U64 enemy_pawn_attacks;

    if (side == WHITE)
    {
        enemy_pawn_attacks = ((enemy_pawns & ~file_masks[0]) >> 9) | ((enemy_pawns & ~file_masks[7]) >> 7);
    }
    else
    {
        enemy_pawn_attacks = ((enemy_pawns & ~file_masks[0]) << 7) | ((enemy_pawns & ~file_masks[7]) << 9);
    }

    int safe_squares_count = 0;

    for (int f = 2; f <= 5; ++f) // C/D/E/F files
    {
        U64 pawns_on_file = own_pawns & file_masks[f];
        if (!pawns_on_file)
            continue;

        if (side == WHITE)
        {
            int max_rank = -1;
            U64 tmp = pawns_on_file;
            while (tmp)
            {
                int sq = bitboard_pop_lsb(&tmp);
                int r = sq / 8;
                if (r > max_rank)
                    max_rank = r;
            } // Find most advanced pawn on file

            for (int r = 1; r <= 3; ++r) // 2/3/4 ranks
            {
                if (r < max_rank) // Behind pawn
                {
                    int sq = r * 8 + f;
                    if (((1ULL << sq) & enemy_pawn_attacks) == 0) // Not attacked by enemy pawn
                    {
                        safe_squares_count++;
                    }
                }
            }
        }
        else
        {
            int min_rank = 8;
            U64 tmp = pawns_on_file;
            while (tmp)
            {
                int sq = bitboard_pop_lsb(&tmp);
                int r = sq / 8;
                if (r < min_rank)
                    min_rank = r;
            }

            for (int r = 6; r >= 4; --r)
            {
                if (r > min_rank)
                {
                    int sq = r * 8 + f;
                    if (((1ULL << sq) & enemy_pawn_attacks) == 0)
                    {
                        safe_squares_count++;
                    }
                }
            }
        }
    }

    return safe_squares_count; // Will be multiplied by SPACE_BONUS
}


int evaluate_position(Board *board)
{
    if (!eval_initialised)
    {
        init_eval();
    }

    if (board == NULL)
        return 0;

    int phase = get_endgame_weight(board); // 0 (MG) to 1024 (EG)

    const EndgameEntry *eg_entry = NULL;
    if (phase >= 512)
    {
        eg_entry = endgame_probe(board);
        if (eg_entry != NULL && eg_entry->eval_fn != NULL)
        {
            return eg_entry->eval_fn(board, eg_entry->strong_side);
        }
    }

    Score white_score = make_score(0, 0);
    Score black_score = make_score(0, 0);

    U64 white_pawns = board->pieces[WHITE_PAWN];
    U64 black_pawns = board->pieces[BLACK_PAWN];
    U64 all_pawns = white_pawns | black_pawns;

    U64 white_central_blocked_mask = (white_pawns & king_sq_central_files[board->king_square[WHITE]]) << 8;
    U64 black_central_blocked_mask = (black_pawns & king_sq_central_files[board->king_square[BLACK]]) >> 8;

    U64 all_pieces = board->occupancy[BOTH];

    int pawns_lost = 16 - __builtin_popcountll(all_pawns);
    int knight_open_position_penalty_mg = KNIGHT_PAWN_COUNT_PENALTY_MG * pawns_lost;
    int knight_open_position_penalty_eg = KNIGHT_PAWN_COUNT_PENALTY_EG * pawns_lost;
    
    // Probe pawn structure cache
    U64 pawn_key = board_pawn_key(white_pawns, black_pawns);
    int pawn_idx = (int)(pawn_key & (PAWN_TABLE_SIZE - 1));
    Score pawn_score;
    U64 white_passed_pawns;
    U64 black_passed_pawns;
    U64 white_outposts;
    U64 black_outposts;

    if (pawn_table[pawn_idx].key == pawn_key && pawn_table[pawn_idx].eval_version == current_eval_version)
    {
        pawn_score.mg = pawn_table[pawn_idx].mg;
        pawn_score.eg = pawn_table[pawn_idx].eg;
        white_passed_pawns = pawn_table[pawn_idx].white_passed_pawns;
        black_passed_pawns = pawn_table[pawn_idx].black_passed_pawns;
        white_outposts = pawn_table[pawn_idx].white_outposts;
        black_outposts = pawn_table[pawn_idx].black_outposts;
    }
    else
    {
        white_passed_pawns = mark_passed_pawns(board, WHITE);
        black_passed_pawns = mark_passed_pawns(board, BLACK);

        white_outposts = compute_outposts(black_pawns, WHITE);
        black_outposts = compute_outposts(white_pawns, BLACK);

        int white_pawns_per_file[8];
        int black_pawns_per_file[8];
        count_pawns_per_file(white_pawns, white_pawns_per_file);
        count_pawns_per_file(black_pawns, black_pawns_per_file);

        pawn_score = evaluate_pawn_structure(board, white_pawns, black_pawns, white_passed_pawns, black_passed_pawns, white_pawns_per_file, black_pawns_per_file);

        pawn_table[pawn_idx].key = pawn_key;
        pawn_table[pawn_idx].mg = pawn_score.mg;
        pawn_table[pawn_idx].eg = pawn_score.eg;
        pawn_table[pawn_idx].white_passed_pawns = white_passed_pawns;
        pawn_table[pawn_idx].black_passed_pawns = black_passed_pawns;
        pawn_table[pawn_idx].white_outposts = white_outposts;
        pawn_table[pawn_idx].black_outposts = black_outposts;
        pawn_table[pawn_idx].eval_version = current_eval_version;
    }

    // Space advantage bonus
    int white_space = evaluate_space_advantage(board, WHITE, black_pawns);
    int black_space = evaluate_space_advantage(board, BLACK, white_pawns);

    white_score.mg += white_space * SPACE_BONUS_MG;
    white_score.eg += white_space * SPACE_BONUS_EG;
    black_score.mg += black_space * SPACE_BONUS_MG;
    black_score.eg += black_space * SPACE_BONUS_EG;

    // Passed pawn king proximity adjustments
    int white_king_sq = board->king_square[WHITE];
    int black_king_sq = board->king_square[BLACK];

    U64 w_pass = white_passed_pawns;
    while (w_pass)
    {
        int square = bitboard_pop_lsb(&w_pass);
        int friendly_dist = manhattan_distance(square, white_king_sq);
        int enemy_dist = manhattan_distance(square, black_king_sq);

        white_score.mg -= PASSED_PAWN_FRIENDLY_KING_PROXIMITY_MG * friendly_dist;
        white_score.eg -= PASSED_PAWN_FRIENDLY_KING_PROXIMITY_EG * friendly_dist;

        white_score.mg += PASSED_PAWN_ENEMY_KING_PROXIMITY_MG * enemy_dist;
        white_score.eg += PASSED_PAWN_ENEMY_KING_PROXIMITY_EG * enemy_dist;
    }

    U64 b_pass = black_passed_pawns;
    while (b_pass)
    {
        int square = bitboard_pop_lsb(&b_pass);
        int friendly_dist = manhattan_distance(square, black_king_sq);
        int enemy_dist = manhattan_distance(square, white_king_sq);

        black_score.mg -= PASSED_PAWN_FRIENDLY_KING_PROXIMITY_MG * friendly_dist;
        black_score.eg -= PASSED_PAWN_FRIENDLY_KING_PROXIMITY_EG * friendly_dist;

        black_score.mg += PASSED_PAWN_ENEMY_KING_PROXIMITY_MG * enemy_dist;
        black_score.eg += PASSED_PAWN_ENEMY_KING_PROXIMITY_EG * enemy_dist;
    }

    U64 white_king_ring = bitboard_king_attacks(board->king_square[WHITE]);
    U64 black_king_ring = bitboard_king_attacks(board->king_square[BLACK]);

    U64 black_pawn_attacks = __builtin_popcountll((((black_pawns & ~file_masks[0]) >> 9) & white_king_ring)|
                                                  (((black_pawns & ~file_masks[7]) >> 7) & white_king_ring));

    U64 white_pawn_attacks = __builtin_popcountll((((white_pawns & ~file_masks[0]) << 7) & black_king_ring)|
                                                  (((white_pawns & ~file_masks[7]) << 9) & black_king_ring));

    U64 white_pawn_defenses = __builtin_popcountll((((white_pawns & ~file_masks[0]) << 7) & white_king_ring)|
                                                   (((white_pawns & ~file_masks[7]) << 9) & white_king_ring));

    U64 black_pawn_defenses = __builtin_popcountll((((black_pawns & ~file_masks[0]) >> 9) & black_king_ring)|
                                                   (((black_pawns & ~file_masks[7]) >> 7) & black_king_ring));

    int white_king_ring_attackers_mg =
        piece_attack_weights_mg[0] * black_pawn_attacks;

    int white_king_ring_attackers_eg =
        piece_attack_weights_eg[0] * black_pawn_attacks;

    int black_king_ring_attackers_mg =
        piece_attack_weights_mg[0] * white_pawn_attacks;

    int black_king_ring_attackers_eg =
        piece_attack_weights_eg[0] * white_pawn_attacks;

    int white_king_ring_defenders_mg =
        piece_defense_weights_mg[0] * white_pawn_defenses;

    int white_king_ring_defenders_eg =
        piece_defense_weights_eg[0] * white_pawn_defenses;

    int black_king_ring_defenders_mg =
        piece_defense_weights_mg[0] * black_pawn_defenses;

    int black_king_ring_defenders_eg =
        piece_defense_weights_eg[0] * black_pawn_defenses;

    int white_hanging_penalty_sum = 0;
    int black_hanging_penalty_sum = 0;
    U64 white_hanging_mask = 0ULL;
    U64 black_hanging_mask = 0ULL;
    calculate_hanging_piece_penalties(board, &white_hanging_penalty_sum, &black_hanging_penalty_sum, &white_hanging_mask, &black_hanging_mask);

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
            U64 own_king_ring = (side == WHITE) ? white_king_ring : black_king_ring;
            int *king_ring_attackers_mg = (side == WHITE) ? &black_king_ring_attackers_mg : &white_king_ring_attackers_mg;
            int *king_ring_attackers_eg = (side == WHITE) ? &black_king_ring_attackers_eg : &white_king_ring_attackers_eg;
            int *king_ring_defenders_mg = (side == WHITE) ? &white_king_ring_defenders_mg : &black_king_ring_defenders_mg;
            int *king_ring_defenders_eg = (side == WHITE) ? &white_king_ring_defenders_eg : &black_king_ring_defenders_eg;
            bool is_hanging = (((side == WHITE) ? white_hanging_mask : black_hanging_mask) & (1ULL << square)) != 0;
            U64 outposts = (side == WHITE) ? white_outposts : black_outposts;

            Score value = evaluate_piece(board, piece, square, passed, own_pawns, NULL, NULL, all_pieces, all_pawns, white_central_blocked_mask, black_central_blocked_mask, knight_open_position_penalty_mg, knight_open_position_penalty_eg, enemy_king_ring, king_ring_attackers_mg, king_ring_attackers_eg, own_king_ring, king_ring_defenders_mg, king_ring_defenders_eg, is_hanging, outposts);

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

    // Safe Promotion Path for passed pawns
    if ((PASSED_PAWN_SAFE_PATH_MG | PASSED_PAWN_SAFE_PATH_EG) != 0)// Only calculate if passed pawns exist
    {
        U64 occ = board->occupancy[BOTH];

        U64 w_passers = white_passed_pawns;
        if (w_passers)
        {
            U64 bp_attacks = (((black_pawns & ~file_masks[0]) >> 9) | ((black_pawns & ~file_masks[7]) >> 7));
            U64 bk_attacks = bitboard_king_attacks(board->king_square[BLACK]);
            U64 b_knights = board->pieces[BLACK_KNIGHT];
            U64 b_diag = board->pieces[BLACK_BISHOP] | board->pieces[BLACK_QUEEN];
            U64 b_ortho = board->pieces[BLACK_ROOK] | board->pieces[BLACK_QUEEN];

            while (w_passers)
            {
                int square = bitboard_pop_lsb(&w_passers);
                U64 path = bitboard_pawn_push_path_mask(WHITE, square);
                if ((path & occ) == 0 && (path & bp_attacks) == 0 && (path & bk_attacks) == 0)
                {
                    bool safe = true;
                    U64 kn = b_knights;
                    while (kn)
                    {
                        if (bitboard_knight_attacks(bitboard_pop_lsb(&kn)) & path)
                        {
                            safe = false;
                            break;
                        }
                    }
                    if (safe)
                    {
                        U64 diag = b_diag;
                        while (diag)
                        {
                            if (bitboard_bishop_attacks(bitboard_pop_lsb(&diag), occ) & path)
                            {
                                safe = false;
                                break;
                            }
                        }
                    }
                    if (safe)
                    {
                        U64 ortho = b_ortho;
                        while (ortho)
                        {
                            if (bitboard_rook_attacks(bitboard_pop_lsb(&ortho), occ) & path)
                            {
                                safe = false;
                                break;
                            }
                        }
                    }
                    if (safe)
                    {
                        int scale = rank_of(square) + 1 - (board->side != WHITE);
                        white_score.mg += PASSED_PAWN_SAFE_PATH_MG * scale;
                        white_score.eg += PASSED_PAWN_SAFE_PATH_EG * scale;
                    }
                }
            }
        }

        U64 b_passers = black_passed_pawns;
        if (b_passers)
        {
            U64 wp_attacks = (((white_pawns & ~file_masks[0]) << 7) | ((white_pawns & ~file_masks[7]) << 9));
            U64 wk_attacks = bitboard_king_attacks(board->king_square[WHITE]);
            U64 w_knights = board->pieces[WHITE_KNIGHT];
            U64 w_diag = board->pieces[WHITE_BISHOP] | board->pieces[WHITE_QUEEN];
            U64 w_ortho = board->pieces[WHITE_ROOK] | board->pieces[WHITE_QUEEN];

            while (b_passers)
            {
                int square = bitboard_pop_lsb(&b_passers);
                U64 path = bitboard_pawn_push_path_mask(BLACK, square);
                if ((path & occ) == 0 && (path & wp_attacks) == 0 && (path & wk_attacks) == 0)
                {
                    bool safe = true;
                    U64 kn = w_knights;
                    while (kn)
                    {
                        if (bitboard_knight_attacks(bitboard_pop_lsb(&kn)) & path)
                        {
                            safe = false;
                            break;
                        }
                    }
                    if (safe)
                    {
                        U64 diag = w_diag;
                        while (diag)
                        {
                            if (bitboard_bishop_attacks(bitboard_pop_lsb(&diag), occ) & path)
                            {
                                safe = false;
                                break;
                            }
                        }
                    }
                    if (safe)
                    {
                        U64 ortho = w_ortho;
                        while (ortho)
                        {
                            if (bitboard_rook_attacks(bitboard_pop_lsb(&ortho), occ) & path)
                            {
                                safe = false;
                                break;
                            }
                        }
                    }
                    if (safe)
                    {
                        int scale = 8 - rank_of(square) - (board->side != BLACK);
                        black_score.mg += PASSED_PAWN_SAFE_PATH_MG * scale;
                        black_score.eg += PASSED_PAWN_SAFE_PATH_EG * scale;
                    }
                }
            }
        }
    }


    int net_white_attackers_mg = white_king_ring_attackers_mg - white_king_ring_defenders_mg;
    if (net_white_attackers_mg < 0) net_white_attackers_mg = 0;

    int net_white_attackers_eg = white_king_ring_attackers_eg - white_king_ring_defenders_eg;
    if (net_white_attackers_eg < 0) net_white_attackers_eg = 0;

    int net_black_attackers_mg = black_king_ring_attackers_mg - black_king_ring_defenders_mg;
    if (net_black_attackers_mg < 0) net_black_attackers_mg = 0;

    int net_black_attackers_eg = black_king_ring_attackers_eg - black_king_ring_defenders_eg;
    if (net_black_attackers_eg < 0) net_black_attackers_eg = 0;

    white_score.mg -= (net_white_attackers_mg * net_white_attackers_mg) >> 6; // Quadratic penalty
    black_score.mg -= (net_black_attackers_mg * net_black_attackers_mg) >> 6;

    white_score.eg -= (net_white_attackers_eg * net_white_attackers_eg) >> 6;
    black_score.eg -= (net_black_attackers_eg * net_black_attackers_eg) >> 6;

    int mg_total = white_score.mg - black_score.mg + pawn_score.mg;
    int eg_total = white_score.eg - black_score.eg + pawn_score.eg;

    // Apply Hanging Piece Penalty (reusing sums computed before piece evaluation)
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

    int eg_scale = SCALE_NORMAL;
    if (phase >= 512)
    {
        if (eg_entry != NULL && eg_entry->scale_fn != NULL)
        {
            eg_scale = eg_entry->scale_fn(board, eg_entry->strong_side);
        }
        else if (is_ocb(board))
        {
            eg_scale = OCB_SCALE_EG;
        }
    }

    if (eg_scale != SCALE_NORMAL)
    {
        eg_total = (eg_total * eg_scale) >> 8;
    }

    /* Phase Interpolation */
    int final_score = ((1024 - phase) * mg_total + phase * eg_total) >> 10;

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

static void apply_evaluation_weights(const int *weights)
{
    if (weights == NULL)
    {
        if (!eval_initialised)
        {
            init_eval();
        }
        return;
    }

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

    for (int i = 0; i < 24; ++i) {
        if (eval_parameters_mg[i] != weights[offset]) {
            eval_parameters_mg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    for (int i = 0; i < 24; ++i) {
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

    for (int i = 0; i < 5; ++i) {
        if (piece_defense_weights_mg[i] != weights[offset]) {
            piece_defense_weights_mg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }
    for (int i = 0; i < 5; ++i) {
        if (piece_defense_weights_eg[i] != weights[offset]) {
            piece_defense_weights_eg[i] = weights[offset];
            weights_changed = true;
        }
        offset++;
    }

    if (weights_changed || !eval_initialised)
    {
        init_eval();
    }
}


/* ==============================================================================
 * POSITION FEATURE EXTRACTION
 * ============================================================================== */

typedef struct {
    int16_t phase;                  // 0 (MG) to 1024 (EG)
    int16_t eg_scale;               // Endgame scale (normally 256)
    int8_t  is_ocb;                 // Opposite-colored bishop flag
    int8_t  side_to_move;           // +1 for WHITE, -1 for BLACK
    int8_t  is_special_eval;        // 1 if eg_entry->eval_fn was used
    int32_t special_eval_score;

    int8_t  piece_counts[5];        // [0]=Pawn, [1]=N, [2]=B, [3]=R, [4]=Q
    int8_t  passed_pawn_counts[6];  // Rank 2 to 7 -> index 0 to 5
    int8_t  phalanx_pawn_counts[6]; // Rank 2 to 7 -> index 0 to 5
    int16_t eval_param_counts[24];  // 24 general evaluation parameters

    int16_t white_attackers[5];     // [0]=Pawn, [1]=N, [2]=B, [3]=R, [4]=Q
    int16_t white_defenders[5];
    int16_t black_attackers[5];
    int16_t black_defenders[5];
} PositionFeatures;

static PositionFeatures *tuning_features = NULL;

static void extract_position_features(const Board *board, PositionFeatures *feat)
{
    memset(feat, 0, sizeof(PositionFeatures));
    if (board == NULL) return;

    int phase = get_endgame_weight(board);
    feat->phase = (int16_t)phase;
    feat->side_to_move = (board->side == WHITE) ? 1 : -1;

    const EndgameEntry *eg_entry = NULL;
    if (phase >= 512)
    {
        eg_entry = endgame_probe(board);
        if (eg_entry != NULL && eg_entry->eval_fn != NULL)
        {
            feat->is_special_eval = 1;
            feat->special_eval_score = eg_entry->eval_fn(board, eg_entry->strong_side);
            return;
        }
    }

    U64 white_pawns = board->pieces[WHITE_PAWN];
    U64 black_pawns = board->pieces[BLACK_PAWN];
    U64 all_pawns = white_pawns | black_pawns;
    U64 all_pieces = board->occupancy[BOTH];

    U64 white_central_blocked_mask = (white_pawns & king_sq_central_files[board->king_square[WHITE]]) << 8;
    U64 black_central_blocked_mask = (black_pawns & king_sq_central_files[board->king_square[BLACK]]) >> 8;

    int pawns_lost = 16 - __builtin_popcountll(all_pawns);

    U64 white_passed_pawns = mark_passed_pawns(board, WHITE);
    U64 black_passed_pawns = mark_passed_pawns(board, BLACK);
    U64 white_outposts = compute_outposts(black_pawns, WHITE);
    U64 black_outposts = compute_outposts(white_pawns, BLACK);

    int white_pawns_per_file[8];
    int black_pawns_per_file[8];
    count_pawns_per_file(white_pawns, white_pawns_per_file);
    count_pawns_per_file(black_pawns, black_pawns_per_file);

    // Material piece counts (White - Black)
    feat->piece_counts[0] = (int8_t)(__builtin_popcountll(white_pawns) - __builtin_popcountll(black_pawns));
    feat->piece_counts[1] = (int8_t)(__builtin_popcountll(board->pieces[WHITE_KNIGHT]) - __builtin_popcountll(board->pieces[BLACK_KNIGHT]));
    feat->piece_counts[2] = (int8_t)(__builtin_popcountll(board->pieces[WHITE_BISHOP]) - __builtin_popcountll(board->pieces[BLACK_BISHOP]));
    feat->piece_counts[3] = (int8_t)(__builtin_popcountll(board->pieces[WHITE_ROOK]) - __builtin_popcountll(board->pieces[BLACK_ROOK]));
    feat->piece_counts[4] = (int8_t)(__builtin_popcountll(board->pieces[WHITE_QUEEN]) - __builtin_popcountll(board->pieces[BLACK_QUEEN]));

    // Evaluate White Pawns
    U64 wp = white_pawns;
    while (wp)
    {
        int square = bitboard_pop_lsb(&wp);
        int file = file_of(square);
        int rank = rank_of(square);
        int pawn_rank = rank;

        if (white_passed_pawns & (1ULL << square))
        {
            feat->passed_pawn_counts[pawn_rank - 1]++;
        }

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

        if (white_pawns & connected_mask)
        {
            feat->phalanx_pawn_counts[pawn_rank - 1]++;
        }

        int this_file_count = white_pawns_per_file[file];
        if (this_file_count > 1)
        {
            feat->eval_param_counts[4] -= (this_file_count - 1); // DOUBLED_PAWN_PENALTY
        }

        bool has_left = (file > 0) && (white_pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (white_pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right)
        {
            feat->eval_param_counts[5] -= 1; // ISOLATED_PAWN_PENALTY
        }
    }

    // Evaluate Black Pawns
    U64 bp = black_pawns;
    while (bp)
    {
        int square = bitboard_pop_lsb(&bp);
        int file = file_of(square);
        int rank = rank_of(square);
        int pawn_rank = 7 - rank;

        if (black_passed_pawns & (1ULL << square))
        {
            feat->passed_pawn_counts[pawn_rank - 1]--;
        }

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

        if (black_pawns & connected_mask)
        {
            feat->phalanx_pawn_counts[pawn_rank - 1]--;
        }

        int this_file_count = black_pawns_per_file[file];
        if (this_file_count > 1)
        {
            feat->eval_param_counts[4] += (this_file_count - 1); // DOUBLED_PAWN_PENALTY
        }

        bool has_left = (file > 0) && (black_pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (black_pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right)
        {
            feat->eval_param_counts[5] += 1; // ISOLATED_PAWN_PENALTY
        }
    }

    // Hanging piece penalties
    int white_hanging_penalty_sum = 0;
    int black_hanging_penalty_sum = 0;
    U64 white_hanging_mask = 0ULL;
    U64 black_hanging_mask = 0ULL;
    calculate_hanging_piece_penalties(board, &white_hanging_penalty_sum, &black_hanging_penalty_sum, &white_hanging_mask, &black_hanging_mask);

    if (board->side == WHITE)
    {
        feat->eval_param_counts[13] -= white_hanging_penalty_sum; // HANGING_PIECE_STM_PENALTY
        feat->eval_param_counts[14] += black_hanging_penalty_sum; // HANGING_PIECE_NSTM_PENALTY
    }
    else
    {
        feat->eval_param_counts[13] += black_hanging_penalty_sum; // HANGING_PIECE_STM_PENALTY
        feat->eval_param_counts[14] -= white_hanging_penalty_sum; // HANGING_PIECE_NSTM_PENALTY
    }

    U64 white_king_ring = bitboard_king_attacks(board->king_square[WHITE]);
    U64 black_king_ring = bitboard_king_attacks(board->king_square[BLACK]);

    U64 black_pawn_attacks = __builtin_popcountll((((black_pawns & ~file_masks[0]) >> 9) & white_king_ring)|
                                                  (((black_pawns & ~file_masks[7]) >> 7) & white_king_ring));

    U64 white_pawn_attacks = __builtin_popcountll((((white_pawns & ~file_masks[0]) << 7) & black_king_ring)|
                                                  (((white_pawns & ~file_masks[7]) << 9) & black_king_ring));

    U64 white_pawn_defenses = __builtin_popcountll((((white_pawns & ~file_masks[0]) << 7) & white_king_ring)|
                                                   (((white_pawns & ~file_masks[7]) << 9) & white_king_ring));

    U64 black_pawn_defenses = __builtin_popcountll((((black_pawns & ~file_masks[0]) >> 9) & black_king_ring)|
                                                   (((black_pawns & ~file_masks[7]) >> 7) & black_king_ring));

    feat->white_attackers[0] = (int16_t)black_pawn_attacks;
    feat->black_attackers[0] = (int16_t)white_pawn_attacks;
    feat->white_defenders[0] = (int16_t)white_pawn_defenses;
    feat->black_defenders[0] = (int16_t)black_pawn_defenses;

    // Evaluate non-pawn pieces
    for (int piece = 0; piece < PIECE_NB; ++piece)
    {
        if (piece == WHITE_PAWN || piece == BLACK_PAWN)
            continue;

        U64 bb = board->pieces[piece];
        while (bb)
        {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            bool is_white = (side == WHITE);
            int sign = is_white ? +1 : -1;
            int type = is_white ? piece : (piece - 6);

            U64 outposts = is_white ? white_outposts : black_outposts;
            bool is_hanging = (((is_white ? white_hanging_mask : black_hanging_mask) & (1ULL << square)) != 0);

            int16_t *atk_target = is_white ? &feat->black_attackers[type] : &feat->white_attackers[type];
            int16_t *def_target = is_white ? &feat->white_defenders[type] : &feat->black_defenders[type];
            U64 enemy_king_ring = is_white ? black_king_ring : white_king_ring;
            U64 own_king_ring = is_white ? white_king_ring : black_king_ring;

            switch (type)
            {
            case WHITE_KNIGHT:
            {
                feat->eval_param_counts[2] -= sign * pawns_lost; // KNIGHT_PAWN_COUNT_PENALTY

                U64 attacks = bitboard_knight_attacks(square);
                int mobility = __builtin_popcountll(attacks);
                feat->eval_param_counts[6] += sign * mobility; // KNIGHT_MOBILITY_BONUS

                U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
                if (blocked_mask & (1ULL << square))
                {
                    feat->eval_param_counts[1] -= sign * 1; // PAWN_BLOCKING_PENALTY
                }

                if (outposts & (1ULL << square))
                {
                    feat->eval_param_counts[18] += sign * 1; // KNIGHT_OUTPOST_BONUS
                }

                U64 legal_moves = attacks & ~board->occupancy[side];
                int outpost_moves = __builtin_popcountll(legal_moves & outposts);
                feat->eval_param_counts[23] += sign * outpost_moves; // OUTPOST_MOVE_BONUS

                *atk_target += __builtin_popcountll(attacks & enemy_king_ring);
                if (!is_hanging)
                {
                    *def_target += __builtin_popcountll(attacks & own_king_ring);
                }
                break;
            }
            case WHITE_BISHOP:
            {
                int mobility = __builtin_popcountll(bitboard_bishop_attacks(square, all_pawns));
                feat->eval_param_counts[7] += sign * mobility; // BISHOP_MOBILITY_BONUS

                U64 blocked_mask = is_white ? white_central_blocked_mask : black_central_blocked_mask;
                if (blocked_mask & (1ULL << square))
                {
                    feat->eval_param_counts[1] -= sign * 1; // PAWN_BLOCKING_PENALTY
                }

                if (outposts & (1ULL << square))
                {
                    feat->eval_param_counts[19] += sign * 1; // BISHOP_OUTPOST_BONUS
                }

                U64 attacks = bitboard_bishop_attacks(square, all_pieces);
                U64 legal_moves = attacks & ~board->occupancy[side];
                int outpost_moves = __builtin_popcountll(legal_moves & outposts);
                feat->eval_param_counts[23] += sign * outpost_moves; // OUTPOST_MOVE_BONUS

                int direct_count = __builtin_popcountll(attacks & enemy_king_ring);
                int xray_atk_count = 0;
                U64 own_bishops_queens = board->pieces[is_white ? WHITE_BISHOP : BLACK_BISHOP] | board->pieces[is_white ? WHITE_QUEEN : BLACK_QUEEN];
                U64 enemy_bishops_queens = board->pieces[is_white ? BLACK_BISHOP : WHITE_BISHOP] | board->pieces[is_white ? BLACK_QUEEN : WHITE_QUEEN];
                U64 frontline_diagonals = attacks & (own_bishops_queens | enemy_bishops_queens);
                if (frontline_diagonals)
                {
                    U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                    U64 xray_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                    xray_atk_count = __builtin_popcountll(xray_attacks & enemy_king_ring);
                }
                *atk_target += direct_count + xray_atk_count;

                if (!is_hanging)
                {
                    int def_direct = __builtin_popcountll(attacks & own_king_ring);
                    int def_xray = 0;
                    if (frontline_diagonals)
                    {
                        U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                        U64 xray_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                        def_xray = __builtin_popcountll(xray_attacks & own_king_ring);
                    }
                    *def_target += def_direct + def_xray;
                }
                break;
            }
            case WHITE_ROOK:
            {
                U64 attacks = bitboard_rook_attacks(square, all_pieces);
                int control = __builtin_popcountll(attacks);
                feat->eval_param_counts[8] += sign * control; // ROOK_CONTROL_BONUS

                int file = file_of(square);
                if ((all_pawns & file_masks[file]) == 0)
                {
                    feat->eval_param_counts[9] += sign * 1; // ROOK_OPEN_FILE_BONUS
                }

                int direct_count = __builtin_popcountll(attacks & enemy_king_ring);
                int xray_atk_count = 0;
                U64 own_rooks_queens = board->pieces[is_white ? WHITE_ROOK : BLACK_ROOK] | board->pieces[is_white ? WHITE_QUEEN : BLACK_QUEEN];
                U64 enemy_rooks_queens = board->pieces[is_white ? BLACK_ROOK : WHITE_ROOK] | board->pieces[is_white ? BLACK_QUEEN : WHITE_QUEEN];
                U64 frontline_orthogonals = attacks & (own_rooks_queens | enemy_rooks_queens);
                if (frontline_orthogonals)
                {
                    U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                    U64 xray_attacks = bitboard_rook_attacks(square, xray_occupancy);
                    xray_atk_count = __builtin_popcountll(xray_attacks & enemy_king_ring);
                }
                *atk_target += direct_count + xray_atk_count;

                if (!is_hanging)
                {
                    int def_direct = __builtin_popcountll(attacks & own_king_ring);
                    int def_xray = 0;
                    if (frontline_orthogonals)
                    {
                        U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                        U64 xray_attacks = bitboard_rook_attacks(square, xray_occupancy);
                        def_xray = __builtin_popcountll(xray_attacks & own_king_ring);
                    }
                    *def_target += def_direct + def_xray;
                }
                break;
            }
            case WHITE_QUEEN:
            {
                U64 bishop_atk = bitboard_bishop_attacks(square, all_pieces);
                U64 rook_atk = bitboard_rook_attacks(square, all_pieces);
                U64 attacks = bishop_atk | rook_atk;
                int mobility = __builtin_popcountll(attacks);
                feat->eval_param_counts[10] += sign * mobility; // QUEEN_MOBILITY_BONUS

                // Diagonal direct & xray
                int bishop_direct = __builtin_popcountll(bishop_atk & enemy_king_ring);
                int bishop_xray = 0;
                U64 own_bishops_queens = board->pieces[is_white ? WHITE_BISHOP : BLACK_BISHOP] | board->pieces[is_white ? WHITE_QUEEN : BLACK_QUEEN];
                U64 enemy_bishops_queens = board->pieces[is_white ? BLACK_BISHOP : WHITE_BISHOP] | board->pieces[is_white ? BLACK_QUEEN : WHITE_QUEEN];
                U64 frontline_diagonals = bishop_atk & (own_bishops_queens | enemy_bishops_queens);
                if (frontline_diagonals)
                {
                    U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                    U64 xray_bishop_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                    bishop_xray = __builtin_popcountll(xray_bishop_attacks & enemy_king_ring);
                }

                // Orthogonal direct & xray
                int rook_direct = __builtin_popcountll(rook_atk & enemy_king_ring);
                int rook_xray = 0;
                U64 own_rooks_queens = board->pieces[is_white ? WHITE_ROOK : BLACK_ROOK] | board->pieces[is_white ? WHITE_QUEEN : BLACK_QUEEN];
                U64 enemy_rooks_queens = board->pieces[is_white ? BLACK_ROOK : WHITE_ROOK] | board->pieces[is_white ? BLACK_QUEEN : WHITE_QUEEN];
                U64 frontline_orthogonals = rook_atk & (own_rooks_queens | enemy_rooks_queens);
                if (frontline_orthogonals)
                {
                    U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                    U64 xray_rook_attacks = bitboard_rook_attacks(square, xray_occupancy);
                    rook_xray = __builtin_popcountll(xray_rook_attacks & enemy_king_ring);
                }

                *atk_target += bishop_direct + bishop_xray + rook_direct + rook_xray;

                if (!is_hanging)
                {
                    int def_bishop_direct = __builtin_popcountll(bishop_atk & own_king_ring);
                    int def_bishop_xray = 0;
                    if (frontline_diagonals)
                    {
                        U64 xray_occupancy = all_pieces ^ frontline_diagonals;
                        U64 xray_attacks = bitboard_bishop_attacks(square, xray_occupancy);
                        def_bishop_xray = __builtin_popcountll(xray_attacks & own_king_ring);
                    }

                    int def_rook_direct = __builtin_popcountll(rook_atk & own_king_ring);
                    int def_rook_xray = 0;
                    if (frontline_orthogonals)
                    {
                        U64 xray_occupancy = all_pieces ^ frontline_orthogonals;
                        U64 xray_attacks = bitboard_rook_attacks(square, xray_occupancy);
                        def_rook_xray = __builtin_popcountll(xray_attacks & own_king_ring);
                    }

                    *def_target += def_bishop_direct + def_bishop_xray + def_rook_direct + def_rook_xray;
                }
                break;
            }
            case WHITE_KING:
            {
                int attacks_all = __builtin_popcountll(bitboard_queen_attacks(square, all_pieces));
                int attacks_pawns = __builtin_popcountll(bitboard_queen_attacks(square, all_pawns));

                feat->eval_param_counts[11] -= sign * attacks_all;   // KING_EXPOSURE_PENALTY
                feat->eval_param_counts[3]  -= sign * attacks_pawns; // PAWN_SHIELD_PENALTY
                feat->eval_param_counts[12] -= sign * king_corner_pst[square]; // KING_CORNER_DISTANCE_BONUS
                break;
            }
            }
        }
    }

    // Safe promotion path
    U64 occ = board->occupancy[BOTH];
    U64 w_passers = white_passed_pawns;
    if (w_passers)
    {
        U64 bp_attacks = (((black_pawns & ~file_masks[0]) >> 9) | ((black_pawns & ~file_masks[7]) >> 7));
        U64 bk_attacks = bitboard_king_attacks(board->king_square[BLACK]);
        U64 b_knights = board->pieces[BLACK_KNIGHT];
        U64 b_diag = board->pieces[BLACK_BISHOP] | board->pieces[BLACK_QUEEN];
        U64 b_ortho = board->pieces[BLACK_ROOK] | board->pieces[BLACK_QUEEN];

        while (w_passers)
        {
            int square = bitboard_pop_lsb(&w_passers);
            U64 path = bitboard_pawn_push_path_mask(WHITE, square);
            if ((path & occ) == 0 && (path & bp_attacks) == 0 && (path & bk_attacks) == 0)
            {
                bool safe = true;
                U64 kn = b_knights;
                while (kn) {
                    int k_sq = bitboard_pop_lsb(&kn);
                    if (bitboard_knight_attacks(k_sq) & path) { safe = false; break; }
                }
                if (safe) {
                    U64 diag = b_diag;
                    while (diag) {
                        int d_sq = bitboard_pop_lsb(&diag);
                        if (bitboard_bishop_attacks(d_sq, occ) & path) { safe = false; break; }
                    }
                }
                if (safe) {
                    U64 ortho = b_ortho;
                    while (ortho) {
                        int o_sq = bitboard_pop_lsb(&ortho);
                        if (bitboard_rook_attacks(o_sq, occ) & path) { safe = false; break; }
                    }
                }
                if (safe) {
                    int scale = rank_of(square) + 1 - (board->side != WHITE);
                    feat->eval_param_counts[21] += scale; // PASSED_PAWN_SAFE_PATH
                }
            }
        }
    }

    U64 b_passers = black_passed_pawns;
    if (b_passers)
    {
        U64 wp_attacks = (((white_pawns & ~file_masks[0]) << 7) | ((white_pawns & ~file_masks[7]) << 9));
        U64 wk_attacks = bitboard_king_attacks(board->king_square[WHITE]);
        U64 w_knights = board->pieces[WHITE_KNIGHT];
        U64 w_diag = board->pieces[WHITE_BISHOP] | board->pieces[WHITE_QUEEN];
        U64 w_ortho = board->pieces[WHITE_ROOK] | board->pieces[WHITE_QUEEN];

        while (b_passers)
        {
            int square = bitboard_pop_lsb(&b_passers);
            U64 path = bitboard_pawn_push_path_mask(BLACK, square);
            if ((path & occ) == 0 && (path & wp_attacks) == 0 && (path & wk_attacks) == 0)
            {
                bool safe = true;
                U64 kn = w_knights;
                while (kn) {
                    int k_sq = bitboard_pop_lsb(&kn);
                    if (bitboard_knight_attacks(k_sq) & path) { safe = false; break; }
                }
                if (safe) {
                    U64 diag = w_diag;
                    while (diag) {
                        int d_sq = bitboard_pop_lsb(&diag);
                        if (bitboard_bishop_attacks(d_sq, occ) & path) { safe = false; break; }
                    }
                }
                if (safe) {
                    U64 ortho = w_ortho;
                    while (ortho) {
                        int o_sq = bitboard_pop_lsb(&ortho);
                        if (bitboard_rook_attacks(o_sq, occ) & path) { safe = false; break; }
                    }
                }
                if (safe) {
                    int scale = 8 - rank_of(square) - (board->side != BLACK);
                    feat->eval_param_counts[21] -= scale; // PASSED_PAWN_SAFE_PATH
                }
            }
        }
    }

    // Passed pawn king proximity adjustments
    int white_king_sq = board->king_square[WHITE];
    int black_king_sq = board->king_square[BLACK];

    U64 w_pass = white_passed_pawns;
    while (w_pass)
    {
        int square = bitboard_pop_lsb(&w_pass);
        int friendly_dist = manhattan_distance(square, white_king_sq);
        int enemy_dist = manhattan_distance(square, black_king_sq);

        feat->eval_param_counts[15] -= friendly_dist; // PASSED_PAWN_FRIENDLY_KING_PROXIMITY
        feat->eval_param_counts[16] += enemy_dist;    // PASSED_PAWN_ENEMY_KING_PROXIMITY
    }

    U64 b_pass = black_passed_pawns;
    while (b_pass)
    {
        int square = bitboard_pop_lsb(&b_pass);
        int friendly_dist = manhattan_distance(square, black_king_sq);
        int enemy_dist = manhattan_distance(square, white_king_sq);

        feat->eval_param_counts[15] += friendly_dist; // PASSED_PAWN_FRIENDLY_KING_PROXIMITY
        feat->eval_param_counts[16] -= enemy_dist;    // PASSED_PAWN_ENEMY_KING_PROXIMITY
    }

    // Bishop Pair Bonus
    U64 white_bishops = board->pieces[WHITE_BISHOP];
    int has_white_bishop_pair = (white_bishops & (white_bishops - 1)) != 0;
    U64 black_bishops = board->pieces[BLACK_BISHOP];
    int has_black_bishop_pair = (black_bishops & (black_bishops - 1)) != 0;
    feat->eval_param_counts[17] = (int16_t)(has_white_bishop_pair - has_black_bishop_pair); // BISHOP_PAIR_BONUS

    // Tempo bonus
    feat->eval_param_counts[0] = (board->side == WHITE) ? 1 : -1; // TEMPO_BONUS

    // Space advantage
    int white_space = evaluate_space_advantage(board, WHITE, black_pawns);
    int black_space = evaluate_space_advantage(board, BLACK, white_pawns);
    feat->eval_param_counts[22] = (int16_t)(white_space - black_space); // SPACE_BONUS

    // Endgame scale
    feat->eg_scale = 256;
    feat->is_ocb = 0;
    if (phase >= 512)
    {
        if (eg_entry != NULL && eg_entry->scale_fn != NULL)
        {
            feat->eg_scale = eg_entry->scale_fn(board, eg_entry->strong_side);
        }
        else if (is_ocb(board))
        {
            feat->is_ocb = 1;
        }
    }
}

static inline int fast_eval_from_features(const PositionFeatures *feat, const int *weights)
{
    if (feat->is_special_eval)
    {
        return feat->special_eval_score;
    }

    const int *pw_mg = &weights[0];
    const int *pw_eg = &weights[6];
    const int *ep_mg = &weights[12];
    const int *ep_eg = &weights[36];
    const int *pp_mg = &weights[60];
    const int *pp_eg = &weights[66];
    const int *px_mg = &weights[72];
    const int *px_eg = &weights[78];
    const int *at_mg = &weights[84];
    const int *at_eg = &weights[89];
    const int *df_mg = &weights[94];
    const int *df_eg = &weights[99];

    int mg_total = 0;
    int eg_total = 0;

    // 1. Piece material (Pawns, Knights, Bishops, Rooks, Queens)
    for (int i = 0; i < 5; ++i)
    {
        mg_total += pw_mg[i] * feat->piece_counts[i];
        eg_total += pw_eg[i] * feat->piece_counts[i];
    }

    // 2. Passed pawn rank bonus
    for (int r = 0; r < 6; ++r)
    {
        mg_total += pp_mg[r] * feat->passed_pawn_counts[r];
        eg_total += pp_eg[r] * feat->passed_pawn_counts[r];
    }

    // 3. Phalanx pawn rank bonus
    for (int r = 0; r < 6; ++r)
    {
        mg_total += px_mg[r] * feat->phalanx_pawn_counts[r];
        eg_total += px_eg[r] * feat->phalanx_pawn_counts[r];
    }

    // 4. General evaluation parameters
    for (int p = 0; p < 24; ++p)
    {
        mg_total += ep_mg[p] * feat->eval_param_counts[p];
        eg_total += ep_eg[p] * feat->eval_param_counts[p];
    }

    // 5. King safety
    int w_atk_mg = 0, w_atk_eg = 0, w_def_mg = 0, w_def_eg = 0;
    int b_atk_mg = 0, b_atk_eg = 0, b_def_mg = 0, b_def_eg = 0;

    for (int i = 0; i < 5; ++i)
    {
        w_atk_mg += at_mg[i] * feat->white_attackers[i];
        w_atk_eg += at_eg[i] * feat->white_attackers[i];
        w_def_mg += df_mg[i] * feat->white_defenders[i];
        w_def_eg += df_eg[i] * feat->white_defenders[i];

        b_atk_mg += at_mg[i] * feat->black_attackers[i];
        b_atk_eg += at_eg[i] * feat->black_attackers[i];
        b_def_mg += df_mg[i] * feat->black_defenders[i];
        b_def_eg += df_eg[i] * feat->black_defenders[i];
    }

    int net_w_mg = w_atk_mg - w_def_mg; if (net_w_mg < 0) net_w_mg = 0;
    int net_w_eg = w_atk_eg - w_def_eg; if (net_w_eg < 0) net_w_eg = 0;
    int net_b_mg = b_atk_mg - b_def_mg; if (net_b_mg < 0) net_b_mg = 0;
    int net_b_eg = b_atk_eg - b_def_eg; if (net_b_eg < 0) net_b_eg = 0;

    mg_total -= (net_w_mg * net_w_mg) >> 6;
    mg_total += (net_b_mg * net_b_mg) >> 6;
    eg_total -= (net_w_eg * net_w_eg) >> 6;
    eg_total += (net_b_eg * net_b_eg) >> 6;

    // 6. Scale and phase interpolation
    int eg_scale = feat->is_ocb ? ep_eg[20] : feat->eg_scale;
    if (eg_scale != 256)
    {
        eg_total = (eg_total * eg_scale) >> 8;
    }

    int final_score = ((1024 - feat->phase) * mg_total + feat->phase * eg_total) >> 10;
    if (final_score > 30000) final_score = 30000;
    else if (final_score < -30000) final_score = -30000;

    return (feat->side_to_move == 1) ? final_score : -final_score;
}

static Board *tuning_boards = NULL;
static double *tuning_targets = NULL;
static int tuning_dataset_size = 0;

void free_tuning_dataset(void)
{
    if (tuning_boards != NULL)
    {
        free(tuning_boards);
        tuning_boards = NULL;
    }
    if (tuning_features != NULL)
    {
        free(tuning_features);
        tuning_features = NULL;
    }
    if (tuning_targets != NULL)
    {
        free(tuning_targets);
        tuning_targets = NULL;
    }
    tuning_dataset_size = 0;
}

int get_tuning_dataset_size(void)
{
    return tuning_dataset_size;
}

int init_tuning_dataset(const char *dataset_path)
{
    free_tuning_dataset();

    bitboard_init_tables();
    zobrist_init();
    if (!eval_initialised) {
        init_eval();
    }

    FILE *f = fopen(dataset_path, "r");
    if (!f) {
        return -1;
    }

    int capacity = 100000;
    tuning_boards = (Board *)malloc(capacity * sizeof(Board));
    tuning_features = (PositionFeatures *)malloc(capacity * sizeof(PositionFeatures));
    tuning_targets = (double *)malloc(capacity * sizeof(double));
    if (!tuning_boards || !tuning_features || !tuning_targets) {
        fclose(f);
        free_tuning_dataset();
        return -1;
    }

    char line[512];
    int count = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        char *first_comma = strchr(line, ',');
        if (!first_comma) continue;
        *first_comma = '\0';
        char *fen = line;

        char *second_comma = strchr(first_comma + 1, ',');
        double target_cp = 0.0;
        if (second_comma) {
            target_cp = atof(second_comma + 1);
        } else {
            target_cp = atof(first_comma + 1);
        }

        if (count >= capacity) {
            capacity *= 2;
            Board *nb = (Board *)realloc(tuning_boards, capacity * sizeof(Board));
            PositionFeatures *nf = (PositionFeatures *)realloc(tuning_features, capacity * sizeof(PositionFeatures));
            double *nt = (double *)realloc(tuning_targets, capacity * sizeof(double));
            if (!nb || !nf || !nt) {
                if (nb) tuning_boards = nb;
                if (nf) tuning_features = nf;
                if (nt) tuning_targets = nt;
                break;
            }
            tuning_boards = nb;
            tuning_features = nf;
            tuning_targets = nt;
        }

        board_clear(&tuning_boards[count]);
        if (board_set_fen(&tuning_boards[count], fen)) {
            tuning_targets[count] = 1.0 / (1.0 + exp(-target_cp * 0.00575646273248511421));
            extract_position_features(&tuning_boards[count], &tuning_features[count]);
            count++;
        }
    }

    fclose(f);
    tuning_dataset_size = count;
    return count;
}

double calculate_tuning_mse(const int *weights)
{
    if (tuning_dataset_size <= 0 || tuning_features == NULL)
    {
        return 0.0;
    }

    double total_loss = 0.0;
    #pragma omp parallel for reduction(+:total_loss) schedule(static)
    for (int i = 0; i < tuning_dataset_size; ++i)
    {
        int relative_score = fast_eval_from_features(&tuning_features[i], weights);
        int white_score = (tuning_features[i].side_to_move == 1) ? relative_score : -relative_score;
        double cp = (double)white_score / 10.0;
        double pred_wp = 1.0 / (1.0 + exp(-cp * 0.00575646273248511421));
        double err = pred_wp - tuning_targets[i];
        total_loss += err * err;
    }

    return total_loss / (double)tuning_dataset_size;
}

void get_tuning_evaluations(const int *weights, double *out_cep_wp)
{
    if (tuning_dataset_size <= 0 || tuning_features == NULL || out_cep_wp == NULL)
    {
        return;
    }

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < tuning_dataset_size; ++i)
    {
        int relative_score = fast_eval_from_features(&tuning_features[i], weights);
        int white_score = (tuning_features[i].side_to_move == 1) ? relative_score : -relative_score;
        double cp = (double)white_score / 10.0;
        out_cep_wp[i] = 1.0 / (1.0 + exp(-cp * 0.00575646273248511421));
    }
}

int evaluate_position_with_weights(const char *fen, const int *weights)
{
    apply_evaluation_weights(weights);

    Board board;
    board_init(&board); 

    if (!board_set_fen(&board, fen))
    {
        return 0;
    }

    // 3. Check for theoretical material draws or 50-move rule
    if (board_is_draw(&board, NULL, 0, false))
    {
        return 0;
    }

    // 4. Generate pseudo-legal moves and check for legal moves (checkmate / stalemate detection)
    MoveList list;
    movegen_generate_pseudo_legal(&board, &list);

    U64 pinned = board.pinned_mask;
    U64 checkers = board.checkers;
    bool has_legal_move = false;

    for (int i = 0; i < list.count; i++)
    {
        if (board_is_move_legal(&board, list.moves[i], pinned, checkers))
        {
            has_legal_move = true;
            break;
        }
    }

    if (!has_legal_move)
    {
        if (checkers != 0)
        {
            // Checkmate: return White-centric mate score
            return (board.side == WHITE) ? -MATE_SCORE : MATE_SCORE;
        }
        else
        {
            // Stalemate: draw score
            return 0;
        }
    }

    // 5. Calculate relative score from evaluation function
    int relative_score = evaluate_position(&board);

    // 6. Convert perspective (White-centric)
    int final_score = relative_score;
    if (board.side == BLACK)
    {
        final_score = -final_score;
    }

    return final_score;
}