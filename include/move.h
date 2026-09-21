#ifndef MOVE_H
#define MOVE_H

#include <stdbool.h>
#include <stdint.h>

typedef uint16_t Move;

#define MOVE_NONE ((Move)0)

#define MOVE_FLAG_CAPTURE   ((Move)(1u << 15))
#define MOVE_FLAG_PROMOTION ((Move)(1u << 14))

typedef enum {
    MOVE_SPECIAL_NONE = 0,
    MOVE_SPECIAL_CASTLE = 1,
    MOVE_SPECIAL_EN_PASSANT = 2,
    MOVE_SPECIAL_DOUBLE_PAWN = 3,
    MOVE_SPECIAL_PROMO_N = 4,
    MOVE_SPECIAL_PROMO_B = 5,
    MOVE_SPECIAL_PROMO_R = 6,
    MOVE_SPECIAL_PROMO_Q = 7
} MoveSpecial;

struct Board;

static inline Move move_make(int from, int to, MoveSpecial special, bool is_capture) {
    return (Move)((from & 0x3F) | ((to & 0x3F) << 6) | ((special & 0x7) << 12) | ((Move)is_capture << 15));
}

static inline int move_from(Move move) {
    return (int)(move & 0x3Fu);
}

static inline int move_to(Move move) {
    return (int)((move >> 6) & 0x3Fu);
}

static inline MoveSpecial move_special(Move move) {
    return (MoveSpecial)((move >> 12) & 0x7u);
}

static inline bool move_iscapture(Move move) {
    return (move & MOVE_FLAG_CAPTURE) != 0;
}

static inline bool move_is_promotion(Move move) {
    return (move & MOVE_FLAG_PROMOTION) != 0;
}

static inline bool move_is_quiet(Move move) {
    return (move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTION)) == 0;
}

static inline bool move_is_noisy(Move move) {
    return (move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTION)) != 0;
}

static inline bool move_is_castle(Move move) {
    return move_special(move) == MOVE_SPECIAL_CASTLE;
}

static inline bool move_is_en_passant(Move move) {
    return move_special(move) == MOVE_SPECIAL_EN_PASSANT;
}

static inline bool move_is_double_pawn(Move move) {
    return move_special(move) == MOVE_SPECIAL_DOUBLE_PAWN;
}

static inline int move_promotion_piece_type(Move move) {
    return move_is_promotion(move) ? (int)(((move >> 12) & 0x3u) + 1) : 0;
}
bool move_ischeck(const struct Board *board, Move move);
bool move_is_in_list(Move move, const Move *list, int count);
void move_to_string(Move move, const struct Board *board, char buffer[6]);
void zobrist_hash_to_string(uint64_t hash, char buffer[17]);
bool zobrist_hash_from_string(const char *text, uint64_t *hash_out);

#endif
