#include "defs.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// clang-format off

static const CommandMap UciCommands[] = {
    {"d", &uci_d}, {"debug", &uci_debug}, {"go", &uci_go}, {"isready", &uci_isready},
    {"ponderhit", &uci_ponderhit}, {"position", &uci_position}, {"quit", &uci_quit},
    {"setoption", &uci_setoption}, {"stop", &uci_stop}, {"uci", &uci_uci},
    {"ucinewgame", &uci_ucinewgame}, {NULL, NULL}};

__attribute__((noreturn)) static void uci_oom(const char *what) {
        write(STDERR_FILENO, "Unable to allocate ", 19);
        write(STDERR_FILENO, what, strlen(what));
        write(STDERR_FILENO, ": out of memory\n", 16);
        exit(ENOMEM);
}

static void *xmalloc(size_t n, const char *what) {
        void *p = malloc(n);
        if (!p) uci_oom(what);
        return p;
}

static char *xstrdup(const char *s, const char *what) {
        char *p = strdup(s);
        if (!p) uci_oom(what);
        return p;
}

char *get_next_token(char **str) {
        while (isspace((unsigned char)**str) && **str != '\0') ++(*str);
        if (**str == '\0') return NULL;
        char *retval = *str;
        while (!isspace((unsigned char)**str) && **str != '\0') ++(*str);
        if (**str == '\0') return retval;
        **str = '\0';
        ++(*str);
        return retval;
}

const char *move_to_str(move_t move, bool isChess960) {
        static char buf[6];
        if (move == NO_MOVE) return "none";
        if (move == NULL_MOVE) return "0000";
        square_t from = from_sq(move), to = to_sq(move);
        if (move_type(move) == CASTLING && !isChess960)
                to = create_sq(to > from ? FILE_G : FILE_C, sq_rank(from));
        buf[0] = sq_file(from) + 'a';
        buf[1] = sq_rank(from) + '1';
        buf[2] = sq_file(to) + 'a';
        buf[3] = sq_rank(to) + '1';
        if (move_type(move) == PROMOTION) {
                buf[4] = " pnbrqk"[promotion_type(move)];
                buf[5] = '\0';
        } else
                buf[4] = '\0';
        return buf;
}

move_t str_to_move(const Board *board, const char *str) {
        Movelist movelist;
        list_all(&movelist, board);
        for (const ExtendedMove *m = movelist_begin(&movelist); m < movelist_end(&movelist); ++m)
                if (!strcmp(str, move_to_str(m->move, board->chess960))) return m->move;
        return NO_MOVE;
}

int winrate_model(score_t score, int ply) {
        static const double as[4] = {-4.19608390, 27.38380265, 32.62081910, 98.27443919};
        static const double bs[4] = {0.36809906, -1.71950139, 9.71771959, 61.99845003};
        double p = imin(ply, 240) / 64.0;
        double a = ((as[0] * p + as[1]) * p + as[2]) * p + as[3];
        double b = ((bs[0] * p + bs[1]) * p + bs[2]) * p + bs[3];
        double v = fmin(fmax(-4000.0, (double)score), 4000.0);
        return (int)(0.5 + 1000.0 / (1.0 + exp((a - v) / b)));
}

const char *score_to_wdl(score_t score, int ply) {
        static char buf[17];
        if (!UciOptionFields.showWDL) { buf[0] = '\0'; return buf; }
        int w = winrate_model(score, ply), l = winrate_model(-score, ply);
        sprintf(buf, " wdl %d %d %d", w, 1000 - w - l, l);
        return buf;
}

const char *score_to_str(score_t score) {
        static const score_t NormalizeScore = 154;
        static char buf[12];
        if (abs(score) >= MATE_FOUND)
                sprintf(buf, "mate %d", (score > 0 ? MATE - score + 1 : -MATE - score) / 2);
        else {
                if (UciOptionFields.normalizeScore) score = (int32_t)score * 100 / NormalizeScore;
                sprintf(buf, "cp %d", score);
        }
        return buf;
}

void search_print_root_info(
    Board *board, Worker *worker, int multiPv, int depth, clock_t time, int bound) {
        RootMove *rootMove = &worker->rootMoves[worker->pvLine];
        static const char *BoundStr[] = {"", " upperbound", " lowerbound", ""};
        uint64_t nodes        = wpool_get_total_nodes(&SearchWorkerPool);
        uint64_t nps          = nodes / (time + !time) * 1000;
        bool     searchedMove = (rootMove->score != -INF_SCORE);
        score_t  rootScore    = searchedMove ? rootMove->score : rootMove->prevScore;
        const char *pv = rootMove->pv[0]
            ? move_to_str(rootMove->pv[0], board->chess960)
            : "";
        printf("info depth %d seldepth %d multipv %d score %s%s%s nodes %" FMT_INFO
               " nps %" FMT_INFO " hashfull %d time %" FMT_INFO " pv%s%s\n",
               imax(depth + searchedMove, 1), rootMove->seldepth, multiPv,
               score_to_str(rootScore), BoundStr[bound], score_to_wdl(rootScore, board->ply),
               (info_t)nodes,
               (info_t)nps,
               tt_hashfull(),
               (info_t)time,
               *pv ? " " : "",
               pv);
        fflush(stdout);
}

int debug_printf(const char *fmt, ...) {
        if (!UciOptionFields.debug) return 0;
        va_list ap;
        va_start(ap, fmt);
        int result = vprintf(fmt, ap);
        fflush(stdout);
        va_end(ap);
        return result;
}

/* ---------- options table ---------- */

enum OptKind { OPT_SPIN, OPT_CHECK, OPT_BUTTON };

typedef struct {
        const char *name;
        int         kind;
        long        defv, minv, maxv;       /* spin only */
        void       *field;                  /* points into UciOptionFields */
        void      (*hook)(long);            /* optional side effect on set */
} UciOpt;

static void hook_threads(long v) { wpool_init(&SearchWorkerPool, (unsigned long)v); }
static void hook_hash(long v)    { tt_resize((size_t)v); }
static void hook_clearhash(long v) { (void)v; tt_bzero((size_t)UciOptionFields.threads); fflush(stdout); }

static const UciOpt OptTable[] = {
    {"Threads",        OPT_SPIN,   1, 1, 256,      &UciOptionFields.threads,        hook_threads},
    {"Hash",           OPT_SPIN,  16, 1, MAX_HASH, &UciOptionFields.hash,           hook_hash},
    {"Move Overhead",  OPT_SPIN, 100, 0, 30000,    &UciOptionFields.moveOverhead,   NULL},
    {"MultiPV",        OPT_SPIN,   1, 1, 500,      &UciOptionFields.multiPv,        NULL},
    {"UCI_Chess960",   OPT_CHECK,  0, 0, 0,        &UciOptionFields.chess960,       NULL},
    {"UCI_ShowWDL",    OPT_CHECK,  0, 0, 0,        &UciOptionFields.showWDL,        NULL},
    {"NormalizeScore", OPT_CHECK,  0, 0, 0,        &UciOptionFields.normalizeScore, NULL},
    {"Ponder",         OPT_CHECK,  0, 0, 0,        &UciOptionFields.ponder,         NULL},
    {"Clear Hash",     OPT_BUTTON, 0, 0, 0,        NULL,                            hook_clearhash},
    {NULL, 0, 0, 0, 0, NULL, NULL}};

void show_options(void) {
        for (const UciOpt *o = OptTable; o->name; ++o) {
                if (o->kind == OPT_SPIN)
                        printf("option name %s type spin default %ld min %ld max %ld\n",
                               o->name, o->defv, o->minv, o->maxv);
                else if (o->kind == OPT_CHECK)
                        printf("option name %s type check default false\n", o->name);
                else
                        printf("option name %s type button\n", o->name);
        }
        fflush(stdout);
}

void set_option(const char *name, const char *value) {
        for (const UciOpt *o = OptTable; o->name; ++o) {
                if (strcasecmp(name, o->name)) continue;
                if (o->kind == OPT_SPIN) {
                        char *end;
                        long v = strtol(value, &end, 10);
                        if (end == value || v < o->minv || v > o->maxv) return;
                        *(long *)o->field = v;
                        if (o->hook) o->hook(v);
                } else if (o->kind == OPT_CHECK) {
                        bool bv;
                        if      (!strcasecmp(value, "true"))  bv = true;
                        else if (!strcasecmp(value, "false")) bv = false;
                        else return;
                        *(bool *)o->field = bv;
                        if (o->hook) o->hook(bv);
                } else {
                        if (o->hook) o->hook(0);
                }
                return;
        }
        debug_printf("info error Unknown option '%s'\n", name);
}

/* ---------- UCI commands ---------- */

void uci_isready(const char *args __attribute__((unused)))   { puts("readyok"); fflush(stdout); }
void uci_quit(const char *args __attribute__((unused)))      { wpool_stop(&SearchWorkerPool); }
void uci_stop(const char *args __attribute__((unused)))      { wpool_stop(&SearchWorkerPool); }
void uci_ponderhit(const char *args __attribute__((unused))) { wpool_ponderhit(&SearchWorkerPool); }

void uci_uci(const char *args __attribute__((unused))) {
        puts("id name Grizzly 5.0");
        puts("id author Motive9999");
        show_options();
        puts("uciok");
        fflush(stdout);
}

void uci_ucinewgame(const char *args __attribute__((unused))) {
        worker_wait_search_end(wpool_main_worker(&SearchWorkerPool));
        tt_bzero((size_t)UciOptionFields.threads);
        wpool_reset(&SearchWorkerPool);
}

void uci_debug(const char *args) {
        char *copy = xstrdup(args ? args : "", "command copy");
        char *token = strtok(copy, Delimiters);
        UciOptionFields.debug = token && (strcmp(token, "on") == 0);
        free(copy);
}

void uci_d(const char *args __attribute__((unused))) {
        const char *grid = "+---+---+---+---+---+---+---+---+";
        extern const char PieceIndexes[PIECE_NB];
        puts(grid);
        for (file_t rank = RANK_8; rank >= RANK_1; --rank) {
                for (file_t file = FILE_A; file <= FILE_H; ++file)
                        printf("| %c ", PieceIndexes[piece_on(&UciBoard, create_sq(file, rank))]);
                puts("|");
                puts(grid);
        }
        printf("\nFEN: %s\nKey: 0x%" KEY_INFO "\n",
               board_fen(&UciBoard), (info_t)UciBoard.stack->boardKey);
        printf("Eval (from %s's POV): %+.2lf\n\n",
               UciBoard.sideToMove == WHITE ? "White" : "Black",
               (double)evaluate(&UciBoard) / 100.0);
        fflush(stdout);
}

void uci_setoption(const char *args) {
        if (!args) return;
        char *copy = xstrdup(args, "command copy");
        char *nameToken  = strstr(copy, "name");
        char *valueToken = strstr(copy, "value");
        if (!nameToken) { free(copy); return; }
        char nameBuf[1024]  = {0};
        char valueBuf[1024] = {0};
        if (valueToken) {
                *(valueToken - 1) = '\0';
                strcpy(valueBuf, valueToken + 6);
                char *nl = &valueBuf[strlen(valueBuf) - 1];
                if (*nl == '\n') *nl = '\0';
        }
        char *token = strtok(nameToken + 4, Delimiters);
        while (token) {
                strcat(nameBuf, token);
                token = strtok(NULL, Delimiters);
                if (token) strcat(nameBuf, " ");
        }
        set_option(nameBuf, valueBuf);
        free(copy);
}

void uci_position(const char *args) {
        static const char *StartPosFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        char *copy = xstrdup(args, "command copy");
        char *ptr = copy, *token = get_next_token(&ptr);
        char  fen[256] = {0};
        if (!strcmp(token, "startpos")) {
                strcpy(fen, StartPosFEN);
                token = get_next_token(&ptr);
        } else if (!strcmp(token, "fen")) {
                token = get_next_token(&ptr);
                while (token && strcmp(token, "moves")) {
                        if (fen[0]) strcat(fen, " ");
                        strcat(fen, token);
                        token = get_next_token(&ptr);
                }
        } else { free(copy); return; }

        free_boardstack(UciBoard.stack);
        Boardstack *firstStack = xmalloc(sizeof(Boardstack), "board stack");
        int result = board_from_fen(&UciBoard, fen, UciOptionFields.chess960, firstStack);
        if (result < 0)
                board_from_fen(&UciBoard, StartPosFEN, UciOptionFields.chess960, firstStack);
        UciBoard.worker = wpool_main_worker(&SearchWorkerPool);

        if (result >= 0) {
                token = get_next_token(&ptr);
                size_t i = 1;
                move_t move;
                while (token && (move = str_to_move(&UciBoard, token)) != NO_MOVE) {
                        Boardstack *nextStack = xmalloc(sizeof(Boardstack), "board stack");
                        do_move(&UciBoard, move, nextStack);
                        token = get_next_token(&ptr);
                        ++i;
                }
                if (token)
                        debug_printf("info string Failed to parse move token #%lu ('%s')\n",
                                     (unsigned long)i, token);
                debug_printf("info string Final board state: %s\n", board_fen(&UciBoard));
        }
        free(copy);
}

/* ---------- go params table ---------- */

enum GoKind { GO_CLOCK, GO_INT, GO_U64, GO_FLAG };

typedef struct {
        const char *name;
        int         kind;
        size_t      offset;
} GoParam;

#define GO_OFF(f) offsetof(SearchParams, f)

static const GoParam GoParams[] = {
    {"wtime",     GO_CLOCK, GO_OFF(wtime)},
    {"btime",     GO_CLOCK, GO_OFF(btime)},
    {"winc",      GO_CLOCK, GO_OFF(winc)},
    {"binc",      GO_CLOCK, GO_OFF(binc)},
    {"movetime",  GO_CLOCK, GO_OFF(movetime)},
    {"movestogo", GO_INT,   GO_OFF(movestogo)},
    {"depth",     GO_INT,   GO_OFF(depth)},
    {"mate",      GO_INT,   GO_OFF(mate)},
    {"perft",     GO_INT,   GO_OFF(perft)},
    {"nodes",     GO_U64,   GO_OFF(nodes)},
    {"infinite",  GO_FLAG,  GO_OFF(infinite)},
    {"ponder",    GO_FLAG,  GO_OFF(ponder)},
    {NULL, 0, 0}};

void uci_go(const char *args) {
        worker_wait_search_end(wpool_main_worker(&SearchWorkerPool));
        memset(&UciSearchParams, 0, sizeof(SearchParams));
        list_all(&UciSearchMoves, &UciBoard);

        char *copy  = xstrdup(args ? args : "", "command copy");
        char *token = strtok(copy, Delimiters);
        char *base  = (char *)&UciSearchParams;

        while (token) {
                if (!strcmp(token, "searchmoves")) {
                        token = strtok(NULL, Delimiters);
                        ExtendedMove *m = UciSearchMoves.moves;
                        while (token) {
                                (m++)->move = str_to_move(&UciBoard, token);
                                token = strtok(NULL, Delimiters);
                        }
                        UciSearchMoves.last = m;
                        break;
                }
                const GoParam *p;
                for (p = GoParams; p->name; ++p)
                        if (!strcmp(token, p->name)) break;
                if (!p->name) { token = strtok(NULL, Delimiters); continue; }

                if (p->kind == GO_FLAG) {
                        *(int *)(base + p->offset) = 1;
                } else {
                        token = strtok(NULL, Delimiters);
                        if (!token) break;
                        if      (p->kind == GO_CLOCK) *(clock_t *) (base + p->offset) = (clock_t) atoll(token);
                        else if (p->kind == GO_U64)   *(uint64_t *)(base + p->offset) = (uint64_t)atoll(token);
                        else                          *(int *)     (base + p->offset) = atoi(token);
                }
                token = strtok(NULL, Delimiters);
        }

        wpool_start_search(&SearchWorkerPool, &UciBoard, &UciSearchParams);
        free(copy);
}

int execute_uci_cmd(const char *command) {
        char *dup = xstrdup(command, "command string");
        char *cmd = strtok(dup, Delimiters);
        if (!cmd) { free(dup); return 1; }
        for (size_t i = 0; UciCommands[i].commandName; ++i)
                if (!strcmp(UciCommands[i].commandName, cmd)) {
                        UciCommands[i].call(strtok(NULL, ""));
                        break;
                }
        int keepLooping = !!strcmp(cmd, "quit");
        free(dup);
        return keepLooping;
}

void uci_loop(int argc, char **argv) {
        uci_position("startpos");
        if (argc > 1) {
                for (int i = 1; i < argc; ++i) execute_uci_cmd(argv[i]);
        } else {
                char *line = xmalloc(16384, "input buffer");
                while (fgets(line, 16384, stdin))
                        if (execute_uci_cmd(line) == 0) break;
                free(line);
        }
        uci_quit(NULL);
}