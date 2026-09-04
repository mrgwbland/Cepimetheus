#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "movegen.h"
#include <stdint.h>

typedef struct {
    int16_t custom_eg_scale;        // Endgame scale from scale_fn (default 256)
    int8_t  is_ocb;                 // Opposite-colored bishop flag
    int8_t  side_to_move;           // +1 for WHITE, -1 for BLACK
    int8_t  is_special_eval;        // 1 if eg_entry->eval_fn was used
    int32_t special_eval_score;

    int8_t  total_pieces[4];        // [0]=N, [1]=B, [2]=R, [3]=Q (White + Black total on board)
    int8_t  piece_counts[5];        // [0]=Pawn, [1]=N, [2]=B, [3]=R, [4]=Q (White - Black)
    int8_t  passed_pawn_counts[6];  // Rank 2 to 7 -> index 0 to 5
    int8_t  phalanx_pawn_counts[6]; // Rank 2 to 7 -> index 0 to 5
    int16_t eval_param_counts_mg[24]; // 24 general evaluation parameters for MG
    int16_t eval_param_counts_eg[24]; // 24 general evaluation parameters for EG

    int16_t white_attackers[5];     // [0]=Pawn, [1]=N, [2]=B, [3]=R, [4]=Q
    int16_t white_defenders[5];
    int16_t black_attackers[5];
    int16_t black_defenders[5];
} PositionFeatures;

typedef PositionFeatures EvalTrace;

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

