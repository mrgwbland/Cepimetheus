#include "bitboard.h"
#include "board.h"
#include <immintrin.h>

static U64 knight_table[64];
static U64 king_table[64];
static U64 pawn_table[2][64];
static U64 passed_pawn_masks[2][64];

// 102,400 is the exact number of rook blocker permutations across all 64 squares
U64 rook_table[102400];
// 5,248 is the exact number of bishop blocker permutations across all 64 squares
U64 bishop_table[5248];

// Pointers to the start of each square's lookup table
U64* rook_attacks[64];
U64* bishop_attacks[64];

// Masks for each square
U64 rook_masks[64];
U64 bishop_masks[64];

static int tables_ready = 0;

static int file_of(int square) {
    return square & 7;
}

static int rank_of(int square) {
    return square >> 3;
}

static int on_board(int file, int rank) {
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

static U64 bitboard_pext(U64 value, U64 mask) {
#if defined(__BMI2__)
    return _pext_u64(value, mask);
#else
    U64 result = 0;
    U64 bit = 1;

    while (mask) {
        U64 lsb = mask & (~mask + 1ULL);
        if (value & lsb) {
            result |= bit;
        }
        mask &= (mask - 1ULL);
        bit <<= 1;
    }

    return result;
#endif
}

// Masks exclude the outer edges of the board relative to the piece because pieces on the edge don't affect what is visible behind them.

U64 generate_rook_mask(int sq) {
    int r = sq / 8, c = sq % 8;
    U64 mask = 0;
    // North, South, East, West (excluding the outermost rank/file)
    for(int i = r + 1; i <= 6; i++) mask |= (1ULL << (i * 8 + c));
    for(int i = r - 1; i >= 1; i--) mask |= (1ULL << (i * 8 + c));
    for(int i = c + 1; i <= 6; i++) mask |= (1ULL << (r * 8 + i));
    for(int i = c - 1; i >= 1; i--) mask |= (1ULL << (r * 8 + i));
    return mask;
}

U64 generate_bishop_mask(int sq) {
    int r = sq / 8, c = sq % 8;
    U64 mask = 0;
    // Diagonals (excluding the outermost ranks/files)
    for(int i = r + 1, j = c + 1; i <= 6 && j <= 6; i++, j++) mask |= (1ULL << (i * 8 + j));
    for(int i = r + 1, j = c - 1; i <= 6 && j >= 1; i++, j--) mask |= (1ULL << (i * 8 + j));
    for(int i = r - 1, j = c + 1; i >= 1 && j <= 6; i--, j++) mask |= (1ULL << (i * 8 + j));
    for(int i = r - 1, j = c - 1; i >= 1 && j >= 1; i--, j--) mask |= (1ULL << (i * 8 + j));
    return mask;
}

// Used only during initialisation to figure out the true attacks.

U64 generate_rook_attacks_otb(int sq, U64 blockers) {
    U64 attacks = 0;
    int r = sq / 8, c = sq % 8;
    for(int i = r + 1; i <= 7; i++) { attacks |= (1ULL << (i*8+c)); if(blockers & (1ULL << (i*8+c))) break; }
    for(int i = r - 1; i >= 0; i--) { attacks |= (1ULL << (i*8+c)); if(blockers & (1ULL << (i*8+c))) break; }
    for(int i = c + 1; i <= 7; i++) { attacks |= (1ULL << (r*8+i)); if(blockers & (1ULL << (r*8+i))) break; }
    for(int i = c - 1; i >= 0; i--) { attacks |= (1ULL << (r*8+i)); if(blockers & (1ULL << (r*8+i))) break; }
    return attacks;
}

U64 generate_bishop_attacks_otb(int sq, U64 blockers) {
    U64 attacks = 0;
    int r = sq / 8, c = sq % 8;
    for(int i = r + 1, j = c + 1; i <= 7 && j <= 7; i++, j++) { attacks |= (1ULL << (i*8+j)); if(blockers & (1ULL << (i*8+j))) break; }
    for(int i = r + 1, j = c - 1; i <= 7 && j >= 0; i++, j--) { attacks |= (1ULL << (i*8+j)); if(blockers & (1ULL << (i*8+j))) break; }
    for(int i = r - 1, j = c + 1; i >= 0 && j <= 7; i--, j++) { attacks |= (1ULL << (i*8+j)); if(blockers & (1ULL << (i*8+j))) break; }
    for(int i = r - 1, j = c - 1; i >= 0 && j >= 0; i--, j--) { attacks |= (1ULL << (i*8+j)); if(blockers & (1ULL << (i*8+j))) break; }
    return attacks;
}

static void build_tables(void) {
    int rook_table_index = 0;
    int bishop_table_index = 0;

    for (int square = 0; square < 64; ++square) {
        int file = file_of(square);
        int rank = rank_of(square);

        U64 knight = 0;
        const int knight_offsets[8][2] = {
            {1, 2}, {2, 1}, {2, -1}, {1, -2},
            {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}
        };
        for (int i = 0; i < 8; ++i) {
            int nf = file + knight_offsets[i][0];
            int nr = rank + knight_offsets[i][1];
            if (on_board(nf, nr)) {
                knight |= 1ULL << (nr * 8 + nf);
            }
        }
        knight_table[square] = knight;

        U64 king = 0;
        for (int df = -1; df <= 1; ++df) {
            for (int dr = -1; dr <= 1; ++dr) {
                if (df == 0 && dr == 0) {
                    continue;
                }
                int nf = file + df;
                int nr = rank + dr;
                if (on_board(nf, nr)) {
                    king |= 1ULL << (nr * 8 + nf);
                }
            }
        }
        king_table[square] = king;

        U64 white_pawns = 0;
        if (on_board(file - 1, rank + 1)) {
            white_pawns |= 1ULL << ((rank + 1) * 8 + (file - 1));
        }
        if (on_board(file + 1, rank + 1)) {
            white_pawns |= 1ULL << ((rank + 1) * 8 + (file + 1));
        }
        pawn_table[WHITE][square] = white_pawns;

        U64 black_pawns = 0;
        if (on_board(file - 1, rank - 1)) {
            black_pawns |= 1ULL << ((rank - 1) * 8 + (file - 1));
        }
        if (on_board(file + 1, rank - 1)) {
            black_pawns |= 1ULL << ((rank - 1) * 8 + (file + 1));
        }
        pawn_table[BLACK][square] = black_pawns;

        U64 white_passed_pawn_mask = 0;
        for (int passed_file = file - 1; passed_file <= file + 1; ++passed_file)
        {
            if (passed_file < 0 || passed_file > 7)
            {
                continue;
            }

            for (int passed_rank = rank + 1; passed_rank < 8; ++passed_rank)
            {
                white_passed_pawn_mask |= 1ULL << (passed_rank * 8 + passed_file);
            }
        }
        passed_pawn_masks[WHITE][square] = white_passed_pawn_mask;

        U64 black_passed_pawn_mask = 0;
        for (int passed_file = file - 1; passed_file <= file + 1; ++passed_file)
        {
            if (passed_file < 0 || passed_file > 7)
            {
                continue;
            }

            for (int passed_rank = rank - 1; passed_rank >= 0; --passed_rank)
            {
                black_passed_pawn_mask |= 1ULL << (passed_rank * 8 + passed_file);
            }
        }
        passed_pawn_masks[BLACK][square] = black_passed_pawn_mask;

        /* Generate and store rook/bishop masks for this square */
        rook_masks[square] = generate_rook_mask(square);
        bishop_masks[square] = generate_bishop_mask(square);

        /* Build rook attack table for this square */
        {
            U64 mask = rook_masks[square];
            int positions[64];
            int bits = 0;
            for (int i = 0; i < 64; ++i) {
                if (mask & (1ULL << i)) positions[bits++] = i;
            }
            int permutations = 1 << bits;
            rook_attacks[square] = &rook_table[rook_table_index];
            for (int idx = 0; idx < permutations; ++idx) {
                U64 blockers = 0;
                for (int b = 0; b < bits; ++b) {
                    if (idx & (1 << b)) blockers |= (1ULL << positions[b]);
                }
                rook_table[rook_table_index + idx] = generate_rook_attacks_otb(square, blockers);
            }
            rook_table_index += permutations;
        }

        /* Build bishop attack table for this square */
        {
            U64 mask = bishop_masks[square];
            int positions[64];
            int bits = 0;
            for (int i = 0; i < 64; ++i) {
                if (mask & (1ULL << i)) positions[bits++] = i;
            }
            int permutations = 1 << bits;
            bishop_attacks[square] = &bishop_table[bishop_table_index];
            for (int idx = 0; idx < permutations; ++idx) {
                U64 blockers = 0;
                for (int b = 0; b < bits; ++b) {
                    if (idx & (1 << b)) blockers |= (1ULL << positions[b]);
                }
                bishop_table[bishop_table_index + idx] = generate_bishop_attacks_otb(square, blockers);
            }
            bishop_table_index += permutations;
        }
    }
}

void bitboard_init_tables(void) {
    if (!tables_ready) {
        build_tables();
        tables_ready = 1;
    }
}

U64 bitboard_square(int square) {
    return 1ULL << square;
}
//Returns the index of the least significant set bit and clears it from the bitboard
int bitboard_pop_lsb(U64 *bitboard) {
    if (*bitboard == 0) {
        return -1;
    }

    U64 value = *bitboard;
    int square = __builtin_ctzll(value);
    *bitboard = value & (value - 1ULL);// Clear the least significant bit
    return square;
}

U64 bitboard_knight_attacks(int square) {
    bitboard_init_tables();
    return knight_table[square];
}

U64 bitboard_king_attacks(int square) {
    bitboard_init_tables();
    return king_table[square];
}

U64 bitboard_pawn_attacks(int side, int square) {
    bitboard_init_tables();
    return pawn_table[side][square];
}

U64 bitboard_bishop_attacks(int square, U64 occupancy) {
    U64 index = bitboard_pext(occupancy, bishop_masks[square]);
    return bishop_attacks[square][index];
}

U64 bitboard_rook_attacks(int square, U64 occupancy) {
    U64 index = bitboard_pext(occupancy, rook_masks[square]);
    return rook_attacks[square][index];
}

U64 bitboard_queen_attacks(int square, U64 occupancy) {
    return bitboard_bishop_attacks(square, occupancy) | bitboard_rook_attacks(square, occupancy);
}

U64 bitboard_passed_pawn_mask(int side, int square) {
    bitboard_init_tables();
    return passed_pawn_masks[side][square];
}
