#include "uci.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                      task->stop_requested);
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
    options.overhead_ms = 100;
    options.multipv = 1;
    options.lichess_draw_rules = false;

    SearchThreadState search_thread = {0};
    pthread_mutex_init(&search_thread.mutex, NULL);

    char line[4096];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        search_thread_join_if_finished(&search_thread);

        if (strncmp(line, "uci", 3) == 0 && (line[3] == '\0' || line[3] == ' ' || line[3] == '\t' || line[3] == '\r' || line[3] == '\n')) {
            printf("id name Cepimetheus\n");
            printf("id author  George Bland\n");
            printf("option name overhead type spin default 100 min 0 max 10000\n");
            printf("option name MultiPV type spin default 1 min 1 max 256\n");
            printf("option name lichess_draw_rules type check default false\n");
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
                } else if (strncmp(nametoken, "lichess_draw_rules", 18) == 0) {
                    if (valuetoken != NULL) {
                        valuetoken += 5;
                        while (*valuetoken == ' ' || *valuetoken == '\t') valuetoken++;
                        options.lichess_draw_rules = (strncmp(valuetoken, "true", 4) == 0);
                    } else {
                        // No value token means set to true
                        options.lichess_draw_rules = true;
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
