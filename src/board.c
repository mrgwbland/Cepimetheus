#include "board.h"
#include "movegen.h"
#include "../include/zobrist.h"
#include <assert.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int piece_for_side_at_type(int side, int type) {
    return side == WHITE ? type : type + 6;
}

static int file_of(int square) {
    return square & 7;
}

static int rank_of(int square) {
    return square >> 3;
}

static int piece_index_for_char(char piece_char) {
    switch (piece_char) {
        case 'P': return WHITE_PAWN;
        case 'N': return WHITE_KNIGHT;
        case 'B': return WHITE_BISHOP;
        case 'R': return WHITE_ROOK;
        case 'Q': return WHITE_QUEEN;
        case 'K': return WHITE_KING;
        case 'p': return BLACK_PAWN;
        case 'n': return BLACK_KNIGHT;
        case 'b': return BLACK_BISHOP;
        case 'r': return BLACK_ROOK;
        case 'q': return BLACK_QUEEN;
        case 'k': return BLACK_KING;
        default: return -1;
    }
}

static int promotion_piece(int side, int promotion) {
    switch (promotion) {
        case MOVE_PROMO_KNIGHT: return side == WHITE ? WHITE_KNIGHT : BLACK_KNIGHT;
        case MOVE_PROMO_BISHOP: return side == WHITE ? WHITE_BISHOP : BLACK_BISHOP;
        case MOVE_PROMO_ROOK: return side == WHITE ? WHITE_ROOK : BLACK_ROOK;
        case MOVE_PROMO_QUEEN: return side == WHITE ? WHITE_QUEEN : BLACK_QUEEN;
        default: return -1;
    }
}

static int rook_from_castle_square(int side, int to_square) {
    if (side == WHITE) {
        return to_square == 6 ? 7 : 0;
    }
    return to_square == 62 ? 63 : 56;
}

static int rook_to_castle_square(int side, int to_square) {
    if (side == WHITE) {
        return to_square == 6 ? 5 : 3;
    }
    return to_square == 62 ? 61 : 59;
}

static void board_sync_occupancy(Board *board) {
    board->occupancy[WHITE] = 0;
    board->occupancy[BLACK] = 0;
    for (int piece = 0; piece < PIECE_NB; ++piece) {
        if (piece < BLACK_PAWN) {
            board->occupancy[WHITE] |= board->pieces[piece];
        } else {
            board->occupancy[BLACK] |= board->pieces[piece];
        }
    }
    board->occupancy[BOTH] = board->occupancy[WHITE] | board->occupancy[BLACK];
}

static void board_sync_kings(Board *board) {
    U64 white_king = board->pieces[WHITE_KING];
    U64 black_king = board->pieces[BLACK_KING];
    board->king_square[WHITE] = white_king ? bitboard_pop_lsb(&white_king) : -1;
    board->king_square[BLACK] = black_king ? bitboard_pop_lsb(&black_king) : -1;
}


void repetition_history_init(RepetitionHistory *history) {
    if (history != NULL) {
        history->count = 0;
    }
}

bool repetition_history_push(RepetitionHistory *history, U64 key) {
    if (history == NULL || history->count >= REPETITION_HISTORY_MAX) {
        return false;
    }

    history->keys[history->count++] = key;
    return true;
}

static bool board_has_en_passant_capture(const Board *board) {
    if (board == NULL || board->ep_square < 0 || board->ep_square >= 64) {
        return false;
    }

    int ep_square = board->ep_square;
    int file = file_of(ep_square);
    int rank = rank_of(ep_square);

    if (board->side == WHITE) {
        int captured_square = ep_square - 8;
        if (file > 0 && rank > 0 &&
            (board->pieces[WHITE_PAWN] & (1ULL << (ep_square - 9))) &&
            (captured_square >= 0 && (board->pieces[BLACK_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
        if (file < 7 && rank > 0 &&
            (board->pieces[WHITE_PAWN] & (1ULL << (ep_square - 7))) &&
            (captured_square >= 0 && (board->pieces[BLACK_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
    } else {
        int captured_square = ep_square + 8;
        if (file > 0 && rank < 7 &&
            (board->pieces[BLACK_PAWN] & (1ULL << (ep_square + 7))) &&
            (captured_square < 64 && (board->pieces[WHITE_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
        if (file < 7 && rank < 7 &&
            (board->pieces[BLACK_PAWN] & (1ULL << (ep_square + 9))) &&
            (captured_square < 64 && (board->pieces[WHITE_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
    }

    return false;
}

static bool board_has_valid_en_passant_target(const Board *board, int ep_square, int side) {
    if (board == NULL || ep_square < 0 || ep_square >= 64) {
        return false;
    }

    int file = file_of(ep_square);
    int rank = rank_of(ep_square);

    if (side == WHITE) {
        int captured_square = ep_square - 8;
        if (file > 0 && rank > 0 &&
            (board->pieces[WHITE_PAWN] & (1ULL << (ep_square - 9))) &&
            (captured_square >= 0 && (board->pieces[BLACK_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
        if (file < 7 && rank > 0 &&
            (board->pieces[WHITE_PAWN] & (1ULL << (ep_square - 7))) &&
            (captured_square >= 0 && (board->pieces[BLACK_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
    } else {
        int captured_square = ep_square + 8;
        if (file > 0 && rank < 7 &&
            (board->pieces[BLACK_PAWN] & (1ULL << (ep_square + 7))) &&
            (captured_square < 64 && (board->pieces[WHITE_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
        if (file < 7 && rank < 7 &&
            (board->pieces[BLACK_PAWN] & (1ULL << (ep_square + 9))) &&
            (captured_square < 64 && (board->pieces[WHITE_PAWN] & (1ULL << captured_square)))) {
            return true;
        }
    }

    return false;
}

U64 board_position_key(const Board *board) {
    if (board == NULL) {
        return 0ULL;
    }
    return board->hash;
}

// Checks for 50-move rule, threefold repetition, and Lichess 300-move rule if enabled; stalemate should be checked when legal moves are already generated
bool board_is_draw(const Board *board, const RepetitionHistory *history, bool lichess_draw_rules) {
    if (board == NULL) {
        return false;
    }

    if (board->halfmove_clock >= 100) {
        return true;
    }

    // Lichess draw rule: draw if 300 moves (600 ply) have been played
    if (lichess_draw_rules && history != NULL && history->count > 600) {
        return true;
    }

    U64 current_key = board_position_key(board);
    if (history != NULL && history->count > 1) {
        int start = 0;
        int history_limit = history->count - 1;
        int halfmove_limit = history->count - 1 - board->halfmove_clock;

        if (halfmove_limit > start) {
            start = halfmove_limit;
        }

        // Declare draw on threefold repetition (requires two prior identical positions)
        int matches = 0;
        for (int i = start; i < history_limit; ++i) {
            if (history->keys[i] == current_key) {
                ++matches;
                if (matches >= 2) {
                    return true;
                }
            }
        }
    }

    return false;
}

void board_clear(Board *board) {
    memset(board, 0, sizeof(*board));
    for (int i = 0; i < 64; ++i) {
        board->squares[i] = -1;
    }
    board->ep_square = -1;
    board->fullmove_number = 1;
    board->king_square[WHITE] = -1;
    board->king_square[BLACK] = -1;
}

void board_init(Board *board) {
    bitboard_init_tables();
    zobrist_init();
    board_clear(board);
    board_set_startpos(board);
}

void board_set_startpos(Board *board) {
    board_set_fen(board, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

int board_piece_color(int piece) {
    if (piece < 0) {
        return -1;
    }
    return piece < BLACK_PAWN ? WHITE : BLACK;
}

int board_piece_type(int piece) {
    if (piece < 0) {
        return -1;
    }
    return piece % 6;
}

int board_piece_at(const Board *board, int square) {
    return board->squares[square];
}

int board_parse_square(const char *text) {
    if (text == NULL || strlen(text) < 2) {
        return -1;
    }
    int file = tolower((unsigned char)text[0]) - 'a';
    int rank = text[1] - '1';
    if (file < 0 || file > 7 || rank < 0 || rank > 7) {
        return -1;
    }
    return rank * 8 + file;
}

void board_square_to_string(int square, char buffer[3]) {
    buffer[0] = (char)('a' + file_of(square));
    buffer[1] = (char)('1' + rank_of(square));
    buffer[2] = '\0';
}

static bool board_parse_fen_piece_placement(Board *board, const char *placement) {
    int rank = 7;
    int file = 0;

    for (const char *cursor = placement; *cursor; ++cursor) {
        char c = *cursor;
        if (c == '/') {
            --rank;
            file = 0;
            if (rank < 0) {
                return false;
            }
            continue;
        }
        if (isdigit((unsigned char)c)) {
            file += c - '0';
            if (file > 8) {
                return false;
            }
            continue;
        }
        int piece = piece_index_for_char(c);
        if (piece < 0 || file > 7 || rank < 0) {
            return false;
        }
        int square = rank * 8 + file;
        board->pieces[piece] |= 1ULL << square;
        board->squares[square] = piece;
        if (piece == WHITE_KING) {
            board->king_square[WHITE] = square;
        } else if (piece == BLACK_KING) {
            board->king_square[BLACK] = square;
        }
        ++file;
        if (file > 8) {
            return false;
        }
    }

    return rank == 0 && file == 8;
}

bool board_set_fen(Board *board, const char *fen) {
    if (board == NULL || fen == NULL) {
        return false;
    }

    char fen_copy[512];
    if (strlen(fen) >= sizeof(fen_copy)) {
        return false;
    }

    strcpy(fen_copy, fen);
    board_clear(board);

    char *tokens[6] = {0};
    int token_count = 0;
    for (char *token = strtok(fen_copy, " \t\r\n"); token != NULL && token_count < 6; token = strtok(NULL, " \t\r\n")) {
        tokens[token_count++] = token;
    }

    if (token_count < 4) {
        return false;
    }

    if (!board_parse_fen_piece_placement(board, tokens[0])) {
        return false;
    }

    if (strcmp(tokens[1], "w") == 0) {
        board->side = WHITE;
    } else if (strcmp(tokens[1], "b") == 0) {
        board->side = BLACK;
    } else {
        return false;
    }

    if (strcmp(tokens[2], "-") != 0) {
        if (strchr(tokens[2], 'K') != NULL) {
            board->castling_rights |= CASTLE_WHITE_KING;
        }
        if (strchr(tokens[2], 'Q') != NULL) {
            board->castling_rights |= CASTLE_WHITE_QUEEN;
        }
        if (strchr(tokens[2], 'k') != NULL) {
            board->castling_rights |= CASTLE_BLACK_KING;
        }
        if (strchr(tokens[2], 'q') != NULL) {
            board->castling_rights |= CASTLE_BLACK_QUEEN;
        }
    }

    if (strcmp(tokens[3], "-") == 0) {
        board->ep_square = -1;
    } else {
        board->ep_square = board_parse_square(tokens[3]);
        if (board->ep_square < 0) {
            return false;
        }
    }
    if (!board_has_valid_en_passant_target(board, board->ep_square, board->side)) {
        board->ep_square = -1;
    }

    if (token_count >= 5) {
        board->halfmove_clock = atoi(tokens[4]);
    }
    if (token_count >= 6) {
        board->fullmove_number = atoi(tokens[5]);
    }

    board_sync_occupancy(board);
    board_sync_kings(board);
    board->hash = zobrist_hash_full(board);
    return board->king_square[WHITE] >= 0 && board->king_square[BLACK] >= 0;
}

bool board_is_square_attacked_with_occupancy(const Board *board, int square, int attacker_side, U64 occupancy) {
    if (square < 0 || square >= 64) {
        return false;
    }

    int file = file_of(square);
    int rank = rank_of(square);

    if (attacker_side == WHITE) {
        if (file > 0 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 9)))) {
            return true;
        }
        if (file < 7 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 7)))) {
            return true;
        }
    } else {
        if (file > 0 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 7)))) {
            return true;
        }
        if (file < 7 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 9)))) {
            return true;
        }
    }

    if (bitboard_knight_attacks(square) & (attacker_side == WHITE ? board->pieces[WHITE_KNIGHT] : board->pieces[BLACK_KNIGHT])) {
        return true;
    }
    if (bitboard_king_attacks(square) & (attacker_side == WHITE ? board->pieces[WHITE_KING] : board->pieces[BLACK_KING])) {
        return true;
    }

    U64 bishops = attacker_side == WHITE ? (board->pieces[WHITE_BISHOP] | board->pieces[WHITE_QUEEN]) : (board->pieces[BLACK_BISHOP] | board->pieces[BLACK_QUEEN]);
    U64 rooks = attacker_side == WHITE ? (board->pieces[WHITE_ROOK] | board->pieces[WHITE_QUEEN]) : (board->pieces[BLACK_ROOK] | board->pieces[BLACK_QUEEN]);
    if (bitboard_bishop_attacks(square, occupancy) & bishops) {
        return true;
    }
    if (bitboard_rook_attacks(square, occupancy) & rooks) {
        return true;
    }

    return false;
}

bool board_is_square_attacked(const Board *board, int square, int attacker_side) {
    return board_is_square_attacked_with_occupancy(board, square, attacker_side, board->occupancy[BOTH]);
}

bool board_is_in_check(const Board *board, int side) {
    int king_square = board->king_square[side];
    if (king_square < 0) {
        return false;
    }
    return board_is_square_attacked(board, king_square, side ^ 1);
}

U64 board_checkers(const Board *board, int side) {
    int king_sq = board->king_square[side];
    if (king_sq < 0) return 0ULL;

    int enemy = side ^ 1;
    U64 occupied = board->occupancy[BOTH];
    U64 checkers = 0ULL;

    // Check pawns
    U64 pawns = board->pieces[piece_for_side_at_type(enemy, 0)]; // Pawn
    checkers |= bitboard_pawn_attacks(side, king_sq) & pawns;

    // Check knights
    U64 knights = board->pieces[piece_for_side_at_type(enemy, 1)]; // Knight
    checkers |= bitboard_knight_attacks(king_sq) & knights;

    // Check bishops/queens
    U64 bishops = board->pieces[piece_for_side_at_type(enemy, 2)] |
                  board->pieces[piece_for_side_at_type(enemy, 4)]; // Bishop / Queen
    checkers |= bitboard_bishop_attacks(king_sq, occupied) & bishops;

    // Check rooks/queens
    U64 rooks = board->pieces[piece_for_side_at_type(enemy, 3)] |
                board->pieces[piece_for_side_at_type(enemy, 4)]; // Rook / Queen
    checkers |= bitboard_rook_attacks(king_sq, occupied) & rooks;

    return checkers;
}

U64 board_pinned_mask(const Board *board, int side) {
    int king_sq = board->king_square[side];
    if (king_sq < 0) return 0ULL;

    int enemy = side ^ 1;
    U64 us = board->occupancy[side];
    U64 occupied = board->occupancy[BOTH];

    U64 enemy_queens = board->pieces[piece_for_side_at_type(enemy, 4)];
    U64 enemy_rooks = board->pieces[piece_for_side_at_type(enemy, 3)] | enemy_queens;
    U64 enemy_bishops = board->pieces[piece_for_side_at_type(enemy, 2)] | enemy_queens;

    // attackers aligned with king square on empty board
    U64 attackers = (bitboard_rook_attacks(king_sq, 0ULL) & enemy_rooks)
                  | (bitboard_bishop_attacks(king_sq, 0ULL) & enemy_bishops);

    U64 pinned = 0ULL;
    while (attackers) {
        int attacker_sq = bitboard_pop_lsb(&attackers);
        U64 between = bitboard_in_between_mask(king_sq, attacker_sq) & occupied;
        
        // If there is exactly one blocker between king and attacker
        if (between && !(between & (between - 1))) {
            // Check if that blocker belongs to us
            if (between & us) {
                pinned |= between;
            }
        }
    }

    return pinned;
}

bool board_is_move_legal(const Board *board, Move move, U64 pinned_mask, U64 checkers) {
    if (board == NULL) {
        return false;
    }

    int from = move_from(move);
    int to = move_to(move);
    int side = board->side;
    int king_sq = board->king_square[side];
    int mover_piece = board_piece_at(board, from);
    if (mover_piece < 0) {
        return false;
    }
    int piece_type = board_piece_type(mover_piece);
    int flags = move_flags(move);

    // 1. King moves
    if (piece_type == 5) { // KING
        if (flags & MOVE_FLAG_CASTLE) {
            if (checkers != 0) return false;
            int enemy = side ^ 1;
            int step = to > from ? 1 : -1;
            U64 occupied = board->occupancy[BOTH];
            for (int sq = from + step; sq != to + step; sq += step) {
                if (board_is_square_attacked_with_occupancy(board, sq, enemy, occupied)) {
                    return false;
                }
            }
            return true;
        }

        U64 occ_after = board->occupancy[BOTH] ^ (1ULL << from);
        return !board_is_square_attacked_with_occupancy(board, to, side ^ 1, occ_after);
    }

    // 2. En Passant moves (discover check risk and check evasion verification)
    if (flags & MOVE_FLAG_EN_PASSANT) {
        int captured_sq = side == WHITE ? to - 8 : to + 8;
        if (checkers != 0) {
            if (checkers & (checkers - 1)) {
                return false; // double check, EP cannot resolve it
            }
            int checker_sq = __builtin_ctzll(checkers);
            if (checker_sq != captured_sq && !((1ULL << to) & bitboard_in_between_mask(king_sq, checker_sq))) {
                return false;
            }
        }

        U64 occ_after = (board->occupancy[BOTH] ^ (1ULL << from) ^ (1ULL << captured_sq)) | (1ULL << to);
        
        U64 bishop_attacks = bitboard_bishop_attacks(king_sq, occ_after);
        U64 rook_attacks = bitboard_rook_attacks(king_sq, occ_after);
        
        int enemy = side ^ 1;
        U64 enemy_bishops = board->pieces[piece_for_side_at_type(enemy, 2)] | board->pieces[piece_for_side_at_type(enemy, 4)];
        U64 enemy_rooks = board->pieces[piece_for_side_at_type(enemy, 3)] | board->pieces[piece_for_side_at_type(enemy, 4)];
        
        if ((bishop_attacks & enemy_bishops) || (rook_attacks & enemy_rooks)) {
            return false;
        }
        return true;
    }

    // 3. In Check handling (non-king pieces)
    if (checkers != 0) {
        if (pinned_mask & (1ULL << from)) {
            return false; // A pinned piece can never block/capture a check on a different ray
        }
        if (checkers & (checkers - 1)) {
            return false; // double check, only king move is legal
        }
        int checker_sq = __builtin_ctzll(checkers);
        U64 target_mask = checkers | bitboard_in_between_mask(king_sq, checker_sq);
        return (target_mask & (1ULL << to)) != 0;
    }

    // 4. Pinned pieces
    if (pinned_mask & (1ULL << from)) {
        return (bitboard_line_mask(king_sq, from) & (1ULL << to)) != 0;
    }

    return true;
}

static inline void remove_piece_at(Board *board, int piece, int square) {
    U64 mask = 1ULL << square;
    board->pieces[piece] &= ~mask;
    board->squares[square] = -1;

    int side = (piece >= BLACK_PAWN);
    board->occupancy[side] &= ~mask;
    board->occupancy[BOTH] &= ~mask;

    if (piece == WHITE_KING) {
        board->king_square[WHITE] = -1;
    } else if (piece == BLACK_KING) {
        board->king_square[BLACK] = -1;
    }
}

static inline void add_piece_at(Board *board, int piece, int square) {
    U64 mask = 1ULL << square;
    board->pieces[piece] |= mask;
    board->squares[square] = piece;

    int side = (piece >= BLACK_PAWN);
    board->occupancy[side] |= mask;
    board->occupancy[BOTH] |= mask;

    if (piece == WHITE_KING) {
        board->king_square[WHITE] = square;
    } else if (piece == BLACK_KING) {
        board->king_square[BLACK] = square;
    }
}


static void clear_castling_rights_for_square(Board *board, int square, int piece) {
    if (piece == WHITE_KING) {
        board->castling_rights &= ~(CASTLE_WHITE_KING | CASTLE_WHITE_QUEEN);
        return;
    }
    if (piece == BLACK_KING) {
        board->castling_rights &= ~(CASTLE_BLACK_KING | CASTLE_BLACK_QUEEN);
        return;
    }
    if (piece == WHITE_ROOK) {
        if (square == 0) {
            board->castling_rights &= ~CASTLE_WHITE_QUEEN;
        } else if (square == 7) {
            board->castling_rights &= ~CASTLE_WHITE_KING;
        }
    } else if (piece == BLACK_ROOK) {
        if (square == 56) {
            board->castling_rights &= ~CASTLE_BLACK_QUEEN;
        } else if (square == 63) {
            board->castling_rights &= ~CASTLE_BLACK_KING;
        }
    }
}

uint64_t zobrist_hash_full(const Board *board) {
    if (board == NULL) {
        return 0ULL;
    }

    uint64_t hash = 0ULL;

    for (int piece = 0; piece < PIECE_NB; ++piece) {
        U64 bb = board->pieces[piece];
        while (bb) {
            int sq = bitboard_pop_lsb(&bb);
            hash ^= ZOBRIST_PIECES[piece][sq];
        }
    }

    if (board->side == BLACK) {
        hash ^= ZOBRIST_SIDE_KEY;
    }

    hash ^= ZOBRIST_CASTLE_KEYS[board->castling_rights];

    if (board_has_en_passant_capture(board)) {
        hash ^= ZOBRIST_EP_KEYS[file_of(board->ep_square)];
    }

    return hash;
}

void board_unmake_move(Board *board, const Undo *undo) {
    if (board == NULL || undo == NULL) {
        return;
    }

    if (undo->move == MOVE_NONE) {
        int original_side = board->side ^ 1;
        if (original_side == BLACK) {
            --board->fullmove_number;
        }
        board->side = original_side;
        board->castling_rights = undo->castling_rights;
        board->ep_square = undo->ep_square;
        board->halfmove_clock = undo->halfmove_clock;
        board->hash = undo->hash;
        return;
    }

    Move move = undo->move;
    int from = move_from(move);
    int to = move_to(move);
    int promotion = move_promotion(move);
    int flags = move_flags(move);
    
    int original_side = board->side ^ 1;
    int moved_piece = board_piece_at(board, to);

    int original_piece = moved_piece;
    if (board_piece_type(moved_piece) != 0 && promotion != MOVE_PROMO_NONE) {
        original_piece = piece_for_side_at_type(original_side, 0); // Pawn
    }

    remove_piece_at(board, moved_piece, to);
    add_piece_at(board, original_piece, from);

    if (undo->captured_piece >= 0) {
        int captured_square = to;
        if (flags & MOVE_FLAG_EN_PASSANT) {
            captured_square = original_side == WHITE ? to - 8 : to + 8;
        }
        add_piece_at(board, undo->captured_piece, captured_square);
    }

    if (flags & MOVE_FLAG_CASTLE) {
        int rook_from = rook_from_castle_square(original_side, to);
        int rook_to = rook_to_castle_square(original_side, to);
        int rook_piece = piece_for_side_at_type(original_side, 3);
        remove_piece_at(board, rook_piece, rook_to);
        add_piece_at(board, rook_piece, rook_from);
    }

    if (original_side == BLACK) {
        --board->fullmove_number;
    }

    board->side = original_side;
    board->castling_rights = undo->castling_rights;
    board->ep_square = undo->ep_square;
    board->halfmove_clock = undo->halfmove_clock;
    board->hash = undo->hash;
}

bool board_make_move(Board *board, Move move, Undo *undo) {
    if (board == NULL || undo == NULL) {
        return false;
    }

    U64 checkers = board_checkers(board, board->side);
    U64 pinned = board_pinned_mask(board, board->side);
    if (!board_is_move_legal(board, move, pinned, checkers)) {
        return false;
    }

    undo->hash = board->hash;
    undo->castling_rights = board->castling_rights;
    undo->ep_square = board->ep_square;
    undo->halfmove_clock = board->halfmove_clock;
    undo->move = move;

    int from = move_from(move);
    int to = move_to(move);
    int promotion = move_promotion(move);
    int flags = move_flags(move);
    int side = board->side;
    int mover_piece = board_piece_at(board, from);

    if (mover_piece < 0 || board_piece_color(mover_piece) != side) {
        return false;
    }

    int target_piece = board_piece_at(board, to);
    int piece_type = board_piece_type(mover_piece);
    int piece_to_move = mover_piece;
    int captured_square = to;

    // XOR out old EP key if any
    if (board_has_en_passant_capture(board)) {
        board->hash ^= ZOBRIST_EP_KEYS[file_of(board->ep_square)];
    }

    // XOR out old castling rights
    board->hash ^= ZOBRIST_CASTLE_KEYS[board->castling_rights];

    // XOR out mover from source square
    board->hash ^= ZOBRIST_PIECES[mover_piece][from];

    int captured_piece = -1;
    if (flags & MOVE_FLAG_EN_PASSANT) {
        captured_square = side == WHITE ? to - 8 : to + 8;
        captured_piece = piece_for_side_at_type(side ^ 1, 0); // opponent pawn
        if (to != board->ep_square || target_piece >= 0 || captured_square < 0 || captured_square >= 64 ||
            board_piece_at(board, captured_square) != captured_piece) {
            return false;
        }
        remove_piece_at(board, captured_piece, captured_square);
        board->hash ^= ZOBRIST_PIECES[captured_piece][captured_square];
    } else if (target_piece >= 0) {
        captured_piece = target_piece;
        remove_piece_at(board, target_piece, to);
        board->hash ^= ZOBRIST_PIECES[target_piece][to];
    }
    undo->captured_piece = captured_piece;

    remove_piece_at(board, mover_piece, from);

    if (flags & MOVE_FLAG_CASTLE) {
        int rook_from = rook_from_castle_square(side, to);
        int rook_to = rook_to_castle_square(side, to);
        int rook_piece = piece_for_side_at_type(side, 3);
        remove_piece_at(board, rook_piece, rook_from);
        add_piece_at(board, rook_piece, rook_to);
        board->hash ^= ZOBRIST_PIECES[rook_piece][rook_from];
        board->hash ^= ZOBRIST_PIECES[rook_piece][rook_to];
    }

    if (piece_type == 0 && promotion != MOVE_PROMO_NONE) {
        piece_to_move = promotion_piece(side, promotion);
        if (piece_to_move < 0) {
            return false;
        }
    }

    add_piece_at(board, piece_to_move, to);
    board->hash ^= ZOBRIST_PIECES[piece_to_move][to];

    clear_castling_rights_for_square(board, from, mover_piece);
    if (target_piece >= 0) {
        clear_castling_rights_for_square(board, captured_square, target_piece);
    }

    board->ep_square = -1;
    if ((flags & MOVE_FLAG_DOUBLE_PAWN) != 0) {
        board->ep_square = side == WHITE ? from + 8 : from - 8;
    }

    if (piece_type == 0 || target_piece >= 0 || (flags & MOVE_FLAG_EN_PASSANT) != 0) {
        board->halfmove_clock = 0;
    } else {
        ++board->halfmove_clock;
    }

    if (side == BLACK) {
        ++board->fullmove_number;
    }

    board->side ^= 1;

    // XOR in new castling rights
    board->hash ^= ZOBRIST_CASTLE_KEYS[board->castling_rights];

    // XOR in side key (since side changes)
    board->hash ^= ZOBRIST_SIDE_KEY;

    // XOR in new EP key if any
    if (board_has_en_passant_capture(board)) {
        board->hash ^= ZOBRIST_EP_KEYS[file_of(board->ep_square)];
    }

    return true;
}

void board_make_null_move(Board *board, Undo *undo) {
    if (board == NULL || undo == NULL) {
        return;
    }

    undo->hash = board->hash;
    undo->castling_rights = board->castling_rights;
    undo->ep_square = board->ep_square;
    undo->halfmove_clock = board->halfmove_clock;
    undo->captured_piece = -1;
    undo->move = MOVE_NONE;

    if (board_has_en_passant_capture(board)) {
        board->hash ^= ZOBRIST_EP_KEYS[file_of(board->ep_square)];
    }

    if (board->side == BLACK) {
        ++board->fullmove_number;
    }
    board->side ^= 1;
    board->ep_square = -1;
    ++board->halfmove_clock;

    board->hash ^= ZOBRIST_SIDE_KEY;
}
