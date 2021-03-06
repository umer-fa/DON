#include "pawns.h"

#include <algorithm>
#include <cassert>

#include "bitboard.h"
#include "thread.h"

namespace Pawns {

    namespace {
        // Connected pawn bonus
        constexpr int32_t Connected[RANKS]{
            0, 5, 7, 11, 24, 48, 86, 0
        };

    #define S(mg, eg) makeScore(mg, eg)

        constexpr Score Backward      { S( 8,25) };
        constexpr Score Isolated      { S( 3,15) };
        constexpr Score Unopposed     { S(13,25) };
        constexpr Score WeakDoubled   { S(10,55) };
        constexpr Score WeakTwiceLever{ S( 3,55) };

        // Bonus for blocked pawns at 5th or 6th rank
        constexpr Score BlockedPawn[2]{
            S(-13, -4), S(-5, 2)
        };

    #undef S

    }

    /// Entry::evaluate()
    template<Color Own>
    void Entry::evaluate(Position const &pos) {
        constexpr auto Opp{ ~Own };
        constexpr auto Push{ PawnPush[Own] };

        Bitboard const pawns{ pos.pieces(PAWN) };
        Bitboard const ownPawns{ pos.pieces(Own) & pawns };
        Bitboard const oppPawns{ pos.pieces(Opp) & pawns };

        sglAttacks [Own] =
        attacksSpan[Own] = pawnSglAttackBB<Own>(ownPawns);
        dblAttacks [Opp] = pawnDblAttackBB<Opp>(oppPawns);
        blockeds |= ownPawns
                  & pawnSglPushBB<Opp>(oppPawns | dblAttacks[Opp]);

        passeds    [Own] = 0;
        score      [Own] = SCORE_ZERO;

        Square const *ps{ pos.squares(Own|PAWN) };
        Square s;
        while ((s = *ps++) != SQ_NONE) {
            assert(pos[s] == (Own|PAWN));

            auto const r{ relativeRank(Own, s) };
            assert(RANK_2 <= r && r <= RANK_7);

            Bitboard const neighbours { ownPawns & adjacentFilesBB(s) };
            Bitboard const supporters { neighbours & rankBB(s - Push) };
            Bitboard const phalanxes  { neighbours & rankBB(s) };
            Bitboard const stoppers   { oppPawns & pawnPassSpan(Own, s) };
            Bitboard const levers     { stoppers & pawnAttacksBB(Own, s) };
            Bitboard const sentres    { stoppers & pawnAttacksBB(Own, s + Push) }; // push levers
            Bitboard const opposers   { stoppers & frontSquaresBB(Own, s) };
            Bitboard const blocker    { stoppers & (s + Push) };

            bool const opposed { opposers != 0 };
            bool const blocked { blocker != 0 };
            // Backward: A pawn is backward when it is behind all pawns of the same color
            // on the adjacent files and cannot be safely advanced.
            bool const backward{ (neighbours & frontRanksBB(Opp, s + Push)) == 0
                              && (blocker | sentres) != 0 };

            // Compute additional span if pawn is not blocked nor backward
            if (!blocked
             && !backward) {
                attacksSpan[Own] |= pawnAttackSpan(Own, s);
            }

            // A pawn is passed if no forward friend pawn with
            // one of the three following conditions is true:
            // - Lever there is no stoppers except the levers
            // - Sentry there is no stoppers except the sentres, but we outnumber them
            // - Sneaker there is only one front stopper which can be levered.
            //   (Refined in Evaluation)
            if ((ownPawns & frontSquaresBB(Own, s)) == 0
             && (// Lever
                 (stoppers == levers)
                 // Lever + Sentry
              || (stoppers == (levers | sentres)
               && popCount(phalanxes) >= popCount(sentres))
                 // Sneaker => Blocked pawn
              || (stoppers == blocker
               && r >= RANK_5
               && ( pawnSglPushBB<Own>(supporters)
                 & ~(oppPawns
                   | dblAttacks[Opp])) != 0))) {
                // Passed pawns will be properly scored later in evaluation when we have full attack info.
                passeds[Own] |= s;
            }

            Score sc{ SCORE_ZERO };

            if (supporters != 0
             || phalanxes != 0) {
                int32_t const v{ Connected[r] * (2 + 1 * (phalanxes != 0) - 1 * opposed)
                               + 22 * popCount(supporters) };
                sc += makeScore(v, v * (r - RANK_3) / 4);
            }
            else
            if (neighbours == 0) {
                if (opposed
                 && (ownPawns & frontSquaresBB(Opp, s)) != 0
                 && (oppPawns & adjacentFilesBB(s)) == 0) {
                    sc -= WeakDoubled;
                }
                else {
                    sc -= Isolated
                        + Unopposed * !opposed;
                }
            }
            else
            if (backward) {
                sc -= Backward
                    + Unopposed * !opposed;
            }

            if (supporters == 0) {
                sc -= WeakDoubled * contains(ownPawns, s - Push)
                    // Attacked twice by enemy pawns
                    + WeakTwiceLever * moreThanOne(levers);
            }

            if (blocked
             && r >= RANK_5) {
                sc += BlockedPawn[r - RANK_5];
            }

            score[Own] += sc;
        }
    }
    // Explicit template instantiations
    template void Entry::evaluate<WHITE>(Position const&);
    template void Entry::evaluate<BLACK>(Position const&);

    /// Pawns::probe() looks up a current position's pawn configuration in the pawn hash table
    /// and returns a pointer to it if found, otherwise a new Entry is computed and stored there.
    Entry* probe(Position const &pos) {
        Key const pawnKey{ pos.pawnKey() };
        auto *e{ pos.thread()->pawnHash[pawnKey] };

        if (e->key == pawnKey) {
            return e;
        }

        e->key = pawnKey;
        e->blockeds = 0;
        e->pawnsOnBothFlank = (pos.pieces(PAWN) & SlotFileBB[CS_KING]) != 0
                           && (pos.pieces(PAWN) & SlotFileBB[CS_QUEN]) != 0;
        e->evaluate<WHITE>(pos);
        e->evaluate<BLACK>(pos);
        e->complexity = 12 * pos.count(PAWN)
                      +  9 * e->passedCount();
        return e;
    }

}
