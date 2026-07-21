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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char PieceIndexes[PIECE_NB] = " PNBRQK  pnbrqk";

Bitboard LineBB[SQUARE_NB][SQUARE_NB], PseudoMoves[PIECETYPE_NB][SQUARE_NB];
Bitboard PawnMoves[COLOR_NB][SQUARE_NB], HiddenRookTable[0x19000];
Bitboard HiddenBishopTable[0x1480];
Magic    BishopMagics[SQUARE_NB], RookMagics[SQUARE_NB];
int      SquareDistance[SQUARE_NB][SQUARE_NB];
HashKey  CyclicKeys[8192], ZobristPsq[PIECE_NB][SQUARE_NB];
HashKey  ZobristEnPassant[FILE_NB], ZobristCastling[CASTLING_NB], ZobristSideToMove;
Move     CyclicMoves[8192];

Bitboard sliding_attack(const Direction *dirs, Square sq, Bitboard occ) {
        Bitboard attack = 0;
        for (int i = 0; i < 4; ++i)
                for (Square s = sq + dirs[i];
                is_valid_sq(s) && SquareDistance[s][s - dirs[i]] == 1;
                s += dirs[i]) {
                        attack |= square_bb(s);
                        if (occ & square_bb(s))
                                break;
                }
        return attack;
}

void magic_init(Bitboard *table, Magic *magics, const Direction *dirs) {
        Bitboard reference[4096], b;
#ifndef USE_PEXT
        Bitboard occupancy[4096];
        int      epoch[4096] = {0}, currentEpoch = 0;
        uint64_t seed = 64;
#endif
        int size = 0;

        for (Square sq = SQ_A1; sq <= SQ_H8; ++sq) {
                Bitboard edges = ((RANK_1_BB | RANK_8_BB) & ~sq_rank_bb(sq)) |
                ((FILE_A_BB | FILE_H_BB) & ~sq_file_bb(sq));
                Magic *m = magics + sq;
                m->mask  = sliding_attack(dirs, sq, 0) & ~edges;
                m->shift = 64 - popcount(m->mask);
                m->moves = (sq == SQ_A1) ? table : magics[sq - 1].moves + size;
                b        = 0;
                size     = 0;
                do {
#ifndef USE_PEXT
                        occupancy[size] = b;
#endif
                        reference[size] = sliding_attack(dirs, sq, b);
#ifdef USE_PEXT
                        m->moves[_pext_u64(b, m->mask)] = reference[size];
#endif
                        size++;
                        b = (b - m->mask) & m->mask;
                } while (b);
#ifndef USE_PEXT
                for (int i = 0; i < size;) {
                        for (m->magic = 0; popcount((m->magic * m->mask) >> 56) < 6;)
                                m->magic = qrandom(&seed) & qrandom(&seed) &
                                qrandom(&seed);
                        for (++currentEpoch, i = 0; i < size; ++i) {
                                unsigned int idx = magic_index(m, occupancy[i]);
                                if (epoch[idx] < currentEpoch) {
                                        epoch[idx]    = currentEpoch;
                                        m->moves[idx] = reference[i];
                                } else if (m->moves[idx] != reference[i])
                                        break;
                        }
                }
#endif
        }
}

void bitboard_init(void) {
        static const Direction kingDirs[8]   = {-9, -8, -7, -1, 1, 7, 8, 9};
        static const Direction knightDirs[8] = {-17, -15, -10, -6, 6, 10, 15, 17};
        static const Direction bishopDirs[4] = {-9, -7, 7, 9};
        static const Direction rookDirs[4]   = {-8, -1, 1, 8};

        for (Square a = SQ_A1; a <= SQ_H8; ++a)
                for (Square b = SQ_A1; b <= SQ_H8; ++b)
                        SquareDistance[a][b] = imax(abs(sq_file(a) - sq_file(b)),
                        abs(sq_rank(a) - sq_rank(b)));

        for (Square sq = SQ_A1; sq <= SQ_H8; ++sq) {
                const Bitboard b     = square_bb(sq);
                PawnMoves[WHITE][sq] = shift_up_left(b) | shift_up_right(b);
                PawnMoves[BLACK][sq] = shift_down_left(b) | shift_down_right(b);
                for (int i = 0; i < 8; ++i) {
                        const Square kto = sq + kingDirs[i], nto = sq + knightDirs[i];
                        if (is_valid_sq(kto) && SquareDistance[sq][kto] <= 2)
                                PseudoMoves[KING][sq] |= square_bb(kto);
                        if (is_valid_sq(nto) && SquareDistance[sq][nto] <= 2)
                                PseudoMoves[KNIGHT][sq] |= square_bb(nto);
                }
        }

        magic_init(HiddenBishopTable, BishopMagics, bishopDirs);
        magic_init(HiddenRookTable, RookMagics, rookDirs);

        for (Square a = SQ_A1; a <= SQ_H8; ++a) {
                PseudoMoves[BISHOP][a] = bishop_moves_bb(a, 0);
                PseudoMoves[ROOK][a]   = rook_moves_bb(a, 0);
                PseudoMoves[QUEEN][a]  = PseudoMoves[BISHOP][a] | PseudoMoves[ROOK][a];
                for (Square b = SQ_A1; b <= SQ_H8; ++b) {
                        const Bitboard ends = square_bb(a) | square_bb(b);
                        if (PseudoMoves[BISHOP][a] & square_bb(b))
                                LineBB[a][b] = (bishop_moves_bb(a, 0) &
                                               bishop_moves_bb(b, 0)) |
                                ends;
                        if (PseudoMoves[ROOK][a] & square_bb(b))
                                LineBB[a][b] = (rook_moves_bb(a, 0) &
                                               rook_moves_bb(b, 0)) |
                                ends;
                }
        }
}

void zobrist_init(void) {
        uint64_t seed = 0x7F6E5D4C3B2A1908ull;
        for (Piece pc = WHITE_PAWN; pc <= BLACK_KING; ++pc)
                for (Square sq = SQ_A1; sq <= SQ_H8; ++sq)
                        ZobristPsq[pc][sq] = qrandom(&seed);
        for (File f = FILE_A; f <= FILE_H; ++f)
                ZobristEnPassant[f] = qrandom(&seed);
        for (int cr = 0; cr < CASTLING_NB; ++cr) {
                ZobristCastling[cr] = 0;
                Bitboard b          = cr;
                while (b) {
                        HashKey k = ZobristCastling[1ull << bb_pop_first_sq(&b)];
                        ZobristCastling[cr] ^= k ? k : qrandom(&seed);
                }
        }
        ZobristSideToMove = qrandom(&seed);
}

INLINED uint16_t cyclic_index_lo(HashKey key) {
        return key & 0x1FFFu;
}
INLINED uint16_t cyclic_index_hi(HashKey key) {
        return (key >> 13) & 0x1FFFu;
}

static void cyclic_init_move(Piece pc, Square from, Square to) {
        Move     move = create_move(from, to);
        HashKey  key  = ZobristPsq[pc][from] ^ ZobristPsq[pc][to] ^ ZobristSideToMove;
        uint16_t idx  = cyclic_index_lo(key);
        while (true) {
                HashKey tmpKey   = CyclicKeys[idx];
                CyclicKeys[idx]  = key;
                key              = tmpKey;
                Move tmpMove     = CyclicMoves[idx];
                CyclicMoves[idx] = move;
                move             = tmpMove;
                if (move == NO_MOVE)
                        break;
                idx ^= cyclic_index_lo(key) ^ cyclic_index_hi(key);
        }
}

void cyclic_init(void) {
        for (PieceType pt = KNIGHT; pt <= KING; ++pt)
                for (Color c = WHITE; c <= BLACK; ++c)
                        for (Square from = SQ_A1; from <= SQ_H8; ++from)
                                for (Square to = from + 1; to <= SQ_H8; ++to)
                                        if (piece_moves(pt, from, 0) & square_bb(to))
                                                cyclic_init_move(create_piece(c, pt),
                                                from,
                                                to);
}

static int fen_error(const char *message) {
        debug_printf("info error Invalid FEN: %s\n", message);
        return -1;
}

static const char *fen_next(const char *fen, size_t length) {
        fen += length;
        return fen + strspn(fen, Delimiters);
}

static int
fen_long(const char **fen, long min, long max, const char *field, long *value) {
        char        *end;
        const size_t length = strcspn(*fen, Delimiters);

        *value = strtol(*fen, &end, 10);
        if (*fen + length != end || *value < min || *value > max)
                return fen_error(field);

        *fen = fen_next(*fen, length);
        return 0;
}

static int board_parse_fen_pieces(Board *b, const char *fen) {
        Square sq      = SQ_A8;
        Rank   curRank = RANK_8;
        size_t next    = strcspn(fen, Delimiters);
        for (size_t i = 0; i < next; ++i) {
                const char c = fen[i];
                if (c >= '1' && c <= '8') {
                        sq += c - '0';
                        if (sq_rank(sq - 1) > curRank)
                                return fen_error("too many squares on rank");
                } else if (c == '/') {
                        if (sq != create_sq(FILE_A, curRank + 1))
                                return fen_error("incomplete rank");
                        if (curRank == RANK_1)
                                return fen_error("too many ranks");
                        sq += 2 * SOUTH;
                        --curRank;
                } else {
                        const char *p = strchr(PieceIndexes, c);
                        if (!p)
                                return fen_error("invalid character");
                        if (sq_rank(sq) > curRank)
                                return fen_error("too many squares on rank");
                        put_piece(b, (Piece)(p - PieceIndexes), sq++);
                }
        }
        if (curRank != RANK_1)
                return fen_error("missing ranks");
        if (sq_file(sq) != FILE_A)
                return fen_error("incomplete last rank");
        if (b->pieceCount[WHITE_KING] != 1 || b->pieceCount[BLACK_KING] != 1)
                return fen_error("invalid number of Kings");
        if (SquareDistance[get_king_square(b, WHITE)][get_king_square(b, BLACK)] == 1)
                return fen_error("Kings adjacent");
        if (piecetype_bb(b, PAWN) & (RANK_1_BB | RANK_8_BB))
                return fen_error("Pawns on back ranks");
        return (int)next;
}

static int board_set_castling(Board *b, Color c, Square rookSq) {
        const Square kingSq   = get_king_square(b, c);
        const bool   kingside = kingSq < rookSq;
        const int    castling = (c == WHITE ? WHITE_CASTLING : BLACK_CASTLING) &
        (kingside ? KINGSIDE_CASTLING : QUEENSIDE_CASTLING);
        if (relative_sq_rank(kingSq, c) != RANK_1)
                return fen_error("castling with king not on back-rank");
        if (sq_file(kingSq) != FILE_E ||
        (sq_file(rookSq) != FILE_A && sq_file(rookSq) != FILE_H))
                b->chess960 = true;
        b->stack->castlings |= castling;
        b->castlingMask[kingSq] |= castling;
        b->castlingMask[rookSq] |= castling;
        b->castlingRookSquare[castling] = rookSq;
        const Square kingAfter          = relative_sq(kingside ? SQ_G1 : SQ_C1, c);
        const Square rookAfter          = relative_sq(kingside ? SQ_F1 : SQ_D1, c);
        b->castlingPath[castling] = (between_bb(rookSq, rookAfter) |
                                    between_bb(kingSq, kingAfter) | square_bb(rookAfter) |
                                    square_bb(kingAfter)) &
        ~(square_bb(kingSq) | square_bb(rookSq));
        return 0;
}

static int board_parse_castling(Board *b, const char *fen) {
        size_t next = strcspn(fen, Delimiters);
        for (size_t i = 0; i < next; ++i) {
                if (fen[i] == '-') {
                        if (next > 1)
                                return fen_error("'-' with extra castling characters");
                        break;
                }
                Square      rookSq;
                const Color side = islower((unsigned char)fen[i]) ? BLACK : WHITE;
                const Piece rook = create_piece(side, ROOK);
                const char  cc   = toupper((unsigned char)fen[i]);
                if (cc == 'K' || cc == 'Q') {
                        const bool      kingside = cc == 'K';
                        const File      edge     = kingside ? FILE_A : FILE_H;
                        const Direction step     = kingside ? WEST : EAST;
                        for (rookSq = relative_sq(kingside ? SQ_H1 : SQ_A1, side);
                        sq_file(rookSq) != edge;
                        rookSq += step)
                                if (piece_on(b, rookSq) == rook)
                                        break;
                } else if (cc >= 'A' && cc <= 'H') {
                        rookSq = create_sq(cc - 'A', relative_rank(RANK_1, side));
                } else {
                        return fen_error("invalid castling character");
                }
                if (board_set_castling(b, side, rookSq) < 0)
                        return -1;
        }
        return (int)next;
}

static int board_parse_en_passant(Board *b, const char *fen) {
        size_t next               = strcspn(fen, Delimiters);
        b->stack->enPassantSquare = SQ_NONE;
        if (next == 1 && *fen != '-')
                return fen_error("invalid en-passant");
        if (next > 2)
                return fen_error("invalid en-passant format");
        if (next <= 1)
                return (int)next;
        const char fc = fen[0], rc = fen[1];
        if (fc < 'a' || fc > 'h' || rc != (b->sideToMove == WHITE ? '6' : '3'))
                return fen_error("invalid en-passant square");
        const Square epSq = create_sq(fc - 'a', rc - '1');
        const Color  us = b->sideToMove, them = not_color(us);
        if (!(piece_bb(b, them, PAWN) & square_bb(epSq + pawn_direction(them))))
                return fen_error("en-passant without pawn");
        if (attackers_to(b, epSq) & piece_bb(b, us, PAWN))
                b->stack->enPassantSquare = epSq;
        return (int)next;
}

int board_from_fen(Board *b, const char *fen, bool is960, Boardstack *bs) {
        memset(b, 0, sizeof(Board));
        memset(bs, 0, sizeof(Boardstack));
        b->stack = bs;
        fen += strspn(fen, Delimiters);

        int r;
        if ((r = board_parse_fen_pieces(b, fen)) < 0)
                return r;
        fen = fen_next(fen, r);
        r   = strcspn(fen, Delimiters);
        if (r > 1)
                return fen_error("invalid side to move");
        if (*fen == 'b')
                b->sideToMove = BLACK;
        else if (*fen != 'w' && *fen != '\0')
                return fen_error("invalid side to move character");
        fen = fen_next(fen, r);
        if (attackers_to(b, get_king_square(b, not_color(b->sideToMove))) &
        color_bb(b, b->sideToMove))
                return fen_error("opposite King in check");
        if ((r = board_parse_castling(b, fen)) < 0)
                return r;
        fen = fen_next(fen, r);
        if (!is960 && b->chess960)
                debug_printf("info string Warning: FRC position without "
                             "UCI_Chess960 "
                             "flag\n");
        b->chess960 = is960;
        if ((r = board_parse_en_passant(b, fen)) < 0)
                return r;
        fen = fen_next(fen, r);

        long r50, moveNum;
        if (fen_long(&fen, -1024, 1024, "invalid rule50", &r50) < 0 ||
        fen_long(&fen, 0, 2048, "invalid move number", &moveNum) < 0)
                return -1;
        b->stack->rule50 = r50;
        b->ply           = imax(0, 2 * ((int)moveNum - 1)) + (b->sideToMove == BLACK);

        set_boardstack(b, b->stack);
        if (popcount(b->stack->checkers) > 2)
                return fen_error(">2 checkers");
        return 0;
}

void set_boardstack(Board *b, Boardstack *s) {
        s->boardKey = s->pawnKey = s->materialKey = 0;
        s->material[WHITE] = s->material[BLACK] = 0;
        s->checkers = attackers_to(b, get_king_square(b, b->sideToMove)) &
        color_bb(b, not_color(b->sideToMove));
        set_check(b, s);
        for (Bitboard bb = occupancy_bb(b); bb;) {
                const Square sq = bb_pop_first_sq(&bb);
                const Piece  pc = piece_on(b, sq);
                s->boardKey ^= ZobristPsq[pc][sq];
                if (piece_type(pc) == PAWN)
                        s->pawnKey ^= ZobristPsq[pc][sq];
                else if (piece_type(pc) != KING)
                        s->material[piece_color(pc)] += PieceScores[MIDGAME][pc];
        }
        if (s->enPassantSquare != SQ_NONE)
                s->boardKey ^= ZobristEnPassant[sq_file(s->enPassantSquare)];
        if (b->sideToMove == BLACK)
                s->boardKey ^= ZobristSideToMove;
        for (Color c = WHITE; c <= BLACK; ++c)
                for (PieceType pt = PAWN; pt <= KING; ++pt) {
                        const Piece pc = create_piece(c, pt);
                        for (int i = 0; i < b->pieceCount[pc]; ++i)
                                s->materialKey ^= ZobristPsq[pc][i];
                }
        s->boardKey ^= ZobristCastling[s->castlings];
}

void set_check(Board *restrict b, Boardstack *restrict s) {
        s->kingBlockers[WHITE]  = slider_blockers(b,
        color_bb(b, BLACK),
        get_king_square(b, WHITE),
        &s->pinners[BLACK]);
        s->kingBlockers[BLACK]  = slider_blockers(b,
        color_bb(b, WHITE),
        get_king_square(b, BLACK),
        &s->pinners[WHITE]);
        const Square kingSq     = get_king_square(b, not_color(b->sideToMove));
        s->checkSquares[PAWN]   = pawn_moves(kingSq, not_color(b->sideToMove));
        s->checkSquares[KNIGHT] = knight_moves(kingSq);
        s->checkSquares[BISHOP] = bishop_moves(b, kingSq);
        s->checkSquares[ROOK]   = rook_moves(b, kingSq);
        s->checkSquares[QUEEN]  = s->checkSquares[BISHOP] | s->checkSquares[ROOK];
        s->checkSquares[KING]   = 0;
}

Boardstack *dup_boardstack(const Boardstack *s) {
        if (!s)
                return NULL;
        Boardstack *const new = malloc(sizeof(Boardstack));
        *new                  = *s;
        new->prev             = dup_boardstack(s->prev);
        return new;
}

void free_boardstack(Boardstack *s) {
        while (s) {
                Boardstack *const next = s->prev;
                free(s);
                s = next;
        }
}

const char *board_fen(const Board *b) {
        static char fen[128];
        char       *ptr = fen;
        for (Rank r = RANK_8; r >= RANK_1; --r) {
                for (File f = FILE_A; f <= FILE_H; ++f) {
                        int empty;
                        for (empty = 0; f <= FILE_H && empty_square(b, create_sq(f, r));
                        ++f)
                                ++empty;
                        if (empty)
                                *(ptr++) = empty + '0';
                        if (f <= FILE_H)
                                *(ptr++) = PieceIndexes[piece_on(b, create_sq(f, r))];
                }
                if (r > RANK_1)
                        *(ptr++) = '/';
        }
        *(ptr++) = ' ';
        *(ptr++) = (b->sideToMove == WHITE) ? 'w' : 'b';
        *(ptr++) = ' ';
        static const struct {
                int  mask;
                char std, base960;
        } CR[4] = {
        {WHITE_OO, 'K', 'A'},
        {WHITE_OOO, 'Q', 'A'},
        {BLACK_OO, 'k', 'a'},
        {BLACK_OOO, 'q', 'a'},
        };
        for (int i = 0; i < 4; ++i)
                if (b->stack->castlings & CR[i].mask)
                        *(ptr++) = b->chess960
                        ? CR[i].base960 + sq_file(b->castlingRookSquare[CR[i].mask])
                        : CR[i].std;
        if (!(b->stack->castlings & ANY_CASTLING))
                *(ptr++) = '-';
        *(ptr++) = ' ';
        if (b->stack->enPassantSquare == SQ_NONE)
                *(ptr++) = '-';
        else {
                *(ptr++) = 'a' + sq_file(b->stack->enPassantSquare);
                *(ptr++) = '1' + sq_rank(b->stack->enPassantSquare);
        }
        sprintf(ptr,
        " %d %d",
        b->stack->rule50,
        1 + (b->ply - (b->sideToMove == BLACK)) / 2);
        return fen;
}

void do_move_gc(Board *restrict b, Move m, Boardstack *restrict next, bool gc) {
        const Boardstack *prev  = b->stack;
        next->castlings         = prev->castlings;
        next->rule50            = prev->rule50;
        next->pliesFromNullMove = prev->pliesFromNullMove;
        next->enPassantSquare   = prev->enPassantSquare;
        next->pawnKey           = prev->pawnKey;
        next->materialKey       = prev->materialKey;
        next->material[WHITE]   = prev->material[WHITE];
        next->material[BLACK]   = prev->material[BLACK];
        next->prev              = b->stack;

        HashKey key = prev->boardKey ^ ZobristSideToMove;
        b->stack    = next;
        b->ply += 1;
        next->rule50 += 1;
        next->pliesFromNullMove += 1;

        const Color us = b->sideToMove, them = not_color(us);
        Square      from = from_sq(m), to = to_sq(m);
        const Piece pc   = piece_on(b, from);
        const int   type = move_type(m);
        Piece       cap = type == EN_PASSANT ? create_piece(them, PAWN) : piece_on(b, to);
        if (type == CASTLING) {
                Square rookFrom, rookTo;
                do_castling(b, us, from, &to, &rookFrom, &rookTo);
                key ^= ZobristPsq[cap][rookFrom] ^ ZobristPsq[cap][rookTo];
                cap = NO_PIECE;
        }
        if (cap) {
                Square capSq = to;
                if (piece_type(cap) == PAWN) {
                        if (type == EN_PASSANT)
                                capSq -= pawn_direction(us);
                        next->pawnKey ^= ZobristPsq[cap][capSq];
                } else
                        next->material[them] -= PieceScores[MIDGAME][cap];
                remove_piece(b, capSq);
                if (type == EN_PASSANT)
                        b->table[capSq] = NO_PIECE;
                key ^= ZobristPsq[cap][capSq];
                next->materialKey ^= ZobristPsq[cap][b->pieceCount[cap]];
                next->rule50 = 0;
        }
        key ^= ZobristPsq[pc][from] ^ ZobristPsq[pc][to];
        if (next->enPassantSquare != SQ_NONE) {
                key ^= ZobristEnPassant[sq_file(next->enPassantSquare)];
                next->enPassantSquare = SQ_NONE;
        }
        if (next->castlings && (b->castlingMask[from] | b->castlingMask[to])) {
                const int c = b->castlingMask[from] | b->castlingMask[to];
                key ^= ZobristCastling[next->castlings & c];
                next->castlings &= ~c;
        }
        if (type != CASTLING)
                move_piece(b, from, to);
        if (piece_type(pc) == PAWN) {
                if ((to ^ from) == 16 &&
                (pawn_moves(to - pawn_direction(us), us) & piece_bb(b, them, PAWN))) {
                        next->enPassantSquare = to - pawn_direction(us);
                        key ^= ZobristEnPassant[sq_file(next->enPassantSquare)];
                } else if (type == PROMOTION) {
                        const Piece newPc = create_piece(us, promotion_type(m));
                        remove_piece(b, to);
                        put_piece(b, newPc, to);
                        key ^= ZobristPsq[pc][to] ^ ZobristPsq[newPc][to];
                        next->pawnKey ^= ZobristPsq[pc][to];
                        next->material[us] += PieceScores[MIDGAME][promotion_type(m)];
                        next->materialKey ^= ZobristPsq[newPc][b->pieceCount[newPc] - 1];
                        next->materialKey ^= ZobristPsq[pc][b->pieceCount[pc]];
                }
                next->pawnKey ^= ZobristPsq[pc][from] ^ ZobristPsq[pc][to];
                next->rule50 = 0;
        }
        next->capturedPiece = cap;
        next->boardKey      = key;
        prefetch(tt_entry_at(key));
        next->checkers = gc ? attackers_to(b, get_king_square(b, them)) & color_bb(b, us)
                            : 0;
        b->sideToMove  = not_color(b->sideToMove);
        set_check(b, next);
        next->repetition   = 0;
        const int repPlies = imin(next->rule50, next->pliesFromNullMove);
        if (repPlies >= 4) {
                Boardstack *rw = b->stack->prev->prev;
                for (int i = 4; i <= repPlies; i += 2) {
                        rw = rw->prev->prev;
                        if (rw->boardKey == next->boardKey) {
                                next->repetition = rw->repetition ? -i : i;
                                break;
                        }
                }
        }
}

void undo_move(Board *b, Move m) {
        b->sideToMove    = not_color(b->sideToMove);
        const Color us   = b->sideToMove;
        Square      from = from_sq(m), to = to_sq(m);
        const int   type = move_type(m);
        if (type == PROMOTION) {
                remove_piece(b, to);
                put_piece(b, create_piece(us, PAWN), to);
        }
        if (type == CASTLING) {
                Square rf, rt;
                undo_castling(b, us, from, &to, &rf, &rt);
        } else {
                move_piece(b, to, from);
                if (b->stack->capturedPiece) {
                        Square capSq = to;
                        if (type == EN_PASSANT)
                                capSq -= pawn_direction(us);
                        put_piece(b, b->stack->capturedPiece, capSq);
                }
        }
        b->stack = b->stack->prev;
        b->ply -= 1;
}

INLINED void move_castling(Board *restrict b,
Color  us,
Square kf,
Square *restrict kt,
Square *restrict rf,
Square *restrict rt,
bool undo) {
        const bool ks      = *kt > kf;
        *rf                = *kt;
        *rt                = relative_sq(ks ? SQ_F1 : SQ_D1, us);
        *kt                = relative_sq(ks ? SQ_G1 : SQ_C1, us);
        const Square fromK = undo ? *kt : kf, fromR = undo ? *rt : *rf;
        const Square toK = undo ? kf : *kt, toR = undo ? *rf : *rt;
        remove_piece(b, fromK);
        remove_piece(b, fromR);
        b->table[fromK] = b->table[fromR] = NO_PIECE;
        put_piece(b, create_piece(us, KING), toK);
        put_piece(b, create_piece(us, ROOK), toR);
}

void do_castling(Board *restrict b,
Color  us,
Square kf,
Square *restrict kt,
Square *restrict rf,
Square *restrict rt) {
        move_castling(b, us, kf, kt, rf, rt, false);
}

void undo_castling(Board *restrict b,
Color  us,
Square kf,
Square *restrict kt,
Square *restrict rf,
Square *restrict rt) {
        move_castling(b, us, kf, kt, rf, rt, true);
}

void do_null_move(Board *restrict b, Boardstack *restrict s) {
        memcpy(s, b->stack, sizeof(Boardstack));
        s->prev  = b->stack;
        b->stack = s;
        if (s->enPassantSquare != SQ_NONE) {
                s->boardKey ^= ZobristEnPassant[sq_file(s->enPassantSquare)];
                s->enPassantSquare = SQ_NONE;
        }
        s->boardKey ^= ZobristSideToMove;
        prefetch(tt_entry_at(s->boardKey));
        ++s->rule50;
        s->pliesFromNullMove = 0;
        b->sideToMove        = not_color(b->sideToMove);
        set_check(b, s);
        s->repetition = 0;
}

void undo_null_move(Board *b) {
        b->stack      = b->stack->prev;
        b->sideToMove = not_color(b->sideToMove);
}

Bitboard attackers_list(const Board *b, Square s, Bitboard occ) {
        return ((pawn_moves(s, BLACK) & piece_bb(b, WHITE, PAWN)) |
        (pawn_moves(s, WHITE) & piece_bb(b, BLACK, PAWN)) |
        (knight_moves(s) & piecetype_bb(b, KNIGHT)) |
        (rook_moves_bb(s, occ) & piecetypes_bb(b, ROOK, QUEEN)) |
        (bishop_moves_bb(s, occ) & piecetypes_bb(b, BISHOP, QUEEN)) |
        (king_moves(s) & piecetype_bb(b, KING)));
}

Bitboard slider_blockers(const Board *restrict b,
Bitboard sliders,
Square   sq,
Bitboard *restrict pinners) {
        Bitboard blockers = *pinners = 0;
        Bitboard snipers = ((PseudoMoves[ROOK][sq] & piecetypes_bb(b, QUEEN, ROOK)) |
                           (PseudoMoves[BISHOP][sq] & piecetypes_bb(b, QUEEN, BISHOP))) &
        sliders;
        const Bitboard occ = occupancy_bb(b) ^ snipers;
        while (snipers) {
                const Square   sniper = bb_pop_first_sq(&snipers);
                const Bitboard bet    = between_bb(sq, sniper) & occ;
                if (bet && !more_than_one(bet)) {
                        blockers |= bet;
                        if (bet & color_bb(b, piece_color(piece_on(b, sq))))
                                *pinners |= square_bb(sniper);
                }
        }
        return blockers;
}

bool game_is_drawn(const Board *b, int ply) {
        if (b->stack->rule50 > 99) {
                if (!b->stack->checkers)
                        return true;
                Movelist ml;
                list_all(&ml, b);
                if (movelist_size(&ml))
                        return true;
        }
        return !!b->stack->repetition && b->stack->repetition < ply;
}

bool game_has_cycle(const Board *b, int ply) {
        const int max = imin(b->stack->rule50, b->stack->pliesFromNullMove);
        if (max < 3)
                return false;
        const HashKey     orig = b->stack->boardKey;
        const Boardstack *si   = b->stack->prev;
        for (int i = 3; i <= max; i += 2) {
                si                 = si->prev->prev;
                const HashKey mkey = orig ^ si->boardKey;
                uint16_t      idx  = cyclic_index_lo(mkey);
                if (CyclicKeys[idx] != mkey) {
                        idx = cyclic_index_hi(mkey);
                        if (CyclicKeys[idx] != mkey)
                                continue;
                }
                const Move   mv   = CyclicMoves[idx];
                const Square from = from_sq(mv), to = to_sq(mv);
                if (between_bb(from, to) & occupancy_bb(b))
                        continue;
                if (ply > i)
                        return true;
                if (!si->repetition)
                        continue;
                if (piece_color(piece_on(b, empty_square(b, from) ? to : from)) !=
                b->sideToMove)
                        return true;
        }
        return false;
}

bool move_gives_check(const Board *b, Move m) {
        const Square from = from_sq(m), to = to_sq(m);
        const Color  us = b->sideToMove, them = not_color(us);
        if (b->stack->checkSquares[piece_type(piece_on(b, from))] & square_bb(to))
                return true;
        const Square theirKing = get_king_square(b, them);
        if ((b->stack->kingBlockers[them] & square_bb(from)) &&
        !sq_aligned(from, to, theirKing))
                return true;
        switch (move_type(m)) {
                case NORMAL_MOVE:
                        return false;
                case PROMOTION:
                        return (piece_moves(promotion_type(m),
                                to,
                                occupancy_bb(b) ^ square_bb(from)) &
                        square_bb(theirKing));
                case EN_PASSANT: {
                        Square   capSq = create_sq(sq_file(to), sq_rank(from));
                        Bitboard occ   = (occupancy_bb(b) ^ square_bb(from) ^
                                         square_bb(capSq)) |
                        square_bb(to);
                        return (rook_moves_bb(theirKing, occ) &
                               pieces_bb(b, us, QUEEN, ROOK)) |
                        (bishop_moves_bb(theirKing, occ) &
                        pieces_bb(b, us, QUEEN, BISHOP));
                }
                case CASTLING: {
                        Square kf = from, rf = to;
                        Square kt = relative_sq(rf > kf ? SQ_G1 : SQ_C1, us);
                        Square rt = relative_sq(rf > kf ? SQ_F1 : SQ_D1, us);
                        return (PseudoMoves[ROOK][rt] & square_bb(theirKing)) &&
                        (rook_moves_bb(rt,
                         (occupancy_bb(b) ^ square_bb(kf) ^ square_bb(rf)) |
                         square_bb(kt) | square_bb(rt)) &
                        square_bb(theirKing));
                }
                default:
                        __builtin_unreachable();
                        return false;
        }
}

bool move_is_legal(const Board *b, Move m) {
        const Color us   = b->sideToMove;
        Square      from = from_sq(m), to = to_sq(m);
        if (move_type(m) == EN_PASSANT) {
                const Square   kingSq = get_king_square(b, us);
                const Square   capSq  = to - pawn_direction(us);
                const Bitboard occ    = (occupancy_bb(b) ^ square_bb(from) ^
                                        square_bb(capSq)) |
                square_bb(to);
                return !(rook_moves_bb(kingSq, occ) &
                       pieces_bb(b, not_color(us), QUEEN, ROOK)) &&
                !(bishop_moves_bb(kingSq, occ) &
                pieces_bb(b, not_color(us), QUEEN, BISHOP));
        }
        if (move_type(m) == CASTLING) {
                to             = relative_sq((to > from ? SQ_G1 : SQ_C1), us);
                Direction side = (to > from ? WEST : EAST);
                for (Square sq = to; sq != from; sq += side)
                        if (attackers_to(b, sq) & color_bb(b, not_color(us)))
                                return false;
                return !b->chess960 ||
                !(rook_moves_bb(to, occupancy_bb(b) ^ square_bb(to_sq(m))) &
                pieces_bb(b, not_color(us), ROOK, QUEEN));
        }
        if (piece_type(piece_on(b, from)) == KING)
                return (!(attackers_to(b, to) & color_bb(b, not_color(us))));
        return !(b->stack->kingBlockers[us] & square_bb(from)) ||
        sq_aligned(from, to, get_king_square(b, us));
}

bool move_pseudo_legal(const Board *b, Move m) {
        const Color  us   = b->sideToMove;
        const Square from = from_sq(m), to = to_sq(m);
        const Piece  pc = piece_on(b, from);
        if (move_type(m) != NORMAL_MOVE) {
                Movelist list;
                list_pseudo(&list, b);
                return movelist_has_move(&list, m);
        }
        if (promotion_type(m) != KNIGHT || pc == NO_PIECE || piece_color(pc) != us ||
        (color_bb(b, us) & square_bb(to)))
                return false;
        if (piece_type(pc) == PAWN) {
                if ((RANK_8_BB | RANK_1_BB) & square_bb(to))
                        return false;
                if (!(pawn_moves(from, us) & color_bb(b, not_color(us)) &
                    square_bb(to)) &&
                !((from + pawn_direction(us) == to) && empty_square(b, to)) &&
                !((from + 2 * pawn_direction(us) == to) &&
                relative_sq_rank(from, us) == RANK_2 && empty_square(b, to) &&
                empty_square(b, to - pawn_direction(us))))
                        return false;
        } else if (!(piece_moves(piece_type(pc), from, occupancy_bb(b)) & square_bb(to)))
                return false;
        if (b->stack->checkers) {
                if (piece_type(pc) != KING) {
                        if (more_than_one(b->stack->checkers))
                                return false;
                        if (!((between_bb(bb_first_sq(b->stack->checkers),
                               get_king_square(b, us)) |
                              b->stack->checkers) &
                            square_bb(to)))
                                return false;
                } else if (attackers_list(b, to, occupancy_bb(b) ^ square_bb(from)) &
                color_bb(b, not_color(us)))
                        return false;
        }
        return true;
}

bool see_greater_than(const Board *b, Move m, Score threshold) {
        if (move_type(m) != NORMAL_MOVE)
                return threshold <= 0;
        const Square from = from_sq(m), to = to_sq(m);
        Score        next = PieceScores[MIDGAME][piece_on(b, to)] - threshold;
        if (next < 0)
                return false;
        next = PieceScores[MIDGAME][piece_on(b, from)] - next;
        if (next <= 0)
                return true;

        Bitboard occ       = occupancy_bb(b) ^ square_bb(from) ^ square_bb(to);
        Bitboard attackers = attackers_list(b, to, occ);
        Color    stm       = piece_color(piece_on(b, from));
        int      result    = 1;

        while (true) {
                stm = not_color(stm);
                attackers &= occ;
                Bitboard mine = attackers & color_bb(b, stm);
                if (!mine)
                        break;
                if (b->stack->pinners[not_color(stm)] & occ) {
                        mine &= ~b->stack->kingBlockers[stm];
                        if (!mine)
                                break;
                }
                result ^= 1;

                Bitboard  bb = 0;
                PieceType pt = PAWN;
                for (; pt < KING; ++pt)
                        if ((bb = mine & piecetype_bb(b, pt)))
                                break;
                if (pt == KING)
                        return (attackers & ~color_bb(b, stm)) ? result ^ 1 : result;
                if ((next = PieceScores[MIDGAME][pt] - next) < result)
                        break;

                occ ^= square_bb(bb_first_sq(bb));
                if (pt != KNIGHT && pt != ROOK)
                        attackers |= bishop_moves_bb(to, occ) &
                        piecetypes_bb(b, BISHOP, QUEEN);
                if (pt >= ROOK)
                        attackers |= rook_moves_bb(to, occ) &
                        piecetypes_bb(b, ROOK, QUEEN);
        }

        return result;
}