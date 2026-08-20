#include "move.h"

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#include "board.h"

static void square_to_string(int square, char buffer[3]) {
    board_square_to_string(square, buffer);
}

void move_to_string(Move move, const Board *board, char buffer[6]) {
    int from_sq = move_from(move);
    int to_sq = move_to(move);

    if (option_chess960 && board != NULL && (move_flags(move) & MOVE_FLAG_CASTLE)) {
        if (to_sq == 6) {
            to_sq = board->castling_rook_square[0];
        } else if (to_sq == 2) {
            to_sq = board->castling_rook_square[1];
        } else if (to_sq == 62) {
            to_sq = board->castling_rook_square[2];
        } else if (to_sq == 58) {
            to_sq = board->castling_rook_square[3];
        }
    }

    char from[3];
    char to[3];
    square_to_string(from_sq, from);
    square_to_string(to_sq, to);
    buffer[0] = from[0];
    buffer[1] = from[1];
    buffer[2] = to[0];
    buffer[3] = to[1];
    switch (move_promotion(move)) {
        case MOVE_PROMO_KNIGHT: buffer[4] = 'n'; break;
        case MOVE_PROMO_BISHOP: buffer[4] = 'b'; break;
        case MOVE_PROMO_ROOK: buffer[4] = 'r'; break;
        case MOVE_PROMO_QUEEN: buffer[4] = 'q'; break;
        default: buffer[4] = '\0'; break;
    }
    buffer[5] = '\0';
}

void zobrist_hash_to_string(uint64_t hash, char buffer[17]) {
    if (buffer == NULL) {
        return;
    }

    snprintf(buffer, 17, "%016llx", (unsigned long long)hash);
}

bool zobrist_hash_from_string(const char *text, uint64_t *hash_out) {
    if (text == NULL || hash_out == NULL || *text == '\0') {
        return false;
    }

    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 16);
    if (end == text || *end != '\0') {
        return false;
    }

    *hash_out = (uint64_t)value;
    return true;
}

bool move_iscapture(Move move) {
    if ((move_flags(move) & MOVE_FLAG_CAPTURE) != 0) {
        return true;
    }
    return false;
}

bool move_ischeck(const struct Board *board, Move move) {
    if (board == NULL) {
        return false;
    }
    Board temp = *board;
    Undo undo;
    if (!board_make_move(&temp, move, &undo)) {
        return false;
    }
    return board_is_in_check(&temp, temp.side);
}

bool move_is_in_list(Move move, const Move *list, int count) {
    if (list == NULL || count <= 0) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (list[i] == move) {
            return true;
        }
    }
    return false;
}

