#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "movegen.h"

typedef enum {
	EVAL_TERMINAL_NONE = 0,
	EVAL_TERMINAL_STALEMATE,
	EVAL_TERMINAL_CHECKMATE,
} EvalTerminalState;

bool eval_is_endgame_position(const Board *board);
extern const int piece_values[6];
EvalTerminalState eval_terminal_state(const Board *board, bool has_legal_move);
float eval_terminal_score(EvalTerminalState terminal_state, int ply);
float evaluate_position(Board *board, const RepetitionHistory *history, int ply, const MoveList *list, bool lichess_draw_rules);

#endif
