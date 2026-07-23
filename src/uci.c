#include "uci.h"
#include "eval.h"

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
            move_to_string(list.moves[i], move_str);
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

void uci_loop(void) {
    init_eval();
    init_lmr();
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
            printf("id version 13.0.0\n");
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
                {"pos23", "r2qkb1r/p2b1ppp/p4n2/2p5/P1Ppp3/RN2PP3/1P2QPPP/1NB1K2R b Kkq - 0 12"},
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
            unsigned long long total_nodes = 0;
            unsigned long long bench_hash = 14695981039346656037ULL;
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
                repetition_history_push(&bench_history, bench_board.hash);

                SearchLimits limits = {0};
                limits.depth = target_depth;

                unsigned long long nodes = 0;
                long long pos_start = current_time_ms();
                volatile bool stop_signal = false;
                SearchResult result = {0};

                Move best = think(&bench_board, &limits, &options, &bench_history, &stop_signal, &nodes, &result);

                long long pos_time = current_time_ms() - pos_start;
                if (pos_time < 0) {
                    pos_time = 0;
                }

                // Update benchmark hash with search result fields to verify correctness across edits
                fnv1a_update(&bench_hash, &result.score, sizeof(result.score));
                fnv1a_update(&bench_hash, &result.move, sizeof(result.move));
                fnv1a_update(&bench_hash, &result.pv_length, sizeof(result.pv_length));
                for (int j = 0; j < result.pv_length; ++j) {
                    fnv1a_update(&bench_hash, &result.pv[j], sizeof(result.pv[j]));
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
            printf("Overall hash: 0x%016llx\n", bench_hash);
            printf("==================================================\n");
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
