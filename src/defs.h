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


#ifndef DEFS_H
#define DEFS_H

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/timeb.h>
#include <time.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L &&                          \
!defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#else
typedef volatile bool atomic_bool;
typedef enum {
        memory_order_relaxed,
        memory_order_consume,
        memory_order_acquire,
        memory_order_release,
        memory_order_acq_rel,
        memory_order_seq_cst
} memory_order;
#define _Atomic(T)             volatile T
#define ATOMIC_VAR_INIT(value) (value)
static inline bool atomic_load_explicit(volatile bool *ptr, memory_order order) {
        (void)order;
        return *ptr;
}
static inline void
atomic_store_explicit(volatile bool *ptr, bool val, memory_order order) {
        (void)order;
        *ptr = val;
}
#endif

#if (defined(USE_PREFETCH) || defined(USE_POPCNT) || defined(USE_PEXT))
#include <immintrin.h>
#endif

#if (SIZE_MAX == UINT64_MAX)
#define FMT_INFO PRIu64
#define KEY_INFO PRIx64
typedef uint64_t info_t;
#define MAX_HASH 33554432
#else
#define FMT_INFO PRIu32
#define KEY_INFO PRIx32
typedef uint32_t info_t;
#define MAX_HASH 2048
#endif

#define INLINED static inline

int                debug_printf(const char *fmt, ...);
extern const char *Delimiters;

typedef int8_t   Color;
typedef int8_t   Piece;
typedef int8_t   PieceType;
typedef int16_t  Square;
typedef int16_t  Direction;
typedef int8_t   File;
typedef int8_t   Rank;
typedef int32_t  Move;
typedef int32_t  MoveType;
typedef int16_t  Score;
typedef int32_t  ScorePair;
typedef uint64_t Bitboard;
typedef uint64_t HashKey;

// Lowercase aliases for consistency with search.c
typedef Score     score_t;
typedef Move      move_t;
typedef Piece     piece_t;
typedef PieceType piecetype_t;
typedef Square    square_t;
typedef HashKey   hashkey_t;
typedef File      file_t;

enum { WHITE, BLACK, COLOR_NB = 2 };
enum {
        NO_PIECE,
        WHITE_PAWN,
        WHITE_KNIGHT,
        WHITE_BISHOP,
        WHITE_ROOK,
        WHITE_QUEEN,
        WHITE_KING,
        BLACK_PAWN = 9,
        BLACK_KNIGHT,
        BLACK_BISHOP,
        BLACK_ROOK,
        BLACK_QUEEN,
        BLACK_KING,
        PIECE_NB = 16
};
enum {
        NO_PIECETYPE,
        PAWN,
        KNIGHT,
        BISHOP,
        ROOK,
        QUEEN,
        KING,
        ALL_PIECES   = 0,
        PIECETYPE_NB = 8
};
enum {
        SQ_A1,
        SQ_C1 = 2,
        SQ_D1,
        SQ_F1 = 5,
        SQ_G1,
        SQ_H1,
        SQ_A8     = 56,
        SQ_H8     = 63,
        SQ_NONE   = 64,
        SQUARE_NB = 64
};
enum { NORTH = 8, SOUTH = -8, EAST = 1, WEST = -1 };
enum { FILE_A, FILE_C = 2, FILE_E = 4, FILE_G = 6, FILE_H, FILE_NB };
enum { RANK_1, RANK_2, RANK_8 = 7 };
enum {
        WHITE_OO           = 1,
        WHITE_OOO          = 2,
        WHITE_CASTLING     = 3,
        BLACK_OO           = 4,
        KINGSIDE_CASTLING  = 5,
        BLACK_OOO          = 8,
        QUEENSIDE_CASTLING = 10,
        BLACK_CASTLING     = 12,
        ANY_CASTLING       = 15,
        CASTLING_NB        = 16
};
enum {
        NO_MOVE       = 0,
        NULL_MOVE     = 65,
        NORMAL_MOVE   = 0,
        PROMOTION     = 1 << 14,
        EN_PASSANT    = 2 << 14,
        CASTLING      = 3 << 14,
        MOVETYPE_MASK = 3 << 14
};
enum {
        MAX_PLIES  = 238,
        VICTORY    = 10000,
        MATE       = 32000,
        MATE_FOUND = MATE - MAX_PLIES,
        INF_SCORE  = 32001,
        NO_SCORE   = 32002,
        NO_BOUND   = 0,
        UPPER_BOUND,
        LOWER_BOUND,
        EXACT_BOUND
};
enum { MIDGAME, ENDGAME, PHASE_NB };
enum {
        PAWN_MG_SCORE   = 106,
        KNIGHT_MG_SCORE = 353,
        BISHOP_MG_SCORE = 373,
        ROOK_MG_SCORE   = 509,
        QUEEN_MG_SCORE  = 1065
};

INLINED int imax(int a, int b) {
        return a > b ? a : b;
}
INLINED int imin(int a, int b) {
        return a < b ? a : b;
}
INLINED int iclamp(int v, int lo, int hi) {
        return v < lo ? lo : v > hi ? hi : v;
}
INLINED Color not_color(Color c) {
        return c ^ BLACK;
}
INLINED PieceType piece_type(Piece p) {
        return p & 7;
}
INLINED Color piece_color(Piece p) {
        return p >> 3;
}
INLINED Piece create_piece(Color c, PieceType pt) {
        return pt + (c << 3);
}
INLINED File sq_file(Square sq) {
        return sq & 7;
}
INLINED Rank sq_rank(Square sq) {
        return sq >> 3;
}
INLINED Square create_sq(File f, Rank r) {
        return f + (r << 3);
}
INLINED Square relative_sq(Square sq, Color c) {
        return sq ^ (SQ_A8 * c);
}
INLINED Rank relative_rank(Rank r, Color c) {
        return r ^ (RANK_8 * c);
}
INLINED Rank relative_sq_rank(Square sq, Color c) {
        return relative_rank(sq_rank(sq), c);
}
INLINED bool is_valid_sq(Square sq) {
        return sq >= SQ_A1 && sq <= SQ_H8;
}
INLINED Direction pawn_direction(Color c) {
        return c == WHITE ? NORTH : SOUTH;
}
INLINED Square from_sq(Move m) {
        return (Square)((m >> 6) & SQ_H8);
}
INLINED Square to_sq(Move m) {
        return (Square)(m & SQ_H8);
}
INLINED int square_mask(Move m) {
        return m & 0xFFF;
}
INLINED MoveType move_type(Move m) {
        return m & MOVETYPE_MASK;
}
INLINED PieceType promotion_type(Move m) {
        return (PieceType)(((m >> 12) & 3) + KNIGHT);
}
INLINED Move create_move(Square from, Square to) {
        return (Move)((from << 6) + to);
}
INLINED Move reverse_move(Move m) {
        return create_move(to_sq(m), from_sq(m));
}
INLINED Move create_promotion(Square from, Square to, PieceType pt) {
        return (Move)(PROMOTION + ((pt - KNIGHT) << 12) + (from << 6) + to);
}
INLINED Move create_en_passant(Square from, Square to) {
        return (Move)(EN_PASSANT + (from << 6) + to);
}
INLINED Move create_castling(Square from, Square to) {
        return (Move)(CASTLING + (from << 6) + to);
}
INLINED Score mate_in(int ply) {
        return MATE - ply;
}
INLINED Score mated_in(int ply) {
        return ply - MATE;
}

extern int         SquareDistance[SQUARE_NB][SQUARE_NB];
extern const Score PieceScores[PHASE_NB][PIECE_NB];
extern ScorePair   PsqScore[PIECE_NB][SQUARE_NB];

static const Bitboard FILE_A_BB = 0x0101010101010101ul, FILE_H_BB = 0x8080808080808080ul;
static const Bitboard RANK_1_BB = 0x00000000000000FFul, RANK_2_BB = 0x000000000000FF00ul,
                      RANK_3_BB = 0x0000000000FF0000ul, RANK_6_BB = 0x0000FF0000000000ul,
                      RANK_7_BB = 0x00FF000000000000ul, RANK_8_BB = 0xFF00000000000000ul;
static const Bitboard ALL_BB = 0xFFFFFFFFFFFFFFFFul, DSQ_BB = 0xAA55AA55AA55AA55ul;

extern Bitboard LineBB[SQUARE_NB][SQUARE_NB];
extern Bitboard PseudoMoves[PIECETYPE_NB][SQUARE_NB];
extern Bitboard PawnMoves[COLOR_NB][SQUARE_NB];

typedef struct {
        Bitboard     mask, magic, *moves;
        unsigned int shift;
} Magic;

INLINED unsigned int magic_index(const Magic *m, Bitboard occ) {
#ifdef USE_PEXT
        return _pext_u64(occ, m->mask);
#else
        return (unsigned int)(((occ & m->mask) * m->magic) >> m->shift);
#endif
}

extern Magic RookMagics[SQUARE_NB], BishopMagics[SQUARE_NB];

void bitboard_init(void);

INLINED Bitboard square_bb(Square sq) {
        return (Bitboard)1 << sq;
}
INLINED Bitboard shift_up(Bitboard b) {
        return b << 8;
}
INLINED Bitboard shift_down(Bitboard b) {
        return b >> 8;
}
INLINED Bitboard shift_left(Bitboard b) {
        return (b & ~FILE_A_BB) >> 1;
}
INLINED Bitboard shift_right(Bitboard b) {
        return (b & ~FILE_H_BB) << 1;
}
INLINED Bitboard shift_up_left(Bitboard b) {
        return (b & ~FILE_A_BB) << 7;
}
INLINED Bitboard shift_up_right(Bitboard b) {
        return (b & ~FILE_H_BB) << 9;
}
INLINED Bitboard shift_down_left(Bitboard b) {
        return (b & ~FILE_A_BB) >> 9;
}
INLINED Bitboard shift_down_right(Bitboard b) {
        return (b & ~FILE_H_BB) >> 7;
}
INLINED Bitboard relative_shift_up(Bitboard b, Color c) {
        return c == WHITE ? shift_up(b) : shift_down(b);
}
INLINED bool more_than_one(Bitboard b) {
        return b & (b - 1);
}
INLINED Bitboard file_bb(File f) {
        return FILE_A_BB << f;
}
INLINED Bitboard sq_file_bb(Square sq) {
        return file_bb(sq_file(sq));
}
INLINED Bitboard rank_bb(Rank r) {
        return RANK_1_BB << (8 * r);
}
INLINED Bitboard sq_rank_bb(Square sq) {
        return rank_bb(sq_rank(sq));
}
INLINED Bitboard between_bb(Square sq1, Square sq2) {
        return LineBB[sq1][sq2] &
        ((ALL_BB << (sq1 + (sq1 < sq2))) ^ (ALL_BB << (sq2 + !(sq1 < sq2))));
}
INLINED bool sq_aligned(Square sq1, Square sq2, Square sq3) {
        return LineBB[sq1][sq2] & square_bb(sq3);
}
INLINED Bitboard bishop_moves_bb(Square sq, Bitboard occ) {
        const Magic *m = &BishopMagics[sq];
        return m->moves[magic_index(m, occ)];
}
INLINED Bitboard rook_moves_bb(Square sq, Bitboard occ) {
        const Magic *m = &RookMagics[sq];
        return m->moves[magic_index(m, occ)];
}
INLINED int popcount(Bitboard b) {
#ifndef USE_POPCNT
        const Bitboard m1 = 0x5555555555555555ull, m2 = 0x3333333333333333ull,
                       m4 = 0x0F0F0F0F0F0F0F0Full, hx = 0x0101010101010101ull;
        b -= (b >> 1) & m1;
        b = (b & m2) + ((b >> 2) & m2);
        b = (b + (b >> 4)) & m4;
        return (b * hx) >> 56;
#elif defined(_MSC_VER) || defined(__INTEL_COMPILER)
        return (int)_mm_popcnt_u64(b);
#else
        return __builtin_popcountll(b);
#endif
}

#if defined(__GNUC__)
INLINED Square bb_first_sq(Bitboard b) {
        return __builtin_ctzll(b);
}
#elif defined(_MSC_VER)
INLINED Square bb_first_sq(Bitboard b) {
        unsigned long idx;
        _BitScanForward64(&idx, b);
        return (Square)idx;
}
#else
#error "Unsupported compiler."
#endif

INLINED Square bb_pop_first_sq(Bitboard *b) {
        const Square sq = bb_first_sq(*b);
        *b &= *b - 1;
        return sq;
}

INLINED void prefetch(void *ptr __attribute__((unused))) {
#ifdef USE_PREFETCH
        _mm_prefetch(ptr, _MM_HINT_T0);
#elif defined(__GNUC__)
        __builtin_prefetch(ptr);
#endif
}

INLINED uint64_t mul_hi64(uint64_t x, uint64_t n) {
        uint64_t xlo = (uint32_t)x, xhi = x >> 32, nlo = (uint32_t)n, nhi = n >> 32;
        uint64_t c1 = (xlo * nlo) >> 32, c2 = (xhi * nlo) + c1,
                 c3 = (xlo * nhi) + (uint32_t)c2;
        return xhi * nhi + (c2 >> 32) + (c3 >> 32);
}

extern HashKey ZobristPsq[PIECE_NB][SQUARE_NB];
extern HashKey ZobristEnPassant[FILE_NB];
extern HashKey ZobristCastling[CASTLING_NB];
extern HashKey ZobristSideToMove;

void zobrist_init(void);
void psq_score_init(void);

INLINED uint64_t qrandom(uint64_t *seed) {
        uint64_t x = *seed;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        *seed = x;
        return x * UINT64_C(0x2545F4914F6CDD1D);
}

enum {
        HistoryMaxScore   = 8192,
        HistoryScale      = 2,
        HistoryResolution = HistoryMaxScore * HistoryScale
};

typedef int16_t         butterfly_history_t[COLOR_NB][SQUARE_NB * SQUARE_NB];
typedef int16_t         piece_history_t[PIECE_NB][SQUARE_NB];
typedef int16_t         capture_history_t[PIECE_NB][SQUARE_NB][PIECETYPE_NB];
typedef piece_history_t continuation_history_t[PIECE_NB][SQUARE_NB];
typedef Move            countermove_history_t[PIECE_NB][SQUARE_NB];

INLINED int history_bonus(int d) {
        return d <= 11 ? 24 * d * d + d : 2563;
}
INLINED void add_bf_history(butterfly_history_t hist, Piece pc, Move m, int32_t bonus) {
        int16_t *e = &hist[piece_color(pc)][square_mask(m)];
        *e += bonus - (int32_t)*e * abs(bonus) / HistoryResolution;
}
INLINED Score get_bf_history_score(const butterfly_history_t hist, Piece pc, Move m) {
        return hist[piece_color(pc)][square_mask(m)] / HistoryScale;
}
INLINED void add_pc_history(piece_history_t hist, Piece pc, Square to, int32_t bonus) {
        int16_t *e = &hist[pc][to];
        *e += bonus - (int32_t)*e * abs(bonus) / HistoryResolution;
}
INLINED Score get_pc_history_score(const piece_history_t hist, Piece pc, Square to) {
        return hist[pc][to] / HistoryScale;
}
INLINED void
add_cap_history(capture_history_t hist, Piece pc, Square to, Piece cap, int32_t bonus) {
        int16_t *e = &hist[pc][to][piece_type(cap)];
        *e += bonus - (int32_t)*e * abs(bonus) / HistoryResolution;
}
INLINED Score get_cap_history_score(const capture_history_t hist,
Piece                                                       pc,
Square                                                      to,
Piece                                                       cap) {
        return hist[pc][to][piece_type(cap)] / HistoryScale;
}

typedef struct Boardstack {
        int      castlings, rule50, pliesFromNullMove;
        Square   enPassantSquare;
        HashKey  pawnKey, materialKey, boardKey;
        Score    material[COLOR_NB];
        Bitboard checkers, kingBlockers[COLOR_NB], pinners[COLOR_NB],
        checkSquares[PIECETYPE_NB];
        Piece              capturedPiece;
        struct Boardstack *prev;
        int                repetition;
} Boardstack;

typedef struct {
        Piece       table[SQUARE_NB];
        Bitboard    piecetypeBB[PIECETYPE_NB], colorBB[COLOR_NB];
        int         pieceCount[PIECE_NB], castlingMask[SQUARE_NB], ply;
        Square      castlingRookSquare[CASTLING_NB];
        Bitboard    castlingPath[CASTLING_NB];
        Color       sideToMove;
        ScorePair   psqScorePair;
        Boardstack *stack;
        void       *worker;
        bool        chess960;
} Board;

extern Board UciBoard;

void        cyclic_init(void);
Bitboard    attackers_list(const Board *b, Square s, Bitboard occ);
void        do_castling(Board *restrict b,
Color  us,
Square kf,
Square *restrict kt,
Square *restrict rf,
Square *restrict rt);
void        do_move_gc(Board *restrict b, Move m, Boardstack *restrict s, bool gc);
void        do_null_move(Board *restrict b, Boardstack *restrict s);
const char *board_fen(const Board *b);
bool        game_is_drawn(const Board *b, int ply);
bool        game_has_cycle(const Board *b, int ply);
bool        move_is_legal(const Board *b, Move m);
bool        move_pseudo_legal(const Board *b, Move m);
bool        move_gives_check(const Board *b, Move m);
bool        see_greater_than(const Board *b, Move m, Score threshold);
int         board_from_fen(Board *b, const char *fen, bool is960, Boardstack *bs);
void        set_boardstack(Board *b, Boardstack *s);
void        set_check(Board *restrict b, Boardstack *restrict s);
Bitboard    slider_blockers(const Board *restrict b,
Bitboard sliders,
Square   sq,
Bitboard *restrict pinners);
void        undo_castling(Board *restrict b,
Color  us,
Square kf,
Square *restrict kt,
Square *restrict rf,
Square *restrict rt);
void        undo_move(Board *b, Move m);
void        undo_null_move(Board *b);
Boardstack *dup_boardstack(const Boardstack *s);
void        free_boardstack(Boardstack *s);

INLINED Piece piece_on(const Board *b, Square sq) {
        return b->table[sq];
}
INLINED bool empty_square(const Board *b, Square sq) {
        return piece_on(b, sq) == NO_PIECE;
}
INLINED Bitboard piecetype_bb(const Board *b, PieceType pt) {
        return b->piecetypeBB[pt];
}
INLINED Bitboard piecetypes_bb(const Board *b, PieceType pt1, PieceType pt2) {
        return piecetype_bb(b, pt1) | piecetype_bb(b, pt2);
}
INLINED Bitboard color_bb(const Board *b, Color c) {
        return b->colorBB[c];
}
INLINED Bitboard piece_bb(const Board *b, Color c, PieceType pt) {
        return piecetype_bb(b, pt) & color_bb(b, c);
}
INLINED Bitboard pieces_bb(const Board *b, Color c, PieceType pt1, PieceType pt2) {
        return piecetypes_bb(b, pt1, pt2) & color_bb(b, c);
}
INLINED Bitboard occupancy_bb(const Board *b) {
        return piecetype_bb(b, ALL_PIECES);
}
INLINED Square get_king_square(const Board *b, Color c) {
        return bb_first_sq(piece_bb(b, c, KING));
}
INLINED Bitboard king_moves(Square sq) {
        return PseudoMoves[KING][sq];
}
INLINED Bitboard knight_moves(Square sq) {
        return PseudoMoves[KNIGHT][sq];
}
INLINED Bitboard pawn_moves(Square sq, Color c) {
        return PawnMoves[c][sq];
}
INLINED Bitboard bishop_moves(const Board *b, Square sq) {
        return bishop_moves_bb(sq, occupancy_bb(b));
}
INLINED Bitboard rook_moves(const Board *b, Square sq) {
        return rook_moves_bb(sq, occupancy_bb(b));
}
INLINED Bitboard piece_moves(PieceType pt, Square sq, Bitboard occ) {
        switch (pt) {
                case KNIGHT:
                        return knight_moves(sq);
                case BISHOP:
                        return bishop_moves_bb(sq, occ);
                case ROOK:
                        return rook_moves_bb(sq, occ);
                case QUEEN:
                        return bishop_moves_bb(sq, occ) | rook_moves_bb(sq, occ);
                case KING:
                        return king_moves(sq);
                default:
                        __builtin_unreachable();
                        return 0;
        }
}
INLINED Bitboard attackers_to(const Board *b, Square sq) {
        return attackers_list(b, sq, occupancy_bb(b));
}
INLINED bool castling_blocked(const Board *b, int c) {
        return occupancy_bb(b) & b->castlingPath[c];
}
INLINED bool capture_or_promotion(const Board *b, Move m) {
        return move_type(m) == NORMAL_MOVE ? !empty_square(b, to_sq(m))
                                           : move_type(m) != CASTLING;
}
INLINED void put_piece(Board *b, Piece pc, Square sq) {
        Bitboard sqbb = square_bb(sq);
        b->table[sq]  = pc;
        b->piecetypeBB[ALL_PIECES] |= sqbb;
        b->piecetypeBB[piece_type(pc)] |= sqbb;
        b->colorBB[piece_color(pc)] |= sqbb;
        b->pieceCount[pc]++;
        b->pieceCount[create_piece(piece_color(pc), ALL_PIECES)]++;
        b->psqScorePair += PsqScore[pc][sq];
}
INLINED void move_piece(Board *b, Square from, Square to) {
        Piece    pc     = piece_on(b, from);
        Bitboard moveBB = square_bb(from) | square_bb(to);
        b->piecetypeBB[ALL_PIECES] ^= moveBB;
        b->piecetypeBB[piece_type(pc)] ^= moveBB;
        b->colorBB[piece_color(pc)] ^= moveBB;
        b->table[from] = NO_PIECE;
        b->table[to]   = pc;
        b->psqScorePair += PsqScore[pc][to] - PsqScore[pc][from];
}
INLINED void remove_piece(Board *b, Square sq) {
        Piece    pc   = piece_on(b, sq);
        Bitboard sqbb = square_bb(sq);
        b->piecetypeBB[ALL_PIECES] ^= sqbb;
        b->piecetypeBB[piece_type(pc)] ^= sqbb;
        b->colorBB[piece_color(pc)] ^= sqbb;
        b->pieceCount[pc]--;
        b->pieceCount[create_piece(piece_color(pc), ALL_PIECES)]--;
        b->psqScorePair -= PsqScore[pc][sq];
}
INLINED void do_move(Board *restrict b, Move m, Boardstack *restrict s) {
        do_move_gc(b, m, s, move_gives_check(b, m));
}

typedef struct {
        Move  move;
        Score score;
} ExtendedMove;
typedef struct {
        ExtendedMove moves[256], *last;
} Movelist;

extern Movelist UciSearchMoves;

ExtendedMove *generate_all(ExtendedMove *restrict ml, const Board *restrict b);
ExtendedMove *generate_classic(ExtendedMove *restrict ml, const Board *restrict b);
ExtendedMove *generate_evasions(ExtendedMove *restrict ml, const Board *restrict b);
ExtendedMove *
generate_captures(ExtendedMove *restrict ml, const Board *restrict b, bool inQs);
ExtendedMove *generate_quiet(ExtendedMove *restrict ml, const Board *restrict b);
void          place_top_move(ExtendedMove *begin, ExtendedMove *end);

INLINED void list_all(Movelist *restrict ml, const Board *restrict b) {
        ml->last = generate_all(ml->moves, b);
}
INLINED void list_pseudo(Movelist *restrict ml, const Board *restrict b) {
        ml->last = b->stack->checkers ? generate_evasions(ml->moves, b)
                                      : generate_classic(ml->moves, b);
}
INLINED size_t movelist_size(const Movelist *ml) {
        return (ml->last - ml->moves);
}
INLINED const ExtendedMove *movelist_begin(const Movelist *ml) {
        return (ml->moves);
}
INLINED const ExtendedMove *movelist_end(const Movelist *ml) {
        return (ml->last);
}
INLINED bool movelist_has_move(const Movelist *ml, Move m) {
        for (const ExtendedMove *em = movelist_begin(ml); em < movelist_end(ml); ++em)
                if (em->move == m)
                        return true;
        return false;
}

Score evaluate(const Board *b);

typedef struct {
        int              plies, doubleExtensions;
        Score            staticEval;
        Move             killers[2], excludedMove, currentMove, *pv;
        piece_history_t *pieceHistory;
} Searchstack;

void  init_search_tables(void);
Score search(bool pvNode,
Board            *b,
int               depth,
Score             alpha,
Score             beta,
Searchstack      *ss,
bool              cutNode);
int   lmr_base_value(int depth, int moves, bool improving, bool quiet);
void  update_pv(Move *pv, Move best, Move *sub);

INLINED Score
search_zw(Board *b, int depth, Score beta, Searchstack *ss, bool cut) {
        return -search(false, b, depth, -beta, 1 - beta, ss + 1, cut);
}

INLINED Score
search_qs(bool pv, Board *b, Score alpha, Score beta, Searchstack *ss) {
        return -search(pv, b, -MAX_PLIES, -beta, -alpha, ss + 1, false);
}

typedef struct {
        HashKey  key;
        Score    score, eval;
        uint8_t  depth, genbound;
        uint16_t bestmove;
} TT_Entry;

enum { ClusterSize = 4 };

typedef struct {
        TT_Entry clEntry[ClusterSize];
} TT_Cluster;
typedef struct {
        size_t      clusterCount;
        TT_Cluster *table;
        uint8_t     generation;
} TranspositionTable;

extern TranspositionTable SearchTT;
extern int                Pruning[2][16];

INLINED TT_Entry *tt_entry_at(HashKey k) {
        return SearchTT.table[mul_hi64(k, SearchTT.clusterCount)].clEntry;
}
INLINED void tt_clear(void) {
        SearchTT.generation += 4;
}
INLINED Score score_to_tt(Score s, int plies) {
        return s >= MATE_FOUND ? s + plies : s <= -MATE_FOUND ? s - plies : s;
}
INLINED Score score_from_tt(Score s, int plies) {
        return s >= MATE_FOUND ? s - plies : s <= -MATE_FOUND ? s + plies : s;
}

void      tt_bzero(size_t threadCount);
TT_Entry *tt_probe(HashKey key, bool *found);
void      tt_save(TT_Entry *e, HashKey k, Score s, Score ev, int d, int b, Move m);
int       tt_hashfull(void);
void      tt_resize(size_t mbsize);

typedef struct {
        long threads, hash, moveOverhead, multiPv;
        bool chess960, ponder, debug, showWDL, normalizeScore;
} OptionFields;

extern OptionFields UciOptionFields;

typedef struct {
        const char *commandName;
        void (*call)(const char *);
} CommandMap;

char       *get_next_token(char **str);
const char *move_to_str(Move m, bool chess960);
Move        str_to_move(const Board *b, const char *str);
void        uci_loop(int argc, char **argv);
void        uci_d(const char *args);
void        uci_debug(const char *args);
void        uci_go(const char *args);
void        uci_isready(const char *args);
void        uci_ponderhit(const char *args);
void        uci_position(const char *args);
void        uci_quit(const char *args);
void        uci_setoption(const char *args);
void        uci_stop(const char *args);
void        uci_uci(const char *args);
void        uci_ucinewgame(const char *args);

typedef struct {
        clock_t  wtime, btime, winc, binc;
        int      movestogo, depth, mate, infinite, perft, ponder;
        uint64_t nodes;
        clock_t  movetime;
} SearchParams;

extern SearchParams   UciSearchParams;
extern pthread_attr_t WorkerSettings;

typedef struct {
        Move  move;
        int   seldepth;
        Score prevScore, score;
        Move  pv[512];
} RootMove;

RootMove *find_root_move(RootMove *begin, RootMove *end, Move m);

typedef struct {
        Board                  board;
        Boardstack            *stack;
        butterfly_history_t    bfHistory;
        continuation_history_t ctHistory;
        countermove_history_t  cmHistory;
        capture_history_t      capHistory;
        int                    seldepth, rootDepth, verifPlies;
        _Atomic uint64_t       nodes;
        RootMove              *rootMoves;
        size_t                 rootCount, idx;
        int                    pvLine;
        pthread_t              thread;
        pthread_mutex_t        mutex;
        pthread_cond_t         condVar;
        bool                   exit, searching;
} Worker;

INLINED Worker *get_worker(const Board *b) {
        return b->worker;
}
INLINED Score draw_score(const Worker *w) {
        return (atomic_load_explicit(&w->nodes, memory_order_relaxed) & 2) - 1;
}
INLINED void count_node(Worker *w) {
        atomic_fetch_add_explicit(&w->nodes, 1, memory_order_relaxed);
}

void  worker_init(Worker *w, size_t idx);
void  worker_destroy(Worker *w);
void  worker_search(Worker *w);
void  main_worker_search(Worker *w);
void  worker_reset(Worker *w);
void  worker_start_search(Worker *w);
void  worker_wait_search_end(Worker *w);
void *worker_entry(void *w);
void  search_print_root_info(Board *b,
Worker                            *w,
int                                mpv,
int                                iter,
clock_t                            time,
int                                bound);

typedef struct {
        size_t      size;
        int         checks;
        atomic_bool ponder, stop;
        Worker    **workerList;
} WorkerPool;

extern WorkerPool SearchWorkerPool;

INLINED Worker *wpool_main_worker(WorkerPool *wp) {
        return wp->workerList[0];
}
INLINED void wpool_ponderhit(WorkerPool *wp) {
        atomic_store_explicit(&wp->ponder, false, memory_order_relaxed);
}
INLINED bool wpool_is_pondering(const WorkerPool *wp) {
        return atomic_load_explicit(&wp->ponder, memory_order_relaxed);
}
INLINED void wpool_stop(WorkerPool *wp) {
        atomic_store_explicit(&wp->stop, true, memory_order_relaxed);
}
INLINED bool wpool_is_stopped(const WorkerPool *wp) {
        return atomic_load_explicit(&wp->stop, memory_order_relaxed);
}

void wpool_init(WorkerPool *wp, size_t threads);
void wpool_new_search(WorkerPool *wp);
void wpool_reset(WorkerPool *wp);
void wpool_start_search(WorkerPool *wp, const Board *rootBoard, const SearchParams *sp);
void wpool_start_workers(WorkerPool *wp);
void wpool_wait_search_end(WorkerPool *wp);
uint64_t wpool_get_total_nodes(WorkerPool *wp);

typedef enum {
        PICK_TT,
        GEN_INSTABLE,
        PICK_GOOD_INSTABLE,
        PICK_KILLER1,
        PICK_KILLER2,
        GEN_QUIET,
        PICK_QUIET,
        PICK_BAD_INSTABLE,
        CHECK_PICK_TT,
        CHECK_GEN_ALL,
        CHECK_PICK_ALL
} mp_stage_t;

typedef struct {
        Movelist         list;
        ExtendedMove    *cur, *badCaptures;
        bool             inQsearch;
        mp_stage_t       stage;
        Move             ttMove, killer1, killer2;
        const Board     *board;
        const Worker    *worker;
        piece_history_t *pieceHistory[2];
} Movepicker;

INLINED clock_t chess_clock(void) {
#if defined(_WIN32) || defined(_WIN64)
        struct timeb tp;
        ftime(&tp);
        return (clock_t)tp.time * 1000 + tp.millitm;
#else
        struct timespec tp;
        clock_gettime(CLOCK_REALTIME, &tp);
        return (clock_t)tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
#endif
}

typedef enum {
        NO_BM_TYPE = -1,
        OneLegalMove,
        Promotion,
        SoundCapture,
        SoundCheck,
        Capture,
        Quiet,
        WeirdCheck,
        WeirdQuiet,
        BM_TYPE_NB
} bestmove_type_t;

extern const double BestmoveTypeScale[BM_TYPE_NB];

typedef enum { Tournament, Movetime, NoTimeman } timeman_mode_t;

typedef struct {
        clock_t         start, averageTime, maximalTime, optimalTime;
        timeman_mode_t  mode;
        bool            pondering;
        int             checkFrequency;
        Score           prevScore;
        Move            prevBestmove;
        int             stability;
        bestmove_type_t type;
} Timeman;

extern Timeman SearchTimeman;

void timeman_init(const Board *b, Timeman *tm, SearchParams *params, clock_t start);
void timeman_update(Timeman *tm,
const Board                 *b,
Move                         bestmove,
Score                        score,
int                          seldepth,
int                          rootDepth,
int                          aspFails);
void check_time(void);

INLINED bool timeman_must_stop_search(Timeman *tm, clock_t cur) {
        if (tm->pondering && wpool_is_pondering(&SearchWorkerPool))
                return false;
        return tm->mode != NoTimeman && cur >= tm->start + tm->maximalTime;
}

#endif
