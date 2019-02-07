#ifndef _PAWNS_H_INC_
#define _PAWNS_H_INC_

#include "Type.h"
#include "Position.h"

namespace Pawns {

    const i08 MaxCache = 4;
    /// Pawns::Entry contains various information about a pawn structure.
    struct Entry
    {
    public:
        Key key;

        u08      asymmetry;
        Score    scores[CLR_NO];
        Bitboard any_attacks[CLR_NO];
        Bitboard dbl_attacks[CLR_NO];
        Bitboard attack_span[CLR_NO];
        Bitboard passers[CLR_NO];
        Bitboard weak_unopposed[CLR_NO];
        u08      semiopens[CLR_NO];

        u08      index[CLR_NO];
        Square   king_square[CLR_NO][MaxCache];
        Value    king_safety[CLR_NO][MaxCache];
        u08      king_pawn_dist[CLR_NO][MaxCache];

        template<Color Own>
        bool file_semiopen (File f) const
        {
            return 0 != (semiopens[Own] & (u08(1) << f));
        }

        template<Color Own>
        Value evaluate_safety (const Position&, Square) const;

        template<Color Own>
        u08 king_safety_on (const Position &pos, Square fk_sq)
        {
            auto idx = std::find (king_square[Own], king_square[Own] + index[Own] + 1, fk_sq) - king_square[Own];
            assert(0 <= idx);
            if (idx <= index[Own])
            {
                return u08(idx);
            }
            
            idx = index[Own];
            if (idx < MaxCache - 1)
            {
                ++index[Own];
            }

            u08 kp_dist = 0;
            Bitboard pawns = pos.pieces (Own, PAWN);
            if (0 != pawns)
            {
                while (0 == (pawns & BitBoard::dist_rings_bb (fk_sq, ++kp_dist))) {}
            }

            king_square[Own][idx] = fk_sq;
            king_pawn_dist[Own][idx] = kp_dist;
            king_safety[Own][idx] = evaluate_safety<Own> (pos, fk_sq);
            return u08(idx);
        }

    };

    typedef HashTable<Entry, 0x4000> Table;

    extern Entry* probe (const Position&);

    extern void initialize ();
}

#endif // _PAWNS_H_INC_
