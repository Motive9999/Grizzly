#include "defs.h"
#include <string.h>

INLINED ExtendedMove *create_promotions(ExtendedMove *ml, Square to, Direction dir) {
        (ml++)->move = create_promotion(to - dir, to, QUEEN);
        (ml++)->move = create_promotion(to - dir, to, ROOK);
        (ml++)->move = create_promotion(to - dir, to, BISHOP);
        (ml++)->move = create_promotion(to - dir, to, KNIGHT);
        return ml;
}

INLINED ExtendedMove *create_underpromotions(ExtendedMove *ml, Square to, Direction dir) {
        (ml++)->move = create_promotion(to - dir, to, ROOK);
        (ml++)->move = create_promotion(to - dir, to, BISHOP);
        (ml++)->move = create_promotion(to - dir, to, KNIGHT);
        return ml;
}

void place_top_move(ExtendedMove *begin, ExtendedMove *end) {
        ExtendedMove tmp, *top = begin;
        for (ExtendedMove *i = begin + 1; i < end; ++i)
                if (i->score > top->score)
                        top = i;
        tmp    = *top;
        *top   = *begin;
        *begin = tmp;
}

ExtendedMove *generate_piece_moves(ExtendedMove *restrict ml,
    const Board *restrict b,
    Color     us,
    PieceType pt,
    Bitboard  target) {
        Bitboard bb  = piece_bb(b, us, pt);
        Bitboard occ = occupancy_bb(b);
        while (bb) {
                Square   from  = bb_pop_first_sq(&bb);
                Bitboard moves = piece_moves(pt, from, occ) & target;
                while (moves)
                        (ml++)->move = create_move(from, bb_pop_first_sq(&moves));
        }
        return ml;
}

ExtendedMove *generate_pawn_capture_moves(ExtendedMove *restrict ml,
    const Board *restrict b,
    Color    us,
    Bitboard theirPcs,
    bool     inQs) {
        int      push        = pawn_direction(us);
        Bitboard pawnsOnLast = piece_bb(b, us, PAWN) &
            (us == WHITE ? RANK_7_BB : RANK_2_BB);
        Bitboard pawnsNotOnLast = piece_bb(b, us, PAWN) & ~pawnsOnLast;
        Bitboard empty          = ~occupancy_bb(b);

        if (pawnsOnLast) {
                Bitboard promote = relative_shift_up(pawnsOnLast, us);
                for (Bitboard bb = promote & empty; bb;) {
                        Square to    = bb_pop_first_sq(&bb);
                        (ml++)->move = create_promotion(to - push, to, QUEEN);
                        if (!inQs)
                                ml = create_underpromotions(ml, to, push);
                }
                for (Bitboard bb = shift_left(promote) & theirPcs; bb;) {
                        Square to    = bb_pop_first_sq(&bb);
                        (ml++)->move = create_promotion(to - push - WEST, to, QUEEN);
                        if (!inQs)
                                ml = create_underpromotions(ml, to, push + WEST);
                }
                for (Bitboard bb = shift_right(promote) & theirPcs; bb;) {
                        Square to    = bb_pop_first_sq(&bb);
                        (ml++)->move = create_promotion(to - push - EAST, to, QUEEN);
                        if (!inQs)
                                ml = create_underpromotions(ml, to, push + EAST);
                }
        }

        Bitboard capture = relative_shift_up(pawnsNotOnLast, us);
        for (Bitboard bb = shift_left(capture) & theirPcs; bb;) {
                Square to    = bb_pop_first_sq(&bb);
                (ml++)->move = create_move(to - push - WEST, to);
        }
        for (Bitboard bb = shift_right(capture) & theirPcs; bb;) {
                Square to    = bb_pop_first_sq(&bb);
                (ml++)->move = create_move(to - push - EAST, to);
        }

        if (b->stack->enPassantSquare != SQ_NONE) {
                Bitboard capEP = pawnsNotOnLast &
                    pawn_moves(b->stack->enPassantSquare, not_color(us));
                while (capEP)
                        (ml++)->move = create_en_passant(bb_pop_first_sq(&capEP),
                            b->stack->enPassantSquare);
        }
        return ml;
}

ExtendedMove *
generate_captures(ExtendedMove *restrict ml, const Board *restrict b, bool inQs) {
        Color    us     = b->sideToMove;
        Bitboard target = color_bb(b, not_color(us));
        Square   kingSq = get_king_square(b, us);
        ml              = generate_pawn_capture_moves(ml, b, us, target, inQs);
        for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
                ml = generate_piece_moves(ml, b, us, pt, target);
        for (Bitboard bb = king_moves(kingSq) & target; bb;)
                (ml++)->move = create_move(kingSq, bb_pop_first_sq(&bb));
        return ml;
}

ExtendedMove *
generate_quiet_pawn_moves(ExtendedMove *restrict ml, const Board *restrict b, Color us) {
        int      push           = pawn_direction(us);
        Bitboard pawnsNotOnLast = piece_bb(b, us, PAWN) &
            ~(us == WHITE ? RANK_7_BB : RANK_2_BB);
        Bitboard empty = ~occupancy_bb(b);
        Bitboard push1 = relative_shift_up(pawnsNotOnLast, us) & empty;
        Bitboard push2 = relative_shift_up(push1 & (us == WHITE ? RANK_3_BB : RANK_6_BB),
                             us) &
            empty;
        while (push1) {
                Square to    = bb_pop_first_sq(&push1);
                (ml++)->move = create_move(to - push, to);
        }
        while (push2) {
                Square to    = bb_pop_first_sq(&push2);
                (ml++)->move = create_move(to - push - push, to);
        }
        return ml;
}

ExtendedMove *generate_quiet(ExtendedMove *restrict ml, const Board *restrict b) {
        Color    us     = b->sideToMove;
        Bitboard target = ~occupancy_bb(b);
        ml              = generate_quiet_pawn_moves(ml, b, us);
        for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
                ml = generate_piece_moves(ml, b, us, pt, target);
        Square   kingSq = get_king_square(b, us);
        Bitboard bb     = king_moves(kingSq) & target;
        while (bb)
                (ml++)->move = create_move(kingSq, bb_pop_first_sq(&bb));
        int ks = (us == WHITE) ? WHITE_OO : BLACK_OO;
        int qs = (us == WHITE) ? WHITE_OOO : BLACK_OOO;
        if (!castling_blocked(b, ks) && (b->stack->castlings & ks))
                (ml++)->move = create_castling(kingSq, b->castlingRookSquare[ks]);
        if (!castling_blocked(b, qs) && (b->stack->castlings & qs))
                (ml++)->move = create_castling(kingSq, b->castlingRookSquare[qs]);
        return ml;
}

ExtendedMove *generate_classic_pawn_moves(ExtendedMove *restrict ml,
    const Board *restrict b,
    Color us) {
        int      push        = pawn_direction(us);
        Bitboard pawnsOnLast = piece_bb(b, us, PAWN) &
            (us == WHITE ? RANK_7_BB : RANK_2_BB);
        Bitboard pawnsNotOnLast = piece_bb(b, us, PAWN) & ~pawnsOnLast;
        Bitboard empty          = ~occupancy_bb(b);
        Bitboard theirPcs       = color_bb(b, not_color(us));
        Bitboard push1          = relative_shift_up(pawnsNotOnLast, us) & empty;
        Bitboard push2 = relative_shift_up(push1 & (us == WHITE ? RANK_3_BB : RANK_6_BB),
                             us) &
            empty;
        while (push1) {
                Square to    = bb_pop_first_sq(&push1);
                (ml++)->move = create_move(to - push, to);
        }
        while (push2) {
                Square to    = bb_pop_first_sq(&push2);
                (ml++)->move = create_move(to - push - push, to);
        }
        if (pawnsOnLast) {
                Bitboard promote = relative_shift_up(pawnsOnLast, us);
                for (Bitboard bb = promote & empty; bb;)
                        ml = create_promotions(ml, bb_pop_first_sq(&bb), push);
                for (Bitboard bb = shift_left(promote) & theirPcs; bb;)
                        ml = create_promotions(ml, bb_pop_first_sq(&bb), push + WEST);
                for (Bitboard bb = shift_right(promote) & theirPcs; bb;)
                        ml = create_promotions(ml, bb_pop_first_sq(&bb), push + EAST);
        }
        Bitboard capture = relative_shift_up(pawnsNotOnLast, us);
        for (Bitboard bb = shift_left(capture) & theirPcs; bb;) {
                Square to    = bb_pop_first_sq(&bb);
                (ml++)->move = create_move(to - push - WEST, to);
        }
        for (Bitboard bb = shift_right(capture) & theirPcs; bb;) {
                Square to    = bb_pop_first_sq(&bb);
                (ml++)->move = create_move(to - push - EAST, to);
        }
        if (b->stack->enPassantSquare != SQ_NONE) {
                Bitboard capEP = pawnsNotOnLast &
                    pawn_moves(b->stack->enPassantSquare, not_color(us));
                while (capEP)
                        (ml++)->move = create_en_passant(bb_pop_first_sq(&capEP),
                            b->stack->enPassantSquare);
        }
        return ml;
}

ExtendedMove *generate_classic(ExtendedMove *restrict ml, const Board *restrict b) {
        Color    us     = b->sideToMove;
        Bitboard target = ~color_bb(b, us);
        ml              = generate_classic_pawn_moves(ml, b, us);
        for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
                ml = generate_piece_moves(ml, b, us, pt, target);
        Square   kingSq = get_king_square(b, us);
        Bitboard bb     = king_moves(kingSq) & target;
        while (bb)
                (ml++)->move = create_move(kingSq, bb_pop_first_sq(&bb));
        int ks = (us == WHITE) ? WHITE_OO : BLACK_OO;
        int qs = (us == WHITE) ? WHITE_OOO : BLACK_OOO;
        if (!castling_blocked(b, ks) && (b->stack->castlings & ks))
                (ml++)->move = create_castling(kingSq, b->castlingRookSquare[ks]);
        if (!castling_blocked(b, qs) && (b->stack->castlings & qs))
                (ml++)->move = create_castling(kingSq, b->castlingRookSquare[qs]);
        return ml;
}

ExtendedMove *generate_pawn_evasion_moves(ExtendedMove *restrict ml,
    const Board *restrict b,
    Bitboard blockSqs,
    Color    us) {
        int      push        = pawn_direction(us);
        Bitboard pawnsOnLast = piece_bb(b, us, PAWN) &
            (us == WHITE ? RANK_7_BB : RANK_2_BB);
        Bitboard pawnsNotOnLast = piece_bb(b, us, PAWN) & ~pawnsOnLast;
        Bitboard empty          = ~occupancy_bb(b);
        Bitboard theirPcs       = color_bb(b, not_color(us)) & blockSqs;
        Bitboard push1          = relative_shift_up(pawnsNotOnLast, us) & empty;
        Bitboard push2 = relative_shift_up(push1 & (us == WHITE ? RANK_3_BB : RANK_6_BB),
                             us) &
            empty;
        push1 &= blockSqs;
        push2 &= blockSqs;
        while (push1) {
                Square to    = bb_pop_first_sq(&push1);
                (ml++)->move = create_move(to - push, to);
        }
        while (push2) {
                Square to    = bb_pop_first_sq(&push2);
                (ml++)->move = create_move(to - push - push, to);
        }
        if (pawnsOnLast) {
                empty &= blockSqs;
                Bitboard promote = relative_shift_up(pawnsOnLast, us);
                for (Bitboard bb = promote & empty; bb;)
                        ml = create_promotions(ml, bb_pop_first_sq(&bb), push);
                for (Bitboard bb = shift_left(promote) & theirPcs; bb;)
                        ml = create_promotions(ml, bb_pop_first_sq(&bb), push + WEST);
                for (Bitboard bb = shift_right(promote) & theirPcs; bb;)
                        ml = create_promotions(ml, bb_pop_first_sq(&bb), push + EAST);
        }
        Bitboard capture = relative_shift_up(pawnsNotOnLast, us);
        for (Bitboard bb = shift_left(capture) & theirPcs; bb;) {
                Square to    = bb_pop_first_sq(&bb);
                (ml++)->move = create_move(to - push - WEST, to);
        }
        for (Bitboard bb = shift_right(capture) & theirPcs; bb;) {
                Square to    = bb_pop_first_sq(&bb);
                (ml++)->move = create_move(to - push - EAST, to);
        }
        if (b->stack->enPassantSquare != SQ_NONE) {
                if (!(blockSqs & square_bb(b->stack->enPassantSquare - push)))
                        return ml;
                Bitboard capEP = pawnsNotOnLast &
                    pawn_moves(b->stack->enPassantSquare, not_color(us));
                while (capEP)
                        (ml++)->move = create_en_passant(bb_pop_first_sq(&capEP),
                            b->stack->enPassantSquare);
        }
        return ml;
}

ExtendedMove *generate_evasions(ExtendedMove *restrict ml, const Board *restrict b) {
        Color    us         = b->sideToMove;
        Square   kingSq     = get_king_square(b, us);
        Bitboard sliderAtks = 0;
        Bitboard sliders    = b->stack->checkers & ~piecetypes_bb(b, KNIGHT, PAWN);
        while (sliders) {
                Square checkSq = bb_pop_first_sq(&sliders);
                sliderAtks |= LineBB[checkSq][kingSq] ^ square_bb(checkSq);
        }
        Bitboard bb = king_moves(kingSq) & ~color_bb(b, us) & ~sliderAtks;
        while (bb)
                (ml++)->move = create_move(kingSq, bb_pop_first_sq(&bb));
        if (more_than_one(b->stack->checkers))
                return ml;
        Square   checkSq = bb_first_sq(b->stack->checkers);
        Bitboard target  = between_bb(checkSq, kingSq) | square_bb(checkSq);
        ml               = generate_pawn_evasion_moves(ml, b, target, us);
        for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
                ml = generate_piece_moves(ml, b, us, pt, target);
        return ml;
}

ExtendedMove *generate_all(ExtendedMove *restrict ml, const Board *restrict b) {
        Color         us     = b->sideToMove;
        Bitboard      pinned = b->stack->kingBlockers[us] & color_bb(b, us);
        Square        kingSq = get_king_square(b, us);
        ExtendedMove *cur    = ml;
        ml = b->stack->checkers ? generate_evasions(ml, b) : generate_classic(ml, b);
        while (cur < ml) {
                if ((pinned || from_sq(cur->move) == kingSq ||
                        move_type(cur->move) == EN_PASSANT) &&
                    !move_is_legal(b, cur->move))
                        cur->move = (--ml)->move;
                else
                        ++cur;
        }
        return ml;
}