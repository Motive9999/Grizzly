/*

* Grizzly — a UCI chess engine with NNUE evaluation
*
* Copyright (C) 2026 Motive9999
*
* Derived from Stash, a UCI chess engine developed by Morgan Houppin.
* Copyright (C) 2019–2025 Morgan Houppin
*
* This program is free software: you may redistribute it and/or modify
* it under the terms of the GNU General Public License, version 3,
* as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see https://www.gnu.org/licenses/.
  */


#include "defs.h"
#include <math.h>

static const double BestmoveStabilityScale[5] = {2.50, 1.20, 0.90, 0.80, 0.75};

INLINED clock_t timemin(clock_t l, clock_t r) {
        return l < r ? l : r;
}

static double score_difference_scale(score_t s) {
        return pow(2.0, iclamp(s, -100, 100) / 100.0);
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
                tm->averageTime = tm->maximalTime = tm->optimalTime = p->movetime <= oh
                ? 1
                : p->movetime - oh;
        } else {
                tm->mode = NoTimeman;
        }

        tm->prevScore    = NO_SCORE;
        tm->prevBestmove = NO_MOVE;
        tm->stability    = 0;
}

void timeman_update(Timeman *tm,
const Board                 *b,
move_t                       best,
score_t                      score,
int                          seldepth,
int                          rootDepth,
int                          aspFails) {
        (void)b;

        if (tm->mode != Tournament)
                return;

        if (tm->prevBestmove != best) {
                tm->prevBestmove = best;
                tm->stability    = 0;
        } else {
                tm->stability = imin(tm->stability + 1, 4);
        }

        double scale = BestmoveStabilityScale[tm->stability];

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