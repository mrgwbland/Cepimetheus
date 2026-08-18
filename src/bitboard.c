#include "bitboard.h"
#include "board.h"
#include <immintrin.h>

static U64 knight_table[64];
static U64 king_table[64];
static U64 pawn_table[2][64];
static U64 passed_pawn_masks[2][64];
static U64 pawn_push_path_masks[2][64];
static U64 line_masks[64][64];
static U64 in_between_masks[64][64];

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

#if defined(NO_PEXT)
    /* Hardware PEXT explicitly disabled */
#elif defined(USE_PEXT)
#   define USE_HARDWARE_PEXT
#elif defined(__BMI2__) && !defined(__znver1__) && !defined(__znver2__) && !defined(__znver1) && !defined(__znver2) && !defined(__tune_znver1__) && !defined(__tune_znver2__)
#   define USE_HARDWARE_PEXT
#endif

// The number which converts blockers into a polite index
static const U64 rook_magics[64] = {
    0x008000e090400180ULL, 0x2e40004120015000ULL, 0x5080100220008028ULL, 0x0080100080058800ULL,
    0x0100080004100300ULL, 0x090012480c000100ULL, 0xd200040092000801ULL, 0x0880008000422100ULL,
    0x0000800040038020ULL, 0x020c402010014000ULL, 0x0200802000900080ULL, 0x0020800800100080ULL,
    0x8000808008002400ULL, 0x01e600382c100a00ULL, 0x0004000250082439ULL, 0x0220800900085080ULL,
    0x4120410020800300ULL, 0x0102820041060121ULL, 0x8008888010006000ULL, 0x0410008010080280ULL,
    0x0081110008010104ULL, 0x0301808042009400ULL, 0x00c0040002010810ULL, 0x00080200004400a1ULL,
    0x808c401080012080ULL, 0xc008200240005004ULL, 0x4211004500200010ULL, 0x4000100080080084ULL,
    0x1480110100040800ULL, 0x0c0a008081000400ULL, 0x8900020400080910ULL, 0x0045000100024082ULL,
    0x0402804000800220ULL, 0x0221400080802000ULL, 0x8150801001802000ULL, 0x2024100081800800ULL,
    0x0d60800800800c00ULL, 0x1062800200804c00ULL, 0x0a22082244000150ULL, 0x1141010446001094ULL,
    0x0101688240028000ULL, 0x0060004030024000ULL, 0x000101a004110040ULL, 0x0011001004210008ULL,
    0x8204a40008008080ULL, 0x004200100c020098ULL, 0x003200c104060028ULL, 0x00100a4301820024ULL,
    0x0180023100804100ULL, 0x1003102080c20200ULL, 0x4502221140820a00ULL, 0xc0802068c2001200ULL,
    0x0208000900049100ULL, 0x0018802200040080ULL, 0x0018100306080400ULL, 0x0205548508442600ULL,
    0x4002802832010042ULL, 0x3421014000188121ULL, 0x008008c100702001ULL, 0xa222002010408846ULL,
    0x008200101409a022ULL, 0x6002004c080d3012ULL, 0x4480083001a60104ULL, 0x8001240061004082ULL
};
// 64 - k possible blockers
static const int rook_shifts[64] = {
    52, 53, 53, 53, 53, 53, 53, 52,
    53, 54, 54, 54, 54, 54, 54, 53,
    53, 54, 54, 54, 54, 54, 54, 53,
    53, 54, 54, 54, 54, 54, 54, 53,
    53, 54, 54, 54, 54, 54, 54, 53,
    53, 54, 54, 54, 54, 54, 54, 53,
    53, 54, 54, 54, 54, 54, 54, 53,
    52, 53, 53, 53, 53, 53, 53, 52
};
// The number which converts blockers into a polite index
static const U64 bishop_magics[64] = {
    0x0040041812184031ULL, 0x0002100204810801ULL, 0x0010008089102000ULL, 0x0404040092008022ULL,
    0x200410288200e080ULL, 0x4006013008040020ULL, 0x040108020a204000ULL, 0x00802104100c1281ULL,
    0x0102446008ca0081ULL, 0x9810280800888204ULL, 0x0100308882810104ULL, 0x1010082244404200ULL,
    0x0000240520000000ULL, 0xc000909010088128ULL, 0x4420210110100490ULL, 0x4001820104023201ULL,
    0x280a0120a0210204ULL, 0x00a0080485840105ULL, 0x2508001000c41020ULL, 0x010400080c600808ULL,
    0x0004000080e00049ULL, 0x1a01002201010108ULL, 0x0882020082102240ULL, 0x002306018048260aULL,
    0x0421140410048822ULL, 0x0008020008064800ULL, 0xb882050008004c00ULL, 0x0428580000820040ULL,
    0x0101010100b04000ULL, 0x5200c18084100400ULL, 0x0404010004009200ULL, 0x0140420113012128ULL,
    0x0010052000104290ULL, 0x0808440400020801ULL, 0x1126020100408105ULL, 0x2408020082880080ULL,
    0x0000454040040100ULL, 0x0420900080010087ULL, 0x8016040c04091180ULL, 0x591812008144208aULL,
    0x4002104209202010ULL, 0x1000420210a82068ULL, 0x4104510808000100ULL, 0x0200002018000102ULL,
    0x0001102200601202ULL, 0x20412008008010c0ULL, 0x0824084841000842ULL, 0x040a020c00201104ULL,
    0x5001043004948441ULL, 0x040040680c100031ULL, 0x1400002084300080ULL, 0x0001080020880814ULL,
    0x0040081002088008ULL, 0x024284b002020600ULL, 0x3020381081004000ULL, 0x0014084081020001ULL,
    0x8113410410014408ULL, 0x8481030180900980ULL, 0x0001024424020806ULL, 0x804012000a840410ULL,
    0x8002004010060200ULL, 0x2100201042900100ULL, 0x0012082104040040ULL, 0x40a0200401104411ULL
};
// 64 - k possible blockers
static const int bishop_shifts[64] = {
    58, 59, 59, 59, 59, 59, 59, 58,
    59, 59, 59, 59, 59, 59, 59, 59,
    59, 59, 57, 57, 57, 57, 59, 59,
    59, 59, 57, 55, 55, 57, 59, 59,
    59, 59, 57, 55, 55, 57, 59, 59,
    59, 59, 57, 57, 57, 57, 59, 59,
    59, 59, 59, 59, 59, 59, 59, 59,
    58, 59, 59, 59, 59, 59, 59, 58
};

static inline U64 bishop_attack_index(int square, U64 occupancy) {
#if defined(USE_HARDWARE_PEXT)
    return _pext_u64(occupancy, bishop_masks[square]);
#else
    return ((occupancy & bishop_masks[square]) * bishop_magics[square]) >> bishop_shifts[square];
#endif
}

static inline U64 rook_attack_index(int square, U64 occupancy) {
#if defined(USE_HARDWARE_PEXT)
    return _pext_u64(occupancy, rook_masks[square]);
#else
    return ((occupancy & rook_masks[square]) * rook_magics[square]) >> rook_shifts[square];
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

        U64 white_push_path = 0;
        for (int passed_rank = rank + 1; passed_rank < 8; ++passed_rank)
        {
            white_push_path |= 1ULL << (passed_rank * 8 + file);
        }
        pawn_push_path_masks[WHITE][square] = white_push_path;

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

        U64 black_push_path = 0;
        for (int passed_rank = rank - 1; passed_rank >= 0; --passed_rank)
        {
            black_push_path |= 1ULL << (passed_rank * 8 + file);
        }
        pawn_push_path_masks[BLACK][square] = black_push_path;

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
                U64 attacks = generate_rook_attacks_otb(square, blockers);
                U64 attack_idx = rook_attack_index(square, blockers);
                rook_attacks[square][attack_idx] = attacks;
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
                U64 attacks = generate_bishop_attacks_otb(square, blockers);
                U64 attack_idx = bishop_attack_index(square, blockers);
                bishop_attacks[square][attack_idx] = attacks;
            }
            bishop_table_index += permutations;
        }
    }

    for (int s1 = 0; s1 < 64; ++s1) {
        for (int s2 = 0; s2 < 64; ++s2) {
            line_masks[s1][s2] = 0ULL;
            in_between_masks[s1][s2] = 0ULL;
            if (s1 == s2) {
                continue;
            }
            int f1 = file_of(s1), r1 = rank_of(s1);
            int f2 = file_of(s2), r2 = rank_of(s2);
            int df = f2 - f1;
            int dr = r2 - r1;
            if (f1 == f2) {
                U64 mask = 0;
                for (int r = 0; r < 8; ++r) mask |= 1ULL << (r * 8 + f1);
                line_masks[s1][s2] = mask;
            } else if (r1 == r2) {
                U64 mask = 0;
                for (int f = 0; f < 8; ++f) mask |= 1ULL << (r1 * 8 + f);
                line_masks[s1][s2] = mask;
            } else if (dr == df) {
                U64 mask = 0;
                for (int d = -7; d <= 7; ++d) {
                    if (on_board(f1 + d, r1 + d)) {
                        mask |= 1ULL << ((r1 + d) * 8 + (f1 + d));
                    }
                }
                line_masks[s1][s2] = mask;
            } else if (dr == -df) {
                U64 mask = 0;
                for (int d = -7; d <= 7; ++d) {
                    if (on_board(f1 + d, r1 - d)) {
                        mask |= 1ULL << ((r1 - d) * 8 + (f1 + d));
                    }
                }
                line_masks[s1][s2] = mask;
            }

            if (line_masks[s1][s2] != 0ULL) {
                int step_f = (df > 0) - (df < 0);
                int step_r = (dr > 0) - (dr < 0);
                int step = step_r * 8 + step_f;
                U64 in_between = 0ULL;
                for (int sq = s1 + step; sq != s2; sq += step) {
                    in_between |= 1ULL << sq;
                }
                in_between_masks[s1][s2] = in_between;
            }
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
    U64 index = bishop_attack_index(square, occupancy);
    return bishop_attacks[square][index];
}

U64 bitboard_rook_attacks(int square, U64 occupancy) {
    U64 index = rook_attack_index(square, occupancy);
    return rook_attacks[square][index];
}

U64 bitboard_queen_attacks(int square, U64 occupancy) {
    return bitboard_bishop_attacks(square, occupancy) | bitboard_rook_attacks(square, occupancy);
}

U64 bitboard_passed_pawn_mask(int side, int square) {
    bitboard_init_tables();
    return passed_pawn_masks[side][square];
}

U64 bitboard_pawn_push_path_mask(int side, int square) {
    bitboard_init_tables();
    return pawn_push_path_masks[side][square];
}

U64 bitboard_line_mask(int sq1, int sq2) {
    bitboard_init_tables();
    return line_masks[sq1][sq2];
}

U64 bitboard_in_between_mask(int sq1, int sq2) {
    bitboard_init_tables();
    return in_between_masks[sq1][sq2];
}
