#include "defs.h"
#include <stdio.h>
#include <stdlib.h>

#define SEARCH_DEPTH         35
#define CURR_DISPLAY         18
#define DISPLAY_PV           1

#define RZR_MARGIN           135
#define RFP_MARGIN_PER_DEPTH 85
#define RFP_IMPROVING_BONUS  73
#define FP_BASE_MARGIN       186
#define FP_DEPTH_MARGIN      67

#define CHP_BASE             421
#define CHP_DEPTH_SCALE      2839

#define SEE_QUIET_PER_DEPTH  -49
#define SEE_NOISY_PER_DEPTH2 -22

#define LMR_HIST_DIV         6307
#define QFP_BASE             110

#define FALL          __attribute__((fallthrough))
#define STOP          wpool_is_stopped(&SearchWorkerPool)
#define NODE          atomic_fetch_add_explicit(&w->nodes, 1, memory_order_relaxed)
#define TTS(x)        score_to_tt(x, ss->plies)
#define ZWS(d, b2, c) (-search(false, b, (d), -(b2), 1 - (b2), ss + 1, (c)))

extern int Pruning[2][16];

extern int  lmr_base_value(int depth, int moves, bool improving, bool quiet);
extern void update_pv(move_t *pv, move_t best, move_t *sub);

TranspositionTable SearchTT = {0, NULL, 0};

typedef struct {
        size_t    start, end;
        pthread_t thread;
} BzeroThread;

static void die(const char *what) {
        perror(what);
        exit(EXIT_FAILURE);
}

static int tt_relevance(const TT_Entry *e) {
        return e->depth - ((259 + SearchTT.generation - e->genbound) & 0xFC);
}

static void *tt_bzero_thread(void *data) {
        BzeroThread   *td   = data;
        const TT_Entry zero = {0, NO_SCORE, NO_SCORE, 0, 0, NO_MOVE};

        for (size_t i = td->start; i < td->end; ++i)
                for (size_t j = 0; j < ClusterSize; ++j)
                        SearchTT.table[i].clEntry[j] = zero;
        return NULL;
}

void tt_bzero(size_t threads) {
        BzeroThread *tl = threads ? malloc(threads * sizeof(BzeroThread)) : NULL;
        const size_t n  = SearchTT.clusterCount;

        if (!tl)
                die("TT zero failed");
        for (size_t i = 0; i < threads; ++i) {
                tl[i].start = n * (i + 0) / threads;
                tl[i].end   = n * (i + 1) / threads;
        }
        for (size_t i = 1; i < threads; ++i)
                if (pthread_create(&tl[i].thread, NULL, tt_bzero_thread, tl + i))
                        die("TT zero thread failed");
        tt_bzero_thread(tl);
        for (size_t i = 1; i < threads; ++i)
                pthread_join(tl[i].thread, NULL);
        free(tl);
}

int tt_hashfull(void) {
        const uint8_t gen   = SearchTT.generation;
        int           count = 0;

        for (int i = 0; i < 1000; ++i)
                for (int j = 0; j < ClusterSize; ++j)
                        count += (SearchTT.table[i].clEntry[j].genbound & 0xFC) == gen;
        return count / ClusterSize;
}

void tt_resize(size_t mb) {
        const size_t count = mb * 1024 * 1024 / sizeof(TT_Cluster);

        free(SearchTT.table);
        SearchTT.clusterCount = count;
        SearchTT.table        = mb ? malloc(count * sizeof(TT_Cluster)) : NULL;
        if (!mb)
                return;
        if (!SearchTT.table)
                die("TT allocation failed");
        tt_bzero((size_t)UciOptionFields.threads);
}

TT_Entry *tt_probe(hashkey_t key, bool *found) {
        const uint8_t gen = SearchTT.generation;
        TT_Entry     *e   = tt_entry_at(key);
        TT_Entry     *rep = e;

        for (int i = 0; i < ClusterSize; ++i)
                if (!e[i].key || e[i].key == key) {
                        e[i].genbound = (uint8_t)(gen | (e[i].genbound & 0x3));
                        *found        = (bool)e[i].key;
                        return e + i;
                }
        for (int i = 1; i < ClusterSize; ++i)
                if (tt_relevance(rep) > tt_relevance(e + i))
                        rep = e + i;
        *found = false;
        return rep;
}

void tt_save(TT_Entry *e, hashkey_t k, score_t s, score_t ev, int d, int b, move_t m) {
        if (m || k != e->key)
                e->bestmove = (uint16_t)m;
        if (b != EXACT_BOUND && k == e->key && d + 4 < e->depth)
                return;
        e->key      = k;
        e->score    = s;
        e->eval     = ev;
        e->genbound = SearchTT.generation | (uint8_t)b;
        e->depth    = d;
}

static inline piecetype_t cap_type(const Board *b, move_t m) {
        return move_type(m) == PROMOTION ? piece_type(promotion_type(m))
            : move_type(m) == EN_PASSANT ? PAWN
                                         : piece_type(piece_on(b, to_sq(m)));
}

static inline void upd_cont(Searchstack *ss, int d, piece_t pc, square_t to, bool good) {
        int bonus = good ? history_bonus(d) : -history_bonus(d);
        for (int i = 1; i <= 4; i += i)
                if ((ss - i)->pieceHistory)
                        add_pc_history(*(ss - i)->pieceHistory, pc, to, bonus);
}

static inline void upd_cap(capture_history_t *ch, const Board *b, move_t m, int bonus) {
        piece_t pc = piece_on(b, from_sq(m));
        add_cap_history(*ch, pc, to_sq(m), cap_type(b, m), bonus);
}

static inline int cont_score(const Board *b, const Searchstack *ss, move_t m) {
        piece_t  pc = piece_on(b, from_sq(m));
        square_t to = to_sq(m);
        int      h  = 0;
        for (int i = 1; i <= 4; i += i)
                if ((ss - i)->pieceHistory)
                        h += get_pc_history_score(*(ss - i)->pieceHistory, pc, to);
        return h;
}

static inline int
hist_score(const Board *b, const Worker *w, const Searchstack *ss, move_t m) {
        piece_t pc = piece_on(b, from_sq(m));
        return get_bf_history_score(w->bfHistory, pc, m) + cont_score(b, ss, m);
}

static inline score_t
quiet_rank(const Movepicker *mp, piece_t pc, square_t to, move_t m) {
        score_t s = get_bf_history_score(mp->worker->bfHistory, pc, m) / 2;
        for (int i = 0; i < 2; ++i)
                if (mp->pieceHistory[i])
                        s += get_pc_history_score(*mp->pieceHistory[i], pc, to);
        return s;
}

uint64_t perft(Board *b, unsigned d) {
        if (d == 0)
                return 1;
        Movelist ml;
        list_all(&ml, b);
        if (d == 1)
                return movelist_size(&ml);
        uint64_t   n = 0;
        Boardstack st;
        for (ExtendedMove *em = ml.moves; em < ml.last; ++em) {
                do_move(b, em->move, &st);
                n += perft(b, d - 1);
                undo_move(b, em->move);
        }
        return n;
}

void search_print_root_info(Board *b,
    Worker                        *w,
    int                            mpv,
    int                            iter,
    clock_t                        time,
    int                            bound) {
        bool one = mpv == 1 && (bound == EXACT_BOUND || time > 3000);
        bool all = mpv > 1 && bound == EXACT_BOUND &&
            (w->pvLine == mpv - 1 || time > 3000);
        if (w->idx || !(one || all))
                return;
        for (int i = 0; i < mpv; ++i) {
                move_t hold                    = w->rootMoves[i].pv[DISPLAY_PV];
                w->rootMoves[i].pv[DISPLAY_PV] = NO_MOVE;
                print_pv(b, w->rootMoves + i, i + 1, iter, time, bound);
                w->rootMoves[i].pv[DISPLAY_PV] = hold;
        }
        fflush(stdout);
}

static void update_quiet_stats(const Board *b,
    int                                     d,
    move_t                                  bm,
    const move_t                           *q,
    int                                     qc,
    Searchstack                            *ss) {
        Worker  *w     = get_worker(b);
        int      bonus = history_bonus(d);
        piece_t  pc    = piece_on(b, from_sq(bm));
        square_t to    = to_sq(bm);
        if ((ss - 1)->pieceHistory) {
                square_t lto                        = to_sq((ss - 1)->currentMove);
                w->cmHistory[piece_on(b, lto)][lto] = bm;
        }
        add_bf_history(w->bfHistory, pc, bm, bonus);
        upd_cont(ss, d, pc, to, true);
        if (ss->killers[0] != bm) {
                ss->killers[1] = ss->killers[0];
                ss->killers[0] = bm;
        }
        for (int i = 0; i < qc; ++i) {
                pc = piece_on(b, from_sq(q[i]));
                add_bf_history(w->bfHistory, pc, q[i], -bonus);
                upd_cont(ss, d, pc, to_sq(q[i]), false);
        }
}

static void update_capture_stats(const Board *b,
    int                                       d,
    move_t                                    bm,
    const move_t                             *c,
    int                                       cc,
    Searchstack                              *ss) {
        (void)ss;
        Worker *w     = get_worker(b);
        int     bonus = history_bonus(d);
        if (capture_or_promotion(b, bm))
                upd_cap(&w->capHistory, b, bm, bonus);
        for (int i = 0; i < cc; ++i)
                upd_cap(&w->capHistory, b, c[i], -bonus);
}

static void rank_captures(Movepicker *mp, ExtendedMove *bg, ExtendedMove *ed) {
        static const score_t victim[PIECETYPE_NB] = {0, 0, 640, 640, 1280, 2560, 0, 0};
        for (; bg < ed; ++bg) {
                piece_t     pc  = piece_on(mp->board, from_sq(bg->move));
                piecetype_t cap = cap_type(mp->board, bg->move);
                square_t    to  = to_sq(bg->move);
                bg->score       = victim[cap];
                bg->score += get_cap_history_score(mp->worker->capHistory, pc, to, cap);
        }
}

static void rank_quiets(Movepicker *mp, ExtendedMove *bg, ExtendedMove *ed) {
        for (; bg < ed; ++bg) {
                piece_t pc = piece_on(mp->board, from_sq(bg->move));
                bg->score  = quiet_rank(mp, pc, to_sq(bg->move), bg->move);
        }
}

static void rank_evasions(Movepicker *mp, ExtendedMove *bg, ExtendedMove *ed) {
        for (; bg < ed; ++bg) {
                move_t  m  = bg->move;
                piece_t pc = piece_on(mp->board, from_sq(m));
                if (capture_or_promotion(mp->board, m))
                        bg->score = 28672 + cap_type(mp->board, m) * 8 - piece_type(pc);
                else
                        bg->score = quiet_rank(mp, pc, to_sq(m), m);
        }
}

static void movepicker_setup(Movepicker *mp,
    bool                                 qs,
    const Board                         *b,
    const Worker                        *w,
    move_t                               tt,
    Searchstack                         *ss) {
        mp->inQsearch = qs;
        mp->board     = b;
        mp->worker    = w;
        mp->ttMove    = tt;
        mp->killer1   = ss->killers[0];
        mp->killer2   = ss->killers[1];
        if (b->stack->checkers)
                mp->stage = CHECK_PICK_TT + !(tt && move_pseudo_legal(b, tt));
        else
                mp->stage = PICK_TT +
                    !(tt && (!qs || capture_or_promotion(b, tt)) &&
                        move_pseudo_legal(b, tt));
        square_t lto = (ss - 1)->pieceHistory ? to_sq((ss - 1)->currentMove) : SQ_NONE;
        mp->counter  = lto != SQ_NONE ? w->cmHistory[piece_on(b, lto)][lto] : NO_MOVE;
        mp->pieceHistory[0] = (ss - 1)->pieceHistory;
        mp->pieceHistory[1] = (ss - 2)->pieceHistory;
}

static move_t movepicker_next(Movepicker *mp, bool skipQuiets, int see_th) {
top:
        switch (mp->stage) {
                case PICK_TT:
                case CHECK_PICK_TT:
                        ++mp->stage;
                        return mp->ttMove;
                case GEN_INSTABLE:
                        ++mp->stage;
                        mp->list.last = generate_captures(mp->list.moves,
                            mp->board,
                            mp->inQsearch);
                        rank_captures(mp, mp->list.moves, mp->list.last);
                        mp->cur = mp->badCaptures = mp->list.moves;
                        FALL;
                case PICK_GOOD_INSTABLE:
                        while (mp->cur < mp->list.last) {
                                place_top_move(mp->cur, mp->list.last);
                                if (mp->cur->move != mp->ttMove &&
                                    see_greater_than(mp->board, mp->cur->move, see_th))
                                        return (mp->cur++)->move;
                                *(mp->badCaptures++) = *(mp->cur++);
                        }
                        if (mp->inQsearch) {
                                mp->cur   = mp->list.moves;
                                mp->stage = PICK_BAD_INSTABLE;
                                goto top;
                        }
                        ++mp->stage;
                        FALL;
                case PICK_KILLER1:
                        ++mp->stage;
                        if (mp->killer1 && mp->killer1 != mp->ttMove &&
                            move_pseudo_legal(mp->board, mp->killer1))
                                return mp->killer1;
                        FALL;
                case PICK_KILLER2:
                        ++mp->stage;
                        if (mp->killer2 && mp->killer2 != mp->ttMove &&
                            mp->killer2 != mp->killer1 &&
                            move_pseudo_legal(mp->board, mp->killer2))
                                return mp->killer2;
                        FALL;
                case PICK_COUNTER:
                        ++mp->stage;
                        if (mp->counter && mp->counter != mp->ttMove &&
                            mp->counter != mp->killer1 && mp->counter != mp->killer2 &&
                            move_pseudo_legal(mp->board, mp->counter))
                                return mp->counter;
                        FALL;
                case GEN_QUIET:
                        ++mp->stage;
                        if (!skipQuiets) {
                                mp->list.last = generate_quiet(mp->cur, mp->board);
                                rank_quiets(mp, mp->cur, mp->list.last);
                        }
                        FALL;
                case PICK_QUIET:
                        while (!skipQuiets && mp->cur < mp->list.last) {
                                place_top_move(mp->cur, mp->list.last);
                                move_t m = (mp->cur++)->move;
                                if (m != mp->ttMove && m != mp->killer1 &&
                                    m != mp->killer2 && m != mp->counter)
                                        return m;
                        }
                        ++mp->stage;
                        mp->cur = mp->list.moves;
                        FALL;
                case PICK_BAD_INSTABLE:
                        for (; mp->cur < mp->badCaptures; ++mp->cur)
                                if (mp->cur->move != mp->ttMove)
                                        return (mp->cur++)->move;
                        break;
                case CHECK_GEN_ALL:
                        ++mp->stage;
                        mp->list.last = generate_evasions(mp->list.moves, mp->board);
                        rank_evasions(mp, mp->list.moves, mp->list.last);
                        mp->cur = mp->list.moves;
                        FALL;
                case CHECK_PICK_ALL:
                        for (; mp->cur < mp->list.last; ++mp->cur) {
                                place_top_move(mp->cur, mp->list.last);
                                if (mp->cur->move != mp->ttMove)
                                        return (mp->cur++)->move;
                        }
                        break;
        }
        return NO_MOVE;
}

score_t search(bool isPV,
    Board          *b,
    int             depth,
    score_t         alpha,
    score_t         beta,
    Searchstack    *ss,
    bool            cut) {
        bool    root = ss->plies == 0;
        Worker *w    = get_worker(b);
        if (root && w->rootDepth > SEARCH_DEPTH) {
                wpool_stop(&SearchWorkerPool);
                return draw_score(w);
        }
        if (!root && b->stack->rule50 >= 3 && alpha < 0 && game_has_cycle(b, ss->plies)) {
                alpha = draw_score(w);
                if (alpha >= beta)
                        return alpha;
        }
        if (depth <= 0)
                return qsearch(isPV, b, alpha, beta, ss);
        if (!w->idx)
                check_time();
        if (STOP)
                return draw_score(w);
        if (isPV && w->seldepth < ss->plies + 1)
                w->seldepth = ss->plies + 1;
        if (!root && game_is_drawn(b, ss->plies))
                return draw_score(w);
        if (ss->plies >= MAX_PLIES)
                return !b->stack->checkers ? evaluate(b) : draw_score(w);
        if (!root) {
                alpha = imax(alpha, mated_in(ss->plies));
                beta  = imin(beta, mate_in(ss->plies + 1));
                if (alpha >= beta)
                        return alpha;
        }
        bool      inCheck = !!b->stack->checkers;
        hashkey_t key     = b->stack->boardKey ^ ((hashkey_t)ss->excludedMove << 16);
        bool      found;
        TT_Entry *e       = tt_probe(key, &found);
        int       ttDepth = 0, ttBound = NO_BOUND;
        score_t   ttScore = NO_SCORE, eval;
        move_t    ttMove  = NO_MOVE;
        if (found) {
                ttScore = score_from_tt(e->score, ss->plies);
                ttBound = e->genbound & 3;
                ttDepth = e->depth;
                ttMove  = e->bestmove;
                if (ttDepth >= depth && !isPV && b->stack->rule50 < 96 &&
                    (abs(ttScore) < VICTORY || ttBound == EXACT_BOUND) &&
                    (((ttBound & LOWER_BOUND) && ttScore >= beta) ||
                        ((ttBound & UPPER_BOUND) && ttScore <= alpha))) {
                        if ((ttBound & LOWER_BOUND) && ttMove &&
                            !capture_or_promotion(b, ttMove))
                                update_quiet_stats(b, depth, ttMove, NULL, 0, ss);
                        return ttScore;
                }
        }
        (ss + 2)->killers[0] = (ss + 2)->killers[1] = NO_MOVE;
        ss->doubleExtensions                        = (ss - 1)->doubleExtensions;
        bool improving                              = false;
        if (inCheck) {
                eval = ss->staticEval = NO_SCORE;
                goto main_loop;
        }
        if (found) {
                eval = ss->staticEval = e->eval;
                if (ttBound & (ttScore > eval ? LOWER_BOUND : UPPER_BOUND))
                        eval = ttScore;
        } else {
                eval = ss->staticEval = evaluate(b);
                tt_save(e, key, NO_SCORE, eval, 0, NO_BOUND, NO_MOVE);
        }
        if (root && w->pvLine)
                ttMove = w->rootMoves[w->pvLine].move;
        if (ss->plies >= 2)
                improving = ss->staticEval > (ss - 2)->staticEval;
        if (!isPV && depth == 1 && ss->staticEval + RZR_MARGIN <= alpha)
                return qsearch(false, b, alpha, beta, ss);
        if (!isPV && depth <= 8 &&
            eval - RFP_MARGIN_PER_DEPTH * depth + RFP_IMPROVING_BONUS * improving >=
                beta &&
            eval < VICTORY)
                return (716 * beta + 308 * eval) / 1024;
        if (!isPV && depth >= 3 && ss->plies >= w->verifPlies && !ss->excludedMove &&
            eval >= beta && eval >= ss->staticEval && b->stack->material[b->sideToMove]) {
                Boardstack st;
                int        R    = (792 + 67 * depth) / 256 + imin((eval - beta) / 109, 5);
                ss->currentMove = NULL_MOVE;
                ss->pieceHistory = NULL;
                do_null_move(b, &st);
                NODE;
                score_t s = ZWS(depth - R, beta, !cut);
                undo_null_move(b);
                if (s >= beta) {
                        if (s > MATE_FOUND)
                                s = beta;
                        if (w->verifPlies || (depth <= 12 && abs(beta) < VICTORY))
                                return s;
                        w->verifPlies = ss->plies + (depth - R) * 3 / 4;
                        score_t
                            v = search(false, b, depth - R, beta - 1, beta, ss, false);
                        w->verifPlies = 0;
                        if (v >= beta)
                                return s;
                }
        }
        score_t pcBeta = beta + 214 - 59 * improving;
        if (!root && depth >= 6 && abs(beta) < VICTORY &&
            !(found && ttDepth >= depth - 4 && ttScore < pcBeta)) {
                score_t    pcSEE = pcBeta - ss->staticEval, sEv = ss->staticEval;
                Movepicker mp;
                move_t     m, pcTT = NO_MOVE;
                if (ttMove && see_greater_than(b, ttMove, pcSEE))
                        pcTT = ttMove;
                movepicker_setup(&mp, true, b, w, pcTT, ss);
                while ((m = movepicker_next(&mp, false, pcSEE)) != NO_MOVE) {
                        if (mp.stage == PICK_BAD_INSTABLE)
                                break;
                        if (!move_is_legal(b, m) || m == ss->excludedMove)
                                continue;
                        piece_t pc       = piece_on(b, from_sq(m));
                        ss->currentMove  = m;
                        ss->pieceHistory = &w->ctHistory[pc][to_sq(m)];
                        Boardstack st;
                        bool       chk = move_gives_check(b, m);
                        do_move_gc(b, m, &st, chk);
                        NODE;
                        score_t s = -qsearch(false, b, -pcBeta, 1 - pcBeta, ss + 1);
                        if (s >= pcBeta)
                                s = ZWS(depth - 4, pcBeta, !cut);
                        undo_move(b, m);
                        if (s < pcBeta)
                                continue;
                        tt_save(e, key, TTS(s), sEv, depth - 3, LOWER_BOUND, m);
                        return s;
                }
        }
        if (!root && !found && depth >= 3)
                --depth;

main_loop:;
        Movepicker mp;
        movepicker_setup(&mp, false, b, w, ttMove, ss);
        move_t  best = NO_MOVE;
        move_t  m;
        move_t  pv[256];
        int     moves = 0;
        int     qc    = 0;
        int     cc    = 0;
        move_t  quiets[64];
        move_t  caps[64];
        bool    skipQ = false;
        score_t Score = -INF_SCORE;
        while ((m = movepicker_next(&mp, skipQ, 0)) != NO_MOVE) {
                if (root) {
                        if (!find_root_move(w->rootMoves + w->pvLine,
                                w->rootMoves + w->rootCount,
                                m))
                                continue;
                } else {
                        if (!move_is_legal(b, m) || m == ss->excludedMove)
                                continue;
                }
                ++moves;
                bool quiet = !capture_or_promotion(b, m);
                if (!root && Score > -MATE_FOUND) {
                        if (depth <= 8 && moves > Pruning[improving][depth])
                                skipQ = true;
                        if (depth <= 7 && !inCheck && quiet &&
                            eval + FP_BASE_MARGIN + FP_DEPTH_MARGIN * depth <= alpha)
                                skipQ = true;
                        if (depth <= 4 &&
                            cont_score(b, ss, m) <
                                CHP_BASE - CHP_DEPTH_SCALE * (depth - 1))
                                continue;
                        if (depth <= 12 && (!quiet || moves > 4) &&
                            !see_greater_than(b,
                                m,
                                quiet ? SEE_QUIET_PER_DEPTH * depth
                                      : SEE_NOISY_PER_DEPTH2 * depth * depth))
                                continue;
                }
                if (root && !w->idx && w->rootDepth >= CURR_DISPLAY) {
                        printf("info depth %d currmove %s currmovenumber %d\n",
                            depth,
                            move_to_str(m, b->chess960),
                            moves + w->pvLine);
                        fflush(stdout);
                }
                Boardstack st;
                int        ext      = 0;
                int        newDepth = depth - 1;
                int        R        = 0;
                bool       chk      = move_gives_check(b, m);
                piece_t    pc       = piece_on(b, from_sq(m));
                int        hist     = quiet ? hist_score(b, w, ss, m) : 0;
                score_t    s        = -NO_SCORE;
                if (!root && ss->plies < 2 * w->rootDepth &&
                    2 * ss->doubleExtensions < w->rootDepth) {
                        if (depth >= 8 && m == ttMove && !ss->excludedMove &&
                            (ttBound & LOWER_BOUND) && abs(ttScore) < VICTORY &&
                            ttDepth >= depth - 3) {
                                score_t sBeta    = ttScore - 11 * depth / 16;
                                int     sDepth   = depth / 2 + 1;
                                ss->excludedMove = ttMove;
                                score_t sScore   = search(false,
                                    b,
                                    sDepth,
                                    sBeta - 1,
                                    sBeta,
                                    ss,
                                    cut);
                                ss->excludedMove = NO_MOVE;
                                if (sScore < sBeta) {
                                        if (!isPV && sBeta - sScore > 17 &&
                                            ss->doubleExtensions <= 9) {
                                                ext = 2;
                                                ss->doubleExtensions++;
                                        } else {
                                                ext = 1;
                                        }
                                } else if (sBeta >= beta) {
                                        return sBeta;
                                }
                        } else if (chk) {
                                ext = 1;
                        }
                }
                ss->currentMove  = m;
                ss->pieceHistory = &w->ctHistory[pc][to_sq(m)];
                do_move_gc(b, m, &st, chk);
                NODE;
                bool lmr = depth >= 3 && moves > 1 + 3 * isPV;
                if (lmr) {
                        R = lmr_base_value(depth, moves, improving, quiet);
                        R += !isPV + cut;
                        R -= (m == mp.killer1 || m == mp.killer2 || m == mp.counter);
                        R -= quiet && !see_greater_than(b, reverse_move(m), 0);
                        R -= iclamp(hist / LMR_HIST_DIV, -3, 3);
                        R = iclamp(R, 0, newDepth - 1);
                }
                if (lmr)
                        s = -search(false,
                            b,
                            newDepth - R,
                            -alpha - 1,
                            -alpha,
                            ss + 1,
                            true);
                newDepth += ext;
                if ((R && s > alpha) || (!lmr && !(isPV && moves == 1))) {
                        if (R && newDepth < depth && s > Score + 52)
                                ++newDepth;
                        s = -search(false, b, newDepth, -alpha - 1, -alpha, ss + 1, !cut);
                        if (R)
                                upd_cont(ss, depth, pc, to_sq(m), s > alpha);
                }
                if (isPV && (moves == 1 || s > alpha)) {
                        (ss + 1)->pv = pv;
                        pv[0]        = NO_MOVE;
                        s = -search(true, b, newDepth, -beta, -alpha, ss + 1, false);
                }
                undo_move(b, m);
                if (STOP)
                        return 0;
                if (root) {
                        RootMove *rm = find_root_move(w->rootMoves + w->pvLine,
                            w->rootMoves + w->rootCount,
                            m);
                        if (moves == 1 || s > alpha) {
                                rm->score    = s;
                                rm->seldepth = w->seldepth;
                                rm->pv[0]    = m;
                                update_pv(rm->pv, m, (ss + 1)->pv);
                        } else {
                                rm->score = -INF_SCORE;
                        }
                }
                if (Score < s) {
                        Score = s;
                        if (alpha < Score) {
                                best  = m;
                                alpha = Score;
                                if (isPV && !root)
                                        update_pv(ss->pv, m, (ss + 1)->pv);
                                if (alpha >= beta) {
                                        if (quiet)
                                                update_quiet_stats(b,
                                                    depth,
                                                    best,
                                                    quiets,
                                                    qc,
                                                    ss);
                                        if (moves != 1)
                                                update_capture_stats(b,
                                                    depth,
                                                    best,
                                                    caps,
                                                    cc,
                                                    ss);
                                        break;
                                }
                        }
                }
                if (qc < 64 && quiet)
                        quiets[qc++] = m;
                else if (cc < 64 && !quiet)
                        caps[cc++] = m;
        }
        if (moves == 0)
                Score = ss->excludedMove ? alpha : inCheck ? mated_in(ss->plies) : 0;
        if (Score >= beta && abs(Score) < VICTORY)
                Score = (Score * depth + beta) / (depth + 1);
        if (!root || w->pvLine == 0) {
                int bound = Score >= beta ? LOWER_BOUND
                    : (isPV && best)      ? EXACT_BOUND
                                          : UPPER_BOUND;
                tt_save(e,
                    key,
                    score_to_tt(Score, ss->plies),
                    ss->staticEval,
                    depth,
                    bound,
                    best);
        }
        return Score;
}

score_t qsearch(bool isPV, Board *b, score_t alpha, score_t beta, Searchstack *ss) {
        Worker       *w        = get_worker(b);
        const score_t oldAlpha = alpha;
        hashkey_t     key      = b->stack->boardKey;
        if (!w->idx)
                check_time();
        if (isPV && w->seldepth < ss->plies + 1)
                w->seldepth = ss->plies + 1;
        if (STOP || game_is_drawn(b, ss->plies))
                return draw_score(w);
        if (ss->plies >= MAX_PLIES)
                return !b->stack->checkers ? evaluate(b) : draw_score(w);
        alpha = imax(alpha, mated_in(ss->plies));
        beta  = imin(beta, mate_in(ss->plies + 1));
        if (alpha >= beta)
                return alpha;
        bool      found;
        TT_Entry *e       = tt_probe(key, &found);
        score_t   ttScore = NO_SCORE;
        int       ttBound = NO_BOUND;
        move_t    ttMove  = NO_MOVE;
        if (found) {
                ttBound = e->genbound & 3;
                ttScore = score_from_tt(e->score, ss->plies);
                ttMove  = e->bestmove;
                if (!isPV && (abs(ttScore) < VICTORY || ttBound == EXACT_BOUND) &&
                    (((ttBound & LOWER_BOUND) && ttScore >= beta) ||
                        ((ttBound & UPPER_BOUND) && ttScore <= alpha)))
                        return ttScore;
        }
        bool    inCheck = !!b->stack->checkers;
        score_t eval, Score;
        if (inCheck) {
                eval  = NO_SCORE;
                Score = -INF_SCORE;
        } else {
                eval = Score = found ? e->eval : evaluate(b);
                if (found && (ttBound & (ttScore > eval ? LOWER_BOUND : UPPER_BOUND)))
                        Score = ttScore;
                alpha = imax(alpha, Score);
                if (alpha >= beta) {
                        if (!found)
                                tt_save(e,
                                    key,
                                    TTS(Score),
                                    eval,
                                    0,
                                    LOWER_BOUND,
                                    NO_MOVE);
                        return alpha;
                }
        }
        Movepicker mp;
        movepicker_setup(&mp, true, b, w, ttMove, ss);
        move_t  best    = NO_MOVE, m, pv[256];
        int     moves   = 0;
        bool    canFP   = !inCheck && popcount(occupancy_bb(b)) >= 5;
        score_t futBase = Score + QFP_BASE;
        if (isPV)
                (ss + 1)->pv = pv;
        while ((m = movepicker_next(&mp, false, 0)) != NO_MOVE) {
                if (Score > -MATE_FOUND && mp.stage == PICK_BAD_INSTABLE)
                        break;
                if (!move_is_legal(b, m))
                        continue;
                ++moves;
                bool     chk = move_gives_check(b, m);
                square_t to  = to_sq(m);
                if (Score > -MATE_FOUND && canFP && !chk && move_type(m) == NORMAL_MOVE) {
                        score_t delta = futBase + PieceScores[ENDGAME][piece_on(b, to)];
                        if (delta < alpha)
                                continue;
                        if (futBase < alpha && !see_greater_than(b, m, 1))
                                continue;
                }
                ss->currentMove  = m;
                ss->pieceHistory = &w->ctHistory[piece_on(b, from_sq(m))][to];
                Boardstack st;
                if (isPV)
                        pv[0] = NO_MOVE;
                do_move_gc(b, m, &st, chk);
                NODE;
                score_t s = -qsearch(isPV, b, -beta, -alpha, ss + 1);
                undo_move(b, m);
                if (STOP)
                        return 0;
                if (s > Score) {
                        Score = s;
                        if (s > alpha) {
                                alpha = s;
                                best  = m;
                                if (isPV)
                                        update_pv(ss->pv, best, (ss + 1)->pv);
                                if (s >= beta)
                                        break;
                        }
                }
        }
        if (moves == 0 && inCheck)
                Score = mated_in(ss->plies);
        int bound = Score >= beta ? LOWER_BOUND
            : Score <= oldAlpha   ? UPPER_BOUND
                                  : EXACT_BOUND;
        tt_save(e, key, TTS(Score), eval, 0, bound, best);
        return Score;
}