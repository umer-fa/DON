#include "Position.h"

#include "MoveGenerator.h"
#include "Notation.h"
#include "Option.h"
#include "Polyglot.h"
#include "TBsyzygy.h"
#include "Thread.h"

using namespace std;
using namespace BitBoard;
using namespace TBSyzygy;

namespace {
    
    // Marcel van Kervink's cuckoo algorithm for fast detection of "upcoming repetition".
    // Description of the algorithm in the following paper:
    // https://marcelk.net/2013-04-06/paper/upcoming-rep-v2.pdf

    struct Cuckoo
    {
        Key key;    // Zobrist key
        Move move;  // Valid reversible move
    };

    constexpr u16 CuckooSize = 0x2000;
    // Cuckoo table
    Cuckoo Cuckoos[CuckooSize];

    // Hash functions for indexing the cuckoo tables

    inline u16 H1 (Key key) { return u16((key >> 0x00) & (CuckooSize - 1)); }
    inline u16 H2 (Key key) { return u16((key >> 0x10) & (CuckooSize - 1)); }

}


void Position::initialize ()
{
    // Prepare the Cuckoo tables
    u16 count = 0;
    for (auto c : { WHITE, BLACK })
    {
        for (auto pt : { NIHT, BSHP, ROOK, QUEN, KING })
        {
            for (auto s1 : SQ)
            {
                for (auto s2 : SQ)
                {
                    if (   s1 < s2
                        && contains (PieceAttacks[pt][s1], s2))
                    {
                        Cuckoo cuckoo;
                        cuckoo.key = RandZob.piece_square[c][pt][s1]
                                   ^ RandZob.piece_square[c][pt][s2]
                                   ^ RandZob.color;
                        cuckoo.move = mk_move<NORMAL> (s1, s2);

                        u16 i = H1 (cuckoo.key);
                        while (true)
                        {
                            std::swap (Cuckoos[i], cuckoo);
                            // Arrived at empty slot ?
                            if (0 == cuckoo.key)
                            {
                                break;
                            }
                            // Push victim to alternative slot
                            i = i == H1 (cuckoo.key) ?
                                H2 (cuckoo.key) :
                                H1 (cuckoo.key);
                        }

                        ++count;
                    }
                }
            }
        }
    }
    assert(count == 3668);
}

/// Position::draw() checks whether position is drawn by: Clock Ply Rule, Repetition.
/// It does not detect Insufficient materials and Stalemate.
bool Position::draw (i16 pp) const
{
    // Draw by Clock Ply Rule?
    // Not in check or in check have legal moves
    if (   si->clock_ply >= 2*i32(Options["Draw MoveCount"])
        && (   0 == si->checkers
            || 0 != MoveList<GenType::LEGAL> (*this).size ()))
    {
        return true;
    }

    // Draw by Repetition?
    u08 end = std::min (si->clock_ply, si->null_ply);
    if (end < 4)
    {
        return false;
    }
    const auto *psi = si->ptr->ptr;
    bool rep = false;
    for (u08 p = 4; p <= end; p += 2)
    {
        psi = psi->ptr->ptr;
        if (psi->posi_key == si->posi_key)
        {
            // Return a draw score
            // - Repeats once earlier but strictly after the root, or
            // - Repeats twice before or at the root.
            if (   rep
                || pp > p)
            {
                return true;
            }
            rep = true;
        }
    }
    return false;
}

/// Position::cycled() tests if the position has a move which draws by repetition,
/// or an earlier position has a move that directly reaches the current position.
bool Position::cycled (i16 pp) const
{
    u08 end = std::min (si->clock_ply, si->null_ply);
    if (end < 3)
    {
        return false;
    }

    Key original_key = si->posi_key;
    const auto *psi = si->ptr;

    for (u08 p = 3; p <= end; p += 2)
    {
        psi = psi->ptr->ptr;
        Key key = original_key ^ psi->posi_key;

        u16 j;
        if (   (j = H1 (key), key == Cuckoos[j].key)
            || (j = H2 (key), key == Cuckoos[j].key))
        {
            Move move = Cuckoos[j].move;

            if (0 == (between_bb (org_sq (move), dst_sq (move)) & pieces()))
            {
                // Take care to reverse the move in the no-progress case (opponent to move)
                if (empty (org_sq (move)))
                {
                    move = mk_move<NORMAL> (dst_sq (move), org_sq (move));
                }
                assert(empty (dst_sq (move)));

                if (pp > p)
                {
                    return true;
                }
                // For repetitions before or at the root, require one more
                const auto *next_psi = psi;
                for (u08 k = p + 2; k <= end; k += 2)
                {
                    next_psi = next_psi->ptr->ptr;
                    if (next_psi->posi_key == psi->posi_key)
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/// Position::repeated() tests whether there has been at least one repetition of positions since the last capture or pawn move.
bool Position::repeated () const
{
    const auto *csi = si;
    while (nullptr != csi)
    {
        u08 end = std::min (csi->clock_ply, csi->null_ply);
        if (end < 4)
        {
            break;
        }
        const auto *psi = csi->ptr->ptr;
        u08 p = 4;
        do
        {
            psi = psi->ptr->ptr;
            // Check first repetition
            if (psi->posi_key == si->posi_key)
            {
                return true;
            }
            p += 2;
        }
        while (p <= end);
        csi = csi->ptr;
    }
    return false;
}

/// Position::pick_least_val_att() helper function used by see_ge() to locate the least valuable attacker for the side to move,
/// remove the attacker just found from the bitboards and scan for new X-ray attacks behind it.
PieceType Position::pick_least_val_att (PieceType pt, Square dst, Bitboard stm_attackers, Bitboard &mocc, Bitboard &attackers) const
{
    assert(KING > pt);
    Bitboard b = stm_attackers & pieces (pt);
    if (0 != b)
    {
        mocc ^= b & ~(b - 1);

        if (   (   PAWN == pt
                || BSHP == pt
                || QUEN == pt)
            && 0 != (b = mocc & pieces (BSHP, QUEN) & PieceAttacks[BSHP][dst]))
        {
            attackers |= b & attacks_bb<BSHP> (dst, mocc);
        }
        if (   (   ROOK == pt
                || QUEN == pt)
            && 0 != (b = mocc & pieces (ROOK, QUEN) & PieceAttacks[ROOK][dst]))
        {
            attackers |= b & attacks_bb<ROOK> (dst, mocc);
        }
        // Remove already processed pieces in x-ray.
        attackers &= mocc;
        return pt;
    }

    return QUEN > pt ?
            pick_least_val_att (++pt, dst, stm_attackers, mocc, attackers) :
            KING;
}

/// Position::see_ge() Static Exchange Evaluator (SEE): It tries to estimate the material gain or loss resulting from a move.
bool Position::see_ge (Move m, Value threshold) const
{
    assert(_ok (m));

    // Only deal with normal moves, assume others pass a simple see
    if (NORMAL != mtype (m))
    {
        return VALUE_ZERO >= threshold;
    }

    auto org = org_sq (m);
    auto dst = dst_sq (m);

    // The opponent may be able to recapture so this is the best result we can hope for.
    auto balance = PieceValues[MG][ptype (board[dst])] - threshold;
    if (VALUE_ZERO > balance)
    {
        return false;
    }

    auto victim = ptype (board[org]);
    assert(PAWN <= victim && victim <= KING);

    // Now assume the worst possible result: that the opponent can capture our piece for free.
    balance -= PieceValues[MG][victim];
    // If it is enough (like in PxQ) then return immediately.
    // Note that if victim == KING we always return here, this is ok if the given move is legal.
    if (VALUE_ZERO <= balance)
    {
        return true;
    }

    auto own = color (board[org]);
    auto stm = ~own; // First consider opponent's move
    Bitboard mocc = pieces () ^ org ^ dst;
    // Find all attackers to the destination square, with the moving piece
    // removed, but possibly an X-ray attacker added behind it.
    Bitboard attackers = attackers_to (dst, mocc) & mocc;
    while (0 != attackers)
    {
        Bitboard stm_attackers = attackers & pieces (stm);

        // Don't allow pinned pieces for defensive capture,
        // as long all pinners are on their original square.
        if (   0 != stm_attackers
            && 0 != (si->king_checkers[stm] & pieces (~stm) & mocc))
        {
            stm_attackers &= ~si->king_blockers[stm];
        }
        // Only allow king for defensive capture to evade the discovered check,
        // as long all discovered are on their original square.
        if (   0 != stm_attackers
            && 0 != (si->king_checkers[~stm] & pieces (~stm) & mocc & attacks_bb<QUEN> (square<KING> (stm), mocc | dst)))
        {
            stm_attackers &= pieces (KING);
        }

        // If stm has no more attackers then give up: stm loses
        if (0 == stm_attackers)
        {
            break;
        }

        // Locate and remove the next least valuable attacker, and add to
        // the bitboard 'attackers' the possibly X-ray attackers behind it.
        victim = pick_least_val_att (PAWN, dst, stm_attackers, mocc, attackers);

        stm = ~stm;

        // Negamax the balance with alpha = balance, beta = balance+1 and add victim's value.
        //
        //      (balance, balance+1) -> (-balance-1, -balance)
        //
        assert(VALUE_ZERO > balance);

        balance = -balance - 1 - PieceValues[MG][victim];

        // If balance is still non-negative after giving away victim then we win.
        // The only thing to be careful about it is that we should revert stm
        // if we captured with the king when the opponent still has attackers.
        if (VALUE_ZERO <= balance)
        {
            if (   KING == victim
                && 0 != (attackers & pieces (stm)))
            {
                stm = ~stm;
            }
            break;
        }
        assert(KING != victim);
    }
    return own != stm; // We break the above loop when stm loses
}

/// Position::slider_blockers() returns a bitboard of all the pieces that are blocking attacks on the square.
/// King-attack piece can be either pinner or hidden piece.
Bitboard Position::slider_blockers (Square s, Color own, Bitboard ex_attackers, Bitboard &pinners, Bitboard &hiddens) const
{
    // Sliders are attackers that are aligned on square in x-ray.
    Bitboard sliders = (pieces (~own) & ~ex_attackers)
                     & (  (pieces (BSHP, QUEN) & PieceAttacks[BSHP][s])
                        | (pieces (ROOK, QUEN) & PieceAttacks[ROOK][s]));
    Bitboard blockers = 0;
    while (0 != sliders)
    {
        auto slide_sq = pop_lsq (sliders);
        Bitboard b = between_bb (s, slide_sq) & pieces ();
        if (   0 != b
            && !more_than_one (b))
        {
            blockers |= b;
            if (0 != (b & pieces (own)))
            {
                pinners |= slide_sq;
            }
            else
            {
                hiddens |= slide_sq;
            }
        }
    }
    return blockers;
}

/// Position::pseudo_legal() tests whether a random move is pseudo-legal.
/// It is used to validate moves from TT that can be corrupted
/// due to SMP concurrent access or hash position key aliasing.
bool Position::pseudo_legal (Move m) const
{
    assert(_ok (m));

    // If the org square is not occupied by a piece belonging to the side to move,
    // then the move is obviously not legal.
    if (!contains (pieces (active), org_sq (m)))
    {
        return false;
    }

    if (CASTLE == mtype (m))
    {
        auto cs = dst_sq (m) > org_sq (m) ? CS_KING : CS_QUEN;
        // Check whether the destination square is attacked by the opponent.
        // Castling moves are checked for legality during move generation.
        if (   KING != ptype (board[org_sq (m)])
            || R_1 != rel_rank (active, org_sq (m))
            || R_1 != rel_rank (active, dst_sq (m))
            || !contains (pieces (active, ROOK), dst_sq (m))
            || !si->can_castle (active, cs)
            || !expeded_castle (active, cs)
            || 0 != si->checkers)
        {
            return false;
        }
        // Castle is always encoded as "King captures friendly Rook".
        assert(dst_sq (m) == castle_rook_sq[active][cs]);
        Bitboard b = king_path_bb[active][cs];
        // Check king's path for attackers.
        while (0 != b)
        {
            if (0 != attackers_to (pop_lsq (b), ~active))
            {
                return false;
            }
        }
        auto king_dst = rel_sq (active, dst_sq (m) > org_sq (m) ? SQ_G1 : SQ_C1);
        // Chess960
        // For instance an enemy queen in SQ_A1 when castling rook is in SQ_B1.
        if (   0 != (b = pieces (~active, ROOK, QUEN) & rank_bb (rel_rank (active, R_1)))
            && 0 != (b & attacks_bb<ROOK> (king_dst, pieces () ^ dst_sq (m))))
        {
            return false;
        }
        return true; // No capture
    }

    // The captured square cannot be occupied by a friendly piece
    if (contains (pieces (active), ENPASSANT != mtype (m) ? dst_sq (m) : dst_sq (m) - pawn_push (active)))
    {
        return false;
    }

    // Handle the special case of a piece move
    switch (ptype (board[org_sq (m)]))
    {
    case PAWN:
        if (    // Single push
               (   (   (   NORMAL != mtype (m)
                        || R_6 < rel_rank (active, org_sq (m))
                        || R_7 < rel_rank (active, dst_sq (m)))
                    && (   PROMOTE != mtype (m)
                        || R_7 != rel_rank (active, org_sq (m))
                        || R_8 != rel_rank (active, dst_sq (m))))
                || !empty (dst_sq (m))
                || dst_sq (m) != org_sq (m) + pawn_push (active))
                // Normal capture
            && (   (   (   NORMAL != mtype (m)
                        || R_6 < rel_rank (active, org_sq (m))
                        || R_7 < rel_rank (active, dst_sq (m)))
                    && (   PROMOTE != mtype (m)
                        || R_7 != rel_rank (active, org_sq (m))
                        || R_8 != rel_rank (active, dst_sq (m))))
                || !contains (pieces (~active) & PawnAttacks[active][org_sq (m)], dst_sq (m)))
                // Enpassant capture
            && (   ENPASSANT != mtype (m)
                || R_5 != rel_rank (active, org_sq (m))
                || R_6 != rel_rank (active, dst_sq (m))
                || si->enpassant_sq != dst_sq (m)
                || !empty (dst_sq (m))
                || !contains (pieces (~active, PAWN), dst_sq (m) - pawn_push (active)))
                // Double push
            && (   NORMAL != mtype (m)
                || R_2 != rel_rank (active, org_sq (m))
                || R_4 != rel_rank (active, dst_sq (m))
                || !empty (dst_sq (m) - pawn_push (active))
                || !empty (dst_sq (m))
                || dst_sq (m) != org_sq (m) + pawn_push (active) * 2))
        {
            return false;
        }
        break;
    case NIHT:
        if (   NORMAL != mtype (m)
            || !contains (PieceAttacks[NIHT][org_sq (m)], dst_sq (m))) { return false; }
        break;
    case BSHP:
        if (   NORMAL != mtype (m)
            || !contains (PieceAttacks[BSHP][org_sq (m)], dst_sq (m))
            || !contains (attacks_bb<BSHP> (org_sq (m), pieces ()), dst_sq (m))) { return false; }
        break;
    case ROOK:
        if (   NORMAL != mtype (m)
            || !contains (PieceAttacks[ROOK][org_sq (m)], dst_sq (m))
            || !contains (attacks_bb<ROOK> (org_sq (m), pieces ()), dst_sq (m))) { return false; }
        break;
    case QUEN:
        if (   NORMAL != mtype (m)
            || !contains (PieceAttacks[QUEN][org_sq (m)], dst_sq (m))
            || !contains (attacks_bb<QUEN> (org_sq (m), pieces ()), dst_sq (m))) { return false; }
        break;
    case KING:
        if (   NORMAL != mtype (m)
            || !contains (PieceAttacks[KING][org_sq (m)], dst_sq (m))) { return false; }
        break;
    default:
        assert(false);
        break;
    }
    // Evasions generator already takes care to avoid some kind of illegal moves and legal() relies on this.
    // So have to take care that the same kind of moves are filtered out here.
    if (0 != si->checkers)
    {
        // In case of king moves under check, remove king so to catch
        // as invalid moves like B1A1 when opposite queen is on C1.
        if (KING == ptype (board[org_sq (m)]))
        {
            return 0 == attackers_to (dst_sq (m), ~active, pieces () ^ org_sq (m));
        }
        // Double check? In this case a king move is required
        if (!more_than_one (si->checkers))
        {
            return ENPASSANT != mtype (m) ?
                // Move must be a capture of the checking piece or a blocking evasion of the checking piece
                   contains (si->checkers | between_bb (scan_lsq (si->checkers), square<KING> (active)), dst_sq (m)) :
                // Move must be a capture of the checking Enpassant pawn or a blocking evasion of the checking piece
                   (0 != (si->checkers & pieces (~active, PAWN)) && contains (si->checkers, dst_sq (m) - pawn_push (active)))
                || contains (between_bb (scan_lsq (si->checkers), square<KING> (active)), dst_sq (m));
        }
        return false;
    }
    return true;
}
/// Position::legal() tests whether a pseudo-legal move is legal.
bool Position::legal (Move m) const
{
    assert(_ok (m));
    assert(contains (pieces (active), org_sq (m)));
    switch (mtype (m))
    {
    case NORMAL:
        // Only king moves to non attacked squares, sliding check x-rays the king
        // In case of king moves under check have to remove king so to catch
        // as invalid moves like B1-A1 when opposite queen is on SQ_C1.
        // check whether the destination square is attacked by the opponent.
        if (contains (pieces (KING), org_sq (m)))
        {
            return 0 == attackers_to (dst_sq (m), ~active, pieces () ^ org_sq (m));
        }
        // A non-king move is legal if and only if
        // - not pinned
        // - moving along the ray from the king
        return !contains (si->king_blockers[active], org_sq (m))
            || sqrs_aligned (org_sq (m), dst_sq (m), square<KING> (active));
    case PROMOTE:
        assert(PAWN == ptype (board[org_sq (m)])
            && R_7 == rel_rank (active, org_sq (m))
            && R_8 == rel_rank (active, dst_sq (m))
            && NIHT <= promote (m) && promote (m) <= QUEN);
        // A non-king move is legal if and only if
        // - not pinned
        // - moving along the ray from the king
        return !contains (si->king_blockers[active], org_sq (m))
            || sqrs_aligned (org_sq (m), dst_sq (m), square<KING> (active));
    case CASTLE:
        // Castling moves are checked for legality during move generation.
        assert(KING == ptype (board[org_sq (m)])
            && R_1 == rel_rank (active, org_sq (m))
            && R_1 == rel_rank (active, dst_sq (m))
            && contains (pieces (active, KING), org_sq (m))
            && contains (pieces (active, ROOK), dst_sq (m))
            && expeded_castle (active, dst_sq (m) > org_sq (m) ? CS_KING : CS_QUEN));
        return true;
    case ENPASSANT:
    {
        // Enpassant captures are a tricky special case. Because they are rather uncommon,
        // do it simply by testing whether the king is attacked after the move is made.
        assert(contains (pieces (active, PAWN), org_sq (m))
            && R_5 == rel_rank (active, org_sq (m))
            && R_6 == rel_rank (active, dst_sq (m))
            && empty (dst_sq (m))
            && si->enpassant_sq == dst_sq (m)
            && contains (pieces (~active, PAWN), dst_sq (m) - pawn_push (active)));
        Bitboard mocc = (pieces () ^ org_sq (m) ^ (dst_sq (m) - pawn_push (active))) | dst_sq (m);
        // If any attacker then in check and not legal move.
        return (   0 == (pieces (~active, BSHP, QUEN) & PieceAttacks[BSHP][square<KING> (active)])
                || 0 == (pieces (~active, BSHP, QUEN) & attacks_bb<BSHP> (square<KING> (active), mocc)))
            && (   0 == (pieces (~active, ROOK, QUEN) & PieceAttacks[ROOK][square<KING> (active)])
                || 0 == (pieces (~active, ROOK, QUEN) & attacks_bb<ROOK> (square<KING> (active), mocc)));
    }
    default:
        assert(false);
        return false;
    }
}
/// Position::gives_check() tests whether a pseudo-legal move gives a check.
bool Position::gives_check (Move m) const
{
    assert(_ok (m));
    assert(contains (pieces (active), org_sq (m)));

    if (    // Direct check ?
           contains (si->checks[ptype (board[org_sq (m)])], dst_sq (m))
            // Discovered check ?
        || (   contains (si->king_blockers[~active], org_sq (m))
            && !sqrs_aligned (org_sq (m), dst_sq (m), square<KING> (~active))))
    {
        return true;
    }

    switch (mtype (m))
    {
    case NORMAL:
        return false;
    case CASTLE:
    {
        assert(KING == ptype (board[org_sq (m)])
            && R_1 == rel_rank (active, org_sq (m))
            && R_1 == rel_rank (active, dst_sq (m))
            && contains (pieces (active, KING), org_sq (m))
            && contains (pieces (active, ROOK), dst_sq (m))
            && expeded_castle (active, dst_sq (m) > org_sq (m) ? CS_KING : CS_QUEN));
        // Castling with check?
        auto king_dst = rel_sq (active, dst_sq (m) > org_sq (m) ? SQ_G1 : SQ_C1);
        auto rook_dst = rel_sq (active, dst_sq (m) > org_sq (m) ? SQ_F1 : SQ_D1);
        return contains (PieceAttacks[ROOK][rook_dst], square<KING> (~active))
            && contains (attacks_bb<ROOK> (rook_dst, (pieces () ^ org_sq (m) ^ dst_sq (m)) | king_dst | rook_dst), square<KING> (~active));
    }
    case ENPASSANT:
    {
        assert(PAWN == ptype (board[org_sq (m)])
            && R_5 == rel_rank (active, org_sq (m))
            && R_6 == rel_rank (active, dst_sq (m))
            && empty (dst_sq (m))
            && 1 >= si->clock_ply);
        // Enpassant capture with check?
        // already handled the case of direct checks and ordinary discovered check,
        // the only case need to handle is the unusual case of a discovered check through the captured pawn.
        Bitboard mocc = (pieces () ^ org_sq (m) ^ (_file (dst_sq (m))|_rank (org_sq (m)))) | dst_sq (m);
        return (   0 != (pieces (active, BSHP, QUEN) & PieceAttacks[BSHP][square<KING> (~active)])
                && 0 != (pieces (active, BSHP, QUEN) & attacks_bb<BSHP> (square<KING> (~active), mocc)))
            || (   0 != (pieces (active, ROOK, QUEN) & PieceAttacks[ROOK][square<KING> (~active)])
                && 0 != (pieces (active, ROOK, QUEN) & attacks_bb<ROOK> (square<KING> (~active), mocc)));
    }
    case PROMOTE:
    {
        assert(PAWN == ptype (board[org_sq (m)])
            && R_7 == rel_rank (active, org_sq (m))
            && R_8 == rel_rank (active, dst_sq (m))
            && NIHT <= promote (m) && promote (m) <= QUEN);
        // Promotion with check?
        return NIHT == promote (m) ? contains (PieceAttacks[NIHT][dst_sq (m)], square<KING> (~active)) :
               BSHP == promote (m) ? contains (PieceAttacks[BSHP][dst_sq (m)], square<KING> (~active))
                                  && contains (attacks_bb<BSHP> (dst_sq (m), pieces () ^ org_sq (m)), square<KING> (~active)) :
               ROOK == promote (m) ? contains (PieceAttacks[ROOK][dst_sq (m)], square<KING> (~active))
                                  && contains (attacks_bb<ROOK> (dst_sq (m), pieces () ^ org_sq (m)), square<KING> (~active)) :
               QUEN == promote (m) ? contains (PieceAttacks[QUEN][dst_sq (m)], square<KING> (~active))
                                  && contains (attacks_bb<QUEN> (dst_sq (m), pieces () ^ org_sq (m)), square<KING> (~active)) : (assert(false), false);
    }
    default:
        assert(false);
        return false;
    }
}
/// Position::clear() clear the position.
void Position::clear ()
{
    for (auto s : SQ)
    {
        board[s] = NO_PIECE;
        castle_mask[s] = CR_NONE;
    }
    for (auto pt : { PAWN, NIHT, BSHP, ROOK, QUEN, KING, NONE })
    {
        types_bb[pt] = 0;
    }
    for (auto c : { WHITE, BLACK })
    {
        color_bb[c] = 0;
        for (auto pt : { PAWN, NIHT, BSHP, ROOK, QUEN, KING })
        {
            squares[c][pt].clear ();
        }
        for (auto cs : { CS_KING, CS_QUEN })
        {
            castle_rook_sq[c][cs] = SQ_NO;
            castle_path_bb[c][cs] = 0;
            king_path_bb  [c][cs] = 0;
        }
    }
    psq = SCORE_ZERO;
}
/// Position::set_castle() set the castling right.
void Position::set_castle (Color c, Square rook_org)
{
    auto king_org = square<KING> (c);
    assert(R_1 == rel_rank (c, king_org));
    auto cs = rook_org > king_org ? CS_KING : CS_QUEN;
    assert(contains (pieces (c, ROOK), rook_org)
        && R_1 == rel_rank (c, rook_org));
    castle_rook_sq[c][cs] = rook_org;

    auto cr = castle_right (c, cs);
    auto king_dst = rel_sq (c, rook_org > king_org ? SQ_G1 : SQ_C1);
    auto rook_dst = rel_sq (c, rook_org > king_org ? SQ_F1 : SQ_D1);
    si->castle_rights     |= cr;
    castle_mask[king_org] |= cr;
    castle_mask[rook_org] |= cr;

    for (auto s = std::min (king_org, king_dst); s <= std::max (king_org, king_dst); ++s)
    {
        if (s != king_org)
        {
            king_path_bb[c][cs] |= s;
        }
        if (   s != king_org
            && s != rook_org)
        {
            castle_path_bb[c][cs] |= s;
        }
    }
    for (auto s = std::min (rook_org, rook_dst); s <= std::max (rook_org, rook_dst); ++s)
    {
        if (   s != king_org
            && s != rook_org)
        {
            castle_path_bb[c][cs] |= s;
        }
    }
}
/// Position::can_enpassant() Can the enpassant possible.
bool Position::can_enpassant (Color c, Square ep_sq, bool move_done) const
{
    assert(SQ_NO != ep_sq);
    assert(R_6 == rel_rank (c, ep_sq));
    auto cap = move_done ?
                ep_sq - pawn_push (c) :
                ep_sq + pawn_push (c);
    if (!contains (pieces (~c, PAWN), cap))
    {
        return false;
    }

    // Enpassant attackers
    Bitboard attackers = pieces (c, PAWN) & PawnAttacks[~c][ep_sq];
    assert(2 >= pop_count (attackers));
    Bitboard mocc = (pieces () ^ cap) | ep_sq;
    Bitboard bq = pieces (~c, BSHP, QUEN) & PieceAttacks[BSHP][square<KING> (c)];
    Bitboard rq = pieces (~c, ROOK, QUEN) & PieceAttacks[ROOK][square<KING> (c)];
    if (   0 != attackers
        && 0 == bq
        && 0 == rq)
    {
        return true;
    }
    while (0 != attackers)
    {
        auto org = pop_lsq (attackers);
        assert(contains (mocc, org));
        // Check enpassant is legal for the position
        if (   0 == (bq & attacks_bb<BSHP> (square<KING> (c), mocc ^ org))
            && 0 == (rq & attacks_bb<ROOK> (square<KING> (c), mocc ^ org)))
        {
            return true;
        }
    }
    return false;
}

/// Position::setup() initializes the position object with the given FEN string.
/// This function is not very robust - make sure that input FENs are correct,
/// this is assumed to be the responsibility of the GUI.
Position& Position::setup (const string &ff, StateInfo &nsi, Thread *const th, bool full)
{
    // A FEN string defines a particular position using only the ASCII character set.
    // A FEN string contains six fields separated by a space.
    // 1) Piece placement (from White's perspective).
    //    Each rank is described, starting with rank 8 and ending with rank 1;
    //    within each rank, the contents of each square are described from file A through file H.
    //    Following the Standard Algebraic Notation (SAN),
    //    each piece is identified by a single letter taken from the standard English names.
    //    White pieces are designated using upper-case letters ("PNBRQK") while
    //    Black pieces are designated using lower-case letters ("pnbrqk").
    //    Blank squares are noted using digits 1 through 8 (the number of blank squares),
    //    and "/" separates ranks.
    // 2) Active color. "w" means white, "b" means black - moves next.
    // 3) Castling availability. If neither side can castle, this is "-". 
    //    Otherwise, this has one or more letters:
    //    "K" (White can castle  King side).
    //    "Q" (White can castle Queen side).
    //    "k" (Black can castle  King side).
    //    "q" (Black can castle Queen side).
    //    In Chess 960 file "a-h" is used.
    // 4) Enpassant target square (in algebraic notation).
    //    If there's no enpassant target square, this is "-".
    //    If a pawn has just made a 2-square move, this is the position "behind" the pawn.
    //    This is recorded only if there really is a pawn that might have advanced two squares
    //    and if there is a pawn in position to make an enpassant capture legally!!!. 
    // 5) Half move clock. This is the number of half moves since the last pawn advance or capture.
    //    This is used to determine if a draw can be claimed under the fifty-move rule.
    // 6) Full move number. The number of the full move.
    //    It starts at 1, and is incremented after Black's move.

    istringstream iss (ff);
    iss >> std::noskipws;

    clear ();
    si = &nsi;

    u08 token;
    // 1. Piece placement on Board
    size_t idx;
    i08 f = F_A;
    i08 r = R_8;
    while (   iss >> token
           && F_NO >= f
           && R_1 <= r)
    {
        if (isdigit (token))
        {
            f += token - '0';
        }
        else
        if (   isalpha (token)
            && (idx = PieceChar.find (token)) != string::npos)
        {
            place_piece_on (File(f)|Rank(r), Piece(idx));
            ++f;
        }
        else
        if (isspace (token))
        {
            break;
        }
        else
        {
            assert(token == '/');
            f = F_A;
            --r;
        }
    }
    assert(SQ_NO != square<KING> (WHITE)
        && SQ_NO != square<KING> (BLACK));

    // 2. Active color
    iss >> token;
    active = Color(ColorChar.find (token));

    si->castle_rights = CR_NONE;
    // 3. Castling availability
    iss >> token;
    while (   iss >> token
           && !isspace (token))
    {
        Square rook_org;
        Color c = isupper (token) ? WHITE : BLACK;
        token = char(tolower (token));
        if ('k' == token)
        {
            assert(R_1 == rel_rank (c, square<KING> (c)));
            for (rook_org = rel_sq (c, SQ_H1); rook_org >= rel_sq (c, SQ_A1); --rook_org)
            {
                assert(!contains (pieces (c, KING), rook_org));
                if (contains (pieces (c, ROOK), rook_org))
                {
                    break;
                }
            }
            assert(contains (pieces (c, ROOK), rook_org)
                && rook_org > square<KING> (c));
        }
        else
        if ('q' == token)
        {
            assert(R_1 == rel_rank (c, square<KING> (c)));
            for (rook_org = rel_sq (c, SQ_A1); rook_org <= rel_sq (c, SQ_H1); ++rook_org)
            {
                assert(!contains (pieces (c, KING), rook_org));
                if (contains (pieces (c, ROOK), rook_org))
                {
                    break;
                }
            }
            assert(contains (pieces (c, ROOK), rook_org)
                && rook_org < square<KING> (c));
        }
        else
        // Chess960
        if ('a' <= token && token <= 'h')
        {
            assert(R_1 == rel_rank (c, square<KING> (c)));
            rook_org = to_file (token)|_rank (square<KING> (c));
        }
        else
        {
            assert('-' == token);
            continue;
        }
        set_castle (c, rook_org);
    }

    // 4. Enpassant square. Ignore if no pawn capture is possible.
    si->enpassant_sq = SQ_NO;
    u08 file, rank;
    if (   (iss >> file && ('a' <= file && file <= 'h'))
        && (iss >> rank && ('3' == rank || rank == '6')))
    {
        auto ep_sq = to_square (file, rank);
        if (can_enpassant (active, ep_sq))
        {
            si->enpassant_sq = ep_sq;
        }
    }

    // 5-6. Half move clock and Full move number.
    i16 clock_ply = 0
      , moves = 1;
    if (full)
    {
        iss >> std::skipws
            >> clock_ply
            >> moves;

        if (SQ_NO != si->enpassant_sq)
        {
            clock_ply = 0;
        }
        // Rule 50 draw case.
        assert(100 >= clock_ply);

        // Handle common problem Full move number = 0.
        if (0 >= moves)
        {
            moves = 1;
        }
    }

    // Convert from moves starting from 1 to ply starting from 0.
    ply = i16(2*(moves - 1) + (BLACK == active ? 1 : 0));

    si->posi_key = RandZob.compute_posi_key (*this);
    si->matl_key = RandZob.compute_matl_key (*this);
    si->pawn_key = RandZob.compute_pawn_key (*this);
    si->npm[WHITE] = compute_npm<WHITE> (*this);
    si->npm[BLACK] = compute_npm<BLACK> (*this);
    si->clock_ply = u08(clock_ply);
    si->null_ply = 0;
    si->capture = NONE;
    si->promotion = false;
    si->checkers = attackers_to (square<KING> (active), ~active);
    si->set_check_info (*this);
    thread = th;
    return *this;
}
/// Position::setup() initializes the position object with the given endgame code string like "KBPKN".
/// It is mainly an helper to get the material key out of an endgame code.
Position& Position::setup (const string &code, Color c, StateInfo &nsi)
{
    assert(0 < code.length () && code.length () <= 8);
    assert(code[0] == 'K');
    assert(code.find ('K', 1) != string::npos);

    string sides[CLR_NO] =
    {
        code.substr (   code.find ('K', 1)), // Weak
        code.substr (0, code.find ('K', 1))  // Strong
    };
    assert(sides[WHITE].length () <= 8
        && sides[BLACK].length () <= 8);

    to_lower (sides[c]);
    string fen = "8/" + sides[WHITE] + char('0' + 8 - sides[WHITE].length ()) + "/8/8/8/8/"
                      + sides[BLACK] + char('0' + 8 - sides[BLACK].length ()) + "/8 w - -";

    setup (fen, nsi, nullptr, false);

    return *this;
}

/// Position::do_move() makes a move, and saves all information necessary to a StateInfo object.
/// The move is assumed to be legal.
void Position::do_move (Move m, StateInfo &nsi, bool is_check)
{
    assert(_ok (m));
    assert(&nsi != si);

    thread->nodes.fetch_add (1, std::memory_order::memory_order_relaxed);
    // Copy some fields of old state info to new state info object
    std::memcpy (&nsi, si, offsetof(StateInfo, capture));
    nsi.ptr = si;
    si = &nsi;

    auto org = org_sq (m);
    auto dst = dst_sq (m);
    assert(contains (pieces (active), org)
        && (!contains (pieces (active), dst)
         || CASTLE == mtype (m)));

    auto pasive = ~active;
    auto mpt = ptype (board[org]);
    assert(NONE != mpt);
    auto ppt = mpt;
    auto cap = ENPASSANT != mtype (m) ?
                dst :
                dst - pawn_push (active);
    ++ply;
    ++si->clock_ply;
    ++si->null_ply;

    si->capture = CASTLE != mtype (m) ?
                    ptype (board[cap]) :
                    NONE;
    assert(KING != si->capture);

    if (NONE != si->capture)
    {
        assert(capture (m));
        remove_piece_on (cap);
        if (PAWN == si->capture)
        {
            si->pawn_key ^= RandZob.piece_square[pasive][PAWN][cap];
            prefetch (thread->pawn_table[si->pawn_key]);
        }
        else
        {
            si->npm[pasive] -= PieceValues[MG][si->capture];
        }
        si->matl_key ^= RandZob.piece_square[pasive][si->capture][count (pasive, si->capture)];
        prefetch (thread->matl_table[si->matl_key]);

        si->posi_key ^= RandZob.piece_square[pasive][si->capture][cap];
        si->clock_ply = 0;
    }
    // Reset enpassant square
    if (SQ_NO != si->enpassant_sq)
    {
        assert(1 >= si->clock_ply);
        si->posi_key ^= RandZob.enpassant[_file (si->enpassant_sq)];
        si->enpassant_sq = SQ_NO;
    }

    switch (mtype (m))
    {
    case NORMAL:
        si->promotion = false;
        move_piece_on_to (org, dst);
        if (PAWN == mpt)
        {
            si->pawn_key ^= RandZob.piece_square[active][PAWN][dst]
                          ^ RandZob.piece_square[active][PAWN][org];
            prefetch (thread->pawn_table[si->pawn_key]);
            // Double push pawn
            if (16 == (u08(dst) ^ u08(org)))
            {
                // Set enpassant square if the moved pawn can be captured
                auto ep_sq = org + (dst - org) / 2;
                if (can_enpassant (pasive, ep_sq))
                {
                    si->enpassant_sq = ep_sq;
                    si->posi_key ^= RandZob.enpassant[_file (ep_sq)];
                }
            }
            si->clock_ply = 0;
        }
        break;
    case CASTLE:
    {
        assert(KING == mpt
            && R_1 == rel_rank (active, org)
            && R_1 == rel_rank (active, dst)
            && contains (pieces (active, KING), org)
            && contains (pieces (active, ROOK), dst)
            && expeded_castle (active, dst > org ? CS_KING : CS_QUEN));

        si->promotion = false;
        Square rook_org, rook_dst;
        do_castling (org, dst, rook_org, rook_dst);
        si->posi_key ^= RandZob.piece_square[active][ROOK][rook_dst]
                      ^ RandZob.piece_square[active][ROOK][rook_org];
    }
        break;
    case ENPASSANT:
        // NOTE:: some condition already set so may not work
        assert(PAWN == mpt
            && R_5 == rel_rank (active, org)
            && R_6 == rel_rank (active, dst)
            && empty (dst)
            && 1 >= si->clock_ply);

        board[cap] = NO_PIECE; // Not done by remove_piece_on()
        si->clock_ply = 0;
        si->promotion = false;
        move_piece_on_to (org, dst);
        si->pawn_key ^= RandZob.piece_square[active][PAWN][dst]
                      ^ RandZob.piece_square[active][PAWN][org];
        prefetch (thread->pawn_table[si->pawn_key]);
        break;
    case PROMOTE:
        assert(PAWN == mpt
            && R_7 == rel_rank (active, org)
            && R_8 == rel_rank (active, dst)
            && NIHT <= promote (m) && promote (m) <= QUEN);

        ppt = promote (m);
        si->clock_ply = 0;
        si->promotion = true;
        // Replace the pawn with the promoted piece
        remove_piece_on (org);
        board[org] = NO_PIECE; // Not done by remove_piece_on()
        place_piece_on (dst, active|ppt);
        si->matl_key ^= RandZob.piece_square[active][PAWN][count (active, mpt)]
                      ^ RandZob.piece_square[active][ppt][count (active, ppt) - 1];
        prefetch (thread->matl_table[si->matl_key]);

        si->pawn_key ^= RandZob.piece_square[active][PAWN][org];
        prefetch (thread->pawn_table[si->pawn_key]);
        si->npm[active] += PieceValues[MG][ppt];
        break;
    default:
        assert(false);
    }
    si->posi_key ^= RandZob.piece_square[active][ppt][dst]
                  ^ RandZob.piece_square[active][mpt][org];

    // Update castling rights
    auto b = si->castle_rights & (castle_mask[org]|castle_mask[dst]);
    if (CR_NONE != b)
    {
        if (CR_NONE != (b & CR_WKING)) si->posi_key ^= RandZob.castle_right[WHITE][CS_KING];
        if (CR_NONE != (b & CR_WQUEN)) si->posi_key ^= RandZob.castle_right[WHITE][CS_QUEN];
        if (CR_NONE != (b & CR_BKING)) si->posi_key ^= RandZob.castle_right[BLACK][CS_KING];
        if (CR_NONE != (b & CR_BQUEN)) si->posi_key ^= RandZob.castle_right[BLACK][CS_QUEN];
        si->castle_rights &= ~b;
    }

    assert(0 == attackers_to (square<KING> (active), pasive));

    // Calculate checkers
    si->checkers = is_check ? attackers_to (square<KING> (pasive), active) : 0;
    assert(!is_check
        || 0 != si->checkers);

    // Switch sides
    active = pasive;
    si->posi_key ^= RandZob.color;

    si->set_check_info (*this);

    assert(ok ());
}
/// Position::undo_move() unmakes a move, and restores the position to exactly the same state as before the move was made.
/// The move is assumed to be legal.
void Position::undo_move (Move m)
{
    assert(_ok (m));
    assert(nullptr != si->ptr
        && KING != si->capture);

    auto org = org_sq (m);
    auto dst = dst_sq (m);
    assert(empty (org)
        || CASTLE == mtype (m));

    active = ~active;

    switch (mtype (m))
    {
    case CASTLE:
    {
        assert(R_1 == rel_rank (active, org)
            && R_1 == rel_rank (active, dst)
            && NONE == si->capture);

        Square rook_org, rook_dst;
        undo_castling (org, dst, rook_org, rook_dst);
    }
        break;
    case ENPASSANT:
        assert(R_5 == rel_rank (active, org)
            && R_6 == rel_rank (active, dst)
            && dst == si->ptr->enpassant_sq
            && PAWN == si->capture
            && empty (dst - pawn_push (active))
            && contains (pieces (active, PAWN), dst));
    // Note:: No break
    case NORMAL:
        move_piece_on_to (dst, org);
        break;
    case PROMOTE:
        assert(R_7 == rel_rank (active, org)
            && R_8 == rel_rank (active, dst)
            && si->promotion
            && contains (pieces (active, promote (m)), dst));

        remove_piece_on (dst);
        board[dst] = NO_PIECE; // Not done by remove_piece_on()
        place_piece_on (org, active|PAWN);
        break;
    default:
        assert(false);
    }

    if (NONE != si->capture)
    {
        // Restore the captured piece.
        assert(empty (ENPASSANT != mtype (m) ? dst : dst - pawn_push (active)));
        place_piece_on (ENPASSANT != mtype (m) ? dst : dst - pawn_push (active), (~active)|si->capture);
    }

    // Point state pointer back to the previous state.
    si = si->ptr;
    --ply;

    assert(ok ());
}
/// Position::do_null_move() makes a 'null move'.
/// It flips the side to move without executing any move on the board.
void Position::do_null_move (StateInfo &nsi)
{
    assert(&nsi != si
        && 0 == si->checkers);

    std::memcpy (&nsi, si, sizeof (nsi));
    nsi.ptr = si;
    si = &nsi;
    // Reset enpassant square.
    if (SQ_NO != si->enpassant_sq)
    {
        si->posi_key ^= RandZob.enpassant[_file (si->enpassant_sq)];
        si->enpassant_sq = SQ_NO;
    }
    ++si->clock_ply;
    si->null_ply = 0;
    si->capture = NONE;
    assert(0 == si->checkers);

    si->posi_key ^= RandZob.color;
    active = ~active;

    si->set_check_info (*this);

    assert(ok ());
}
/// Position::undo_null_move() unmakes a 'null move'.
void Position::undo_null_move ()
{
    assert(nullptr != si->ptr
        && NONE == si->capture
        && 0 == si->checkers);

    active = ~active;
    si = si->ptr;

    assert(ok ());
}

/// Position::flip() flips position (White and Black sides swaped).
/// This is only useful for debugging especially for finding evaluation symmetry bugs.
void Position::flip ()
{
    istringstream iss (fen ());
    string ff, token;
    // 1. Piece placement
    for (auto r : { R_8, R_7, R_6, R_5, R_4, R_3, R_2, R_1 })
    {
        std::getline (iss, token, r > R_1 ? '/' : ' ');
        toggle (token);
        token += r < R_8 ? "/" : " ";
        ff = token + ff;
    }
    // 2. Active color
    iss >> token;
    ff += (token == "w" ? "b" : "w");
    ff += " ";
    // 3. Castling availability
    iss >> token;
    if (token != "-")
    {
        toggle (token);
    }
    ff += token;
    ff += " ";
    // 4. Enpassant square
    iss >> token;
    if (token != "-")
    {
        token.replace (1, 1, string(1, to_char (~to_rank (token[1]))));
    }
    ff += token;
    // 5-6. Halfmove clock and Fullmove number
    std::getline (iss, token, '\n');
    ff += token;

    setup (ff, *si, thread);

    assert(ok ());
}
/// Position::mirror() mirrors position (King and Queen sides swaped).
void Position::mirror ()
{
    istringstream iss (fen ());
    string ff, token;
    // 1. Piece placement
    for (auto r : { R_8, R_7, R_6, R_5, R_4, R_3, R_2, R_1 })
    {
        std::getline (iss, token, r > R_1 ? '/' : ' ');
        std::reverse (token.begin (), token.end ());
        token += r > R_1 ? "/" : " ";
        ff = ff + token;
    }
    // 2. Active color
    iss >> token;
    ff += token;
    ff += " ";
    // 3. Castling availability
    iss >> token;
    if (token != "-")
    {
        for (auto &ch : token)
        {
            if (bool(Options["UCI_Chess960"]))
            {
                assert(isalpha (ch));
                ch = to_char (~to_file (char(tolower (ch))), islower (ch));
            }
            else
            {
                switch (ch)
                {
                case 'K': ch = 'Q'; break;
                case 'Q': ch = 'K'; break;
                case 'k': ch = 'q'; break;
                case 'q': ch = 'k'; break;
                default: assert(false);
                }
            }
        }
    }
    ff += token;
    ff += " ";
    // 4. Enpassant square
    iss >> token;
    if (token != "-")
    {
        token.replace (0, 1, string(1, to_char (~to_file (token[0]))));
    }
    ff += token;
    // 5-6. Halfmove clock and Fullmove number
    std::getline (iss, token, '\n');
    ff += token;

    setup (ff, *si, thread);

    assert(ok ());
}

/// Position::fen() returns a FEN representation of the position.
/// In case of Chess960 the Shredder-FEN notation is used.
string Position::fen (bool full) const
{
    ostringstream oss;

    for (auto r : { R_8, R_7, R_6, R_5, R_4, R_3, R_2, R_1 })
    {
        for (auto f = F_A; f <= F_H; ++f)
        {
            i16 empty_count;
            for (empty_count = 0; f <= F_H && empty (f|r); ++f)
            {
                ++empty_count;
            }
            if (0 != empty_count)
            {
                oss << empty_count;
            }
            if (f <= F_H)
            {
                oss << board[f|r];
            }
        }
        if (r > R_1)
        {
            oss << '/';
        }
    }

    oss << " " << active << " ";

    if (si->can_castle (CR_ANY))
    {
        if (si->can_castle (CR_WKING)) oss << (bool(Options["UCI_Chess960"]) ? to_char (_file (castle_rook_sq[WHITE][CS_KING]), false) : 'K');
        if (si->can_castle (CR_WQUEN)) oss << (bool(Options["UCI_Chess960"]) ? to_char (_file (castle_rook_sq[WHITE][CS_QUEN]), false) : 'Q');
        if (si->can_castle (CR_BKING)) oss << (bool(Options["UCI_Chess960"]) ? to_char (_file (castle_rook_sq[BLACK][CS_KING]),  true) : 'k');
        if (si->can_castle (CR_BQUEN)) oss << (bool(Options["UCI_Chess960"]) ? to_char (_file (castle_rook_sq[BLACK][CS_QUEN]),  true) : 'q');
    }
    else
    {
        oss << "-";
    }

    oss << " " << (SQ_NO != si->enpassant_sq ? to_string (si->enpassant_sq) : "-");

    if (full)
    {
        oss << " " << i16(si->clock_ply) << " " << move_num ();
    }

    return oss.str ();
}
/// Position::operator string () returns an ASCII representation of the position.
Position::operator string () const
{
    ostringstream oss;
    oss << " +---+---+---+---+---+---+---+---+\n";
    for (auto r : { R_8, R_7, R_6, R_5, R_4, R_3, R_2, R_1 })
    {
        oss << to_char (r) << "| ";
        for (auto f : { F_A, F_B, F_C, F_D, F_E, F_F, F_G, F_H })
        {
            oss << board[f|r] << " | ";
        }
        oss << "\n +---+---+---+---+---+---+---+---+\n";
    }
    for (auto f : { F_A, F_B, F_C, F_D, F_E, F_F, F_G, F_H })
    {
        oss << "   " << to_char (f, false);
    }

    oss << "\nFEN: "
        << fen ()
        << "\nKey: "
        << std::setfill ('0')
        << std::hex
        << std::uppercase
        << std::setw (16) << si->posi_key
        << std::nouppercase
        << std::dec
        << std::setfill (' ');
    oss << "\nCheckers: ";
    for (Bitboard b = si->checkers; 0 != b; )
    {
        oss << pop_lsq (b) << " ";
    }
    if (Book.enabled)
    {
        oss << "\n" << Book.show (*this);
    }
    if (   MaxLimitPiece >= count ()
        && !si->can_castle (CR_ANY))
    {
        ProbeState wdl_state; auto wdl = probe_wdl (*const_cast<Position*> (this), wdl_state);
        ProbeState dtz_state; auto dtz = probe_dtz (*const_cast<Position*> (this), dtz_state);
        oss << "\nTablebases WDL: " << std::setw (4) << wdl << " (" << wdl_state << ")"
            << "\nTablebases DTZ: " << std::setw (4) << dtz << " (" << dtz_state << ")";
    }
    oss << "\n";
    return oss.str ();
}

#if !defined(NDEBUG)
/// Position::ok() performs some consistency checks for the position,
/// and raises an assert if something wrong is detected.
bool Position::ok () const
{
    constexpr bool Fast = true;

    // BASIC
    if (   (   active != WHITE
            && active != BLACK)
        || (   32 < count ()
            || count () != pop_count (pieces ())))
    {
        assert(false && "Position OK: BASIC");
        return false;
    }
    for (auto c : { WHITE, BLACK })
    {
        if (   16 < count (c)
            || count (c) != pop_count (pieces (c))
            || 1 != std::count (board, board + SQ_NO, (c|KING))
            || 1 != count (c, KING)
            || !_ok (square<KING> (c))
            || board[square<KING> (c)] != (c|KING)
            || (           (count (c, PAWN)
                + std::max (count (c, NIHT)-2, 0)
                + std::max (count (c, BSHP)-2, 0)
                + std::max (count (c, ROOK)-2, 0)
                + std::max (count (c, QUEN)-1, 0)) > 8))
        {
            assert(false && "Position OK: BASIC");
            return false;
        }
    }
    // BITBOARD
    if (   (pieces (WHITE) & pieces (BLACK)) != 0
        || (pieces (WHITE) | pieces (BLACK)) != pieces ()
        || (pieces (WHITE) ^ pieces (BLACK)) != pieces ()
        || (pieces (PAWN)|pieces (NIHT)|pieces (BSHP)|pieces (ROOK)|pieces (QUEN)|pieces (KING))
        != (pieces (PAWN)^pieces (NIHT)^pieces (BSHP)^pieces (ROOK)^pieces (QUEN)^pieces (KING))
        || 0 != (pieces (PAWN) & (R1_bb|R8_bb))
        || 0 != pop_count (attackers_to (square<KING> (~active),  active))
        || 2 <  pop_count (attackers_to (square<KING> ( active), ~active)))
    {
        assert(false && "Position OK: BITBOARD");
        return false;
    }
    for (auto pt1 : { PAWN, NIHT, BSHP, ROOK, QUEN, KING })
    {
        for (auto pt2 : { PAWN, NIHT, BSHP, ROOK, QUEN, KING })
        {
            if (   pt1 != pt2
                && 0 != (pieces (pt1) & pieces (pt2)))
            {
                assert(false && "Position OK: BITBOARD");
                return false;
            }
        }
    }
    for (auto c : { WHITE, BLACK })
    {
        if (   1 != pop_count (pieces (c, KING))
            || (           (pop_count (pieces (c, PAWN))
                + std::max (pop_count (pieces (c, NIHT))-2, 0)
                + std::max (pop_count (pieces (c, BSHP))-2, 0)
                + std::max (pop_count (pieces (c, ROOK))-2, 0)
                + std::max (pop_count (pieces (c, QUEN))-1, 0)) > 8)
            || (           (pop_count (pieces (c, PAWN))
                + std::max (pop_count (pieces (c, BSHP) & Color_bb[WHITE])-1, 0)
                + std::max (pop_count (pieces (c, BSHP) & Color_bb[BLACK])-1, 0)) > 8))
        {
            assert(false && "Position OK: BITBOARD");
            return false;
        }
    }

    // PSQ
    if (psq != compute_psq (*this))
    {
        assert(false && "Position OK: PSQ");
        return false;
    }

    if (Fast)
    {
        return true;
    }

    // SQUARE_LIST
    for (auto c : { WHITE, BLACK })
    {
        for (auto pt : { PAWN, NIHT, BSHP, ROOK, QUEN, KING })
        {
            if (count (c, pt) != pop_count (pieces (c, pt)))
            {
                assert(false && "Position OK: SQUARELIST");
                return false;
            }
            for (auto s : squares[c][pt])
            {
                if (   !_ok (s)
                    || board[s] != (c|pt))
                {
                    assert(false && "Position OK: SQUARELIST");
                    return false;
                }
            }
        }
    }
    // CASTLING
    for (auto c : { WHITE, BLACK })
    {
        for (auto cs : { CS_KING, CS_QUEN })
        {
            auto cr = castle_right (c, cs);
            if (   si->can_castle (cr)
                && (   board[castle_rook_sq[c][cs]] != (c|ROOK)
                    || castle_mask[castle_rook_sq[c][cs]] != cr
                    || (castle_mask[square<KING> (c)] & cr) != cr))
            {
                assert(false && "Position OK: CASTLING");
                return false;
            }
        }
    }
    // STATE_INFO
    if (   si->matl_key != RandZob.compute_matl_key (*this)
        || si->pawn_key != RandZob.compute_pawn_key (*this)
        || si->posi_key != RandZob.compute_posi_key (*this)
        || si->npm[WHITE] != compute_npm<WHITE> (*this)
        || si->npm[BLACK] != compute_npm<BLACK> (*this)
        || si->checkers != attackers_to (square<KING> (active), ~active)
        || (   si->clock_ply > 2*i32(Options["Draw MoveCount"])
            || (   NONE != si->capture
                && 0 != si->clock_ply))
        || (   SQ_NO != si->enpassant_sq
            && (   R_6 != rel_rank (active, si->enpassant_sq)
                || !can_enpassant (active, si->enpassant_sq))))
    {
        assert(false && "Position OK: STATEINFO");
        return false;
    }

    return true;
}
#endif
