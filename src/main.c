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
#include <pthread.h>
#include <stdio.h>

Board          UciBoard = {.stack = NULL};
pthread_attr_t WorkerSettings;
SearchParams   UciSearchParams;
Movelist       UciSearchMoves;

uint64_t Seed = 1048592ul;

OptionFields UciOptionFields = {1, 16, 100, 1, false, false, false, false, true};

Timeman SearchTimeman;

const char *Delimiters = " \r\t\n";

int main(int argc, char **argv) {
        // Initialize various parts of the engine.
        bitboard_init();
        psq_score_init();
        zobrist_init();
        cyclic_init();

#ifdef TUNE



#else

        // Initialize the search-related data along with the worker pool.
        tt_resize(16);
        init_search_tables();
        pthread_attr_init(&WorkerSettings);
        pthread_attr_setstacksize(&WorkerSettings, 4ul * 1024 * 1024);
        wpool_init(&SearchWorkerPool, 1);

        // Wait for the engine thread to be ready, and then start parsing UCI
        // commands.
        worker_wait_search_end(wpool_main_worker(&SearchWorkerPool));
        uci_loop(argc, argv);

        // Destroy all allocated memory.
        wpool_init(&SearchWorkerPool, 0);
        tt_resize(0);
        free_boardstack(UciBoard.stack);

#endif

        return 0;
}
