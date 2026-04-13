#include "think.h"
#include <stdio.h>
#include "float.h"
#include <limits.h>
#include <stdlib.h>
#include <time.h>

#define MAX_PV_MOVES 128

typedef struct {
    float score;
    Move move;
    Move pv[MAX_PV_MOVES];
    int pv_length;
} SearchResult;

typedef struct {
    Move move;
    int score;
} ScoredMove;

typedef struct {
    unsigned long long nodes;
    int seldepth;
} SearchStats;

/* File masks - one per file (A-H) */
static const U64 file_masks[8] = {
    0x0101010101010101ULL, /* A-file */
    0x0202020202020202ULL, /* B-file */
    0x0404040404040404ULL, /* C-file */
    0x0808080808080808ULL, /* D-file */
    0x1010101010101010ULL, /* E-file */
    0x2020202020202020ULL, /* F-file */
    0x4040404040404040ULL, /* G-file */
    0x8080808080808080ULL   /* H-file */
};

/* Rank masks - one per rank (1-8) */
static const U64 rank_masks[8] = {
    0x00000000000000FFULL, /* Rank 1 */
    0x000000000000FF00ULL, /* Rank 2 */
    0x0000000000FF0000ULL, /* Rank 3 */
    0x00000000FF000000ULL, /* Rank 4 */
    0x000000FF00000000ULL, /* Rank 5 */
    0x0000FF0000000000ULL, /* Rank 6 */
    0x00FF000000000000ULL, /* Rank 7 */
    0xFF00000000000000ULL   /* Rank 8 */
};

static int score_to_cp(float score) {
    if (score >= 0.0f) {
        return (int)(score + 0.5f);
    }

    return (int)(score - 0.5f);
}

static void print_move_info(int depth, int move_number, Move move, float score) {
    char move_buffer[6];
    move_to_string(move, move_buffer);
    printf("info depth %d currmove %s currmovenumber %d score cp %d\n",
           depth,
           move_buffer,
           move_number,
           score_to_cp(score));
    fflush(stdout);
}

static long long current_time_ms(void) {
    clock_t now = clock();
    if (now < 0) {
        return 0;
    }

    return (long long)((double)now * 1000.0 / (double)CLOCKS_PER_SEC);
}

static unsigned long long compute_nps(unsigned long long nodes, long long elapsed_ms) {
    if (elapsed_ms <= 0) {
        return nodes * 1000ULL;
    }

    return (nodes * 1000ULL) / (unsigned long long)elapsed_ms;
}

static int random_index(int upper_bound) {
    if (upper_bound <= 1) {
        return 0;
    }

    return rand() % upper_bound;
}

static void print_depth_info(int depth, const SearchResult *result, const SearchStats *stats, long long elapsed_ms) {
    unsigned long long nps = compute_nps(stats->nodes, elapsed_ms);
    printf("info depth %d seldepth %d score cp %d nodes %llu nps %llu time %lld",
           depth,
           stats->seldepth,
           score_to_cp(result->score),
           stats->nodes,
           nps,
           elapsed_ms);

    if (result->pv_length > 0) {
        printf(" pv");
        for (int i = 0; i < result->pv_length; ++i) {
            char move_buffer[6];
            move_to_string(result->pv[i], move_buffer);
            printf(" %s", move_buffer);
        }
    }

    printf("\n");
    fflush(stdout);
}

static const int piece_values[6] = {
    100, /* Pawn */
    310, /* Knight */
    320, /* Bishop */
    500, /* Rook */
    950, /* Queen */
    0    /* King */
};

/* Count the number of set bits in a 64-bit bitboard. */
static int popcount_u64(U64 bb) {
    int count = 0;

    /* Repeatedly remove the least-significant set bit until empty. */
    while (bb) {
        bitboard_pop_lsb(&bb);
        ++count;
    }

    return count;
}

static int rank_of(int square) {
    return square >> 3;
}

static int file_of(int square) {
    return square & 7;
}

static void count_pawns_per_file(U64 pawns, int pawns_per_file[8]) {
    for (int i = 0; i < 8; ++i) {
        pawns_per_file[i] = 0;
    }

    for (int f = 0; f < 8; ++f) {
    pawns_per_file[f] = popcount_u64(pawns & file_masks[f]);
    }
}

/* Get mask for all ranks ahead of given rank (for white pawns: rank+1 to 7) */
static U64 get_ranks_ahead_white(int rank) {
    if (rank >= 7) return 0;
    return 0xFFFFFFFFFFFFFFFFULL << ((rank + 1) * 8);
}

/* Get mask for all ranks ahead of given rank (for black pawns: 0 to rank-1) */
static U64 get_ranks_ahead_black(int rank) {
    if (rank <= 0) return 0;
    return (1ULL << (rank * 8)) - 1;
}

static void mark_passed_pawns(const Board *board, int side, bool passed_pawns[64]) {
    for (int i = 0; i < 64; ++i) {
        passed_pawns[i] = false;
    }

    U64 own_pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    U64 enemy_pawns = board->pieces[side == WHITE ? BLACK_PAWN : WHITE_PAWN];

    U64 bb = own_pawns;
    while (bb) {
        int square = bitboard_pop_lsb(&bb);
        int file = file_of(square);
        int rank = rank_of(square);
        
        /* Create mask for current file and adjacent files */
        U64 check_files = 0;
        if (file > 0) check_files |= file_masks[file - 1];
        check_files |= file_masks[file];
        if (file < 7) check_files |= file_masks[file + 1];
        
        /* Get mask for ranks ahead of this pawn */
        U64 ranks_ahead = (side == WHITE) ? get_ranks_ahead_white(rank) : get_ranks_ahead_black(rank);
        
        /* Check if any enemy pawns exist in the check region */
        if ((enemy_pawns & check_files & ranks_ahead) == 0) {
            passed_pawns[square] = true;
        }
    }
}

static float evaluate_piece(const Board *board,
                            int piece,
                            int square,
                            int endgame,
                            const bool passed_pawns[64],
                            const int white_pawns_per_file[8],
                            const int black_pawns_per_file[8]) {
    int side = board_piece_color(piece);
    int type = board_piece_type(piece);
    int file = file_of(square);
    int rank = rank_of(square);
    bool is_white = (side == WHITE);
    float piece_value = (float)piece_values[type];

    if (type == WHITE_PAWN) {
        int pawn_rank = is_white ? rank : (7 - rank);
        if (endgame == 1) { //Reward advanced pawns in the endgame
            piece_value += 2.0f * (float)pawn_rank;
            if (passed_pawns[square]) {
                //Passed pawns are further rewarded for advancement
                piece_value += 4.0f * (float)pawn_rank;
            }
        }

        const int *pawns_per_file = is_white ? white_pawns_per_file : black_pawns_per_file;
        int this_file_count = pawns_per_file[file];

        if (this_file_count > 1) {
            //Doubled pawn penalty: -5 points for each same colour pawn on the file
            piece_value -= 5.0f * (float)(this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right) {
            //Isolated pawn penalty: -10 points if no same colour pawns on adjacent files
            piece_value -= 10.0f;
        }

        return piece_value;
    }

    if (type == WHITE_KNIGHT) {
        //Knights on the rim are grim
        return piece_value + 3.0f * (float)popcount_u64(bitboard_knight_attacks(square));
    }

    if (type == WHITE_BISHOP) {
        //Slider attacks not blocked by pawns, so this rewards bishops with more mobility
        U64 pawn_occupancy = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
        return piece_value + 1.5f * (float)popcount_u64(bitboard_bishop_attacks(square, pawn_occupancy));
    }

    if (type == WHITE_ROOK) {
        if (endgame == -1) {//None endgame evaluation
            //Reward for squares controlled
            piece_value += 1.0f * (float)popcount_u64(bitboard_rook_attacks(square, board->occupancy[BOTH]));

            U64 all_pawns = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
            U64 file_mask = file_masks[file];
            //Open file bonus: +50 points if no pawns on the file
            if (popcount_u64(all_pawns & file_mask) == 0) {
                piece_value += 50.0f;
            }
        } else {
            //Rooks are better in the endgame
            piece_value += 100.0f;
        }

        return piece_value;
    }

    if (type == WHITE_QUEEN) {
        return piece_value + 0.5f * (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
    }
    // If nothing prior it is a king
    if (endgame == -1) {
        //In the opening/midgame, king safety is important. Penalise kings with more possible attacks against them.
        piece_value -= (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
    }
    //Calculate Manhattan distance to closest corner
    int distance_a1 = file + rank;
    int distance_h1 = (7 - file) + rank;
    int distance_a8 = file + (7 - rank);
    int distance_h8 = (7 - file) + (7 - rank);
    int corner_distance = distance_a1;
    if (distance_h1 < corner_distance) {
        corner_distance = distance_h1;
    }
    if (distance_a8 < corner_distance) {
        corner_distance = distance_a8;
    }
    if (distance_h8 < corner_distance) {
        corner_distance = distance_h8;
    }

    //If endgame then favour activity over safety
    //Penalty in the opening/middlegame becomes reward in the endgame
    piece_value += (float)(endgame * corner_distance * 3);
    return piece_value;
}

static int count_pieces(Board *board) {
    if (board == NULL) {
        return 0;
    }

    int count = 0;
    for (int piece = 0; piece < PIECE_NB; ++piece) {
        count += popcount_u64(board->pieces[piece]);
    }
    return count;
}

static bool is_endgame_position(const Board *board) {
    if (board == NULL) {
        return false;
    }

    int non_king_count = 0;
    non_king_count += popcount_u64(board->pieces[WHITE_KNIGHT]);
    non_king_count += popcount_u64(board->pieces[WHITE_BISHOP]);
    non_king_count += popcount_u64(board->pieces[WHITE_ROOK]);
    non_king_count += popcount_u64(board->pieces[WHITE_QUEEN]);
    non_king_count += popcount_u64(board->pieces[BLACK_KNIGHT]);
    non_king_count += popcount_u64(board->pieces[BLACK_BISHOP]);
    non_king_count += popcount_u64(board->pieces[BLACK_ROOK]);
    non_king_count += popcount_u64(board->pieces[BLACK_QUEEN]);

    return non_king_count <= 4;
}

static int calculate_dynamic_depth(Board *board, const SearchLimits *limits) {
    int depth = 4;  /* Base depth. */

    /* +1 if in check. */
    if (board_is_in_check(board, board->side)) {
        depth++;
    }

    /* Count pieces on the board. */
    int piece_count = count_pieces(board);

    /* +1 if less than 15 pieces. */
    if (piece_count < 15) {
        depth++;
    }

    /* +1 again if less than 10 pieces. */
    if (piece_count < 10) {
        depth++;
    }

    /* -1 if we have less than a minute on the clock. */
    if (limits != NULL) {
        int current_time = (board->side == WHITE) ? limits->wtime_ms : limits->btime_ms;
        if (current_time > 0 && current_time < 60000) {
            depth--;
        }
    }

    /* Ensure depth is at least 1. */
    if (depth < 1) {
        depth = 1;
    }

    return depth;
}

static float evaluate(Board *board, const RepetitionHistory *history) {
    if (board == NULL) {
        return 0.0f;
    }

    if (board_is_draw(board, history)) {
        return 0.0f;
    }

    MoveList list;
    movegen_generate_legal(board, &list);

    if (list.count == 0) {
        /* No legal moves: checkmate or stalemate. */
        if (board_is_in_check(board, board->side)) {
            /* Checkmate: very bad for side to move. */
            return -100000.0f;
        } else {
            /* Stalemate: draw. */
            return 0.0f;
        }
    }

    int side_to_move = board->side;

    int endgame = is_endgame_position(board) ? 1 : -1;

    int white_pawns_per_file[8];
    int black_pawns_per_file[8];
    count_pawns_per_file(board->pieces[WHITE_PAWN], white_pawns_per_file);
    count_pawns_per_file(board->pieces[BLACK_PAWN], black_pawns_per_file);

    bool white_passed_pawns[64];
    bool black_passed_pawns[64];
    mark_passed_pawns(board, WHITE, white_passed_pawns);
    mark_passed_pawns(board, BLACK, black_passed_pawns);

    float white_score = 0.0f;
    float black_score = 0.0f;

    for (int piece = 0; piece < PIECE_NB; ++piece) {
        U64 bb = board->pieces[piece];
        while (bb) {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            const bool *passed = (side == WHITE) ? white_passed_pawns : black_passed_pawns;
            float value = evaluate_piece(board,
                                         piece,
                                         square,
                                         endgame,
                                         passed,
                                         white_pawns_per_file,
                                         black_pawns_per_file);

            if (side == WHITE) {
                white_score += value;
            } else {
                black_score += value;
            }
        }
    }
    //Unless the position is zugzwang, having a move will make the position better
    float tempo_bonus = (endgame == -1) ? 10.0f : 0.0f;

    if (side_to_move == WHITE) {
        return white_score - black_score + tempo_bonus;
    }

    return black_score - white_score + tempo_bonus;
}

/* Estimate move score for move ordering. This is intentionally cheap. */
static int estimate_move_score(Board *board, Move move) {
    int score = 0;

    /* Captures - MVV/LVA (Most Valuable Victim / Least Valuable Aggressor). */
    if (move_iscapture(board, move)) {
        int captured_piece = board_piece_at(board, move_to(move));
        int moving_piece = board_piece_at(board, move_from(move));
        
        if (captured_piece >= 0 && moving_piece >= 0) {
            int captured_value = piece_values[captured_piece % 6];
            int moving_value = piece_values[moving_piece % 6];
            int capture_value = captured_value - (moving_value / 100);
            score += 10000 + capture_value;
        }
    }

    /* Promotions. */
    if (move_promotion(move) != MOVE_PROMO_NONE) {
        score += 9000 + (move_promotion(move) * 100);
    }

    /* Castling. I saw online that this is too much computation to include in move ordering
    Keep the code in case we change our mind
    if ((move_flags(move) & MOVE_FLAG_CASTLE) != 0) {
        score += 1000;
    }
    */

    /* Check if move gives check. */
    Undo undo;
    if (board_make_move(board, move, &undo)) {
        if (board_is_in_check(board, board->side)) {
            score += 500;
        }
        board_unmake_move(board, &undo);
    }

    return score;
}

/* Compare function for qsort - sort in descending order of score. */
static int compare_scored_moves(const void *a, const void *b) {
    const ScoredMove *move_a = (const ScoredMove *)a;
    const ScoredMove *move_b = (const ScoredMove *)b;
    return move_b->score - move_a->score;
}

/* Quiescence search: only explores captures and checks. */
static float quiescence(Board *board,
                        float alpha,
                        float beta,
                        RepetitionHistory *history,
                        SearchStats *stats,
                        int ply) {
    ++stats->nodes;
    if (ply > stats->seldepth) {
        stats->seldepth = ply;
    }

    if (board_is_draw(board, history)) {
        return 0.0f;
    }

    /* Stand-pat: evaluate current position. */
    float stand_pat = evaluate(board, history);

    if (stand_pat >= beta) {
        return beta;
    }

    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    /* Generate legal moves. */
    MoveList list;
    movegen_generate_legal(board, &list);

    /* Build and sort list of captures and checks. */
    ScoredMove scored_moves[256];
    int scored_count = 0;
    for (int i = 0; i < list.count; ++i) {
        Move move = list.moves[i];
        if (move_iscapture(board, move) || move_ischeck(board, move)) {
            scored_moves[scored_count].move = move;
            scored_moves[scored_count].score = estimate_move_score(board, move);
            ++scored_count;
        }
    }

    /* Sort moves by estimated score. */
    qsort(scored_moves, scored_count, sizeof(ScoredMove), compare_scored_moves);

    /* Search moves. */
    for (int i = 0; i < scored_count; ++i) {
        Move move = scored_moves[i].move;
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        float score = -quiescence(board, -beta, -alpha, history, stats, ply + 1);

        --history->count;

        board_unmake_move(board, &undo);

        if (score >= beta) {
            return beta;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    return alpha;
}

/* Alpha-beta pruned negamax search. */
static SearchResult negamax(Board *board,
                            int depth,
                            float alpha,
                            float beta,
                            RepetitionHistory *history,
                            SearchStats *stats,
                            int ply) {
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0};

    ++stats->nodes;

    if (board_is_draw(board, history)) {
        result.score = 0.0f;
        return result;
    }

    /* Null-move pruning, avoided in endgames to avoid zugzwang issues. */
    if (depth >= 3 &&
        beta < 10000.0f &&
        !board_is_in_check(board, board->side) &&
        !is_endgame_position(board)) {
        const int reduction = 2;
        Undo undo;
        undo.snapshot = *board;

        if (board->side == BLACK) {
            ++board->fullmove_number;
        }
        board->side ^= 1;
        board->ep_square = -1;
        ++board->halfmove_clock;

        SearchResult null_child = negamax(board,
                          depth - 1 - reduction,
                          -beta,
                          -beta + 1.0f,
                          history,
                          stats,
                          ply + 1);
        float null_score = -null_child.score;

        board_unmake_move(board, &undo);

        if (null_score >= beta) {
            result.score = beta;
            return result;
        }
    }

    /* Leaf node: run quiescence search. */
    if (depth == 0) {
        result.score = quiescence(board, alpha, beta, history, stats, ply);
        return result;
    }

    /* Generate legal moves. */
    MoveList list;
    movegen_generate_legal(board, &list);

    /* No moves: run quiescence search on terminal position. */
    if (list.count == 0) {
        result.score = quiescence(board, alpha, beta, history, stats, ply);
        return result;
    }

    /* Build and sort move list. */
    ScoredMove scored_moves[256];
    for (int i = 0; i < list.count; ++i) {
        scored_moves[i].move = list.moves[i];
        scored_moves[i].score = estimate_move_score(board, list.moves[i]);
    }
    qsort(scored_moves, list.count, sizeof(ScoredMove), compare_scored_moves);

    /* Search moves with alpha-beta pruning. */
    for (int i = 0; i < list.count; ++i) {
        Move move = scored_moves[i].move;
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        /* Recurse with negated alpha and beta. */
        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, ply + 1);
        float score = -child.score;

        --history->count;

        board_unmake_move(board, &undo);

        /* Update best move and score. */
        if (score > result.score || result.move == MOVE_NONE) {
            result.score = score;
            result.move = move;
            result.pv[0] = move;
            result.pv_length = 1;
            for (int j = 0; j < child.pv_length && result.pv_length < MAX_PV_MOVES; ++j) {
                result.pv[result.pv_length++] = child.pv[j];
            }
        }

        /* Update alpha. */
        if (score > alpha) {
            alpha = score;
        }

        /* Beta cutoff: prune remaining siblings. */
        if (alpha >= beta) {
            break;
        }
    }

    return result;
}

static SearchResult search_root(Board *board, int depth, RepetitionHistory *history, SearchStats *stats) {
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0};
    float alpha = -FLT_MAX;
    float beta = FLT_MAX;
    SearchResult top_cp_candidates[256];
    int top_cp_candidate_count = 0;
    int best_cp = INT_MIN;

    MoveList list;
    movegen_generate_legal(board, &list);

    if (board_is_draw(board, history)) {
        result.score = 0.0f;
        if (list.count > 0) {
            result.move = list.moves[0];
            result.pv[0] = list.moves[0];
            result.pv_length = 1;
        }
        return result;
    }

    if (list.count == 0) {
        result.score = quiescence(board, alpha, beta, history, stats, 0);
        return result;
    }

    ScoredMove scored_moves[256];
    for (int i = 0; i < list.count; ++i) {
        scored_moves[i].move = list.moves[i];
        scored_moves[i].score = estimate_move_score(board, list.moves[i]);
    }
    qsort(scored_moves, list.count, sizeof(ScoredMove), compare_scored_moves);

    for (int i = 0; i < list.count; ++i) {
        Move move = scored_moves[i].move;
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, 1);
        float score = -child.score;

        --history->count;

        board_unmake_move(board, &undo);

        print_move_info(depth, i + 1, move, score);

        SearchResult current = {0.0f, MOVE_NONE, {0}, 0};
        current.score = score;
        current.move = move;
        current.pv[0] = move;
        current.pv_length = 1;
        for (int j = 0; j < child.pv_length && current.pv_length < MAX_PV_MOVES; ++j) {
            current.pv[current.pv_length++] = child.pv[j];
        }

        int score_cp = score_to_cp(score);
        if (score_cp > best_cp) {
            best_cp = score_cp;
            top_cp_candidate_count = 0;
            top_cp_candidates[top_cp_candidate_count++] = current;
        } else if (score_cp == best_cp && top_cp_candidate_count < 256) {
            top_cp_candidates[top_cp_candidate_count++] = current;
        }

        if (score > result.score || result.move == MOVE_NONE) {
            result = current;
        }

        if (score > alpha) {
            alpha = score;
        }

        if (alpha >= beta) {
            break;
        }
    }

    if (top_cp_candidate_count > 0) {
        result = top_cp_candidates[random_index(top_cp_candidate_count)];
    }

    return result;
}

Move think(Board *board, const SearchLimits *limits, const RepetitionHistory *history) {
    static int rng_seeded = 0;

    if (board == NULL) {
        return MOVE_NONE;
    }

    if (!rng_seeded) {
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)clock();
        srand(seed);
        rng_seeded = 1;
    }

    int depth = 0;
    if (limits != NULL && limits->depth > 0) {
        /* Explicit depth set via UCI. */
        depth = limits->depth;
    } else {
        /* Depth not explicitly set, calculate dynamically. */
        depth = calculate_dynamic_depth(board, limits);
    }

    RepetitionHistory search_history;
    repetition_history_init(&search_history);
    if (history != NULL) {
        search_history = *history;
    }

    SearchStats stats = {0ULL, depth};
    long long start_time_ms = current_time_ms();

    SearchResult result = {0.0f, MOVE_NONE, {0}, 0};
    result = search_root(board, depth, &search_history, &stats);

    long long elapsed_ms = current_time_ms() - start_time_ms;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }

    print_depth_info(depth, &result, &stats, elapsed_ms);

    if (result.move == MOVE_NONE) {
        return MOVE_NONE;
    }

    return result.move;
}