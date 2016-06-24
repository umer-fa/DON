#include "PieceSquare.h"

#include "Position.h"

namespace {

    #define S(mg, eg) mk_score (mg, eg)
    // HalfPSQ[piece-type][rank][file/2] contains half Piece-Square scores.
    // Table is defined for files A..D and white side,
    // It is symmetric for second half of the files and negative for black side.
    // For each piece type on a given square a (midgame, endgame) score pair is assigned.
    const Score HalfPSQ[NONE][R_NO][F_NO/2] =
    {
        { // Pawn
            { S(  0, 0), S(  0, 0), S(  0, 0), S( 0, 0) },
            { S(-16, 7), S(  1,-4), S(  7, 8), S( 3,-2) },
            { S(-23,-4), S( -7,-5), S( 19, 5), S(24, 4) },
            { S(-22, 3), S(-14, 3), S( 20,-8), S(35,-3) },
            { S(-11, 8), S(  0, 9), S(  3, 7), S(21,-6) },
            { S(-11, 8), S(-13,-5), S( -6, 2), S(-2, 4) },
            { S( -9, 3), S( 15,-9), S( -8, 1), S(-4,18) },
            { S(  0, 0), S(  0, 0), S(  0, 0), S( 0, 0) }
        },
        { // Knight
            { S(-143, -97), S(-96,-82), S(-80,-46), S(-73,-14) },
            { S( -83, -69), S(-43,-55), S(-21,-17), S(-10,  9) },
            { S( -71, -50), S(-22,-39), S(  0, -8), S(  9, 28) },
            { S( -25, -41), S( 18,-25), S( 43,  7), S( 47, 38) },
            { S( -26, -46), S( 16,-25), S( 38,  2), S( 50, 41) },
            { S( -11, -55), S( 37,-38), S( 56, -8), S( 71, 27) },
            { S( -62, -64), S(-17,-50), S(  5,-24), S( 14, 13) },
            { S(-195,-110), S(-66,-90), S(-42,-50), S(-29,-13) }
        },
        { // Bishop
            { S(-54,-68), S(-23,-40), S(-35,-46), S(-44,-28) },
            { S(-30,-43), S( 10,-17), S(  2,-23), S( -9, -5) },
            { S(-19,-32), S( 17, -9), S( 11,-13), S(  1,  8) },
            { S(-21,-36), S( 18,-13), S( 11,-15), S(  0,  7) },
            { S(-21,-36), S( 14,-14), S(  6,-17), S( -1,  3) },
            { S(-27,-35), S(  6,-13), S(  2,-10), S( -8,  1) },
            { S(-33,-44), S(  7,-21), S( -4,-22), S(-12, -4) },
            { S(-45,-65), S(-21,-42), S(-29,-46), S(-39,-27) }
        },
        { // Rook
            { S(-25, 0), S(-16, 0), S(-16, 0), S(-9, 0) },
            { S(-21, 0), S( -8, 0), S( -3, 0), S( 0, 0) },
            { S(-21, 0), S( -9, 0), S( -4, 0), S( 2, 0) },
            { S(-22, 0), S( -6, 0), S( -1, 0), S( 2, 0) },
            { S(-22, 0), S( -7, 0), S(  0, 0), S( 1, 0) },
            { S(-21, 0), S( -7, 0), S(  0, 0), S( 2, 0) },
            { S(-12, 0), S(  4, 0), S(  8, 0), S(12, 0) },
            { S(-23, 0), S(-15, 0), S(-11, 0), S(-5, 0) }
        },
        { // Queen
            { S( 0,-70), S(-3,-57), S(-4,-41), S(-1,-29) },
            { S(-4,-58), S( 6,-30), S( 9,-21), S( 8, -4) },
            { S(-2,-39), S( 6,-17), S( 9, -7), S( 9,  5) },
            { S(-1,-29), S( 8, -5), S(10,  9), S( 7, 17) },
            { S(-3,-27), S( 9, -5), S( 8, 10), S( 7, 23) },
            { S(-2,-40), S( 6,-16), S( 8,-11), S(10,  3) },
            { S(-2,-54), S( 7,-30), S( 7,-21), S( 6, -7) },
            { S(-1,-75), S(-4,-54), S(-1,-44), S( 0,-30) }
        },
        { // King
            { S(291, 28), S(344, 76), S(294,103), S(219,112) },
            { S(289, 70), S(329,119), S(263,170), S(205,159) },
            { S(226,109), S(271,164), S(202,195), S(136,191) },
            { S(204,131), S(212,194), S(175,194), S(137,204) },
            { S(177,132), S(205,187), S(143,224), S( 94,227) },
            { S(147,118), S(188,178), S(113,199), S( 70,197) },
            { S(116, 72), S(158,121), S( 93,142), S( 48,161) },
            { S( 94, 30), S(120, 76), S( 78,101), S( 31,111) }
        }
    };
    #undef S
}

namespace PieceSquare
{
    // PSQ[color][piece-type][square] contains [color][piece-type][square] scores.
    Score PSQ[CLR_NO][NONE][SQ_NO];

    // compute_psq_score() computes the incremental scores for the middle
    // game and the endgame. These functions are used to initialize the incremental
    // scores when a new position is set up, and to verify that the scores are correctly
    // updated by do_move and undo_move when the program is running in debug mode.
    Score compute_psq_score (const Position &pos)
    {
        auto psqscore = SCORE_ZERO;
        auto occ = pos.pieces ();
        while (occ != 0)
        {
            auto s = pop_lsq (occ);
            auto p = pos[s];
            psqscore += PSQ[color (p)][ptype (p)][s];
        }
        return psqscore;
    }

    // Initialize PSQ table
    void initialize ()
    {
        for (auto pt = PAWN; pt <= KING; ++pt)
        {
            auto score = mk_score (PieceValues[MG][pt], PieceValues[EG][pt]);
            for (auto s = SQ_A1; s <= SQ_H8; ++s)
            {
                auto psq_bonus = score + HalfPSQ[pt][_rank (s)][std::min (_file (s), F_H - _file (s))];
                PSQ[WHITE][pt][s] = +psq_bonus;
                PSQ[BLACK][pt][~s] = -psq_bonus;
            }
        }
    }
}