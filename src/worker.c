#include "defs.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH_LIMIT (MAX_PLIES - 1)

extern uint64_t perft(Board *board, unsigned depth);

WorkerPool      SearchWorkerPool;
static int      Reductions[2][256];
int             Pruning[2][16];

const double    BestmoveTypeScale[BM_TYPE_NB] = {0.20, 0.45, 0.50, 0.85, 0.95, 1.00, 1.20, 1.40};
const double    BestmoveStabilityScale[5] = {2.50, 1.20, 0.90, 0.80, 0.75};

static void die(const char *m) {
        perror(m);
        exit(EXIT_FAILURE);
}

INLINED clock_t timemin(clock_t l, clock_t r) {
        return l < r ? l : r;
}

double score_difference_scale(score_t s) {
        return pow(2.0, iclamp(s, -100, 100) / 100.0);
}

int lmr_base_value(int depth, int moves, bool improving, bool quiet) {
        return (-415 + Reductions[quiet][depth] * Reductions[quiet][moves] +
                   !improving * 538) /
            1024;
}

void init_search_tables(void) {
        for (int i = 1; i < 256; ++i) {
                Reductions[0][i] = (int)(log(i) * 10.81 + 4.15);
                Reductions[1][i] = (int)(log(i) * 20.76 + 10.69);
        }
        for (int d = 1; d < 16; ++d) {
                Pruning[1][d] = 2.57 + 2.97 * pow(d, 0.79);
                Pruning[0][d] = -1.27 + 2.49 * pow(d, 0.60);
        }
}

void init_searchstack(Searchstack *ss) {
        memset(ss, 0, sizeof(Searchstack) * 256);
        for (int i = 0; i < 256; ++i)
                (ss + i)->plies = i - 4;
}

void update_pv(move_t *pv, move_t best, move_t *sub) {
        pv[0] = best;
        size_t i;
        for (i = 0; sub[i] != NO_MOVE; ++i)
                pv[i + 1] = sub[i];
        pv[i + 1] = NO_MOVE;
}

void timeman_init(const Board *b, Timeman *tm, SearchParams *p, clock_t start) {
        clock_t oh         = UciOptionFields.moveOverhead;
        tm->start          = start;
        tm->pondering      = false;
        tm->checkFrequency = p->nodes ? (int)fmin(1000.0, sqrt(p->nodes) + 0.5) : 1000;
        if (p->wtime || p->btime) {
                tm->mode        = Tournament;
                clock_t rawTime = b->sideToMove == WHITE ? p->wtime : p->btime;
                clock_t inc     = b->sideToMove == WHITE ? p->winc : p->binc;
                clock_t time    = rawTime > oh + 10 ? rawTime - oh - 10 : 1;
                double  mtg     = p->movestogo ? (double)p->movestogo : 50.0;
                tm->averageTime = timemin((clock_t)(time / mtg) + inc, time);
                tm->maximalTime = timemin(rawTime / 4, tm->averageTime * 5);
                if (p->ponder) {
                        tm->pondering = true;
                        tm->averageTime += tm->averageTime / 4;
                }
                tm->optimalTime = tm->averageTime;
        } else if (p->movetime) {
                tm->mode        = Movetime;
                tm->averageTime = tm->maximalTime = tm->optimalTime = (p->movetime <= oh)
                    ? 1
                    : (p->movetime - oh);
        } else {
                tm->mode = NoTimeman;
        }
        tm->prevScore    = NO_SCORE;
        tm->prevBestmove = NO_MOVE;
        tm->stability    = 0;
        tm->type         = NO_BM_TYPE;
}

void timeman_update(Timeman *tm,
    const Board             *b,
    move_t                   best,
    score_t                  score,
    int                      seldepth,
    int                      rootDepth,
    int                      aspFails) {
        if (tm->mode != Tournament)
                return;
        if (tm->prevBestmove != best) {
                Movelist list;
                bool     quiet   = !capture_or_promotion(b, best);
                bool     check   = move_gives_check(b, best);
                tm->prevBestmove = best;
                tm->stability    = 0;
                list_all(&list, b);
                tm->type = movelist_size(&list) == 1                       ? OneLegalMove
                    : move_type(best) == PROMOTION                         ? Promotion
                    : !quiet && see_greater_than(b, best, KNIGHT_MG_SCORE) ? SoundCapture
                    : check && see_greater_than(b, best, 0)                ? SoundCheck
                    : !quiet                                               ? Capture
                    : see_greater_than(b, best, 0)                         ? Quiet
                    : check                                                ? WeirdCheck
                                                                           : WeirdQuiet;
        } else {
                tm->stability = imin(tm->stability + 1, 4);
        }
        double scale = BestmoveTypeScale[tm->type] *
            BestmoveStabilityScale[tm->stability];
        if (tm->prevScore != NO_SCORE)
                scale *= score_difference_scale(tm->prevScore - score);
        scale *= fmin(1.35, 1.0 + fmax(0.0, (double)seldepth / rootDepth - 1.5) * 0.35);
        scale *= 1.0 + 0.15 * imin(aspFails, 2);
        tm->prevScore   = score;
        tm->optimalTime = timemin(tm->maximalTime,
            (clock_t)fmin(tm->averageTime * scale, tm->averageTime * 4.0));
}

void check_time(void) {
        if (--SearchWorkerPool.checks > 0)
                return;
        SearchWorkerPool.checks = SearchTimeman.checkFrequency;
        if (UciSearchParams.infinite || wpool_is_stopped(&SearchWorkerPool))
                return;
        if (wpool_get_total_nodes(&SearchWorkerPool) >= UciSearchParams.nodes ||
            timeman_must_stop_search(&SearchTimeman, chess_clock()))
                wpool_stop(&SearchWorkerPool);
}

INLINED int rtm_greater(RootMove *r, RootMove *l) {
        return r->score != l->score ? r->score > l->score : r->prevScore > l->prevScore;
}

void sort_root_moves(RootMove *begin, RootMove *end) {
        for (int i = 1; i < (int)(end - begin); ++i) {
                RootMove tmp = begin[i];
                int      j   = i - 1;
                while (j >= 0 && rtm_greater(&tmp, begin + j)) {
                        begin[j + 1] = begin[j];
                        --j;
                }
                begin[j + 1] = tmp;
        }
}

RootMove *find_root_move(RootMove *begin, RootMove *end, move_t move) {
        for (; begin < end; ++begin)
                if (begin->move == move)
                        return begin;
        return NULL;
}

void worker_init(Worker *w, size_t idx) {
        w->idx       = idx;
        w->stack     = NULL;
        w->exit      = false;
        w->searching = true;
        if (pthread_mutex_init(&w->mutex, NULL) || pthread_cond_init(&w->condVar, NULL) ||
            pthread_create(&w->thread, &WorkerSettings, &worker_entry, w))
                die("Worker initialization failed");
}

void worker_destroy(Worker *w) {
        w->exit = true;
        worker_start_search(w);
        pthread_join(w->thread, NULL);
        pthread_mutex_destroy(&w->mutex);
        pthread_cond_destroy(&w->condVar);
}

void worker_reset(Worker *w) {
        memset(w->bfHistory, 0, sizeof(butterfly_history_t));
        memset(w->ctHistory, 0, sizeof(continuation_history_t));
        memset(w->cmHistory, 0, sizeof(countermove_history_t));
        memset(w->capHistory, 0, sizeof(capture_history_t));
        w->verifPlies = 0;
}

void worker_start_search(Worker *w) {
        pthread_mutex_lock(&w->mutex);
        w->searching = true;
        pthread_cond_signal(&w->condVar);
        pthread_mutex_unlock(&w->mutex);
}

void worker_wait_search_end(Worker *w) {
        pthread_mutex_lock(&w->mutex);
        while (w->searching)
                pthread_cond_wait(&w->condVar, &w->mutex);
        pthread_mutex_unlock(&w->mutex);
}

void *worker_entry(void *ptr) {
        Worker *w = ptr;
        while (true) {
                pthread_mutex_lock(&w->mutex);
                w->searching = false;
                pthread_cond_signal(&w->condVar);
                while (!w->searching)
                        pthread_cond_wait(&w->condVar, &w->mutex);
                if (w->exit)
                        break;
                pthread_mutex_unlock(&w->mutex);
                w->idx ? worker_search(w) : main_worker_search(w);
        }
        return NULL;
}

void wpool_init(WorkerPool *wp, size_t threads) {
        if (wp->size) {
                worker_wait_search_end(wpool_main_worker(wp));
                while (wp->size) {
                        --wp->size;
                        worker_destroy(wp->workerList[wp->size]);
                        free(wp->workerList[wp->size]);
                }
                free(wp->workerList);
        }
        if (threads) {
                wp->workerList = malloc(sizeof(Worker *) * threads);
                if (!wp->workerList)
                        die("Worker pool allocation failed");
                while (wp->size < threads) {
                        wp->workerList[wp->size] = malloc(sizeof(Worker));
                        if (!wp->workerList[wp->size])
                                die("Worker allocation failed");
                        worker_init(wp->workerList[wp->size], wp->size);
                        wp->size++;
                }
                wpool_reset(wp);
        }
}

void wpool_new_search(WorkerPool *wp) {
        for (size_t i = 0; i < wp->size; ++i)
                wp->workerList[i]->verifPlies = 0;
        wp->checks = 1;
}

void wpool_reset(WorkerPool *wp) {
        for (size_t i = 0; i < wp->size; ++i)
                worker_reset(wp->workerList[i]);
        wp->checks = 1;
}

void wpool_start_search(WorkerPool *wp, const Board *root, const SearchParams *sp) {
        worker_wait_search_end(wpool_main_worker(wp));
        atomic_store_explicit(&wp->stop, false, memory_order_relaxed);
        atomic_store_explicit(&wp->ponder, sp->ponder, memory_order_relaxed);
        for (size_t i = 0; i < wp->size; ++i) {
                Worker *w = wp->workerList[i];
                atomic_store_explicit(&w->nodes, 0, memory_order_relaxed);
                w->board = *root;
                w->stack = w->board.stack = dup_boardstack(root->stack);
                w->board.worker           = w;
                w->rootCount              = movelist_size(&UciSearchMoves);
                w->rootMoves              = malloc(sizeof(RootMove) * w->rootCount);
                if (!w->rootMoves && w->rootCount)
                        die("Root moves allocation failed");
                for (size_t k = 0; k < w->rootCount; ++k) {
                        w->rootMoves[k] = (RootMove){.move = UciSearchMoves.moves[k].move,
                            .seldepth                      = 0,
                            .score                         = -INF_SCORE,
                            .prevScore                     = -INF_SCORE,
                            .pv                            = {NO_MOVE, NO_MOVE}};
                }
        }
        worker_start_search(wpool_main_worker(wp));
}

void wpool_start_workers(WorkerPool *wp) {
        for (size_t i = 1; i < wp->size; ++i)
                worker_start_search(wp->workerList[i]);
}

void wpool_wait_search_end(WorkerPool *wp) {
        for (size_t i = 1; i < wp->size; ++i)
                worker_wait_search_end(wp->workerList[i]);
}

uint64_t wpool_get_total_nodes(WorkerPool *wp) {
        uint64_t total = 0;
        for (size_t i = 0; i < wp->size; ++i)
                total += atomic_load_explicit(&wp->workerList[i]->nodes,
                    memory_order_relaxed);
        return total;
}

void main_worker_search(Worker *w) {
        Board *b = &w->board;
        if (UciSearchParams.perft) {
                clock_t  t     = chess_clock();
                uint64_t nodes = perft(b, (unsigned)UciSearchParams.perft);
                t              = chess_clock() - t;
                uint64_t nps   = nodes / (t + !t) * 1000;
                printf("info nodes %" FMT_INFO " nps %" FMT_INFO " time %" FMT_INFO "\n",
                    (info_t)nodes,
                    (info_t)nps,
                    (info_t)t);
                return;
        }
        if (w->rootCount == 0) {
                printf("info depth 0 score %s 0\n", b->stack->checkers ? "mate" : "cp");
                fflush(stdout);
        } else {
                tt_clear();
                wpool_new_search(&SearchWorkerPool);
                timeman_init(b, &SearchTimeman, &UciSearchParams, chess_clock());
                if (!UciSearchParams.depth)
                        UciSearchParams.depth = MAX_PLIES;
                if (!UciSearchParams.nodes)
                        --UciSearchParams.nodes;
                wpool_start_workers(&SearchWorkerPool);
                worker_search(w);
        }
        while (!wpool_is_stopped(&SearchWorkerPool) &&
            (wpool_is_pondering(&SearchWorkerPool) || UciSearchParams.infinite))
                ;
        wpool_stop(&SearchWorkerPool);
        if (w->rootCount == 0) {
                puts("bestmove 0000");
                fflush(stdout);
                free(w->rootMoves);
                free_boardstack(w->stack);
                return;
        }
        wpool_wait_search_end(&SearchWorkerPool);
        printf("bestmove %s", move_to_str(w->rootMoves->move, b->chess960));
        move_t ponder = w->rootMoves->pv[1];
        if (ponder == NO_MOVE) {
                Boardstack stack;
                TT_Entry  *e;
                bool       found;
                do_move(b, w->rootMoves->move, &stack);
                e = tt_probe(b->stack->boardKey, &found);
                undo_move(b, w->rootMoves->move);
                if (found) {
                        ponder = e->bestmove;
                        if (!move_pseudo_legal(b, ponder) || !move_is_legal(b, ponder))
                                ponder = NO_MOVE;
                }
        }
        if (ponder != NO_MOVE)
                printf(" ponder %s", move_to_str(ponder, b->chess960));
        putchar('\n');
        fflush(stdout);
        free(w->rootMoves);
        free_boardstack(w->stack);
}

void worker_search(Worker *w) {
        Board      *b        = &w->board;
        const int   multiPv  = imin(UciOptionFields.multiPv, w->rootCount);
        const int   maxDepth = imin(UciSearchParams.depth, DEPTH_LIMIT);
        Searchstack ss[256];
        init_searchstack(ss);
        for (int iter = 0; iter < maxDepth; ++iter) {
                bool aborted  = false;
                int  aspFails = 0;
                for (w->pvLine = 0; w->pvLine < multiPv; ++w->pvLine) {
                        w->seldepth  = 0;
                        w->rootDepth = iter + 1;
                        score_t alpha, beta, delta;
                        int     depth   = iter;
                        score_t pvScore = w->rootMoves[w->pvLine].prevScore;
                        if (iter <= 7) {
                                delta = 0;
                                alpha = -INF_SCORE;
                                beta  = INF_SCORE;
                        } else {
                                delta = 8 + abs(pvScore) / 82;
                                alpha = imax(-INF_SCORE, pvScore - delta);
                                beta  = imin(INF_SCORE, pvScore + delta);
                        }
                retry:
                        search(true, b, depth + 1, alpha, beta, &ss[4], false);
                        aborted = wpool_is_stopped(&SearchWorkerPool);
                        sort_root_moves(w->rootMoves + w->pvLine,
                            w->rootMoves + w->rootCount);
                        pvScore   = w->rootMoves[w->pvLine].score;
                        int bound = abs(pvScore) == INF_SCORE ? EXACT_BOUND
                            : pvScore >= beta                 ? LOWER_BOUND
                            : pvScore <= alpha                ? UPPER_BOUND
                                                              : EXACT_BOUND;
                        if (aborted)
                                break;

                        if (bound == UPPER_BOUND) {
                                ++aspFails;
                                depth = iter;
                                beta  = (alpha + beta) / 2;
                                alpha = imax(-INF_SCORE, (int)pvScore - delta);
                                delta += delta * 79 / 256;
                                goto retry;
                        } else if (bound == LOWER_BOUND) {
                                ++aspFails;
                                depth -= (depth > iter / 2);
                                beta = imin(INF_SCORE, (int)pvScore + delta);
                                delta += delta * 79 / 256;
                                goto retry;
                        }

                        sort_root_moves(w->rootMoves, w->rootMoves + multiPv);

                        search_print_root_info(b,
                        w,
                        w->pvLine + 1,
                        iter,
                        chess_clock() - SearchTimeman.start,
                        EXACT_BOUND);
                }
                for (RootMove *i = w->rootMoves; i < w->rootMoves + w->rootCount; ++i) {
                        i->prevScore = i->score;
                        i->score     = -INF_SCORE;
                }
                if (aborted)
                        break;
                if (!w->idx) {
                        timeman_update(&SearchTimeman,
                            b,
                            w->rootMoves->move,
                            w->rootMoves->prevScore,
                            w->seldepth,
                            w->rootDepth,
                            aspFails);
                        clock_t elapsed = chess_clock() - SearchTimeman.start;
                        if (SearchTimeman.mode != NoTimeman &&
                            elapsed * 5 >= SearchTimeman.optimalTime * 3)
                                break;
                }
                if (iter >= 18 && abs(w->rootMoves->prevScore) > MATE_FOUND &&
                    abs(w->rootMoves->prevScore) >= mate_in(MAX_PLIES - 1))
                        break;
                if (UciSearchParams.mate &&
                    abs(w->rootMoves->prevScore) >= mate_in(UciSearchParams.mate * 2))
                        break;
                if (w->idx && iter == maxDepth - 1)
                        --iter;
        }
        if (w->idx) {
                free(w->rootMoves);
                free_boardstack(w->stack);
        }
}