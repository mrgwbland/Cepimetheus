#include "uci.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void push_current_position(Board *board, RepetitionHistory *history) {
    if (history == NULL) {
        return;
    }

    repetition_history_init(history);
    repetition_history_push(history, board_position_key(board));
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
        repetition_history_push(history, board_position_key(board));
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
        if (token_count < 8) {
            return;
        }
        char fen[512] = {0};
        snprintf(fen, sizeof(fen), "%s %s %s %s %s %s", tokens[2], tokens[3], tokens[4], tokens[5], tokens[6], tokens[7]);
        if (!board_set_fen(board, fen)) {
            board_set_startpos(board);
        }
        push_current_position(board, history);
        move_start = 8;
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
    volatile bool *stop_requested;
    bool *searching;
    pthread_mutex_t *mutex;
} SearchTask;

static void print_bestmove(Move best) {
    if (best == MOVE_NONE) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    char buffer[6];
    move_to_string(best, buffer);
    printf("bestmove %s\n", buffer);
    fflush(stdout);
}

static void parse_go_limits(SearchLimits *limits, const char *line) {
    memset(limits, 0, sizeof(*limits));

    char parse_buffer[4096];
    strncpy(parse_buffer, line, sizeof(parse_buffer) - 1);
    parse_buffer[sizeof(parse_buffer) - 1] = '\0';

    for (char *token = strtok(parse_buffer, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
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
        } else if (strcmp(token, "infinite") == 0) {
            limits->infinite = true;
        }
    }
}

static void *search_thread_main(void *arg) {
    SearchTask *task = (SearchTask *)arg;
    Move best = think(&task->board,
                      &task->limits,
                      &task->options,
                      &task->history,
                      task->stop_requested,
                      NULL);
    print_bestmove(best);

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
                                const SearchOptions *options) {
    SearchTask *task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return false;
    }

    task->board = *board;
    task->history = *history;
    task->limits = *limits;
    task->options = *options;
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

void uci_loop(void) {
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

    SearchThreadState search_thread = {0};
    pthread_mutex_init(&search_thread.mutex, NULL);

    char line[4096];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        search_thread_join_if_finished(&search_thread);

        if (strncmp(line, "uci", 3) == 0 && (line[3] == '\0' || line[3] == ' ' || line[3] == '\t' || line[3] == '\r' || line[3] == '\n')) {
            printf("id name Cepimetheus\n");           
            printf("id version 9.0.0\n");
            printf("id author  George Bland\n"); 
            printf("option name overhead type spin default 10 min 0 max 10000\n");
            printf("option name MultiPV type spin default 1 min 1 max 256\n");
            printf("option name Hash type spin default 64 min 0 max %d\n", max_hash);
            printf("option name lichess_draw_rules type check default false\n");
            printf("option name display_currmove type check default false\n");
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
                } else if (strncmp(nametoken, "MultiPV", 7) == 0 && valuetoken != NULL) {
                    valuetoken += 5;
                    while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                    int parsed_multipv = atoi(valuetoken);
                    if (parsed_multipv >= 1 && parsed_multipv <= 256) {
                        options.multipv = parsed_multipv;
                    }
                } else if (strncmp(nametoken, "Hash", 4) == 0 && valuetoken != NULL) {
                    valuetoken += 5;
                    while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                    int parsed_hash = atoi(valuetoken);
                    if (parsed_hash > 0 && parsed_hash <= max_hash) {
                        long long bytes = (long long)parsed_hash << 20;
                        options.hash_power = (int)floor(log2((double)bytes / 16));
                    }
                    else if (parsed_hash == 0) {
                        options.hash_power = 0;
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
                }
            }
            continue;
        }

        if (strncmp(line, "ucinewgame", 10) == 0) {
            search_thread_stop_and_join(&search_thread);
            board_set_startpos(&board);
            push_current_position(&board, &history);
            continue;
        }

        if (strncmp(line, "position", 8) == 0) {
            search_thread_stop_and_join(&search_thread);
            apply_position(&board, &history, line);
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

            printf("Running benchmark at depth %d...\n", target_depth);
            fflush(stdout);

            typedef struct {
                const char *name;
                const char *fen;
            } BenchPosition;

            const BenchPosition bench_positions[] = {
                {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
                {"tacticalposition", "3r2rk/pp3p1p/2p1pq2/2P1P2R/2n5/3P4/P3Q1B1/1K1R4 b - - 0 34"},
                {"smotheredmate", "1r2kb1r/3npppp/5n2/QN6/3q4/7P/PPPB1PP1/2KR3R b k - 0 14"},
                {"tactic2", "r2q2k1/1R3p1p/p3B1p1/3Pp3/Pp2P3/1Pb1Q2P/2b2PP1/6K1 b - - 4 35"},
                {"oppositebishops", "8/5p2/2b2B2/2P1P3/4P1k1/4K3/8/8 w - - 89 128"},
                {"matein5", "8/6k1/2RR2pp/p7/N3r3/6K1/PP4PP/5r2 b - - 0 1"}
            };

            size_t num_positions = sizeof(bench_positions) / sizeof(bench_positions[0]);
            unsigned long long total_nodes = 0;
            long long start_time = current_time_ms();

            for (size_t i = 0; i < num_positions; ++i) {
                printf("\nPosition %zu/%zu: %s\n", i + 1, num_positions, bench_positions[i].name);
                printf("FEN: %s\n", bench_positions[i].fen);
                fflush(stdout);

                Board bench_board;
                board_init(&bench_board);
                if (!board_set_fen(&bench_board, bench_positions[i].fen)) {
                    printf("Error: Invalid FEN %s\n", bench_positions[i].fen);
                    continue;
                }

                RepetitionHistory bench_history;
                repetition_history_init(&bench_history);
                repetition_history_push(&bench_history, board_position_key(&bench_board));

                SearchLimits limits = {0};
                limits.depth = target_depth;

                unsigned long long nodes = 0;
                long long pos_start = current_time_ms();
                volatile bool stop_signal = false;

                Move best = think(&bench_board, &limits, &options, &bench_history, &stop_signal, &nodes);

                long long pos_time = current_time_ms() - pos_start;
                if (pos_time < 0) {
                    pos_time = 0;
                }

                char move_str[6];
                move_to_string(best, move_str);

                unsigned long long nps = (nodes * 1000ULL) / (pos_time > 0 ? pos_time : 1);
                printf("Position %s results: Bestmove: %s, Nodes: %llu, Time: %lld ms, NPS: %llu\n",
                       bench_positions[i].name, move_str, nodes, pos_time, nps);
                fflush(stdout);

                total_nodes += nodes;
            }

            long long total_time = current_time_ms() - start_time;
            if (total_time < 0) {
                total_time = 0;
            }

            unsigned long long overall_nps = (total_nodes * 1000ULL) / (total_time > 0 ? total_time : 1);
            printf("\n==================================================\n");
            printf("Total nodes: %llu\n", total_nodes);
            printf("Total time: %lld ms\n", total_time);
            printf("Overall NPS: %llu\n", overall_nps);
            printf("==================================================\n");
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "go", 2) == 0 && (line[2] == '\0' || line[2] == ' ' || line[2] == '\t' || line[2] == '\r' || line[2] == '\n')) {
            if (search_thread.thread_valid) {
                search_thread_stop_and_join(&search_thread);
            }

            SearchLimits limits;
            parse_go_limits(&limits, line);
            if (!search_thread_start(&search_thread, &board, &history, &limits, &options)) {
                print_bestmove(MOVE_NONE);
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
}
