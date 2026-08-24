#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "movegen.h"

typedef enum {
	EVAL_TERMINAL_NONE = 0,
	EVAL_TERMINAL_STALEMATE,
	EVAL_TERMINAL_CHECKMATE,
} EvalTerminalState;

enum { MATE_SCORE = 32000 };

int get_endgame_weight(const Board *board);
EvalTerminalState eval_terminal_state(const Board *board, bool has_legal_move);
int eval_terminal_score(EvalTerminalState terminal_state, int ply);
void init_eval(void);
int evaluate_position(Board *board);

/* Python Bridge Functions */
int init_tuning_dataset(const char *dataset_path);
void free_tuning_dataset(void);
int get_tuning_dataset_size(void);
double calculate_tuning_mse(const int *weights);
void get_tuning_evaluations(const int *weights, double *out_cep_wp);
int evaluate_position_with_weights(const char *fen, const int *weights);

#endif

