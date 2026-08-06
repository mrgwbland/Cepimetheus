#ifndef MOVE_H
#define MOVE_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t Move;

#define MOVE_NONE UINT32_C(0xffffffff)

enum {
    MOVE_FLAG_CAPTURE = 1 << 0,
    MOVE_FLAG_DOUBLE_PAWN = 1 << 1,
    MOVE_FLAG_EN_PASSANT = 1 << 2,
    MOVE_FLAG_CASTLE = 1 << 3
};

enum {
    MOVE_PROMO_NONE = 0,
    MOVE_PROMO_KNIGHT = 1,
    MOVE_PROMO_BISHOP = 2,
    MOVE_PROMO_ROOK = 3,
    MOVE_PROMO_QUEEN = 4
};

struct Board;

static inline Move move_make(int from, int to, int promotion, int flags) {
    return (Move)(from & 63) | (Move)((to & 63) << 6) | (Move)((promotion & 7) << 12) | (Move)((flags & 15) << 15);
}

static inline int move_from(Move move) {
    return (int)(move & 63u);
}

static inline int move_to(Move move) {
    return (int)((move >> 6) & 63u);
}

static inline int move_promotion(Move move) {
    return (int)((move >> 12) & 7u);
}

static inline int move_flags(Move move) {
    return (int)((move >> 15) & 15u);
}

bool move_iscapture(Move move);
bool move_ischeck(const struct Board *board, Move move);
void move_to_string(Move move, const struct Board *board, char buffer[6]);
void zobrist_hash_to_string(uint64_t hash, char buffer[17]);
bool zobrist_hash_from_string(const char *text, uint64_t *hash_out);

#endif
