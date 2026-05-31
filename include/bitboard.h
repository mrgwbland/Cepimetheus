#ifndef BITBOARD_H
#define BITBOARD_H

#include <stdint.h>

typedef uint64_t U64;

void bitboard_init_tables(void);
U64 bitboard_square(int square);
int bitboard_pop_lsb(U64 *bitboard);
U64 bitboard_knight_attacks(int square);
U64 bitboard_king_attacks(int square);
U64 bitboard_pawn_attacks(int side, int square);
U64 bitboard_bishop_attacks(int square, U64 occupancy);
U64 bitboard_rook_attacks(int square, U64 occupancy);
U64 bitboard_queen_attacks(int square, U64 occupancy);
U64 bitboard_passed_pawn_mask(int side, int square);

#endif
