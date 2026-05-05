#include "bitboard.h"

#include "board.h"

static U64 knight_table[64];
static U64 king_table[64];
static U64 pawn_table[2][64];
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

static void build_tables(void) {
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
    int file = file_of(square);
    int rank = rank_of(square);
    U64 attacks = 0;

    for (int nf = file + 1, nr = rank + 1; on_board(nf, nr); ++nf, ++nr) {
        int target = nr * 8 + nf;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }
    for (int nf = file - 1, nr = rank + 1; on_board(nf, nr); --nf, ++nr) {
        int target = nr * 8 + nf;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }
    for (int nf = file + 1, nr = rank - 1; on_board(nf, nr); ++nf, --nr) {
        int target = nr * 8 + nf;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }
    for (int nf = file - 1, nr = rank - 1; on_board(nf, nr); --nf, --nr) {
        int target = nr * 8 + nf;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }

    return attacks;
}

U64 bitboard_rook_attacks(int square, U64 occupancy) {
    int file = file_of(square);
    int rank = rank_of(square);
    U64 attacks = 0;

    for (int nr = rank + 1; nr < 8; ++nr) {
        int target = nr * 8 + file;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }
    for (int nr = rank - 1; nr >= 0; --nr) {
        int target = nr * 8 + file;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }
    for (int nf = file + 1; nf < 8; ++nf) {
        int target = rank * 8 + nf;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }
    for (int nf = file - 1; nf >= 0; --nf) {
        int target = rank * 8 + nf;
        attacks |= 1ULL << target;
        if (occupancy & (1ULL << target)) {
            break;
        }
    }

    return attacks;
}

U64 bitboard_queen_attacks(int square, U64 occupancy) {
    return bitboard_bishop_attacks(square, occupancy) | bitboard_rook_attacks(square, occupancy);
}
