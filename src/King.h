#pragma once

#include <algorithm>
#include <cassert>

#include "Pawns.h"
#include "Position.h"
#include "Type.h"

namespace King {

    /// King::Entry contains information about King & Pawn structure.
    struct Entry {

    private:
        template<Color>
        Score evaluateSafetyOn(Position const&, Square);

    public:
        Key key;
        Pawns::Entry *pawnEntry;

        Score pawnDist[COLORS];

        u08   castleSide[COLORS];
        Score pawnSafety[COLORS];

        template<Color>
        Score evaluateSafety(Position const&, Bitboard);

        template<Color>
        void evaluate(Position const&);
    };

    using Table = HashTable<Entry>;

    extern Entry* probe(Position const&, Pawns::Entry*);

}