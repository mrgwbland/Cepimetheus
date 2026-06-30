#include "../include/zobrist.h"
#include <stddef.h>

uint64_t ZOBRIST_PIECES[PIECE_NB][64];
uint64_t ZOBRIST_EP_KEYS[8];
uint64_t ZOBRIST_CASTLE_KEYS[16];
uint64_t ZOBRIST_SIDE_KEY;

static uint64_t seed = 1070372ULL;

static uint64_t rand64(void) {
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return seed * 2685821657736338717ULL;
}

void zobrist_init(void) {
    // Reset seed to ensure deterministic keys across runs
    seed = 1070372ULL;

    for (int piece = 0; piece < PIECE_NB; piece++) {
        for (int sq = 0; sq < 64; sq++) {
            ZOBRIST_PIECES[piece][sq] = rand64();
        }
    }

    for (int file = 0; file < 8; file++) {
        ZOBRIST_EP_KEYS[file] = rand64();
    }

    for (int castle = 0; castle < 16; castle++) {
        ZOBRIST_CASTLE_KEYS[castle] = rand64();
    }

    ZOBRIST_SIDE_KEY = rand64();
}
