#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "movegen.h"

typedef enum {
	EVAL_TERMINAL_NONE = 0,
	EVAL_TERMINAL_STALEMATE,
	EVAL_TERMINAL_CHECKMATE,
} EvalTerminalState;

enum { MATE_SCORE = 100000 };

int get_endgame_weight(const Board *board);
EvalTerminalState eval_terminal_state(const Board *board, bool has_legal_move);
int eval_terminal_score(EvalTerminalState terminal_state, int ply);
int evaluate_position(Board *board, const RepetitionHistory *history, int ply, const MoveList *list, bool lichess_draw_rules);

#endif
