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
#include <stdio.h>
#include <stdlib.h>

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