#include "../include/endgame.h"
#include <stdlib.h>
#include <string.h>

static EndgameEntry endgame_table[ENDGAME_TABLE_SIZE];
static bool endgame_initialized = false;

static inline int sq_file(int sq) {
    return sq & 7;
}

static inline int sq_rank(int sq) {
    return sq >> 3;
}

// Distance based on the greatest of the distances of the coordinates
static inline int chebyshev_distance(int sq1, int sq2) {
    int fdist = abs(sq_file(sq1) - sq_file(sq2));
    int rdist = abs(sq_rank(sq1) - sq_rank(sq2));
    return fdist > rdist ? fdist : rdist;
}

// Distance based on the sum of the distances of the coordinates
static inline int manhattan_distance(int sq1, int sq2) {
    return abs(sq_file(sq1) - sq_file(sq2)) + abs(sq_rank(sq1) - sq_rank(sq2));
}

static inline bool is_dark_square(int sq) {
    return ((sq_file(sq) + sq_rank(sq)) & 1) == 0;
}

// Hash entry based on count of piece types
uint64_t board_material_key(const Board *board) {
    uint64_t wP = __builtin_popcountll(board->pieces[WHITE_PAWN]);
    uint64_t wN = __builtin_popcountll(board->pieces[WHITE_KNIGHT]);
    uint64_t wB = __builtin_popcountll(board->pieces[WHITE_BISHOP]);
    uint64_t wR = __builtin_popcountll(board->pieces[WHITE_ROOK]);
    uint64_t wQ = __builtin_popcountll(board->pieces[WHITE_QUEEN]);

    uint64_t bP = __builtin_popcountll(board->pieces[BLACK_PAWN]);
    uint64_t bN = __builtin_popcountll(board->pieces[BLACK_KNIGHT]);
    uint64_t bB = __builtin_popcountll(board->pieces[BLACK_BISHOP]);
    uint64_t bR = __builtin_popcountll(board->pieces[BLACK_ROOK]);
    uint64_t bQ = __builtin_popcountll(board->pieces[BLACK_QUEEN]);

    return wP | (wN << 4) | (wB << 8) | (wR << 12) | (wQ << 16) |
           (bP << 20) | (bN << 24) | (bB << 28) | (bR << 32) | (bQ << 36);
}

static uint64_t make_material_key(int wP, int wN, int wB, int wR, int wQ,
                                  int bP, int bN, int bB, int bR, int bQ) {
    return ((uint64_t)wP) | ((uint64_t)wN << 4) | ((uint64_t)wB << 8) | ((uint64_t)wR << 12) | ((uint64_t)wQ << 16) |
           ((uint64_t)bP << 20) | ((uint64_t)bN << 24) | ((uint64_t)bB << 28) | ((uint64_t)bR << 32) | ((uint64_t)bQ << 36);
}

static void add_endgame_entry(int wP, int wN, int wB, int wR, int wQ,
                              int bP, int bN, int bB, int bR, int bQ,
                              EndgameFunc eval_fn, EndgameScaleFunc scale_fn,
                              int strong_side) {
    uint64_t key = make_material_key(wP, wN, wB, wR, wQ, bP, bN, bB, bR, bQ);
    size_t idx = (size_t)(key % ENDGAME_TABLE_SIZE);
    size_t orig_idx = idx;

    while (endgame_table[idx].is_valid && endgame_table[idx].key != key) {
        idx = (idx + 1) % ENDGAME_TABLE_SIZE;
        if (idx == orig_idx) break;
    }

    endgame_table[idx].key = key;
    endgame_table[idx].eval_fn = eval_fn;
    endgame_table[idx].scale_fn = scale_fn;
    endgame_table[idx].strong_side = strong_side;
    endgame_table[idx].is_valid = true;
}

static void register_endgame(int wP, int wN, int wB, int wR, int wQ,
                             int bP, int bN, int bB, int bR, int bQ,
                             EndgameFunc eval_fn, EndgameScaleFunc scale_fn) {
    add_endgame_entry(wP, wN, wB, wR, wQ, bP, bN, bB, bR, bQ, eval_fn, scale_fn, WHITE);
    if (wP != bP || wN != bN || wB != bB || wR != bR || wQ != bQ) {
        add_endgame_entry(bP, bN, bB, bR, bQ, wP, wN, wB, wR, wQ, eval_fn, scale_fn, BLACK);
    }
}

static int eval_draw(const Board *board, int strong_side) {
    (void)board;
    (void)strong_side;
    return 0;
}

// Scaling functions

// Rook vs minor endgames
static int eval_krkb(const Board *board, int strong_side) {
    int weak_side = (strong_side == WHITE) ? BLACK : WHITE;
    U64 bishop_bb = board->pieces[(weak_side == WHITE) ? WHITE_BISHOP : BLACK_BISHOP];
    if (bishop_bb) {
        int bishop_sq = __builtin_ctzll(bishop_bb);
        bool attacked = board_is_square_attacked(board, bishop_sq, strong_side);
        bool defended = board_is_square_attacked(board, bishop_sq, weak_side);
        if (attacked && !defended) {
            int score = 500;
            return (board->side == strong_side) ? score : -score;
        }
    }
    return 0;
}

static int eval_krkn(const Board *board, int strong_side) {
    int weak_side = (strong_side == WHITE) ? BLACK : WHITE;
    U64 knight_bb = board->pieces[(weak_side == WHITE) ? WHITE_KNIGHT : BLACK_KNIGHT];
    if (knight_bb) {
        int knight_sq = __builtin_ctzll(knight_bb);
        bool attacked = board_is_square_attacked(board, knight_sq, strong_side);
        bool defended = board_is_square_attacked(board, knight_sq, weak_side);
        if (attacked && !defended) {
            int score = 500;
            return (board->side == strong_side) ? score : -score;
        }
    }
    return 0;
}

// Pawnless Checkmates
// King and major/two bishops vs king
static int eval_kxvk(const Board *board, int strong_side) {
    int weak_side = (strong_side == WHITE) ? BLACK : WHITE;
    int strong_king = board->king_square[strong_side];
    int weak_king = board->king_square[weak_side];

    int rdist = sq_rank(weak_king) < 4 ? sq_rank(weak_king) : 7 - sq_rank(weak_king);
    int fdist = sq_file(weak_king) < 4 ? sq_file(weak_king) : 7 - sq_file(weak_king);
    int corner_dist = rdist + fdist;
    int kdist = manhattan_distance(strong_king, weak_king);

    int score = 10000 + (14 - corner_dist) * 20 + (14 - kdist) * 20;
    return (board->side == strong_side) ? score : -score;
}
// King and bishop and knight vs king
static int eval_kbnk(const Board *board, int strong_side) {
    int weak_side = (strong_side == WHITE) ? BLACK : WHITE;
    int strong_king = board->king_square[strong_side];
    int weak_king = board->king_square[weak_side];

    U64 bishop_bb = board->pieces[(strong_side == WHITE) ? WHITE_BISHOP : BLACK_BISHOP];
    if (!bishop_bb) return 0;
    int bishop_sq = __builtin_ctzll(bishop_bb);

    bool dark_bishop = is_dark_square(bishop_sq);

    // Dark corners: a1 (0), h8 (63). Light corners: a8 (56), h1 (7).
    int corner_dist1 = dark_bishop ? (sq_rank(weak_king) + sq_file(weak_king))
                                   : ((7 - sq_rank(weak_king)) + sq_file(weak_king));
    int corner_dist2 = dark_bishop ? ((7 - sq_rank(weak_king)) + (7 - sq_file(weak_king)))
                                   : (sq_rank(weak_king) + (7 - sq_file(weak_king)));

    int target_corner_dist = (corner_dist1 < corner_dist2) ? corner_dist1 : corner_dist2;
    int kdist = manhattan_distance(strong_king, weak_king);

    int score = 10000 - target_corner_dist * 40 - kdist * 20;
    return (board->side == strong_side) ? score : -score;
}

//Drawn pawn endgames
static int scale_kpsvk(const Board *board, int strong_side) {
    U64 pawns = board->pieces[(strong_side == WHITE) ? WHITE_PAWN : BLACK_PAWN];
    if (!pawns) return SCALE_NORMAL;

    int pawn_sq = __builtin_ctzll(pawns);
    int file = sq_file(pawn_sq);

    // Only scale down if it is an A-pawn or H-pawn
    if (file == 0 || file == 7) {
        int target_rank = (strong_side == WHITE) ? 7 : 0;
        int queening_sq = (target_rank << 3) | file;
        int weak_king = board->king_square[(strong_side == WHITE) ? BLACK : WHITE];

        if (chebyshev_distance(weak_king, queening_sq) <= 1) {
            return SCALE_DRAW;
        }
    }
    return SCALE_NORMAL;
}

static int scale_kbpsvk(const Board *board, int strong_side) {
    U64 pawns = board->pieces[(strong_side == WHITE) ? WHITE_PAWN : BLACK_PAWN];
    U64 bishops = board->pieces[(strong_side == WHITE) ? WHITE_BISHOP : BLACK_BISHOP];
    if (!pawns || !bishops) return SCALE_NORMAL;

    int pawn_sq = __builtin_ctzll(pawns);
    int bishop_sq = __builtin_ctzll(bishops);
    int file = sq_file(pawn_sq);

    if (file == 0 || file == 7) {
        int target_rank = (strong_side == WHITE) ? 7 : 0;
        int queening_sq = (target_rank << 3) | file;
        int weak_king = board->king_square[(strong_side == WHITE) ? BLACK : WHITE];

        if (is_dark_square(bishop_sq) != is_dark_square(queening_sq) &&
            chebyshev_distance(weak_king, queening_sq) <= 1) {
            return SCALE_DRAW;
        }
    }
    return SCALE_NORMAL;
}

//Interface:

void endgame_init(void) {
    if (endgame_initialized) return;
    memset(endgame_table, 0, sizeof(endgame_table));

    // Legally drawn endgames (eval_draw)
    register_endgame(0, 0, 0, 0, 0,  0, 0, 0, 0, 0, &eval_draw, NULL); // K vs K
    register_endgame(0, 1, 0, 0, 0,  0, 0, 0, 0, 0, &eval_draw, NULL); // KN vs K
    register_endgame(0, 0, 1, 0, 0,  0, 0, 0, 0, 0, &eval_draw, NULL); // KB vs K
    register_endgame(0, 2, 0, 0, 0,  0, 0, 0, 0, 0, &eval_draw, NULL); // KNN vs K
    register_endgame(0, 1, 0, 0, 0,  0, 1, 0, 0, 0, &eval_draw, NULL); // KN vs KN
    register_endgame(0, 0, 1, 0, 0,  0, 0, 1, 0, 0, &eval_draw, NULL); // KB vs KB
    register_endgame(0, 1, 0, 0, 0,  0, 0, 1, 0, 0, &eval_draw, NULL); // KN vs KB

    // Rook vs Minor draws
    register_endgame(0, 0, 0, 1, 0,  0, 0, 1, 0, 0, &eval_krkb, NULL); // KR vs KB
    register_endgame(0, 0, 0, 1, 0,  0, 1, 0, 0, 0, &eval_krkn, NULL); // KR vs KN

    // Pawnless mate endgames
    register_endgame(0, 0, 0, 1, 0,  0, 0, 0, 0, 0, &eval_kxvk, NULL); // KR vs K
    register_endgame(0, 0, 0, 0, 1,  0, 0, 0, 0, 0, &eval_kxvk, NULL); // KQ vs K
    register_endgame(0, 0, 2, 0, 0,  0, 0, 0, 0, 0, &eval_kxvk, NULL); // KBB vs K
    register_endgame(0, 1, 1, 0, 0,  0, 0, 0, 0, 0, &eval_kbnk, NULL); // KBN vs K

    // Scaled pawn endgames
    register_endgame(1, 0, 0, 0, 0,  0, 0, 0, 0, 0, NULL, &scale_kpsvk);  // KP vs K
    register_endgame(1, 0, 1, 0, 0,  0, 0, 0, 0, 0, NULL, &scale_kbpsvk); // KBP vs K

    endgame_initialized = true;
}

const EndgameEntry *endgame_probe(const Board *board) {
    if (!endgame_initialized) {
        endgame_init();
    }
    uint64_t key = board_material_key(board);
    size_t idx = (size_t)(key % ENDGAME_TABLE_SIZE);
    size_t orig_idx = idx;

    while (endgame_table[idx].is_valid) {
        if (endgame_table[idx].key == key) {
            return &endgame_table[idx];
        }
        idx = (idx + 1) % ENDGAME_TABLE_SIZE;
        if (idx == orig_idx) break;
    }
    return NULL;
}

// Opposite coloured bishops
bool is_ocb(const Board *board) {
    U64 white_bishops = board->pieces[WHITE_BISHOP];
    U64 black_bishops = board->pieces[BLACK_BISHOP];

    if (__builtin_popcountll(white_bishops) != 1 || __builtin_popcountll(black_bishops) != 1) {
        return false;
    }

    U64 other_pieces = board->pieces[WHITE_KNIGHT] | board->pieces[BLACK_KNIGHT] |
                       board->pieces[WHITE_ROOK]   | board->pieces[BLACK_ROOK]   |
                       board->pieces[WHITE_QUEEN]  | board->pieces[BLACK_QUEEN];
    if (other_pieces != 0) {
        return false;
    }

    int w_sq = __builtin_ctzll(white_bishops);
    int b_sq = __builtin_ctzll(black_bishops);

    return is_dark_square(w_sq) != is_dark_square(b_sq);
}

// Legally drawn endgame
bool is_material_draw(const Board *board) {
    U64 pawns = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
    if (pawns != 0) return false;

    U64 queens = board->pieces[WHITE_QUEEN] | board->pieces[BLACK_QUEEN];
    U64 rooks  = board->pieces[WHITE_ROOK]  | board->pieces[BLACK_ROOK];
    if (queens != 0 || rooks != 0) return false;

    U64 w_minors = board->pieces[WHITE_KNIGHT] | board->pieces[WHITE_BISHOP];
    U64 b_minors = board->pieces[BLACK_KNIGHT] | board->pieces[BLACK_BISHOP];

    int w_count = __builtin_popcountll(w_minors);
    int b_count = __builtin_popcountll(b_minors);

    // K vs K
    if (w_count == 0 && b_count == 0) return true;

    // KB vs K, KN vs K, KNN vs K
    if ((w_count <= 2 && b_count == 0) || (b_count <= 2 && w_count == 0)) {
        if (w_count == 2) {
            if (board->pieces[WHITE_BISHOP] != 0 && board->pieces[WHITE_KNIGHT] != 0) return false; // KBN vs K is winning
            if (__builtin_popcountll(board->pieces[WHITE_BISHOP]) == 2) return false; // KBB vs K is winning
            return true; // KNN vs K is draw
        }
        if (b_count == 2) {
            if (board->pieces[BLACK_BISHOP] != 0 && board->pieces[BLACK_KNIGHT] != 0) return false;
            if (__builtin_popcountll(board->pieces[BLACK_BISHOP]) == 2) return false;
            return true;
        }
        return true; // KB vs K or KN vs K
    }

    // 1 minor vs 1 minor (KB vs KB, KN vs KN, KB vs KN)
    if (w_count == 1 && b_count == 1) return true;

    return false;
}
