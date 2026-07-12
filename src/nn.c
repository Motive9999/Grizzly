#include "defs.h"
#include "nn.h"
#include <stdint.h>
#include <string.h>

ScorePair PsqScore[PIECE_NB][SQUARE_NB];

#define V_P_MG 100
#define V_P_EG 120
#define V_N_MG 320
#define V_N_EG 320
#define V_B_MG 330
#define V_B_EG 340
#define V_R_MG 500
#define V_R_EG 540
#define V_Q_MG 950
#define V_Q_EG 1000

const Score PieceScores[PHASE_NB][PIECE_NB] = {{0,
                                               V_P_MG,
                                               V_N_MG,
                                               V_B_MG,
                                               V_R_MG,
                                               V_Q_MG,
                                               0,
                                               0,
                                               0,
                                               V_P_MG,
                                               V_N_MG,
                                               V_B_MG,
                                               V_R_MG,
                                               V_Q_MG,
                                               0,
                                               0},
{0,
V_P_EG,
V_N_EG,
V_B_EG,
V_R_EG,
V_Q_EG,
0,
0,
0,
V_P_EG,
V_N_EG,
V_B_EG,
V_R_EG,
V_Q_EG,
0,
0}};

#ifndef NN_SCALE
#define NN_SCALE 400.0f
#endif

#ifndef NN_OUTPUT_CLIP
#define NN_OUTPUT_CLIP 30000.0f
#endif

#define HCE_BLEND_DIV 4
#define TEMPO_BONUS   15

#define PHASE_N   1
#define PHASE_B   1
#define PHASE_R   2
#define PHASE_Q   4
#define PHASE_MAX 24

#define NN_EVAL_CACHE_BITS 18
#define NN_EVAL_CACHE_SIZE (1 << NN_EVAL_CACHE_BITS)

typedef struct {
        hashkey_t key;
        Score     score;
        uint8_t   valid;
} EvalCacheEntry;

/* Thread-locals are zero-initialized per thread; no explicit clearing needed.
 */
static _Thread_local EvalCacheEntry EvalCache[NN_EVAL_CACHE_SIZE];

static float NN_FC1_W_F32[NN_INPUTS * NN_HIDDEN_1];
static float NN_FC1_B_F32[NN_HIDDEN_1];
static float NN_OUT_W_F32[NN_HIDDEN_1 * 2];
static float NN_OUT_B_F32[1];

/* Runs exactly once, from psq_score_init(), before any worker thread exists.
   evaluate() assumes the f32 weights are ready — do not call it earlier. */
static void nn_prepare_f32_weights(void) {
        for (int i = 0; i < NN_INPUTS * NN_HIDDEN_1; ++i)
                NN_FC1_W_F32[i] = (float)NN_FC1_W[i];

        for (int i = 0; i < NN_HIDDEN_1; ++i)
                NN_FC1_B_F32[i] = (float)NN_FC1_B[i];

        for (int i = 0; i < NN_HIDDEN_1 * 2; ++i)
                NN_OUT_W_F32[i] = (float)NN_OUT_W[i];

        NN_OUT_B_F32[0] = (float)NN_OUT_B[0];
}

void psq_score_init(void) {
        memset(PsqScore, 0, sizeof(PsqScore));
        nn_prepare_f32_weights();
}

static inline Square pop_lsb_eval(Bitboard *bb) {
        Bitboard b  = *bb;
        Square   sq = (Square)__builtin_ctzll((uint64_t)b);

        *bb = b & (b - 1);
        return sq;
}

static inline Score blend_score(Score mg, Score eg, int phase) {
        phase = iclamp(phase, 0, PHASE_MAX);
        return (mg * phase + eg * (PHASE_MAX - phase)) / PHASE_MAX;
}

static Score phased_material_eval(const Board *b) {
        int wp = popcount(piece_bb(b, WHITE, PAWN));
        int bp = popcount(piece_bb(b, BLACK, PAWN));
        int wn = popcount(piece_bb(b, WHITE, KNIGHT));
        int bn = popcount(piece_bb(b, BLACK, KNIGHT));
        int wb = popcount(piece_bb(b, WHITE, BISHOP));
        int bb = popcount(piece_bb(b, BLACK, BISHOP));
        int wr = popcount(piece_bb(b, WHITE, ROOK));
        int br = popcount(piece_bb(b, BLACK, ROOK));
        int wq = popcount(piece_bb(b, WHITE, QUEEN));
        int bq = popcount(piece_bb(b, BLACK, QUEEN));

        Score mg = 0, eg = 0;

        mg += (wp - bp) * V_P_MG, eg += (wp - bp) * V_P_EG;
        mg += (wn - bn) * V_N_MG, eg += (wn - bn) * V_N_EG;
        mg += (wb - bb) * V_B_MG, eg += (wb - bb) * V_B_EG;
        mg += (wr - br) * V_R_MG, eg += (wr - br) * V_R_EG;
        mg += (wq - bq) * V_Q_MG, eg += (wq - bq) * V_Q_EG;

        int phase = PHASE_N * (wn + bn) + PHASE_B * (wb + bb) + PHASE_R * (wr + br) +
        PHASE_Q * (wq + bq);

        Score s = blend_score(mg, eg, phase);

        return b->sideToMove == WHITE ? s : -s;
}

static Score total_material_eval(const Board *b) {
        Score m = 0;

        m += popcount(piecetype_bb(b, PAWN)) * V_P_MG;
        m += popcount(piecetype_bb(b, KNIGHT)) * V_N_MG;
        m += popcount(piecetype_bb(b, BISHOP)) * V_B_MG;
        m += popcount(piecetype_bb(b, ROOK)) * V_R_MG;
        m += popcount(piecetype_bb(b, QUEEN)) * V_Q_MG;

        return m;
}

static inline Score abs_score(Score s) {
        return s < 0 ? -s : s;
}

static Score scale_nn_by_material(const Board *b, Score s) {
        Score mat = total_material_eval(b);

        float multiplier = (750.0f + (float)mat / 25.0f) / 1024.0f;

        if (!piece_bb(b, WHITE, QUEEN) || !piece_bb(b, BLACK, QUEEN) || mat < 4000)
                multiplier -= 0.10f;

        multiplier = multiplier < 0.65f ? 0.65f : multiplier > 1.10f ? 1.10f : multiplier;

        return (Score)((float)s * multiplier);
}

static Score material_anchor_eval(const Board *b) {
        Score matTotal = total_material_eval(b);
        Score matScore = phased_material_eval(b);

        if (matTotal < 4000 && abs_score(matScore) >= 300)
                return matScore / 2;

        if (matTotal < 2500 && abs_score(matScore) >= 200)
                return (matScore * 2) / 3;

        return matScore / HCE_BLEND_DIV;
}

static bool insufficient_material(const Board *b) {
        if (piecetype_bb(b, PAWN) | piecetype_bb(b, ROOK) | piecetype_bb(b, QUEEN))
                return false;

        Bitboard wnBB = piece_bb(b, WHITE, KNIGHT);
        Bitboard bnBB = piece_bb(b, BLACK, KNIGHT);
        Bitboard wbBB = piece_bb(b, WHITE, BISHOP);
        Bitboard bbBB = piece_bb(b, BLACK, BISHOP);

        int wn = popcount(wnBB), bn = popcount(bnBB);
        int wb = popcount(wbBB), bb = popcount(bbBB);
        int wm = wn + wb, bm = bn + bb;

        if (wm + bm <= 1)
                return true;

        if (wn == 2 && !wb && !bm)
                return true;

        if (bn == 2 && !bb && !wm)
                return true;

        if (wb == 1 && bb == 1 && !wn && !bn)
                return ((wbBB & DSQ_BB) != 0) == ((bbBB & DSQ_BB) != 0);

        return false;
}

static inline float nn_relu(float x) {
        return x > 0.0f ? x : 0.0f;
}

static inline Square mirror_sq(Square sq) {
        return sq ^ 56;
}

static inline int halfka_piece_index(Piece pc, Color perspective) {
        PieceType pt = piece_type(pc);

        if (pt < PAWN || pt > KING)
                return -1;

        if (piece_color(pc) == perspective)
                return pt == KING ? -1 : pt - PAWN;

        return 5 + (pt - PAWN);
}

static inline int
halfka_feature_raw(Square ksq, Square psq, Piece pc, Color perspective) {
        int piece = halfka_piece_index(pc, perspective);

        if (piece < 0)
                return -1;

        if (perspective == BLACK) {
                ksq = mirror_sq(ksq);
                psq = mirror_sq(psq);
        }

        return (ksq * 11 + piece) * 64 + psq;
}

static inline void nn_add_row(float *restrict h, int feature) {
        const float *restrict w = &NN_FC1_W_F32[feature * NN_HIDDEN_1];

        for (int i = 0; i < NN_HIDDEN_1; ++i)
                h[i] += w[i];
}

static void nn_accumulate_both(const Board *b,
float                                       white[restrict NN_HIDDEN_1],
float                                       black[restrict NN_HIDDEN_1]) {
        Square wk = get_king_square(b, WHITE);
        Square bk = get_king_square(b, BLACK);

        memcpy(white, NN_FC1_B_F32, sizeof(NN_FC1_B_F32));
        memcpy(black, NN_FC1_B_F32, sizeof(NN_FC1_B_F32));

        Bitboard occ = occupancy_bb(b);

        while (occ) {
                Square sq = pop_lsb_eval(&occ);
                Piece  pc = piece_on(b, sq);

                int wf = halfka_feature_raw(wk, sq, pc, WHITE);
                int bf = halfka_feature_raw(bk, sq, pc, BLACK);

                if (wf >= 0)
                        nn_add_row(white, wf);

                if (bf >= 0)
                        nn_add_row(black, bf);
        }

        for (int i = 0; i < NN_HIDDEN_1; ++i) {
                white[i] = nn_relu(white[i]);
                black[i] = nn_relu(black[i]);
        }
}

static inline float nn_output(const float *restrict us, const float *restrict them) {
        const float *restrict wUs   = NN_OUT_W_F32;
        const float *restrict wThem = NN_OUT_W_F32 + NN_HIDDEN_1;

        float s0 = NN_OUT_B_F32[0], s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;

        int i = 0, n = NN_HIDDEN_1 & ~3;

        for (; i < n; i += 4) {
                s0 += us[i + 0] * wUs[i + 0] + them[i + 0] * wThem[i + 0];
                s1 += us[i + 1] * wUs[i + 1] + them[i + 1] * wThem[i + 1];
                s2 += us[i + 2] * wUs[i + 2] + them[i + 2] * wThem[i + 2];
                s3 += us[i + 3] * wUs[i + 3] + them[i + 3] * wThem[i + 3];
        }

        for (; i < NN_HIDDEN_1; ++i)
                s0 += us[i] * wUs[i] + them[i] * wThem[i];

        return (s0 + s1) + (s2 + s3);
}

static Score nn_eval_cp(const Board *b) {
        float white[NN_HIDDEN_1];
        float black[NN_HIDDEN_1];

        nn_accumulate_both(b, white, black);

        float s = (b->sideToMove == WHITE ? nn_output(white, black)
                                          : nn_output(black, white)) *
        NN_SCALE;

        s = s > NN_OUTPUT_CLIP ? NN_OUTPUT_CLIP
        : s < -NN_OUTPUT_CLIP  ? -NN_OUTPUT_CLIP
                               : s;

        return (Score)s;
}

static Score evaluate_uncached(const Board *b) {
        if (insufficient_material(b))
                return 0;

        Score s = scale_nn_by_material(b, nn_eval_cp(b));

        s += material_anchor_eval(b);
        s += TEMPO_BONUS;

        return iclamp(s, -MATE_FOUND + 1, MATE_FOUND - 1);
}

Score evaluate(const Board *b) {
        hashkey_t       key = b->stack->boardKey;
        EvalCacheEntry *e   = &EvalCache[(size_t)key & (NN_EVAL_CACHE_SIZE - 1)];

        if (e->valid && e->key == key)
                return e->score;

        Score s = evaluate_uncached(b);

        e->key   = key;
        e->score = s;
        e->valid = 1;

        return s;
}