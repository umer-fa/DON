#include "position.h"

#include <cstddef> // For offsetof()
#include <cstring> // For memset(), memcpy()
#include <algorithm>
#include <iomanip>
#include <sstream>

#include "cuckoo.h"
#include "movegenerator.h"
#include "notation.h"
#include "polyglot.h"
#include "syzygytb.h"
#include "thread.h"
#include "transposition.h"
#include "zobrist.h"
#include "uci.h"
#include "helper/string.h"
#include "helper/string_view.h"

namespace {

    /// Computes the non-pawn middle game material value for the given side.
    /// Material values are updated incrementally during the search.
    template<Color Own>
    Value computeNPM(Position const &pos) {

        auto npm{ VALUE_ZERO };

        for (PieceType pt = NIHT; pt <= QUEN; ++pt) {
            npm += PieceValues[MG][pt] * pos.count(Own|pt);
        }
        return npm;
    }
    /// Explicit template instantiations
    /// --------------------------------
    template Value computeNPM<WHITE>(Position const&);
    template Value computeNPM<BLACK>(Position const&);

}

//// initialize() static function
//void Position::initialize() {}

Key Position::pgKey() const noexcept {
    return PolyZob.computePosiKey(*this);
}
/// Position::movePosiKey() computes the new hash key after the given moven, needed for speculative prefetch.
/// It doesn't recognize special moves like castling, en-passant and promotions.
Key Position::movePosiKey(Move m) const noexcept {
    assert(isOk(m));
    //assert(pseudoLegal(m)
    //    && legal(m));
    /*
    auto const org{ orgSq(m) };
    auto const dst{ dstSq(m) };
    auto const mp{ board[org] };
    auto const cp{ mType(m) != ENPASSANT ? board[dst] : (~active|PAWN) };

    auto pKey{ posiKey()
             ^ RandZob.side
             ^ (epSquare() != SQ_NONE ? RandZob.enpassant[sFile(epSquare())] : 0) };

    if (mType(m) == CASTLE) {
        // ROOK
        pKey ^= RandZob.psq[cp][dst]
              ^ RandZob.psq[cp][rookCastleSq(org, dst)];
    }
    else {
        if (cp != NO_PIECE) {
            pKey ^= RandZob.psq[cp][mType(m) != ENPASSANT ? dst : dst - PawnPush[active]];
        }
        else
        if (pType(mp) == PAWN
         && dst == org + PawnPush[active] * 2) {
            auto epSq{ org + PawnPush[active] };
            if (canEnpassant(~active, epSq, false)) {
                pKey ^= RandZob.enpassant[sFile(epSq)];
            }
        }
    }
    return pKey
         ^ RandZob.psq[mp][org]
         ^ RandZob.psq[mType(m) != PROMOTE ? mp : active|promoteType(m)][mType(m) != CASTLE ? dst : kingCastleSq(org, dst)]
         ^ RandZob.castling[castleRights() & (sqCastleRight[org]|sqCastleRight[dst])];
    */

    auto const org{ orgSq(m) };
    auto const dst{ dstSq(m) };
    auto pKey{ posiKey()
             ^ RandZob.side
             ^ RandZob.psq[board[org]][org]
             ^ RandZob.psq[board[org]][dst] };
    if (board[dst] != NO_PIECE) {
        pKey ^= RandZob.psq[board[dst]][dst];
    }
    if (epSquare() != SQ_NONE) {
        pKey ^= RandZob.enpassant[sFile(epSquare())];
    }
    return pKey;
}

/// Position::draw() checks whether position is drawn by: Clock Ply Rule, Repetition.
/// It does not detect Insufficient materials and Stalemate.
bool Position::draw(int16_t pp) const noexcept {
    return  // Draw by Clock Ply Rule?
            // Not in check or in check have legal moves
           (clockPly() >= 2 * int16_t(Options["Draw MoveCount"])
         && (checkers() == 0
          || MoveList<LEGAL>(*this).size() != 0))
            // Draw by Repetition?
            // Return a draw score if a position repeats once earlier but strictly
            // after the root, or repeats twice before or at the root.
        || (repetition() != 0
         && repetition() < pp);
}

/// Position::repeated() tests whether there has been at least one repetition of positions since the last capture or pawn move.
bool Position::repeated() const noexcept {
    auto end{ std::min(clockPly(), nullPly()) };
    auto const *csi{ _stateInfo };
    while (end-- >= 4) {
        if (csi->repetition != 0) {
            return true;
        }
        csi = csi->prevState;
    }
    return false;
}

/// Position::cycled() tests if the position has a move which draws by repetition,
/// or an earlier position has a move that directly reaches the current position.
bool Position::cycled(int16_t pp) const noexcept {
    auto end{ std::min(clockPly(), nullPly()) };
    if (end < 3) {
        return false;
    }

    Key const pKey{ posiKey() };

    auto const *psi{ _stateInfo->prevState };
    for (int16_t i = 3; i <= end; i += 2) {
        psi = psi->prevState->prevState;

        Key const moveKey{
            pKey
          ^ psi->posiKey };

        Cuckoo cuckoo;
        if (Cuckoos::lookup(moveKey, cuckoo)) {
            assert(!cuckoo.empty());

            // Legality of a reverting move: clear path
            if ((pieces() & betweenBB(cuckoo.sq1, cuckoo.sq2)) == 0) {

                if (pp > i) {
                    return true;
                }
                assert(cuckoo.piece == board[cuckoo.sq1]
                    || cuckoo.piece == board[cuckoo.sq2]);
                // For nodes before or at the root, check that the move is a repetition one
                // rather than a move to the current position
                // In the cuckoo table, both moves Rc1c5 and Rc5c1 are stored in the same location.
                if (pColor(cuckoo.piece) != active) {
                    continue;
                }
                // For repetitions before or at the root, require one more
                if (psi->repetition != 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

/// Position::sliderBlockersAt() returns a bitboard of all the pieces that are blocking attacks on the square.
/// King-attack piece can be either pinner or hidden piece.
Bitboard Position::sliderBlockersAt(Square s, Bitboard attackers, Bitboard &pinners, Bitboard &hidders) const noexcept {
    Bitboard blockers{ 0 };

    Bitboard const defenders{ pieces(pColor(board[s])) };
    // Snipers are X-ray slider attackers at 's'
    // No need to remove direct attackers at 's' as in check no evaluation
    Bitboard snipers{ attackers
                    & ((pieces(BSHP, QUEN) & attacksBB<BSHP>(s))
                     | (pieces(ROOK, QUEN) & attacksBB<ROOK>(s))) };
    Bitboard const mocc{ pieces() ^ snipers };
    while (snipers != 0) {
        auto const sniperSq{ popLSq(snipers) };
        Bitboard const b{ betweenBB(s, sniperSq) & mocc };
        if (b != 0
         && !moreThanOne(b)) {
            blockers |= b;
            (b & defenders) != 0 ?
                pinners |= sniperSq :
                hidders |= sniperSq;
        }
    }
    return blockers;
}

/// Position::pseudoLegal() tests whether a random move is pseudo-legal.
/// It is used to validate moves from TT that can be corrupted
/// due to SMP concurrent access or hash position key aliasing.
bool Position::pseudoLegal(Move m) const noexcept {
    assert(isOk(m));

    auto const org{ orgSq(m) };
    auto const dst{ dstSq(m) };
    // If the org square is not occupied by a piece belonging to the side to move,
    // then the move is obviously not legal.
    if (!contains(pieces(active), org)) {
        return false;
    }

    auto const chkrs{ checkers() };

    if (mType(m) == CASTLE) {

        auto const cs{ dst > org ? CS_KING : CS_QUEN };
        return board[org] == (active|KING) //&& contains(pieces(active, KING), org)
            && board[dst] == (active|ROOK) //&& contains(pieces(active, ROOK), dst)
            && chkrs == 0
            && castleRookSq(active, cs) == dst
            && castleExpeded(active, cs)
            //&& relativeRank(active, org) == RANK_1
            //&& relativeRank(active, dst) == RANK_1
            && canCastle(active, cs);
    }

    // The captured square cannot be occupied by a friendly piece
    if (contains(pieces(active), dst)) {
        return false;
    }

    // Handle the special case of a piece move
    if (pType(board[org]) == PAWN) {
        auto const orgR{ relativeRank(active, org) };
        auto const dstR{ relativeRank(active, dst) };
        auto const Push{ PawnPush[active] };

        if (// Single push
            (((mType(m) != SIMPLE
            || RANK_2 > orgR || orgR > RANK_6
            || RANK_3 > dstR || dstR > RANK_7)
           && (mType(m) != PROMOTE
            || orgR != RANK_7
            || dstR != RANK_8))
          || dst != org + Push
          || !empty(dst))
            // Normal capture
         && (((mType(m) != SIMPLE
            || RANK_2 > orgR || orgR > RANK_6
            || RANK_3 > dstR || dstR > RANK_7)
           && (mType(m) != PROMOTE
            || orgR != RANK_7
            || dstR != RANK_8))
          || !contains(pawnAttacksBB(active, org), dst)
          || empty(dst))
            // Double push
         && (mType(m) != SIMPLE
          || orgR != RANK_2
          || dstR != RANK_4
          || dst != org + Push * 2
          || !empty(dst)
          || !empty(dst - Push))
            // Enpassant capture
         && (mType(m) != ENPASSANT
          || orgR != RANK_5
          || dstR != RANK_6
          || dst != epSquare()
          || !contains(pawnAttacksBB(active, org), dst)
          || !empty(dst)
          || empty(dst - Push)
          || clockPly() != 0)) {
            return false;
        }
    }
    else {
        if (mType(m) != SIMPLE
         || !contains(attacksBB(pType(board[org]), org, pieces()), dst)) {
            return false;
        }
    }

    // Evasions generator already takes care to avoid some kind of illegal moves and legal() relies on this.
    // So have to take care that the same kind of moves are filtered out here.
    if (chkrs != 0) {
        auto const fkSq{ square(active | KING) };
        // In case of king moves under check, remove king so to catch
        // as invalid moves like B1A1 when opposite queen is on C1.
        if (org == fkSq) {
            return (attackersTo(dst, pieces() ^ fkSq) & pieces(~active)) == 0;
        }
        // Double check? In this case a king move is required
        if (moreThanOne(chkrs)) {
            return false;
        }
        return mType(m) != ENPASSANT ?
                // Move must be a capture of the checking piece or a blocking evasion of the checking piece
                contains(chkrs | betweenBB(scanLSq(chkrs), fkSq), dst) :
                // Move must be a capture of the checking enpassant pawn or a blocking evasion of the checking piece
                (contains(chkrs & pieces(PAWN), dst - PawnPush[active])
              || contains(betweenBB(scanLSq(chkrs), fkSq), dst));
    }
    return true;
}
/// Position::legal() tests whether a pseudo-legal move is legal.
bool Position::legal(Move m) const noexcept {
    assert(isOk(m));
    //assert(pseudoLegal(m));

    auto const org{ orgSq(m) };
    auto const dst{ dstSq(m) };
    assert(contains(pieces(active), org));

    // Castling moves check for clear path for king
    if (mType(m) == CASTLE) {
        assert(board[org] == (active|KING) //&& contains(pieces(active, KING), org)
            && board[dst] == (active|ROOK) //&& contains(pieces(active, ROOK), dst)
            && castleRookSq(active, dst > org ? CS_KING : CS_QUEN) == dst
            && castleExpeded(active, dst > org ? CS_KING : CS_QUEN)
            //&& relativeRank(active, org) == RANK_1
            //&& relativeRank(active, dst) == RANK_1
            && canCastle(active, dst > org ? CS_KING : CS_QUEN)
            && checkers() == 0);

        // Check king's path for attackers
        Bitboard mocc{ pieces() ^ dst };
        Bitboard enemies{ pieces(~active) };
        Bitboard kingPath{ castleKingPath(active, dst > org ? CS_KING : CS_QUEN) };
        while (kingPath != 0) {
            if ((attackersTo(popLSq(kingPath), mocc) & enemies) != 0) {
                return false;
            }
        }
        //// In case of Chess960, verify that when moving the castling rook we do not discover some hidden checker.
        //// For instance an enemy queen in SQ_A1 when castling rook is in SQ_B1.
        //return !Options["UCI_Chess960"]
        //    || (enemies
        //      & pieces(ROOK, QUEN)
        //      & rankBB(org)
        //      & attacksBB<ROOK>(kingCastleSq(org, dst), mocc)) == 0;
        return true;
    }

    auto const fkSq{ square(active|KING) };

    // Enpassant captures are a tricky special case. Because they are rather uncommon,
    // do it simply by testing whether the king is attacked after the move is made.
    if (mType(m) == ENPASSANT) {
        assert(board[org] == (active|PAWN) //&& contains(pieces(active, PAWN), org)
            && relativeRank(active, org) == RANK_5
            && relativeRank(active, dst) == RANK_6
            && clockPly() == 0
            && dst == epSquare()
            && empty(dst)
            && board[dst - PawnPush[active]] == (~active|PAWN));

        Bitboard const mocc{ (pieces() ^ org ^ (dst - PawnPush[active])) | dst };
        return (pieces(~active, BSHP, QUEN) & attacksBB<BSHP>(fkSq, mocc)) == 0
            && (pieces(~active, ROOK, QUEN) & attacksBB<ROOK>(fkSq, mocc)) == 0;
    }

    return
        org == fkSq ?
            // For KING SIMPLE moves
            // Only king moves to non attacked squares, sliding check x-rays the king
            // In case of king moves under check have to remove king so to catch
            // as invalid moves like B1-A1 when opposite queen is on SQ_C1.
            // check whether the destination square is attacked by the opponent.
            (attackersTo(dst, pieces() ^ fkSq) & pieces(~active)) == 0 :
            // For NON-KING SIMPLE + PROMOTE moves
            // A non-king move is legal if and only if
            // - not pinned
            // - moving along the ray from the king
            !contains(kingBlockers(active), org)
         || aligned(fkSq, org, dst);
}

/// Position::giveCheck() tests whether a pseudo-legal move gives a check.
bool Position::giveCheck(Move m) const noexcept {
    assert(isOk(m));

    auto const org{ orgSq(m) };
    auto const dst{ dstSq(m) };
    assert(contains(pieces(active), org));

    auto const ekSq{ square(~active|KING) };

    if (// Direct check ?
        contains(checks(mType(m) != PROMOTE ? pType(board[org]) : promoteType(m)), dst)
        // Discovered check ?
     || (contains(kingBlockers(~active), org)
      && !aligned(ekSq, org, dst))) {
        return true;
    }

    switch (mType(m)) {
    case SIMPLE: {
        return false;
    }
    case ENPASSANT: {
        // Enpassant capture with check?
        // already handled the case of direct checks and ordinary discovered check,
        // the only case need to handle is the unusual case of a discovered check through the captured pawn.
        Bitboard const mocc{ (pieces() ^ org ^ makeSquare(sFile(dst), sRank(org))) | dst };
        return (pieces(active, BSHP, QUEN)
              & attacksBB<BSHP>(ekSq, mocc)) != 0
            || (pieces(active, ROOK, QUEN)
              & attacksBB<ROOK>(ekSq, mocc)) != 0;
    }
    case CASTLE: {
        // Castling with check?
        auto const kingDst{ kingCastleSq(org, dst) };
        auto const rookDst{ rookCastleSq(org, dst) };
        Bitboard const mocc{ (pieces() ^ org ^ dst) | kingDst | rookDst };
        return contains(attacksBB<ROOK>(rookDst, mocc), ekSq);
    }
    // case PROMOTE:
    default: {
        // Promotion with check?
        auto const ppt{ promoteType(m) };
        Bitboard const mocc{ (pieces() ^ org) | dst };
        return
         //   ppt > NIHT
         //&& contains(attacksBB(ppt, dst, mocc), ekSq)
            ((ppt == QUEN || ppt == BSHP)
          && contains(attacksBB<BSHP>(dst, mocc), ekSq))
         || ((ppt == QUEN || ppt == ROOK)
          && contains(attacksBB<ROOK>(dst, mocc), ekSq));
    }
    }
}

bool Position::giveDblCheck(Move m) const noexcept {
    assert(isOk(m));
    //assert(giveCheck(m));

    if (mType(m) == CASTLE) {
        return false;
    }

    auto const org{ orgSq(m) };
    auto const dst{ dstSq(m) };

    auto const ekSq{ square(~active|KING) };

    if (mType(m) == ENPASSANT) {
        Bitboard const mocc{ (pieces() ^ org ^ makeSquare(sFile(dst), sRank(org))) | dst };
        auto const chkrCount{
            popCount( (pieces(active, BSHP, QUEN)
                     & attacksBB<BSHP>(ekSq, mocc))
                   |  (pieces(active, ROOK, QUEN)
                     & attacksBB<ROOK>(ekSq, mocc))) };
        return chkrCount > 1
            || (chkrCount > 0
             && contains(checks(PAWN), dst));
    }

    return
        // Direct check ?
        contains(checks(mType(m) != PROMOTE ? pType(board[org]) : promoteType(m)), dst)
        // Discovered check ?
     && (contains(kingBlockers(~active), org)
      /*&& !aligned(ekSq, org, dst)*/);
}

/// Position::setCastle() set the castling right.
void Position::setCastle(Color c, Square rookOrg) {
    auto const kingOrg{ square(c|KING) };
    assert(isOk(rookOrg)
        && relativeRank(c, kingOrg) == RANK_1
        && relativeRank(c, rookOrg) == RANK_1
        && board[rookOrg] == (c|ROOK)); //&& contains(pieces(c, ROOK), rookOrg)

    auto const cs{rookOrg > kingOrg ? CS_KING : CS_QUEN};
    auto const kingDst{ kingCastleSq(kingOrg, rookOrg) };
    auto const rookDst{ rookCastleSq(kingOrg, rookOrg) };
    auto const cr{ makeCastleRight(c, cs) };
    cslRookSq[c][cs] = rookOrg;
    _stateInfo->castleRights |= cr;
    sqCastleRight[kingOrg]   |= cr;
    sqCastleRight[rookOrg]   |= cr;

    cslKingPath[c][cs] = (betweenBB(kingOrg, kingDst) | kingDst)
                       & ~(kingOrg);
    cslRookPath[c][cs] = ((betweenBB(kingOrg, kingDst) | kingDst)
                        | (betweenBB(rookOrg, rookDst) | rookDst))
                       & ~(kingOrg | rookOrg);
}
/// Position::setCheckInfo() sets check info used for fast check detection.
void Position::setCheckInfo() {
    _stateInfo->kingCheckers[WHITE] = 0;
    _stateInfo->kingCheckers[BLACK] = 0;
    _stateInfo->kingBlockers[WHITE] = sliderBlockersAt(square(WHITE|KING), pieces(BLACK), _stateInfo->kingCheckers[WHITE], _stateInfo->kingCheckers[BLACK]);
    _stateInfo->kingBlockers[BLACK] = sliderBlockersAt(square(BLACK|KING), pieces(WHITE), _stateInfo->kingCheckers[BLACK], _stateInfo->kingCheckers[WHITE]);

    auto const ekSq{ square(~active|KING) };
    _stateInfo->checks[PAWN] = pawnAttacksBB(~active, ekSq);
    _stateInfo->checks[NIHT] = attacksBB<NIHT>(ekSq);
    _stateInfo->checks[BSHP] = attacksBB<BSHP>(ekSq, pieces());
    _stateInfo->checks[ROOK] = attacksBB<ROOK>(ekSq, pieces());
    _stateInfo->checks[QUEN] = _stateInfo->checks[BSHP]|_stateInfo->checks[ROOK];
    _stateInfo->checks[KING] = 0;
}

/// Position::canEnpassant() Can the enpassant possible.
bool Position::canEnpassant(Color c, Square epSq, bool moved) const noexcept {
    assert(isOk(epSq)
        && relativeRank(c, epSq) == RANK_6);

    if (moved
     && !(contains(pieces(~c, PAWN), (epSq + PawnPush[~c]))
       && empty(epSq)
       && empty(epSq + PawnPush[c]))) {
        return false;
    }
    // Enpassant attackers
    Bitboard attackers{ pieces(c, PAWN)
                      & pawnAttacksBB(~c, epSq) };
    assert(popCount(attackers) <= 2);
    if (attackers == 0) {
        return false;
    }

    auto const cap{ moved ? epSq - PawnPush[c] : epSq + PawnPush[c] };
    assert(board[cap] == (~c|PAWN));

    auto const kSq{ square(c|KING) };
    Bitboard const bq{ pieces(~c, BSHP, QUEN) & attacksBB<BSHP>(kSq) };
    Bitboard const rq{ pieces(~c, ROOK, QUEN) & attacksBB<ROOK>(kSq) };
    Bitboard const mocc{ (pieces() ^ cap) | epSq };
    while (attackers != 0) {
        Bitboard const amocc{ mocc ^ popLSq(attackers) };
        // Check enpassant is legal for the position
        if ((bq == 0 || (bq & attacksBB<BSHP>(kSq, amocc)) == 0)
         && (rq == 0 || (rq & attacksBB<ROOK>(kSq, amocc)) == 0)) {
            return true;
        }
    }
    return false;
}

/// Position::see() is Static Exchange Evaluator.
/// Checks the SEE value of move is greater or equal to the given threshold.
/// An algorithm similar to alpha-beta pruning with a null window is used.
bool Position::see(Move m, Value threshold) const noexcept {
    assert(isOk(m));

    // Only deal with normal moves, assume others pass a simple SEE
    if (mType(m) != SIMPLE) {
        return threshold <= VALUE_ZERO;
    }

    auto org{ orgSq(m) };
    auto const dst{ dstSq(m) };

    int32_t swap;
    swap = PieceValues[MG][pType(board[dst])] - threshold;
    if (swap < 0) {
        return false;
    }
    swap = PieceValues[MG][pType(board[org])] - swap;
    if (swap < 1) {
        return true;
    }

    int32_t res{ 1 };
    auto mov{ pColor(board[org]) };
    Bitboard mocc{ pieces() ^ org ^ dst };
    Bitboard attackers{ attackersTo(dst, mocc) };
    while (attackers != 0) {
        mov = ~mov;
        attackers &= mocc;

        Bitboard movAttackers{ attackers & pieces(mov) };

        if (movAttackers != 0) {
            auto const kSq{ square(mov|KING) };
            Bitboard b;
            // Don't allow pinned pieces to attack (except the king) as long as
            // there are pinners on their original square.
            if ((b = kingCheckers(mov)
                   & pieces(~mov)
                   & mocc) != 0) {
                while (b != 0) {
                    movAttackers &= ~betweenBB(kSq, popLSq(b));
                }
            }
            else
            if (contains(kingBlockers(mov), org)
             && !aligned(kSq, org, dst)
             && (kingCheckers(~mov)
               & pieces(~mov)
               & mocc
               & lineBB(kSq, org)) != 0) {
                movAttackers = SquareBB[kSq];
            }
            // If mov has no more attackers then give up: mov loses
            if (movAttackers == 0) {
                break;
            }
        }
        else {
            break;
        }

        res ^= 1;

        // Locate and remove the next least valuable attacker, and add to
        // the bitboard 'attackers' any X-ray attackers behind it.
        Bitboard bb;
        if ((bb = movAttackers & pieces(PAWN)) != 0) {
            if ((swap = VALUE_MG_PAWN - swap) < res) {
                break;
            }
            mocc ^= (org = scanLSq(bb));
            attackers |= (pieces(BSHP, QUEN) & attacksBB<BSHP>(dst, mocc));
        }
        else
        if ((bb = movAttackers & pieces(NIHT)) != 0) {
            if ((swap = VALUE_MG_NIHT - swap) < res) {
                break;
            }
            mocc ^= (org = scanLSq(bb));
        }
        else
        if ((bb = movAttackers & pieces(BSHP)) != 0) {
            if ((swap = VALUE_MG_BSHP - swap) < res) {
                break;
            }
            mocc ^= (org = scanLSq(bb));
            attackers |= (pieces(BSHP, QUEN) & attacksBB<BSHP>(dst, mocc));
        }
        else
        if ((bb = movAttackers & pieces(ROOK)) != 0) {
            if ((swap = VALUE_MG_ROOK - swap) < res) {
                break;
            }
            mocc ^= (org = scanLSq(bb));
            attackers |= (pieces(ROOK, QUEN) & attacksBB<ROOK>(dst, mocc));
        }
        else
        if ((bb = movAttackers & pieces(QUEN)) != 0) {
            if ((swap = VALUE_MG_QUEN - swap) < res) {
                break;
            }
            mocc ^= (org = scanLSq(bb));
            attackers |= (pieces(BSHP, QUEN) & attacksBB<BSHP>(dst, mocc))
                       | (pieces(ROOK, QUEN) & attacksBB<ROOK>(dst, mocc));
        }
        else { // KING
            // If we "capture" with the king but opponent still has attackers, reverse the result.
            if ((attackers & pieces(~mov)) != 0) {
                res ^= 1;
            }
            break;
        }
    }

    return bool(res);
}

/// Position::setup() initializes the position object with the given FEN string.
/// This function is not very robust - make sure that input FENs are correct,
/// this is assumed to be the responsibility of the GUI.
Position& Position::setup(std::string_view ff, StateInfo &si, Thread *th) {
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
    // 4) Enpassant target square(in algebraic notation).
    //    If there's no enpassant target square, this is "-".
    //    If a pawn has just made a 2-square move, this is the position "behind" the pawn.
    //    This is recorded only if there really is a pawn that might have advanced two squares
    //    and if there is a pawn in position to make an enpassant capture legally!!!.
    // 5) Half move clock. This is the number of half moves since the last pawn advance or capture.
    //    This is used to determine if a draw can be claimed under the fifty-move rule.
    // 6) Full move number. The number of the full move.
    //    It starts at 1, and is incremented after Black's move.

    std::memset(this, 0, sizeof (*this));
    std::fill_n(&pieceSquare[0][0], sizeof (pieceSquare) / sizeof (pieceSquare[0][0]), SQ_NONE);
    std::fill_n(&cslRookSq[0][0], sizeof (cslRookSq) / sizeof (cslRookSq[0][0]), SQ_NONE);

    std::memset(&si, 0, sizeof (si));
    _stateInfo = &si;

    std::istringstream iss{ ff.data() };
    iss >> std::noskipws;

    uint8_t token;
    // 1. Piece placement on Board
    Square sq{ SQ_A8 };
    while ((iss >> token)
        && !isspace(token)) {

        size_t pos;
        if ('1' <= token && token <= '8') {
            sq += (token - '0') * EAST;
        }
        else
        if (token == '/') {
            sq += 2 * SOUTH;
        }
        else
        if ((pos = PieceChar.find(token)) != std::string::npos) {
            placePiece(sq, Piece(pos));
            ++sq;
        }
    }
    assert(count(W_KING) == 1
        && count(B_KING) == 1);

    // 2. Active color
    iss >> token;
    active = toColor(token);

    // 3. Castling availability
    iss >> token;
    while ((iss >> token)
        && !isspace(token)) {
        Color const c{ isupper(token) ? WHITE : BLACK };
        Piece const rook{ (c|ROOK) };

        token = char(tolower(token));
        Square rookOrg;
        if (token == 'k') {
            for (rookOrg = relativeSq(c, SQ_H1); rook != board[rookOrg] /*&& rookOrg > square(c|KING)*/; --rookOrg) {}
        }
        else
        if (token == 'q') {
            for (rookOrg = relativeSq(c, SQ_A1); rook != board[rookOrg] /*&& rookOrg < square(c|KING)*/; ++rookOrg) {}
        }
        else
        if ('a' <= token && token <= 'h') {
            rookOrg = makeSquare(toFile(token), relativeRank(c, RANK_1));
        }
        else {
            assert(token == '-');
            continue;
        }

        setCastle(c, rookOrg);
    }

    // 4. Enpassant square.
    // Ignore if square is invalid or not on side to move relative rank 6.
    bool enpassant{ false };
    uint8_t file, rank;
    if ((iss >> file && ('a' <= file && file <= 'h'))
     && (iss >> rank && (rank == (active == WHITE ? '6' : '3')))) {
        _stateInfo->epSquare = makeSquare(toFile(file), toRank(rank));
        enpassant = canEnpassant(active, epSquare());
    }
    if (!enpassant) {
        _stateInfo->epSquare = SQ_NONE;
    }

    // 5-6. Half move clock and Full move number.
    iss >> std::skipws
        >> _stateInfo->clockPly
        >> ply;

    if (epSquare() != SQ_NONE) {
        _stateInfo->clockPly = 0;
    }
    // Rule 50 draw case.
    assert(100 >= clockPly());
    // Convert from moves starting from 1 to ply starting from 0.
    ply = int16_t(std::max(2 * (ply - 1), 0) + active);
    assert(0 <= gamePly());

    npMaterial[WHITE] = computeNPM<WHITE>(*this);
    npMaterial[BLACK] = computeNPM<BLACK>(*this);

    _stateInfo->matlKey = RandZob.computeMatlKey(*this);
    _stateInfo->pawnKey = RandZob.computePawnKey(*this);
    _stateInfo->posiKey = RandZob.computePosiKey(*this);
    _stateInfo->checkers = attackersTo(square(active|KING)) & pieces(~active);
    setCheckInfo();
    _thread = th;

    assert(ok());
    return *this;
}
/// Position::setup() initializes the position object with the given endgame code string like "KBPKN".
/// It is mainly an helper to get the material key out of an endgame code.
Position& Position::setup(std::string_view code, Color c, StateInfo &si) {
    assert(code[0] == 'K'
        && code.find('K', 1) != std::string::npos);

    std::string sides[COLORS]{
        std::string(code.substr(code.find('K', 1))),                             // Weak
        std::string(code.substr(0, std::min(code.find('v'), code.find('K', 1)))) // Strong
    };
    assert(sides[WHITE].length() < 8
        && sides[BLACK].length() < 8);

    sides[c] = toLower(sides[c]);

    std::string fenStr{ "8/" + sides[WHITE] + char(8 - sides[WHITE].length() + '0') + "/8/8/8/8/"
                             + sides[BLACK] + char(8 - sides[BLACK].length() + '0') + "/8 w - - 0 10" };

    return setup(fenStr, si, nullptr);
}

/// Position::doMove() makes a move, and saves all information necessary to a StateInfo object.
/// The move is assumed to be legal.
void Position::doMove(Move m, StateInfo &si, bool isCheck) noexcept {
    assert(isOk(m)
        && pseudoLegal(m)
        && legal(m)
        && &si != _stateInfo);

    _thread->nodes.fetch_add(1, std::memory_order::memory_order_relaxed);

    Key pKey{ posiKey()
            ^ RandZob.side };

    // Copy some fields of the old state to our new StateInfo object except the
    // ones which are going to be recalculated from scratch anyway and then switch
    // our state pointer to point to the new (ready to be updated) state.
    std::memcpy(&si, _stateInfo, offsetof(StateInfo, posiKey));
    si.prevState = _stateInfo;
    _stateInfo = &si;
    // Increment ply counters. In particular, clockPly will be reset to zero later on
    // in case of a capture or a pawn move.
    ++ply;
    ++_stateInfo->clockPly;
    ++_stateInfo->nullPly;
    _stateInfo->promoted = false;

    // Used by NNUE
    _stateInfo->accumulator.accumulationComputed = false;
    auto &mi{ _stateInfo->moveInfo };
    mi.pieceCount = 1;

    auto const pasive{ ~active };

    auto const org{ orgSq(m) };
    auto dst{ dstSq(m) };
    assert(contains(pieces(active), org)
        && (!contains(pieces(active), dst)
         || mType(m) == CASTLE));

    auto const mp{ board[org] };
    assert(mp != NO_PIECE);
    auto cp{ mType(m) != ENPASSANT ? board[dst] : (pasive|PAWN) };

    if (mType(m) == CASTLE) {
        assert(mp == (active|KING)
            && cp == (active|ROOK)
            && castleRookSq(active, dst > org ? CS_KING : CS_QUEN) == dst
            && castleExpeded(active, dst > org ? CS_KING : CS_QUEN)
            && relativeRank(active, org) == RANK_1
            && relativeRank(active, dst) == RANK_1
            && canCastle(active, dst > org ? CS_KING : CS_QUEN)
            && _stateInfo->prevState->checkers == 0); //&& (attackersTo(org) & pieces(pasive))

        auto const rookOrg{ dst }; // Castling is encoded as "King captures friendly Rook"
        auto const rookDst{ rookCastleSq(org, rookOrg) };
        /* king*/dst = kingCastleSq(org, rookOrg);

        if (Evaluator::useNNUE) {
            mi.piece[0] = active|KING;
            mi.org[0] = org;
            mi.dst[0] = dst;
            mi.piece[1] = active|ROOK;
            mi.org[1] = rookOrg;
            mi.dst[1] = rookDst;
            mi.pieceCount = 2; // 2 pieces moved
        }

        // Remove both pieces first since squares could overlap in chess960
        removePiece(org);
        removePiece(rookOrg);
        board[org] = board[rookOrg] = NO_PIECE; // Not done by removePiece()
        placePiece(dst    , mp);
        placePiece(rookDst, cp);
        pKey ^= RandZob.psq[cp][rookOrg]
              ^ RandZob.psq[cp][rookDst];

        cp = NO_PIECE;
    }

    // Set capture piece
    _stateInfo->captured = pType(cp);
    if (cp != NO_PIECE) {
        assert(pType(cp) < KING);

        auto cap = dst;
        if (pType(cp) == PAWN) {
            if (mType(m) == ENPASSANT) {
                cap -= PawnPush[active];

                assert(mp == (active|PAWN)
                    && relativeRank(active, org) == RANK_5
                    && relativeRank(active, dst) == RANK_6
                    && clockPly() == 1
                    && dst == epSquare()
                    && empty(dst) //&& !contains(pieces(), dst)
                    && cp == (pasive|PAWN)
                    && board[cap] == (pasive|PAWN)); //&& contains(pieces(pasive, PAWN), cap));
            }
            _stateInfo->pawnKey ^= RandZob.psq[cp][cap];
        }
        else {
            npMaterial[pasive] -= PieceValues[MG][pType(cp)];
        }

        if (Evaluator::useNNUE) {
            mi.piece[1] = cp;
            mi.org[1] = cap;
            mi.dst[1] = SQ_NONE;
            mi.pieceCount = 2; // 1 piece moved, 1 piece captured
        }

        removePiece(cap);
        if (mType(m) == ENPASSANT) {
            board[cap] = NO_PIECE; // Not done by removePiece()
        }
        pKey ^= RandZob.psq[cp][cap];
        _stateInfo->matlKey ^= RandZob.psq[cp][count(cp)];
        // Reset clock ply counter
        _stateInfo->clockPly = 0;
    }

    // Move the piece. The tricky Chess960 castling is handled earlier
    if (mType(m) != CASTLE) {

        if (Evaluator::useNNUE) {
            mi.piece[0] = mp;
            mi.org[0] = org;
            mi.dst[0] = dst;
        }

        movePiece(org, dst);
    }
    pKey ^= RandZob.psq[mp][org]
          ^ RandZob.psq[mp][dst];

    // Reset enpassant square
    if (epSquare() != SQ_NONE) {
        assert(1 >= clockPly());
        pKey ^= RandZob.enpassant[sFile(epSquare())];
        _stateInfo->epSquare = SQ_NONE;
    }

    // Update castling rights
    if (castleRights() != CR_NONE
     && (sqCastleRight[org]|sqCastleRight[dst]) != CR_NONE) {
        pKey ^= RandZob.castling[castleRights()];
        _stateInfo->castleRights &= ~(sqCastleRight[org]|sqCastleRight[dst]);
        pKey ^= RandZob.castling[castleRights()];
    }

    if (pType(mp) == PAWN) {

        // Double push pawn
        // Set enpassant square if the moved pawn can be captured
        if (dst == org + PawnPush[active] * 2
         && canEnpassant(pasive, org + PawnPush[active])) {
            _stateInfo->epSquare = org + PawnPush[active];
            pKey ^= RandZob.enpassant[sFile(_stateInfo->epSquare)];
        }
        else
        if (mType(m) == PROMOTE) {
            assert(pType(mp) == PAWN
                && relativeRank(active, org) == RANK_7
                && relativeRank(active, dst) == RANK_8);

            auto const pp{ active|promoteType(m) };
            // Replace the pawn with the promoted piece
            removePiece(dst);
            placePiece(dst, pp);

            if (Evaluator::useNNUE) {
                // Promoting pawn to SQ_NONE, promoted piece from SQ_NONE
                mi.dst[0] = SQ_NONE;
                mi.piece[mi.pieceCount] = pp;
                mi.org[mi.pieceCount] = SQ_NONE;
                mi.dst[mi.pieceCount] = dst;
                mi.pieceCount++;
            }

            npMaterial[active] += PieceValues[MG][pType(pp)];
            pKey ^= RandZob.psq[mp][dst]
                  ^ RandZob.psq[pp][dst];
            _stateInfo->pawnKey ^= RandZob.psq[mp][dst];
            _stateInfo->matlKey ^= RandZob.psq[mp][count(mp)]
                                 ^ RandZob.psq[pp][count(pp) - 1];
            _stateInfo->promoted = true;
        }

        // Reset clock ply counter
        _stateInfo->clockPly = 0;
        _stateInfo->pawnKey ^= RandZob.psq[mp][org]
                             ^ RandZob.psq[mp][dst];
    }

    assert((attackersTo(square(active|KING)) & pieces(pasive)) == 0);
    // Calculate checkers
    _stateInfo->checkers = isCheck ? attackersTo(square(pasive|KING)) & pieces(active) : 0;
    assert(!isCheck
        || (checkers() != 0
         && popCount(checkers()) <= 2));

    // Switch sides
    active = pasive;
    // Update the key with the final value
    _stateInfo->posiKey = pKey;

    setCheckInfo();

    // Calculate the repetition info. It is the ply distance from the previous
    // occurrence of the same position, negative in the 3-fold case, or zero
    // if the position was not repeated.
    _stateInfo->repetition = 0;
    auto end = std::min(clockPly(), nullPly());
    if (end >= 4) {
        auto const *psi{ _stateInfo->prevState->prevState };
        for (int16_t i = 4; i <= end; i += 2) {
            psi = psi->prevState->prevState;
            if (psi->posiKey == posiKey()) {
                _stateInfo->repetition = i * (1 - 2 * (psi->repetition != 0));
                break;
            }
        }
    }

    assert(ok());
}
/// Position::undoMove() unmakes a move, and restores the position to exactly the same state as before the move was made.
/// The move is assumed to be legal.
void Position::undoMove(Move m) noexcept {
    assert(isOk(m)
        && _stateInfo->prevState != nullptr);

    active = ~active;

    auto const org{ orgSq(m) };
    auto dst{ dstSq(m) };
    assert(empty(org)
        || mType(m) == CASTLE);
    assert(captured() < KING);

    if (mType(m) == CASTLE) {
        assert(relativeRank(active, org) == RANK_1
            && relativeRank(active, dst) == RANK_1
            && captured() == NONE);

        auto const rookOrg{ dst }; // Castling is encoded as "King captures friendly Rook"
        auto const rookDst{ rookCastleSq(org, rookOrg) };
        /* king*/dst = kingCastleSq(org, rookOrg);

        // Remove both pieces first since squares could overlap in chess960
        removePiece(dst);
        removePiece(rookDst);
        board[dst] = board[rookDst] = NO_PIECE; // Not done by removePiece()
        placePiece(org    , active|KING);
        placePiece(rookOrg, active|ROOK);
    }
    else {
        auto const mp{ board[dst] };
        assert(mp != NO_PIECE
            && pColor(mp) == active);

        if (mType(m) == PROMOTE) {
            assert(NIHT <= pType(mp) && pType(mp) <= QUEN
                && relativeRank(active, org) == RANK_7
                && relativeRank(active, dst) == RANK_8);

            removePiece(dst);
            placePiece(dst, active|PAWN);
            npMaterial[active] -= PieceValues[MG][pType(mp)];
        }
        // Move the piece
        movePiece(dst, org);

        if (captured() != NONE) {
            auto cap{ dst };

            if (mType(m) == ENPASSANT) {

                cap -= PawnPush[active];

                assert(pType(mp) == PAWN //&& contains(pieces(active, PAWN), org)
                    && relativeRank(active, org) == RANK_5
                    && relativeRank(active, dst) == RANK_6
                    && dst == _stateInfo->prevState->epSquare
                    //&& empty(cap)
                    && captured() == PAWN);
            }
            assert(empty(cap));

            // Restore the captured piece.
            placePiece(cap, ~active|captured());

            if (captured() > PAWN) {
                npMaterial[~active] += PieceValues[MG][captured()];
            }
        }
    }

    // Point state pointer back to the previous state.
    _stateInfo = _stateInfo->prevState;

    --ply;

    assert(ok());
}
/// Position::doNullMove() makes a 'null move'.
/// It flips the side to move without executing any move on the board.
void Position::doNullMove(StateInfo &si) noexcept {
    assert(&si != _stateInfo
        && checkers() == 0);

    std::memcpy(&si, _stateInfo, Evaluator::useNNUE ? sizeof (StateInfo) : offsetof(StateInfo, accumulator));

    si.prevState = _stateInfo;
    _stateInfo = &si;

    ++_stateInfo->clockPly;
    _stateInfo->nullPly = 0;
    _stateInfo->captured = NONE;
    _stateInfo->promoted = false;

    // Reset enpassant square
    if (epSquare() != SQ_NONE) {
        _stateInfo->posiKey ^= RandZob.enpassant[sFile(epSquare())];
        _stateInfo->epSquare = SQ_NONE;
    }

    active = ~active;
    _stateInfo->posiKey ^= RandZob.side;

    setCheckInfo();

    _stateInfo->repetition = 0;

    assert(ok());
}
/// Position::undoNullMove() unmakes a 'null move'.
void Position::undoNullMove() noexcept {
    assert(_stateInfo->prevState != nullptr
        && nullPly() == 0
        && captured() == NONE
        && checkers() == 0);

    active = ~active;
    _stateInfo = _stateInfo->prevState;

    assert(ok());
}

/// Position::flip() flips position mean White and Black sides swaped.
/// This is only useful for debugging especially for finding evaluation symmetry bugs.
void Position::flip() {
    std::istringstream iss{ fen() };
    std::string ff, token;
    // 1. Piece placement
    for (Rank r = RANK_8; r >= RANK_1; --r) {
        std::getline(iss, token, r > RANK_1 ? '/' : ' ');
        token = toggleCase(token);
        ff.insert(0, token + (r < RANK_8 ? "/" : " "));
    }
    // 2. Active color
    iss >> token;
    ff += toChar(~toColor(token[0]));
    ff += " ";
    // 3. Castling availability
    iss >> token;
    if (token != "-") {
        token = toggleCase(token);
    }
    ff += token;
    ff += " ";
    // 4. Enpassant square
    iss >> token;
    if (token != "-") {
        token.replace(1, 1, { 1, toChar(~toRank(token[1])) });
    }
    ff += token;
    // 5-6. Halfmove clock and Fullmove number
    std::getline(iss, token, '\n');
    ff += token;

    setup(ff, *_stateInfo, _thread);

    assert(ok());
}
/// Position::mirror() mirrors position mean King and Queen sides swaped.
void Position::mirror() {
    std::istringstream iss{ fen() };
    std::string ff, token;
    // 1. Piece placement
    for (Rank r = RANK_8; r >= RANK_1; --r) {
        std::getline(iss, token, r > RANK_1 ? '/' : ' ');
        std::reverse(token.begin(), token.end());
        ff += token + (r > RANK_1 ? "/" : " ");
    }
    // 2. Active color
    iss >> token;
    ff += token;
    ff += ' ';
    // 3. Castling availability
    iss >> token;
    if (token != "-") {
        for (auto &ch : token) {
            if (Options["UCI_Chess960"]) {
                assert(isalpha(ch));
                ch = toChar(~toFile(char(tolower(ch))), islower(ch));
            }
            else {
            switch (ch) {
            case 'K': ch = 'Q'; break;
            case 'Q': ch = 'K'; break;
            case 'k': ch = 'q'; break;
            case 'q': ch = 'k'; break;
            default: assert(false); break;
            }
            }
        }
    }
    ff += token;
    ff += ' ';
    // 4. Enpassant square
    iss >> token;
    if (token != "-") {
        token.replace(0, 1, { 1, toChar(~toFile(token[0])) });
    }
    ff += token;
    // 5-6. Halfmove clock and Fullmove number
    std::getline(iss, token, '\n');
    ff += token;

    setup(ff, *_stateInfo, _thread);

    assert(ok());
}

/// Position::fen() returns a FEN representation of the position.
/// In case of Chess960 the Shredder-FEN notation is used.
std::string Position::fen(bool full) const {
    std::ostringstream oss;

    for (Rank r = RANK_8; r >= RANK_1; --r) {
        for (File f = FILE_A; f <= FILE_H; ++f) {
            int16_t emptyCount = 0;
            while (f <= FILE_H
                && empty(makeSquare(f, r))) {
                ++emptyCount;
                ++f;
            }
            if (emptyCount != 0) {
                oss << emptyCount;
            }
            if (f <= FILE_H) {
                oss << board[makeSquare(f, r)];
            }
        }
        if (r > RANK_1) {
            oss << '/';
        }
    }

    oss << ' ' << active << ' ';

    if (castleRights() != CR_NONE) {
        if (canCastle(WHITE, CS_KING)) { oss << (Options["UCI_Chess960"] ? toChar(sFile(castleRookSq(WHITE, CS_KING)), false) : 'K'); }
        if (canCastle(WHITE, CS_QUEN)) { oss << (Options["UCI_Chess960"] ? toChar(sFile(castleRookSq(WHITE, CS_QUEN)), false) : 'Q'); }
        if (canCastle(BLACK, CS_KING)) { oss << (Options["UCI_Chess960"] ? toChar(sFile(castleRookSq(BLACK, CS_KING)),  true) : 'k'); }
        if (canCastle(BLACK, CS_QUEN)) { oss << (Options["UCI_Chess960"] ? toChar(sFile(castleRookSq(BLACK, CS_QUEN)),  true) : 'q'); }
    }
    else {
        oss << '-';
    }

    oss << ' ' << (epSquare() != SQ_NONE ? ::toString(epSquare()) : "-");

    if (full) {
        oss << ' ' << clockPly() << ' ' << moveCount();
    }

    return oss.str();
}
/// Position::toString() returns an ASCII representation of the position.
std::string Position::toString() const {
    std::ostringstream oss;
    oss << " +---+---+---+---+---+---+---+---+\n";
    for (Rank r = RANK_8; r >= RANK_1; --r) {
        oss << r << "| ";
        for (File f = FILE_A; f <= FILE_H; ++f) {
            oss << board[makeSquare(f, r)] << " | ";
        }
        oss << "\n +---+---+---+---+---+---+---+---+\n";
    }
    for (File f = FILE_A; f <= FILE_H; ++f) {
        oss << "   " << toChar(f, false);
    }
    oss << "\nFEN: " << fen()
        << "\nKey: " << std::uppercase << std::hex << std::setfill('0')
                     << std::setw(16) << posiKey()
                     << std::nouppercase << std::dec << std::setfill(' ');
    oss << "\nCheckers: ";
    for (Bitboard b = checkers(); b != 0; ) {
        oss << popLSq(b) << ' ';
    }
    if (Book.enabled) {
        oss << '\n' << Book.show(*this);
    }
    if (count() <= SyzygyTB::MaxPieceLimit
     && castleRights() == CR_NONE) {
        SyzygyTB::ProbeState wdlState;
        auto const wdlScore{ SyzygyTB::probeWDL(*const_cast<Position*>(this), wdlState) };
        SyzygyTB::ProbeState dtzState;
        auto const dtzScore{ SyzygyTB::probeDTZ(*const_cast<Position*>(this), dtzState) };
        oss << "\nTablebases WDL: " << std::setw(4) << wdlScore << " (" << wdlState << ")"
            << "\nTablebases DTZ: " << std::setw(4) << dtzScore << " (" << dtzState << ")";
    }
    oss << '\n';

    return oss.str();
}

std::ostream& operator<<(std::ostream &ostream, Position const &pos) {
    ostream << pos.toString();
    return ostream;
}

#if !defined(NDEBUG)
/// Position::ok() performs some consistency checks for the position,
/// and raises an assert if something wrong is detected.
bool Position::ok() const noexcept {
    constexpr bool Fast{ true };

    // BASIC
    if (!isOk(active)
     || (count() > 32
      || count() != popCount(pieces()))) {
        assert(false && "Position OK: BASIC");
        return false;
    }
    for (Color c : { WHITE, BLACK }) {
        if (count(c) > 16
         || count(c) != popCount(pieces(c))
         || std::count(board, board + SQUARES, (c|KING)) != 1
         || count(c|KING) != 1
         || !isOk(square(c|KING))
         || board[square(c|KING)] != (c|KING)
         || (        (count(c|PAWN)
           + std::max(count(c|NIHT) - 2, 0)
           + std::max(count(c|BSHP) - 2, 0)
           + std::max(count(c|ROOK) - 2, 0)
           + std::max(count(c|QUEN) - 1, 0)) > 8)) {
            assert(false && "Position OK: BASIC");
            return false;
        }
    }
    // BITBOARD
    if ((pieces(WHITE) & pieces(BLACK)) != 0
     || (pieces(WHITE) | pieces(BLACK)) != pieces()
     || (pieces(WHITE) ^ pieces(BLACK)) != pieces()
     || (pieces(PAWN)|pieces(NIHT)|pieces(BSHP)|pieces(ROOK)|pieces(QUEN)|pieces(KING))
     != (pieces(PAWN)^pieces(NIHT)^pieces(BSHP)^pieces(ROOK)^pieces(QUEN)^pieces(KING))
     || (pieces(PAWN) & (RankBB[RANK_1]|RankBB[RANK_8])) != 0
     || popCount(attackersTo(square(~active|KING)) & pieces( active)) != 0
     || popCount(attackersTo(square( active|KING)) & pieces(~active)) > 2) {
        assert(false && "Position OK: BITBOARD");
        return false;
    }
    for (PieceType pt1 = PAWN; pt1 <= KING; ++pt1) {
        for (PieceType pt2 = PAWN; pt2 <= KING; ++pt2) {
            if (pt1 != pt2
             && (pieces(pt1) & pieces(pt2)) != 0) {
                assert(false && "Position OK: BITBOARD");
                return false;
            }
        }
    }
    for (Color const c : { WHITE, BLACK }) {
        if (popCount(pieces(c, KING)) != 1
         || (        (popCount(pieces(c, PAWN))
           + std::max(popCount(pieces(c, NIHT)) - 2, 0)
           + std::max(popCount(pieces(c, BSHP)) - 2, 0)
           + std::max(popCount(pieces(c, ROOK)) - 2, 0)
           + std::max(popCount(pieces(c, QUEN)) - 1, 0)) > 8)
         || (        (popCount(pieces(c, PAWN))
           + std::max(popCount(pieces(c, BSHP) & ColorBB[WHITE]) - 1, 0)
           + std::max(popCount(pieces(c, BSHP) & ColorBB[BLACK]) - 1, 0)) > 8)) {
            assert(false && "Position OK: BITBOARD");
            return false;
        }
    }

    // Non-Pawn material & PSQ
    if (nonPawnMaterial(WHITE) != computeNPM<WHITE>(*this)
     || nonPawnMaterial(BLACK) != computeNPM<BLACK>(*this)
     || psqScore() != PSQT::computePSQ(*this)) {
        assert(false && "Position OK: PSQ");
        return false;
    }

    if (Fast) {
        return true;
    }

    // SQUARE_LIST
    for (Piece const p : Pieces) {
        if (count(p) != popCount(pieces(pColor(p), pType(p)))) {
            assert(false && "Position OK: SQUARE_LIST");
            return false;
        }
        for (int16_t i = 0; i < pieceCount[p]; ++i) {
            if (board[pieceSquare[p][i]] != p
             || pieceIndex[pieceSquare[p][i]] != i) {
                assert(false && "Position OK: SQUARE_LIST");
                return false;
            }
        }
    }

    // CASTLING
    for (Color const c : { WHITE, BLACK }) {
        for (CastleSide const cs : { CS_KING, CS_QUEN }) {
            auto const cr{ makeCastleRight(c, cs) };
            if (canCastle(c, cs)
             && (castleRookSq(c, cs) == SQ_NONE
              || board[castleRookSq(c, cs)] != (c|ROOK)
              ||  sqCastleRight[castleRookSq(c, cs)] != cr
              || (sqCastleRight[square(c|KING)] & cr) != cr)) {
                assert(false && "Position OK: CASTLING");
                return false;
            }
        }
    }
    // STATE_INFO
    if (matlKey() != RandZob.computeMatlKey(*this)
     || pawnKey() != RandZob.computePawnKey(*this)
     || posiKey() != RandZob.computePosiKey(*this)
     || checkers() != (attackersTo(square(active|KING)) & pieces(~active))
     || popCount(checkers()) > 2
     || clockPly() > 2 * int16_t(Options["Draw MoveCount"])
     || (captured() != NONE
      && clockPly() != 0)
     || (epSquare() != SQ_NONE
      && (clockPly() != 0
       || relativeRank(active, epSquare()) != RANK_6
       || !canEnpassant(active, epSquare())))) {
        assert(false && "Position OK: STATE_INFO");
        return false;
    }

    return true;
}

/// isOk() Check the validity of FEN string
bool isOk(std::string_view fen) {
    Position pos;
    StateInfo si;
    return !whiteSpaces(fen)
        && pos.setup(fen, si, nullptr).ok();
}
#endif
