#include "uci.h"
#include "eval.h"
#include "search.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

static void push_current_position(Board *board, RepetitionHistory *history) {
    if (history == NULL) {
        return;
    }

    repetition_history_init(history);
    repetition_history_push(history, board->hash);
}

static void apply_uci_move(Board *board, RepetitionHistory *history, const char *move_text) {
    Move move;
    if (!movegen_find_legal_move(board, move_text, &move)) {
        return;
    }
    Undo undo;
    if (!board_make_move(board, move, &undo)) {
        return;
    }

    if (history != NULL) {
        repetition_history_push(history, board->hash);
    }
}

static void apply_position(Board *board, RepetitionHistory *history, char *line) {
    char *tokens[2<<12];
    int token_count = 0;
    for (char *token = strtok(line, " \t\r\n"); token != NULL && token_count < (2<<12); token = strtok(NULL, " \t\r\n")) {
        tokens[token_count++] = token;
    }

    if (token_count < 2) {
        return;
    }

    int move_start = 0;
    if (strcmp(tokens[1], "startpos") == 0) {
        board_set_startpos(board);
        push_current_position(board, history);
        move_start = 2;
    } else if (strcmp(tokens[1], "fen") == 0) {
        int fen_end = 2;
        while (fen_end < token_count && fen_end < 2 + 6 && strcmp(tokens[fen_end], "moves") != 0) {
            fen_end++;
        }
        if (fen_end - 2 < 4) {
            return;
        }
        char fen[512] = {0};
        int offset = 0;
        for (int i = 2; i < fen_end; ++i) {
            offset += snprintf(fen + offset, sizeof(fen) - offset, "%s%s", (i > 2) ? " " : "", tokens[i]);
        }
        if (!board_set_fen(board, fen)) {
            board_set_startpos(board);
        }
        push_current_position(board, history);
        move_start = fen_end;
    } else {
        return;
    }

    if (move_start < token_count && strcmp(tokens[move_start], "moves") == 0) {
        ++move_start;
    }

    for (int i = move_start; i < token_count; ++i) {
        apply_uci_move(board, history, tokens[i]);
    }
}

typedef struct {
    pthread_t thread;
    bool thread_valid;
    bool searching;
    volatile bool stop_requested;
    pthread_mutex_t mutex;
} SearchThreadState;

typedef struct {
    Board board;
    RepetitionHistory history;
    SearchLimits limits;
    SearchOptions options;
    SearchContext *context;
    volatile bool *stop_requested;
    bool *searching;
    pthread_mutex_t *mutex;
} SearchTask;

static void print_bestmove(Move best, const Board *board) {
    if (best == MOVE_NONE) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    char buffer[6];
    move_to_string(best, board, buffer);
    printf("bestmove %s\n", buffer);
    fflush(stdout);
}

static void parse_go_limits(SearchLimits *limits, const char *line, const Board *board) {
    memset(limits, 0, sizeof(*limits));

    char parse_buffer[4096];
    strncpy(parse_buffer, line, sizeof(parse_buffer) - 1);
    parse_buffer[sizeof(parse_buffer) - 1] = '\0';

    bool parsing_searchmoves = false;

    for (char *token = strtok(parse_buffer, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
        // Only search limited moves at root
        if (strcmp(token, "searchmoves") == 0) {
            parsing_searchmoves = true;
            limits->has_search_moves = true;
            continue;
        }

        if (parsing_searchmoves) {
            Move m = MOVE_NONE;
            Board temp_board;
            if (board != NULL) {
                temp_board = *board;
            }
            if (board != NULL && movegen_find_legal_move(&temp_board, token, &m)) {
                if (limits->search_move_count < 256) {
                    bool duplicate = false;
                    for (int i = 0; i < limits->search_move_count; i++) {
                        if (limits->search_moves[i] == m) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        limits->search_moves[limits->search_move_count++] = m;
                    }
                }
                continue;
            } else {
                parsing_searchmoves = false;
            }
        }

        if (strcmp(token, "depth") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                int parsed_depth = atoi(value);
                if (parsed_depth > 0) {
                    limits->depth = parsed_depth;
                }
            }
        } else if (strcmp(token, "movetime") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits->movetime_ms = atoi(value);
            }
        } else if (strcmp(token, "wtime") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits->wtime_ms = atoi(value);
                limits->has_clock_time = true;
            }
        } else if (strcmp(token, "btime") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits->btime_ms = atoi(value);
                limits->has_clock_time = true;
            }
        } else if (strcmp(token, "winc") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits->winc_ms = atoi(value);
                limits->has_clock_time = true;
            }
        } else if (strcmp(token, "binc") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits->binc_ms = atoi(value);
                limits->has_clock_time = true;
            }
        } else if (strcmp(token, "movestogo") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits->movestogo = atoi(value);
            }
        } else if (strcmp(token, "infinite") == 0) {
            limits->infinite = true;
        }
    }
}

static void fnv1a_update(unsigned long long *hash, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < size; ++i) {
        *hash = (*hash ^ bytes[i]) * 1099511628211ULL;
    }
}

static void *search_thread_main(void *arg) {
    SearchTask *task = (SearchTask *)arg;
    Move best = think(&task->board,
                      &task->limits,
                      &task->options,
                      &task->history,
                      task->stop_requested,
                      NULL,
                      NULL,
                      task->context);
    print_bestmove(best, &task->board);

    pthread_mutex_lock(task->mutex);
    *task->searching = false;
    pthread_mutex_unlock(task->mutex);

    free(task);
    return NULL;
}

static bool search_thread_is_running(SearchThreadState *state) {
    bool running = false;
    pthread_mutex_lock(&state->mutex);
    running = state->searching;
    pthread_mutex_unlock(&state->mutex);
    return running;
}

static void search_thread_request_stop(SearchThreadState *state) {
    pthread_mutex_lock(&state->mutex);
    state->stop_requested = true;
    pthread_mutex_unlock(&state->mutex);
}

static void search_thread_join_if_finished(SearchThreadState *state) {
    if (!state->thread_valid) {
        return;
    }

    if (!search_thread_is_running(state)) {
        pthread_join(state->thread, NULL);
        state->thread_valid = false;
    }
}

static void search_thread_stop_and_join(SearchThreadState *state) {
    if (!state->thread_valid) {
        return;
    }

    search_thread_request_stop(state);
    pthread_join(state->thread, NULL);
    state->thread_valid = false;
}

static bool search_thread_start(SearchThreadState *state,
                                const Board *board,
                                const RepetitionHistory *history,
                                const SearchLimits *limits,
                                const SearchOptions *options,
                                SearchContext *context) {
    SearchTask *task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return false;
    }

    task->board = *board;
    task->history = *history;
    task->limits = *limits;
    task->options = *options;
    task->context = context;
    task->stop_requested = &state->stop_requested;
    task->searching = &state->searching;
    task->mutex = &state->mutex;

    pthread_mutex_lock(&state->mutex);
    state->stop_requested = false;
    state->searching = true;
    pthread_mutex_unlock(&state->mutex);

    if (pthread_create(&state->thread, NULL, search_thread_main, task) != 0) {
        pthread_mutex_lock(&state->mutex);
        state->searching = false;
        pthread_mutex_unlock(&state->mutex);
        free(task);
        return false;
    }

    state->thread_valid = true;
    return true;
}

static unsigned long long perft_helper(Board *board, int depth) {
    if (depth == 0) {
        return 1ULL;
    }

    MoveList list;
    movegen_generate_pseudo_legal(board, &list);

    if (list.count == 0) {
        return 0ULL;
    }

    if (depth == 1) {
        unsigned long long nodes = 0;
        for (int i = 0; i < list.count; ++i) {
            Undo undo;
            if (board_make_move(board, list.moves[i], &undo)) {
                nodes++;
                board_unmake_move(board, &undo);
            }
        }
        return nodes;
    }

    unsigned long long nodes = 0;
    for (int i = 0; i < list.count; ++i) {
        Undo undo;
        if (board_make_move(board, list.moves[i], &undo)) {
            nodes += perft_helper(board, depth - 1);
            board_unmake_move(board, &undo);
        }
    }
    return nodes;
}

static void run_perft(Board *board, int depth) {
    printf("\nRunning performance test to depth %d\n\n", depth);
    fflush(stdout);

    if (depth < 0) {
        printf("Total nodes: 0\n");
        printf("Total time: 0ms\n");
        printf("Overall NPS: 0\n\n");
        fflush(stdout);
        return;
    }

    if (depth == 0) {
        printf("Total nodes: 1\n");
        printf("Total time: 0ms\n");
        printf("Overall NPS: 0\n\n");
        fflush(stdout);
        return;
    }

    long long start_time = current_time_ms();

    MoveList list;
    movegen_generate_pseudo_legal(board, &list);

    unsigned long long total_nodes = 0;
    for (int i = 0; i < list.count; ++i) {
        Undo undo;
        if (board_make_move(board, list.moves[i], &undo)) {
            unsigned long long nodes = perft_helper(board, depth - 1);
            total_nodes += nodes;
            board_unmake_move(board, &undo);

            char move_str[6];
            move_to_string(list.moves[i], board, move_str);
            printf("%s : %llu\n", move_str, nodes);
            fflush(stdout);
        }
    }

    long long total_time = current_time_ms() - start_time;
    if (total_time < 0) {
        total_time = 0;
    }

    unsigned long long overall_nps = (total_nodes * 1000ULL) / (total_time > 0 ? total_time : 1);

    printf("\nTotal nodes: %llu\n", total_nodes);
    printf("Total time: %lldms\n", total_time);
    printf("Overall NPS: %llu\n\n", overall_nps);
    fflush(stdout);
}

void uci_loop(int argc, char *argv[]) {
    init_eval();
    init_lmr();
    init_lmp();
    Board board;
    board_init(&board);
    RepetitionHistory history;
    push_current_position(&board, &history);

    SearchOptions options = {0};
    options.overhead_ms = 10;
    options.multipv = 1;
    options.hash_power = 22; // 64 MiB hash
    options.lichess_draw_rules = false;
    options.display_currmove = false;
    
    int max_hash = (1 << 30) / 16;

    SearchContext *global_search_context = search_context_create((size_t)options.hash_power);

    SearchThreadState search_thread = {0};
    pthread_mutex_init(&search_thread.mutex, NULL);

    bool cli_mode = (argc > 1);
    bool cli_done = false;

    char line[4096];
    while (!cli_done) {
        if (cli_mode) {
            line[0] = '\0';
            for (int i = 1; i < argc; ++i) {
                if (i > 1) {
                    strncat(line, " ", sizeof(line) - strlen(line) - 1);
                }
                strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
            }
            cli_done = true;
        } else {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                break;
            }
        }

        search_thread_join_if_finished(&search_thread);

        if (strncmp(line, "uci", 3) == 0 && (line[3] == '\0' || line[3] == ' ' || line[3] == '\t' || line[3] == '\r' || line[3] == '\n')) {
            printf("id name Cepimetheus\n");           
#ifndef ENGINE_VERSION
#define ENGINE_VERSION "NULL"
#endif
            printf("id version %s\n", ENGINE_VERSION);
            printf("id author  George Bland\n"); 
            printf("option name Overhead type spin default 10 min 0 max 10000\n");
            printf("option name MultiPV type spin default 1 min 1 max 256\n");
            printf("option name Hash type spin default 64 min 0 max %d\n", max_hash);
            printf("option name Clear Hash type button\n");
            printf("option name Lichess_Draw_Rules type check default false\n");
            printf("option name Display_Currmove type check default false\n");
            printf("option name UCI_Chess960 type check default false\n");
#ifdef SPSA_TUNING
            printf("option name FutilityMargin type spin default %d min 0 max 10000\n", futility_margin);
            printf("option name RFP_Margin type spin default %d min 0 max 5000\n", rfp_margin);
            printf("option name RFP_MaxDepth type spin default %d min 1 max 32\n", rfp_max_depth);
            printf("option name NMP_MinDepth type spin default %d min 1 max 10\n", nmp_min_depth);
            printf("option name NMP_BaseReduction type spin default %d min 1 max 6\n", nmp_base_reduction);
            printf("option name NMP_DepthScale type spin default %d min 1 max 16\n", nmp_depth_scale);
            printf("option name NMP_MinPieces type spin default %d min 1 max 8\n", nmp_min_pieces);
            printf("option name QS_DeltaMargin type spin default %d min 0 max 10000\n", qs_delta_margin);
            printf("option name LMR_MinDepth type spin default %d min 1 max 10\n", lmr_min_depth);
            printf("option name LMR_Offset type spin default %d min -500 max 500\n", lmr_offset);
            printf("option name LMR_Divisor type spin default %d min 10 max 1000\n", lmr_divisor);
            printf("option name LMR_MoveMultiplier type spin default %d min 10 max 1000\n", lmr_move_multiplier);
            printf("option name LMP_Base type spin default %d min 0 max 1000\n", lmp_base);
            printf("option name LMP_Multiplier type spin default %d min 0 max 1000\n", lmp_multiplier);
            printf("option name History_BonusCap type spin default %d min 1 max 5000\n", history_bonus_cap);
            printf("option name History_Gravity type spin default %d min 1 max 4096\n", history_gravity);
            printf("option name History_Scale type spin default %d min 1 max 256\n", history_scale);
            printf("option name Order_KnightPromo type spin default %d min 0 max 10000\n", order_knight_promo);
            printf("option name Order_BishopPromo type spin default %d min 0 max 10000\n", order_bishop_promo);
            printf("option name Order_RookPromo type spin default %d min 0 max 10000\n", order_rook_promo);
            printf("option name Order_QueenPromo type spin default %d min 0 max 10000\n", order_queen_promo);
            printf("option name Order_VictimMult type spin default %d min 1 max 100\n", order_victim_mult);
            printf("option name Order_Killer1 type spin default %d min 0 max 500000\n", order_killer1);
            printf("option name Order_Killer2 type spin default %d min 0 max 500000\n", order_killer2);
            printf("option name Order_Castle type spin default %d min 0 max 10000\n", order_castle);
            printf("option name Asp_MinDepth type spin default %d min 1 max 32\n", asp_min_depth);
            printf("option name Asp_InitialDelta type spin default %d min 1 max 2000\n", asp_initial_delta);
            printf("option name Asp_GrowthFactor type spin default %d min 100 max 500\n", asp_growth_factor);
#endif
            printf("uciok\n");
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "isready", 7) == 0) {
            printf("readyok\n");
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "setoption", 9) == 0) {
            for (int i = 9; line[i] != '\0'; i++) {
                line[i] = tolower((unsigned char)line[i]);
            }
            char *nametoken = strstr(line, "name");
            char *valuetoken = strstr(line, "value");
            
            if (nametoken != NULL) {
                nametoken += 4;
                while (*nametoken == ' ' || *nametoken == '\t') nametoken++;
                
                if (strncmp(nametoken, "overhead", 8) == 0 && valuetoken != NULL) {
                    valuetoken += 5;
                    while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                    int parsed_overhead = atoi(valuetoken);
                    if (parsed_overhead >= 0 && parsed_overhead <= 10000) {
                        options.overhead_ms = parsed_overhead;
                    }
                } else if (strncmp(nametoken, "multipv", 7) == 0 && valuetoken != NULL) {
                    valuetoken += 5;
                    while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                    int parsed_multipv = atoi(valuetoken);
                    if (parsed_multipv >= 1 && parsed_multipv <= 256) {
                        options.multipv = parsed_multipv;
                    }
                } else if (strncmp(nametoken, "clear hash", 10) == 0 || strncmp(nametoken, "clear_hash", 10) == 0) {
                    search_thread_stop_and_join(&search_thread);
                    search_context_clear(global_search_context);
                } else if (strncmp(nametoken, "hash", 4) == 0 && valuetoken != NULL) {
                    valuetoken += 5;
                    while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                    int parsed_hash = atoi(valuetoken);
                    int new_hash_power = options.hash_power;
                    if (parsed_hash > 0 && parsed_hash <= max_hash) {
                        long long bytes = (long long)parsed_hash << 20;
                        new_hash_power = (int)floor(log2((double)bytes / 16));
                    }
                    else if (parsed_hash == 0) {
                        new_hash_power = 0;
                    }
                    if (new_hash_power != options.hash_power || global_search_context == NULL) {
                        options.hash_power = new_hash_power;
                        search_thread_stop_and_join(&search_thread);
                        search_context_resize(global_search_context, (size_t)options.hash_power);
                    }
                } else if (strncmp(nametoken, "lichess_draw_rules", 18) == 0) {
                    if (valuetoken != NULL) {
                        valuetoken += 5;
                        while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                        options.lichess_draw_rules = (strncmp(valuetoken, "true", 4) == 0);
                    } else {
                        // No value token means set to true
                        options.lichess_draw_rules = true;
                    }
                } else if (strncmp(nametoken, "display_currmove", 16) == 0) {
                    if (valuetoken != NULL) {
                        valuetoken += 5;
                        while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                        options.display_currmove = (strncmp(valuetoken, "true", 4) == 0);
                    } else {
                        options.display_currmove = true;
                    }
                } else if (strncmp(nametoken, "uci_chess960", 12) == 0) {
                    if (valuetoken != NULL) {
                        valuetoken += 5;
                        while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                        option_chess960 = (strncmp(valuetoken, "true", 4) == 0);
                    } else {
                        option_chess960 = true;
                    }
                }
#ifdef SPSA_TUNING
                if (valuetoken != NULL) {
                    char *val_ptr = valuetoken + 5;
                    while (*val_ptr == ' ' || *val_ptr == '\t') val_ptr++;
                    int val = atoi(val_ptr);

                    if (strncmp(nametoken, "futilitymargin", 14) == 0) futility_margin = val;
                    else if (strncmp(nametoken, "rfp_margin", 10) == 0) rfp_margin = val;
                    else if (strncmp(nametoken, "rfp_maxdepth", 12) == 0) rfp_max_depth = val;
                    else if (strncmp(nametoken, "nmp_mindepth", 12) == 0) nmp_min_depth = val;
                    else if (strncmp(nametoken, "nmp_basereduction", 17) == 0) { nmp_base_reduction = val; nmp_reduction = val; }
                    else if (strncmp(nametoken, "nmp_reduction", 13) == 0) { nmp_base_reduction = val; nmp_reduction = val; }
                    else if (strncmp(nametoken, "nmp_depthscale", 14) == 0) nmp_depth_scale = val;
                    else if (strncmp(nametoken, "nmp_minpieces", 13) == 0) nmp_min_pieces = val;
                    else if (strncmp(nametoken, "qs_deltamargin", 14) == 0) qs_delta_margin = val;
                    else if (strncmp(nametoken, "lmr_mindepth", 12) == 0) lmr_min_depth = val;
                    else if (strncmp(nametoken, "lmr_offset", 10) == 0) { lmr_offset = val; reinit_lmr(); }
                    else if (strncmp(nametoken, "lmr_divisor", 11) == 0) { lmr_divisor = val; reinit_lmr(); }
                    else if (strncmp(nametoken, "lmr_movemultiplier", 18) == 0) { lmr_move_multiplier = val; reinit_lmr(); }
                    else if (strncmp(nametoken, "lmp_base", 8) == 0) { lmp_base = val; reinit_lmp(); }
                    else if (strncmp(nametoken, "lmp_multiplier", 14) == 0) { lmp_multiplier = val; reinit_lmp(); }
                    else if (strncmp(nametoken, "history_bonuscap", 16) == 0) history_bonus_cap = val;
                    else if (strncmp(nametoken, "history_gravity", 15) == 0) history_gravity = val;
                    else if (strncmp(nametoken, "history_scale", 13) == 0) history_scale = val;
                    else if (strncmp(nametoken, "order_knightpromo", 17) == 0) order_knight_promo = val;
                    else if (strncmp(nametoken, "order_bishoppromo", 17) == 0) order_bishop_promo = val;
                    else if (strncmp(nametoken, "order_rookpromo", 15) == 0) order_rook_promo = val;
                    else if (strncmp(nametoken, "order_queenpromo", 16) == 0) order_queen_promo = val;
                    else if (strncmp(nametoken, "order_victimmult", 16) == 0) order_victim_mult = val;
                    else if (strncmp(nametoken, "order_killer1", 13) == 0) order_killer1 = val;
                    else if (strncmp(nametoken, "order_killer2", 13) == 0) order_killer2 = val;
                    else if (strncmp(nametoken, "order_castle", 12) == 0) order_castle = val;
                    else if (strncmp(nametoken, "asp_mindepth", 12) == 0) asp_min_depth = val;
                    else if (strncmp(nametoken, "asp_initialdelta", 16) == 0) asp_initial_delta = val;
                    else if (strncmp(nametoken, "asp_growthfactor", 16) == 0) asp_growth_factor = val;
                }
#endif
            }
            continue;
        }

        if (strncmp(line, "ucinewgame", 10) == 0) {
            search_thread_stop_and_join(&search_thread);
            board_set_startpos(&board);
            push_current_position(&board, &history);
            search_context_clear(global_search_context);
            continue;
        }

        if (strncmp(line, "position", 8) == 0) {
            search_thread_stop_and_join(&search_thread);
            apply_position(&board, &history, line);
            continue;
        }

        if (strncmp(line, "eval", 4) == 0 && (line[4] == '\0' || line[4] == ' ' || line[4] == '\t' || line[4] == '\r' || line[4] == '\n')) {
            search_thread_stop_and_join(&search_thread);
            int score = evaluate_position(&board);
            printf("%d\n", score);
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "bench", 5) == 0 && (line[5] == '\0' || line[5] == ' ' || line[5] == '\t' || line[5] == '\r' || line[5] == '\n')) {
            search_thread_stop_and_join(&search_thread);

            int target_depth = 8;
            char *depth_token = strstr(line, "depth");
            if (depth_token != NULL) {
                depth_token += 5;
                while (*depth_token == ' ' || *depth_token == '\t') depth_token++;
                int parsed_depth = atoi(depth_token);
                if (parsed_depth > 0) {
                    target_depth = parsed_depth;
                }
            } else {
                char *p = line + 5;
                while (*p == ' ' || *p == '\t') p++;
                if (*p >= '0' && *p <= '9') {
                    int parsed_depth = atoi(p);
                    if (parsed_depth > 0) {
                        target_depth = parsed_depth;
                    }
                }
            }

            typedef struct {
                const char *name;
                const char *fen;
            } BenchPosition;

            const BenchPosition bench_positions[] = {
                {"pos0", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
                {"pos1", "rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1"},
                {"pos2", "rnbqkb1r/pppppppp/5n2/8/8/5N2/PPPPPPPP/RNBQKB1R w KQkq - 2 2"},
                {"pos3", "rnbqkb1r/pppppppp/5n2/8/P7/5N2/1PPPPPPP/RNBQKB1R b KQkq - 0 2"},
                {"pos4", "rnbqkb1r/ppp1pppp/3p1n2/8/P7/5N2/1PPPPPPP/RNBQKB1R w KQkq - 0 3"},
                {"pos5", "rnbqkb1r/ppp1pppp/3p1n2/8/P7/R4N2/1PPPPPPP/1NBQKB1R b Kkq - 1 3"},
                {"pos6", "rn1qkb1r/ppp1pppp/3p1n2/5b2/P7/R4N2/1PPPPPPP/1NBQKB1R w Kkq - 2 4"},
                {"pos7", "rn1qkb1r/ppp1pppp/3p1n2/5b2/P7/R3PN2/1PPP1PPP/1NBQKB1R b Kkq - 0 4"},
                {"pos8", "r2qkb1r/ppp1pppp/n2p1n2/5b2/P7/R3PN2/1PPP1PPP/1NBQKB1R w Kkq - 1 5"},
                {"pos9", "r2qkb1r/ppp1pppp/n2p1n2/5b2/P2N4/R3P3/1PPP1PPP/1NBQKB1R b Kkq - 2 5"},
                {"pos10", "r2qkb1r/pppbpppp/n2p1n2/8/P2N4/R3P3/1PPP1PPP/1NBQKB1R w Kkq - 3 6"},
                {"pos11", "r2qkb1r/pppbpppp/B2p1n2/8/P2N4/R3P3/1PPP1PPP/1NBQK2R b Kkq - 0 6"},
                {"pos12", "r2qkb1r/p1pbpppp/p2p1n2/8/P2N4/R3P3/1PPP1PPP/1NBQK2R w Kkq - 0 7"},
                {"pos13", "r2qkb1r/p1pbpppp/p2p1n2/8/P2N4/R3P3/1PPPQPPP/1NB1K2R b Kkq - 1 7"},
                {"pos14", "r2qkb1r/p1pb1ppp/p2p1n2/4p3/P2N4/R3P3/1PPPQPPP/1NB1K2R w Kkq - 0 8"},
                {"pos15", "r2qkb1r/p1pb1ppp/p2p1n2/4p3/P7/R3PN2/1PPPQPPP/1NB1K2R b Kkq - 1 8"},
                {"pos16", "r2qkb1r/p1pb1ppp/p2p1n2/8/P3p3/R3PN2/1PPPQPPP/1NB1K2R w Kkq - 0 9"},
                {"pos17", "r2qkb1r/p1pb1ppp/p2p1n2/8/P2Np3/R3P3/1PPPQPPP/1NB1K2R b Kkq - 1 9"},
                {"pos18", "r2qkb1r/p2b1ppp/p2p1n2/2p5/P2Np3/R3P3/1PPPQPPP/1NB1K2R w Kkq - 0 10"},
                {"pos19", "r2qkb1r/p2b1ppp/p2p1n2/2p5/P3p3/RN2P3/1PPPQPPP/1NB1K2R b Kkq - 1 10"},
                {"pos20", "r2qkb1r/p2b1ppp/p4n2/2pp4/P3p3/RN2P3/1PPPQPPP/1NB1K2R w Kkq - 0 11"},
                {"pos21", "r2qkb1r/p2b1ppp/p4n2/2pp4/P1P1p3/RN2P3/1P1PQPPP/1NB1K2R b Kkq - 0 11"},
                {"pos22", "r2qkb1r/p2b1ppp/p4n2/2p5/P1Ppp3/RN2P3/1P1PQPPP/1NB1K2R w Kkq - 0 12"},
                {"pos23", "r2qkb1r/p2b1ppp/p4n2/2p5/P1Ppp3/RN1PP3/1P2QPPP/1NB1K2R b Kkq - 0 12"},
                {"pos24", "r2qkb1r/p2b1ppp/p4n2/2p5/P1Pp4/RN1pP3/1P2QPPP/1NB1K2R w Kkq - 0 13"},
                {"pos25", "r2qkb1r/p2b1ppp/p4n2/2p5/P1Pp4/RN1QP3/1P3PPP/1NB1K2R b Kkq - 0 13"},
                {"pos26", "r2qkb1r/p4ppp/p1b2n2/2p5/P1Pp4/RN1QP3/1P3PPP/1NB1K2R w Kkq - 1 14"},
                {"pos27", "r2qkb1r/p4ppp/p1b2n2/2p5/P1Pp4/RN1QPP2/1P4PP/1NB1K2R b Kkq - 0 14"},
                {"pos28", "r2qk2r/p4ppp/p1bb1n2/2p5/P1Pp4/RN1QPP2/1P4PP/1NB1K2R w Kkq - 1 15"},
                {"pos29", "r2qk2r/p4ppp/p1bb1n2/2p5/P1Pp4/RN1QPP2/1P4PP/1NB2RK1 b kq - 2 15"},
                {"pos30", "r3k2r/p2q1ppp/p1bb1n2/2p5/P1Pp4/RN1QPP2/1P4PP/1NB2RK1 w kq - 3 16"},
                {"pos31", "r3k2r/p2q1ppp/p1bb1n2/2p5/P1PP4/RN1Q1P2/1P4PP/1NB2RK1 b kq - 0 16"},
                {"pos32", "r3k2r/p2q1ppp/p1bb1n2/8/P1Pp4/RN1Q1P2/1P4PP/1NB2RK1 w kq - 0 17"},
                {"pos33", "r3k2r/p2q1ppp/p1bb1n2/8/P1Pp4/RN1Q1P2/1P4PP/1NB1R1K1 b kq - 1 17"},
                {"pos34", "r4k1r/p2q1ppp/p1bb1n2/8/P1Pp4/RN1Q1P2/1P4PP/1NB1R1K1 w - - 2 18"},
                {"pos35", "r4k1r/p2q1ppp/p1bb1n2/2P5/P2p4/RN1Q1P2/1P4PP/1NB1R1K1 b - - 0 18"},
                {"pos36", "r4k1r/p2qbppp/p1b2n2/2P5/P2p4/RN1Q1P2/1P4PP/1NB1R1K1 w - - 1 19"},
                {"pos37", "r4k1r/p2qbppp/Q1b2n2/2P5/P2p4/RN3P2/1P4PP/1NB1R1K1 b - - 0 19"},
                {"pos38", "r4k1r/p2qbppp/Q1b2n2/2P5/P7/RN1p1P2/1P4PP/1NB1R1K1 w - - 0 20"},
                {"pos39", "r4k1r/p2qbppp/Q1b2n2/2P5/P4B2/RN1p1P2/1P4PP/1N2R1K1 b - - 1 20"},
                {"pos40", "r4k1r/p2qbppp/Q1b5/2P4n/P4B2/RN1p1P2/1P4PP/1N2R1K1 w - - 2 21"},
                {"pos41", "r4k1r/p2qbppp/Q1b5/2P1B2n/P7/RN1p1P2/1P4PP/1N2R1K1 b - - 3 21"},
                {"pos42", "r4k1r/p2qb1pp/Q1b2p2/2P1B2n/P7/RN1p1P2/1P4PP/1N2R1K1 w - - 0 22"},
                {"pos43", "r4k1r/p2qb1pp/Q1b2p2/2P4n/P2B4/RN1p1P2/1P4PP/1N2R1K1 b - - 1 22"},
                {"pos44", "r4k1r/p2qb1pp/Q1b2p2/2P5/P2B1n2/RN1p1P2/1P4PP/1N2R1K1 w - - 2 23"},
                {"pos45", "r4k1r/p2qb1pp/Q1b2p2/2P5/P2B1n2/RN1p1P2/1P4PP/1N2R2K b - - 3 23"},
                {"pos46", "r4k1r/p2qb1pp/Q4p2/2P5/P2B1n2/RN1p1b2/1P4PP/1N2R2K w - - 0 24"},
                {"pos47", "r4k1r/p2qb1pp/Q4p2/2P5/P2B1n2/RN1p1b2/1P4PP/1N2R1K1 b - - 1 24"},
                {"pos48", "r4k1r/p3b1pp/Q4p2/2P5/P2B1nq1/RN1p1b2/1P4PP/1N2R1K1 w - - 2 25"},
                {"pos49", "r4k1r/p3b1pp/Q4p2/2P5/P2B1nq1/RN1p1bPn/1P5P/1N2R1K1 b - - 0 25"},
                {"pos50", "r4k1r/p3b1pp/Q4p2/2P5/P2B2q1/RN1p1bPn/1P5P/1N2R1K1 w - - 1 26"},
                {"pos51", "r4k1r/p3b1pp/Q4p2/2P5/P2B2q1/RN1p1bPn/1P5P/1N2RK2 b - - 2 26"},
                {"pos52", "r4k1r/p3b1pp/Q4p2/2P5/P2Bb1q1/RN1p2Pn/1P5P/1N2RK2 w - - 3 27"},
                {"pos53", "r4k1r/p3b1pp/Q4p2/2P5/P2BR1q1/RN1p2Pn/1P5P/1N3K2 b - - 0 27"},
                {"pos54", "r4k1r/p3b1pp/Q4p2/2P5/P2Bq3/RN1p2Pn/1P5P/1N3K2 w - - 0 28"},
                {"pos55", "r4k1r/p3b1pp/Q4p2/2P5/P2Bq3/RN1Q2Pn/1P5P/1N3K2 b - - 0 28"},
                {"pos56", "r4k1r/p3b1pp/5p2/2P5/P2B4/RN1q2Pn/1P5P/1N3K2 w - - 0 29"},
                {"pos57", "r4k1r/p3b1pp/5p2/2P5/P2B4/RN1q2Pn/1P4KP/1N6 b - - 1 29"},
                {"pos58", "r4k1r/p3b1pp/5p2/2P5/P2B4/RN4Pn/1P2q1KP/1N6 w - - 2 30"},
                {"pos59", "r4k1r/p3b1pp/5p2/2P5/P7/RN4Pn/1P2qBKP/1N6 b - - 3 30"},
                {"pos60", "r4k1r/p3b1pp/5p2/2P5/P7/RN4Pn/1P3qKP/1N6 w - - 0 31"},
                {"pos61", "r4k1r/p3b1pp/5p2/2P5/P7/RN4PK/1P3q1P/1N6 b - - 0 31"},
                {"pos62", "r4k1r/p3b1pp/5p2/2P5/P7/RN4PK/1P5P/1N3q2 w - - 1 32"},
                {"pos63", "r4k1r/p3b1pp/5p2/2P5/P6K/RN4P1/1P5P/1N3q2 b - - 2 32"},
                {"pos64", "r4k1r/p3b1pp/8/2P2p2/P6K/RN4P1/1P5P/1N3q2 w - - 0 33"},
                {"pos65", "r4k1r/p3b1pp/8/2P2p1K/P7/RN4P1/1P5P/1N3q2 b - - 1 33"},
                {"pos66", "r4k1r/p3b1pp/8/2P2p1K/P7/RN4Pq/1P5P/1N6 w - - 2 34"}
            };

            size_t num_positions = sizeof(bench_positions) / sizeof(bench_positions[0]);
            printf("Searching %zu positions at depth %d...\n", num_positions, target_depth);
            fflush(stdout);

            unsigned long long total_nodes = 0;
            unsigned long long bench_hash = 14695981039346656037ULL;
            long long start_time = current_time_ms();

            SearchOptions bench_options = options;
            bench_options.silent = true;

            for (size_t i = 0; i < num_positions; ++i) {
                Board bench_board;
                board_init(&bench_board);
                if (!board_set_fen(&bench_board, bench_positions[i].fen)) {
                    printf("Error: Invalid FEN %s\n", bench_positions[i].fen);
                    continue;
                }

                RepetitionHistory bench_history;
                repetition_history_init(&bench_history);
                repetition_history_push(&bench_history, bench_board.hash);

                SearchLimits limits = {0};
                limits.depth = target_depth;

                unsigned long long nodes = 0;
                volatile bool stop_signal = false;
                SearchResult result = {0};

                search_context_clear(global_search_context);
                think(&bench_board, &limits, &bench_options, &bench_history, &stop_signal, &nodes, &result, global_search_context);

                // Update benchmark hash with search result fields to verify correctness across edits
                fnv1a_update(&bench_hash, &result.score, sizeof(result.score));
                fnv1a_update(&bench_hash, &result.move, sizeof(result.move));
                fnv1a_update(&bench_hash, &result.pv_length, sizeof(result.pv_length));
                for (int j = 0; j < result.pv_length; ++j) {
                    fnv1a_update(&bench_hash, &result.pv[j], sizeof(result.pv[j]));
                }

                total_nodes += nodes;
            }

            long long total_time = current_time_ms() - start_time;
            if (total_time < 0) {
                total_time = 0;
            }

            unsigned long long overall_nps = (total_nodes * 1000ULL) / (total_time > 0 ? total_time : 1);
            printf("%llu nodes %llu nps %lld ms 0x%016llx hash\n", total_nodes, overall_nps, total_time, bench_hash);
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "go", 2) == 0 && (line[2] == '\0' || line[2] == ' ' || line[2] == '\t' || line[2] == '\r' || line[2] == '\n')) {
            search_thread_stop_and_join(&search_thread);

            char *perft_token = strstr(line, "perft");
            if (perft_token != NULL) {
                perft_token += 5;
                while (*perft_token == ' ' || *perft_token == '\t') {
                    perft_token++;
                }
                int depth = atoi(perft_token);
                Board temp_board = board;
                run_perft(&temp_board, depth);
                continue;
            }

            SearchLimits limits;
            parse_go_limits(&limits, line, &board);
            if (!search_thread_start(&search_thread, &board, &history, &limits, &options, global_search_context)) {
                print_bestmove(MOVE_NONE, &board);
            }
            continue;
        }

        if (strncmp(line, "stop", 4) == 0) {
            search_thread_stop_and_join(&search_thread);
            continue;
        }

        if (strncmp(line, "quit", 4) == 0) {
            search_thread_stop_and_join(&search_thread);
            break;
        }
    }

    search_thread_stop_and_join(&search_thread);
    search_thread_join_if_finished(&search_thread);
    pthread_mutex_destroy(&search_thread.mutex);
    search_context_destroy(global_search_context);
}
