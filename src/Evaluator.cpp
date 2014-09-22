#include "Evaluator.h"

#include <iomanip>
#include <sstream>

#include "Material.h"
#include "Pawns.h"
#include "MoveGenerator.h"
#include "Thread.h"

namespace Evaluate {

    using namespace std;
    using namespace BitBoard;
    using namespace MoveGen;
    using namespace Threads;
    using namespace UCI;

    namespace {

        // Struct EvalInfo contains various information computed and collected
        // by the evaluation functions.
        struct EvalInfo
        {
            // Pointers to material and pawn hash table entries
            Material::Entry *mi;
            Pawns   ::Entry *pi;

            // ful_attacked_by[Color][PieceT] contains all squares attacked by a given color and piece type,
            // ful_attacked_by[Color][NONE] contains all squares attacked by the given color.
            Bitboard ful_attacked_by[CLR_NO][TOTL];
            // pin_attacked_by[Color][PieceT] contains all squares attacked by a given color and piece type with pinned removed,
            Bitboard pin_attacked_by[CLR_NO][TOTL];

            // pinneds[Color] contains all the pinned pieces
            Bitboard pinneds[CLR_NO];

            // king_ring[Color] is the zone around the king which is considered
            // by the king safety evaluation. This consists of the squares directly
            // adjacent to the king, and the three (or two, for a king on an edge file)
            // squares two ranks in front of the king. For instance, if black's king
            // is on g8, king_ring[BLACK] is a bitboard containing the squares f8, h8,
            // f7, g7, h7, f6, g6 and h6.
            Bitboard king_ring[CLR_NO];

            // king_ring_attackers_count[color] is the number of pieces of the given color
            // which attack a square in the king_ring of the enemy king.
            int king_ring_attackers_count[CLR_NO];

            // king_ring_attackers_weight[Color] is the sum of the "weight" of the pieces
            // of the given color which attack a square in the king_ring of the enemy king.
            // The weights of the individual piece types are given by the variables KING_ATTACK_WEIGHT[PieceT]
            i32 king_ring_attackers_weight[CLR_NO];

            // king_zone_attacks_count[Color] is the sum of attacks of the pieces
            // of the given color which attack a square directly adjacent to the enemy king.
            // The weights of the individual piece types are given by the variables KING_ATTACK_WEIGHT[PieceT]
            // Pieces which attack more than one square are counted multiple times.
            // For instance, if black's king is on g8 and there's a white knight on g5,
            // this knight adds 2 to king_zone_attacks_count[WHITE].
            u08 king_zone_attacks_count[CLR_NO];

        };

        namespace Tracer {

            // Used for tracing
            enum TermT
            {
                MATERIAL = 6, IMBALANCE, MOBILITY, THREAT, PASSED_PAWN, SPACE, TOTAL, TERM_NO
            };

            Score Terms[CLR_NO][TERM_NO];

            inline void add_term (u08 term, Score w_score, Score b_score = SCORE_ZERO)
            {
                Terms[WHITE][term] = w_score;
                Terms[BLACK][term] = b_score;
            }

            inline void format_row (stringstream &ss, const string &name, u08 term)
            {
                Score score[CLR_NO] =
                {
                    Terms[WHITE][term],
                    Terms[BLACK][term]
                };

                switch (term)
                {
                case MATERIAL: case IMBALANCE: case PAWN: case TOTAL:
                    ss  << setw (15) << name << " |  ----  ---- |  ----  ---- | " << showpos
                        << setw ( 5) << value_to_cp (mg_value (score[WHITE] - score[BLACK])) << " "
                        << setw ( 5) << value_to_cp (eg_value (score[WHITE] - score[BLACK])) << "\n";
                    break;

                default:
                    ss  << setw (15) << name << " | " << showpos
                        << setw ( 5) << value_to_cp (mg_value (score[WHITE])) << " "
                        << setw ( 5) << value_to_cp (eg_value (score[WHITE])) << " | "
                        << setw ( 5) << value_to_cp (mg_value (score[BLACK])) << " "
                        << setw ( 5) << value_to_cp (eg_value (score[BLACK])) << " | "
                        << setw ( 5) << value_to_cp (mg_value (score[WHITE] - score[BLACK])) << " "
                        << setw ( 5) << value_to_cp (eg_value (score[WHITE] - score[BLACK])) << "\n";
                    break;
                }
            }

            string trace (const Position &pos);

        }

        enum EvalWeightT { PIECE_MOBILITY, PAWN_STRUCTURE, PASSED_PAWN, SPACE_ACTIVITY, KING_SAFETY, EVAL_NO };
        
        struct Weight { i32 mg, eg; };
        
        // Evaluation weights, initialized from UCI options
        Weight Weights[EVAL_NO];

    #define S(mg, eg) mk_score (mg, eg)

        // Internal evaluation weights. These are applied on top of the evaluation
        // weights read from UCI parameters. The purpose is to be able to change
        // the evaluation weights while keeping the default values of the UCI
        // parameters at 100, which looks prettier.
        const Score INTERNAL_WEIGHTS[EVAL_NO] =
        {
            S(+289,+344), // Mobility
            S(+233,+201), // Pawn Structure
            S(+221,+273), // Passed Pawns
            S(+ 46,+  0), // Space
            S(+318,+  0)  // King Safety
        };

        // MOBILITY_SCORE[PieceT][attacked] contains bonuses for middle and end game,
        // indexed by piece type and number of attacked squares not occupied by friendly pieces.
        const Score MOBILITY_SCORE[NONE][28] =
        {
            {},
            // Knight
            {
                S(-65,-50), S(-42,-30), S(- 9,-10), S(+ 3,  0), S(+15,+10),
                S(+27,+20), S(+37,+28), S(+42,+31), S(+44,+33)
            },
            // Bishop
            {
                S(-52,-47), S(-28,-23), S(+ 6,+ 1), S(+20,+15), S(+34,+29),
                S(+48,+43), S(+60,+55), S(+68,+63), S(+74,+68), S(+77,+72),
                S(+80,+75), S(+82,+77), S(+84,+79), S(+86,+81)
            },
            // Rook
            {
                S(-47,- 53), S(-31,- 26), S(- 5,   0), S(+ 1,+ 16), S(+ 7,+ 32),
                S(+13,+ 48), S(+18,+ 64), S(+22,+ 80), S(+26,+ 96), S(+29,+109),
                S(+31,+115), S(+33,+119), S(+35,+122), S(+36,+123), S(+37,+124),
            },
            // Queen
            {
                S(-42,-40), S(-28,-23), S(- 5,- 7), S(  0,  0), S(+ 6,+10),
                S(+11,+19), S(+13,+29), S(+18,+38), S(+20,+40), S(+21,+41),
                S(+22,+41), S(+22,+41), S(+22,+41), S(+23,+41), S(+24,+41),
                S(+25,+41), S(+25,+41), S(+25,+41), S(+25,+41), S(+25,+41),
                S(+25,+41), S(+25,+41), S(+25,+41), S(+25,+41), S(+25,+41),
                S(+25,+41), S(+25,+41), S(+25,+41)
            },
            // King
            {
                S(  0,-32), S(  0,-16), S(  0,  0), S(  0,+12), S(  0,+21),
                S(  0,+27), S(  0,+30), S(  0,+31), S(  0,+32)
            }
        };

        enum ThreatT { MINOR, MAJOR, ROYAL, THREAT_NO };
        // THREAT_SCORE[attacking][attacked] contains bonuses according to
        // which piece type attacks which one.
        const Score THREAT_SCORE[THREAT_NO][TOTL] =
        {
            { S(+ 7,+39), S(+24,+49), S(+24,+49), S(+38,+100), S(+41,+104) }, // Minor
            { S(+10,+39), S(+15,+45), S(+15,+45), S(+18,+ 49), S(+24,+ 52) }, // Major
            { S(+ 0,+64), S(+0,+128), S(+ 0,+ 0), S(+ 0,+  0), S(+ 0,+  0)  }  // Royal
        };

        // PAWN_THREATEN_SCORE[PieceT] contains a penalty according to
        // which piece type is attacked by an enemy pawn.
        const Score PAWN_THREATEN_SCORE[NONE] =
        {
            S(+ 0,+ 0), S(+80,+119), S(+80,+119), S(+117,+199), S(+127,+218), S(+ 0,+ 0)
        };
        
        //const Score KNIGHT_PAWNS_SCORE            = S(+ 8,+10); // Penalty for knight with less pawns

        const Score BISHOP_PAWNS_SCORE            = S(+ 8,+12); // Penalty for bishop with more pawns on same color
        const Score BISHOP_TRAPPED_SCORE          = S(+50,+40); // Penalty for bishop trapped with pawns

        const Score MINOR_BEHIND_PAWN_SCORE       = S(+16,+ 0);

        const Score ROOK_ON_OPENFILE_SCORE        = S(+43,+21); // Bonus for rook on open file
        const Score ROOK_ON_SEMIOPENFILE_SCORE    = S(+19,+10); // Bonus for rook on semi-open file
        //const Score ROOK_DOUBLED_ON_OPENFILE_SCORE    = S(+23,+10); // Bonus for doubled rook on open file
        //const Score ROOK_DOUBLED_ON_SEMIOPENFILE_SCORE= S(+12,+ 6); // Bonus for doubled rook on semi-open file
        const Score ROOK_ON_PAWNS_SCORE           = S(+10,+28); // Bonus for rook on pawns
        //const Score ROOK_ON_7THR_SCORE            = S(+ 3,+ 6);
        const Score ROOK_TRAPPED_SCORE            = S(+92,+ 0); // Penalty for rook trapped
        
        //const Score QUEEN_ON_PAWNS_SCORE          = S(+ 4,+20);
        //const Score QUEEN_ON_7THR_SCORE           = S(+ 3,+ 8);

        const Score HANGING_SCORE                 = S(+23,+20); // Bonus for each enemy hanging piece       

    #undef S

    #define V Value

        // OUTPOST_VALUE[Square] contains bonus of outpost,
        // indexed by square (from white's point of view).
        const Value OUTPOST_VALUE[2][SQ_NO] =
        {   // A      B      C      D      E      F      G      H
            // Knights
           {V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0),
            V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0),
            V( 0), V( 0), V( 4), V( 8), V( 8), V( 4), V( 0), V( 0),
            V( 0), V( 4), V(17), V(26), V(26), V(17), V( 4), V( 0),
            V( 0), V( 8), V(26), V(35), V(35), V(26), V( 8), V( 0),
            V( 0), V( 4), V(17), V(17), V(17), V(17), V( 4), V( 0),
            V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0),
            V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0)},
            // Bishops
           {V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0),
            V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0),
            V( 0), V( 0), V( 5), V( 5), V( 5), V( 5), V( 0), V( 0),
            V( 0), V( 5), V(10), V(10), V(10), V(10), V( 5), V( 0),
            V( 0), V(10), V(21), V(21), V(21), V(21), V(10), V( 0),
            V( 0), V( 5), V( 8), V( 8), V( 8), V( 8), V( 5), V( 0),
            V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0),
            V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0), V( 0)}
        };

    #undef V

        // The SPACE_MASK[Color] contains the area of the board which is considered
        // by the space evaluation. In the middle game, each side is given a bonus
        // based on how many squares inside this area are safe and available for
        // friendly minor pieces.
        const Bitboard SPACE_MASK[CLR_NO] =
        {
            ((FC_bb | FD_bb | FE_bb | FF_bb) & (R2_bb | R3_bb | R4_bb)),
            ((FC_bb | FD_bb | FE_bb | FF_bb) & (R7_bb | R6_bb | R5_bb))
        };

        // King danger constants and variables. The king danger scores are taken
        // from the KING_DANGER[]. Various little "meta-bonuses" measuring
        // the strength of the enemy attack are added up into an integer, which
        // is used as an index to KING_DANGER[].
        const u08 MAX_ATTACK_UNITS = 100;
        // KING_DANGER[attack_units] contains the king danger weighted score
        // indexed by a calculated integer number.
        Score KING_DANGER[MAX_ATTACK_UNITS];

        // KING_ATTACK_WEIGHT[PieceT] contains king attack weights by piece type
        const i32   KING_ATTACK_WEIGHT[NONE] = { + 1, + 4, + 4, + 6, +10, 0 };

        // Bonuses for safe checks
        const i32    SAFE_CHECK_WEIGHT[NONE] = { + 0, + 3, + 2, + 8, +12, 0 };

        // Bonuses for contact safe checks
        const i32 CONTACT_CHECK_WEIGHT[NONE] = { + 0, + 0, + 3, +16, +24, 0 };

        const ScaleFactor PAWN_SPAN_SCALE[2] = { ScaleFactor(38), ScaleFactor(56) };

        // weight_option() computes the value of an evaluation weight,
        // by combining UCI-configurable weights with an internal weight.
        inline Weight weight_option (i32 opt_value, const Score &internal_weight)
        {
            Weight weight =
            {
                max (opt_value + 1000, 1) * mg_value (internal_weight) / 1000,
                max (opt_value + 1000, 1) * eg_value (internal_weight) / 1000
            };
            return weight;
        }

        // apply_weight() weighs 'score' by factor 'weight' trying to prevent overflow
        inline Score apply_weight (Score score, const Weight &weight)
        {
            return mk_score (
                mg_value (score) * weight.mg / 0x100,
                eg_value (score) * weight.eg / 0x100);
        }

        //  --- init evaluation info --->
        template<Color C>
        // init_evaluation<>() initializes king bitboards for given color adding
        // pawn attacks. To be done at the beginning of the evaluation.
        inline void init_evaluation (const Position &pos, EvalInfo &ei)
        {
            const Color  C_  = WHITE == C ? BLACK : WHITE;
            const Delta PULL = WHITE == C ? DEL_S : DEL_N;

            Square ek_sq = pos.king_sq (C_);

            ei.pinneds[C] = pos.pinneds (C);
            ei.ful_attacked_by[C][NONE] |= ei.ful_attacked_by[C][PAWN] = ei.pi->pawns_attacks[C];
            ei.pin_attacked_by[C][NONE] |= ei.pin_attacked_by[C][PAWN] = ei.pi->pawns_attacks[C];
            
            Bitboard king_attacks        = PIECE_ATTACKS[KING][ek_sq];
            ei.ful_attacked_by[C_][KING] = king_attacks;
            ei.pin_attacked_by[C_][KING] = king_attacks;
            
            

            // Init king safety tables only if going to use them
            Rank ekr = rel_rank (C_, ek_sq);
            ei.king_ring[C_] = king_attacks | (DIST_RINGS_bb[ek_sq][1] &
                                                        (ekr < R_4 ? PAWN_PASS_SPAN[C_][ek_sq] :
                                                            ekr < R_6 ? (PAWN_PASS_SPAN[C_][ek_sq]|rank_bb (ek_sq)) :
                                                                        (PAWN_PASS_SPAN[C_][ek_sq]|PAWN_PASS_SPAN[C][ek_sq]|rank_bb (ek_sq))
                                                        ));

            if (king_attacks & ei.pin_attacked_by[C][PAWN])
            {
                Bitboard attackers = pos.pieces<PAWN> (C) & shift_del<PULL> ((king_attacks|DIST_RINGS_bb[ek_sq][1]) & (rank_bb (ek_sq)|rank_bb (ek_sq + PULL)));
                ei.king_ring_attackers_count [C] = more_than_one (attackers) ? pop_count<MAX15> (attackers) : 1;
                ei.king_ring_attackers_weight[C] = ei.king_ring_attackers_count [C]*KING_ATTACK_WEIGHT[PAWN];
            }
            else
            {
                ei.king_ring_attackers_count [C ] = 0;
                ei.king_ring_attackers_weight[C ] = 0;
            }

            ei.king_zone_attacks_count   [C ] = 0;
        }

        template<Color C, PieceT PT, bool Trace>
        // evaluate_pieces<>() assigns bonuses and penalties to the pieces of a given color except PAWN
        inline Score evaluate_pieces (const Position &pos, EvalInfo &ei, Bitboard mobility_area, Score &mobility)
        {
            const Color  C_      = WHITE == C ? BLACK : WHITE;
            const Delta PUSH     = WHITE == C ? DEL_N : DEL_S;
            const Square fk_sq   = pos.king_sq (C);
            const Bitboard occ   = pos.pieces ();
            const Bitboard pinneds = ei.pinneds[C];

            ei.ful_attacked_by[C][PT] = U64(0);
            ei.pin_attacked_by[C][PT] = U64(0);

            Score score = SCORE_ZERO;
            
            u08 king_ring_attackers_count = 0;
            u08 king_zone_attacks_count   = 0;

            const Square *pl = pos.list<PT> (C);
            Square s;
            while ((s = *pl++) != SQ_NO)
            {
                const File f = _file (s);
                const Rank r = rel_rank (C, s);

                // Find attacked squares, including x-ray attacks for bishops and rooks
                Bitboard attacks =
                    (BSHP == PT) ? attacks_bb<BSHP> (s, (occ ^ pos.pieces (C, QUEN, BSHP)) | pinneds) :
                    (ROOK == PT) ? attacks_bb<ROOK> (s, (occ ^ pos.pieces (C, QUEN, ROOK)) | pinneds) :
                    (QUEN == PT) ? attacks_bb<BSHP> (s, occ) | attacks_bb<ROOK> (s, occ) :
                                   PIECE_ATTACKS[PT][s];

                ei.ful_attacked_by[C][PT] |= attacks;

                if (ei.king_ring[C_] & attacks)
                {
                    ++king_ring_attackers_count;
                    Bitboard zone_attacks = ei.ful_attacked_by[C_][KING] & attacks;
                    if (zone_attacks) king_zone_attacks_count += more_than_one (zone_attacks) ? pop_count<MAX15> (zone_attacks) : 1;
                }

                // Decrease score if attacked by an enemy pawn. Remaining part
                // of threat evaluation must be done later when have full attack info.
                if (ei.pin_attacked_by[C_][PAWN] & s)
                {
                    score -= PAWN_THREATEN_SCORE[PT];
                }

                // Special extra evaluation for pieces
                
                if (NIHT == PT || BSHP == PT)
                {
                if (NIHT == PT)
                {
                    // Penalty for knight when there are few friendly pawns
                    //score -= KNIGHT_PAWNS_SCORE * max (5 - pos.count<PAWN> (C), 0);
                    //score -= KNIGHT_PAWNS_SCORE * max (5 - ei.pi->pawns_on_center<C> (), 0);

                    // Outpost bonus for Knight
                    if (!(pos.pieces<PAWN> (C_) & PAWN_ATTACKS[C][s]))
                    {
                        // Initial bonus based on square
                        Value value = OUTPOST_VALUE[0][rel_sq (C, s)];

                        // Increase bonus if supported by pawn, especially if the opponent has
                        // no minor piece which can exchange the outpost piece.
                        if (value != VALUE_ZERO)
                        {
                            // Supporting pawns
                            if (ei.pin_attacked_by[C][PAWN] & s) //pos.pieces<PAWN> (C) & PAWN_ATTACKS[C_][s]
                            {
                                // If attacked by enemy Knights or Bishops
                                if (  pos.pieces<NIHT> (C_) & PIECE_ATTACKS[NIHT][s]
                                   || pos.pieces<BSHP> (C_) & PIECE_ATTACKS[BSHP][s]
                                   )
                                {
                                    value *= 1.10f;
                                }
                                else
                                {
                                    // If there are enemy Knights or Bishops
                                    if (pos.pieces<NIHT> (C_) || (pos.pieces<BSHP> (C_) & squares_of_color (s)))
                                    {
                                        value *= 1.50f;
                                    }
                                    // If there are no enemy Knights or Bishops
                                    else
                                    {
                                        value *= 2.50f;
                                    }
                                }
                                //// Increase bonus more if the piece blocking enemy pawn
                                //if (pos[s + pawn_push (C)] == (C_|PAWN))
                                //{
                                //    bonus += i32 (bonus)*0.5;
                                //}
                                //// Increase bonus more if the piece blocking enemy semiopen file
                                //if (ei.pi->semiopen_file<C_> (f))
                                //{
                                //    value *= 1.50f;
                                //}
                            }

                            score += mk_score (value * 2, value / 2);
                        }
                    }
                }

                if (BSHP == PT)
                {
                    score -= BISHOP_PAWNS_SCORE * ei.pi->pawns_on_squarecolor<C> (s);

                    // Outpost bonus for Bishop
                    if (!(pos.pieces<PAWN> (C_) & PAWN_ATTACKS[C][s]))
                    {
                        // Initial bonus based on square
                        Value value = OUTPOST_VALUE[1][rel_sq (C, s)];

                        // Increase bonus if supported by pawn, especially if the opponent has
                        // no minor piece which can exchange the outpost piece.
                        if (value != VALUE_ZERO)
                        {
                            // Supporting pawns
                            if (ei.pin_attacked_by[C][PAWN] & s) // pos.pieces<PAWN> (C) & PAWN_ATTACKS[C_][s]
                            {
                                // If attacked by enemy Knights or Bishops
                                if (  pos.pieces<NIHT> (C_) & PIECE_ATTACKS[NIHT][s]
                                   || pos.pieces<BSHP> (C_) & PIECE_ATTACKS[BSHP][s]
                                   )
                                {
                                    value *= 1.10f;
                                }
                                else
                                {
                                    // If there are enemy Knights or Bishops
                                    if (pos.pieces<NIHT> (C_) || (pos.pieces<BSHP> (C_) & squares_of_color (s)))
                                    {
                                        value *= 1.50f;
                                    }
                                    // If there are no enemy Knights or Bishops
                                    else
                                    {
                                        value *= 2.50f;
                                    }
                                }
                                //// Increase bonus more if the piece blocking enemy pawn
                                //if (pos[s + pawn_push (C)] == (C_|PAWN))
                                //{
                                //    bonus += i32 (bonus)*0.5;
                                //}
                                //// Increase bonus more if the piece blocking enemy semiopen file
                                //if (ei.pi->semiopen_file<C_> (f))
                                //{
                                //    value *= 1.50f;
                                //}
                            }

                            score += mk_score (value * 2, value / 2);
                        }
                    }

                    // An important Chess960 pattern: A cornered bishop blocked by a friendly
                    // pawn diagonally in front of it is a very serious problem, especially
                    // when that pawn is also blocked.
                    // Penalty for a bishop on a1/h1 (a8/h8 for black) which is trapped by
                    // a friendly pawn on b2/g2 (b7/g7 for black).
                    if (pos.chess960 ())
                    {
                        if ((FILE_EDGE_bb & R1_bb) & rel_sq (C, s))
                        {
                            const Piece own_pawn = (C | PAWN);
                            Delta del = PUSH + ((F_A == f) ? DEL_E : DEL_W);
                            if (pos[s + del] == own_pawn)
                            {
                                score -= BISHOP_TRAPPED_SCORE *
                                    ( (pos[s + del + PUSH]!=EMPTY) ? 4 :
                                      (pos[s + del + del] == own_pawn) ? 2 : 1);
                            }
                        }
                    }
                }

                // Bishop or knight behind a pawn
                if (  r < R_5
                   && pos.pieces<PAWN> () & (s + PUSH)
                   )
                {
                    score += MINOR_BEHIND_PAWN_SCORE;
                }
                }

                if (ROOK == PT)
                {
                    
                    if (R_4 < r)
                    {
                        // Rook piece attacking enemy pawns on the same rank/file
                        const Bitboard rook_on_pawns = pos.pieces<PAWN> (C_) & PIECE_ATTACKS[ROOK][s];
                        if (rook_on_pawns) score += ROOK_ON_PAWNS_SCORE * (more_than_one (rook_on_pawns) ? pop_count<MAX15> (rook_on_pawns) : 1);
                    
                        //if (  R_7 == r
                        //   && R_8 == rel_rank (C, pos.king_sq (C_))
                        //   )
                        //{
                        //    score += ROOK_ON_7THR_SCORE;
                        //}
                    }

                    // Give a bonus for a rook on a open or semi-open file
                    if (ei.pi->semiopen_file<C > (f))
                    {
                        score += (ei.pi->semiopen_file<C_> (f)) ?
                                 ROOK_ON_OPENFILE_SCORE :
                                 ROOK_ON_SEMIOPENFILE_SCORE;
                        
                        //// Give more if the rook is doubled
                        //if (pos.count<ROOK> (C) > 1 && FILE_bb[f] & pos.pieces<ROOK> (C) & attacks)
                        //{
                        //    score += (ei.pi->semiopen_file<C_> (f)) ?
                        //             ROOK_DOUBLED_ON_OPENFILE_SCORE :
                        //             ROOK_DOUBLED_ON_SEMIOPENFILE_SCORE;
                        //}
                        
                    }
                }

                if (pinneds & s)
                {
                    attacks &= RAY_LINE_bb[fk_sq][s];
                }
                ei.pin_attacked_by[C][PT] |= attacks;

                if (QUEN == PT)
                {
                    //if (R_4 < r)
                    //{
                    //    // Queen piece attacking enemy pawns on the same rank/file
                    //    const Bitboard queen_on_pawns = pos.pieces<PAWN> (C_) & PIECE_ATTACKS[QUEN][s];
                    //    if (queen_on_pawns) score += QUEEN_ON_PAWNS_SCORE * (more_than_one (queen_on_pawns) ? pop_count<MAX15> (queen_on_pawns) : 1);
                    //
                    //    if (  R_7 == r
                    //       && R_8 == rel_rank (C, pos.king_sq (C_))
                    //       )
                    //    {
                    //        score += QUEEN_ON_7THR_SCORE;
                    //    }
                    //}

                    attacks &= (~ei.pin_attacked_by[C_][NONE]|ei.pin_attacked_by[C][NONE]);
                }

                Bitboard mobile = attacks & mobility_area;
                i32 mob = mobile != U64(0) ? pop_count<QUEN != PT ? MAX15 : FULL> (mobile) : 0;
                mobility += MOBILITY_SCORE[PT][mob];

                if (ROOK == PT)
                {
                    if (mob <= 3 && !ei.pi->semiopen_file<C > (f))
                    {
                        const File kf = _file (fk_sq);
                        const Rank kr = rel_rank (C, fk_sq);
                        // Penalize rooks which are trapped by a king.
                        // Penalize more if the king has lost its castling capability.
                        if (  (kf < F_E) == (f < kf)
                           && (kr == R_1 || kr == r)
                           && !ei.pi->semiopen_side<C> (kf, f < kf)
                           )
                        {
                            score -= (ROOK_TRAPPED_SCORE - mk_score (22 * mob, 0)) * (1 + !pos.can_castle (C));
                        }
                    }
                }
            }

            if (king_ring_attackers_count > 0)
            {
                ei.king_ring_attackers_count [C] += king_ring_attackers_count;
                ei.king_ring_attackers_weight[C] += king_ring_attackers_count*KING_ATTACK_WEIGHT[PT];
                ei.king_zone_attacks_count   [C] += king_zone_attacks_count;
            }

            if (Trace)
            {
                Tracer::Terms[C][PT] = score;
            }

            return score;
        }
        //  --- init evaluation info <---

        template<Color C, bool Trace>
        // evaluate_king<>() assigns bonuses and penalties to a king of a given color
        inline Score evaluate_king (const Position &pos, const EvalInfo &ei)
        {
            const Color C_ = WHITE == C ? BLACK : WHITE;

            const Square fk_sq = pos.king_sq (C);

            // King shelter and enemy pawns storm
            ei.pi->evaluate_king_pawn_safety<C> (pos);

            Value value = VALUE_ZERO;
            Rank kr = rel_rank (C, fk_sq);
            if (kr <= R_4)
            {
                // If can castle use the value after the castle if is bigger
                if (kr == R_1 && pos.can_castle (C))
                {
                    value = ei.pi->shelter_storm[C][CS_NO];

                    if (    pos.can_castle (Castling<C, CS_K>::Right)
                       && ! pos.castle_impeded (Castling<C, CS_K>::Right)
                       && !(pos.king_path (Castling<C, CS_K>::Right) & ei.ful_attacked_by[C_][NONE])
                       )
                    {
                        value = max (value, ei.pi->shelter_storm[C][CS_K]);
                    }
                    if (    pos.can_castle (Castling<C, CS_Q>::Right)
                       && ! pos.castle_impeded (Castling<C, CS_Q>::Right)
                       && !(pos.king_path (Castling<C, CS_Q>::Right) & ei.ful_attacked_by[C_][NONE])
                       )
                    {
                        value = max (value, ei.pi->shelter_storm[C][CS_Q]);
                    }
                }
                else
                {
                    value = ei.pi->shelter_storm[C][CS_NO];
                }
            }

            Score score = mk_score (value, -0x10 * ei.pi->min_kp_dist[C]);
            
            // Main king safety evaluation
            if (ei.king_ring_attackers_count[C_] > 0)
            {
                const Bitboard occ = pos.pieces ();

                // Find the attacked squares around the king which has no defenders
                // apart from the king itself
                Bitboard undefended =
                    ei.ful_attacked_by[C ][KING] // King-zone
                  & ei.ful_attacked_by[C_][NONE]
                  & ~( ei.pin_attacked_by[C ][PAWN]
                     | ei.pin_attacked_by[C ][NIHT]
                     | ei.pin_attacked_by[C ][BSHP]
                     | ei.pin_attacked_by[C ][ROOK]
                     | ei.pin_attacked_by[C ][QUEN]);

                // Initialize the 'attack_units' variable, which is used later on as an
                // index to the KING_DANGER[] array. The initial value is based on the
                // number and types of the enemy's attacking pieces, the number of
                // attacked and undefended squares around our king, and the quality of
                // the pawn shelter (current 'mg score' value).
                i32 attack_units =
                    + min (ei.king_ring_attackers_count[C_] * ei.king_ring_attackers_weight[C_]/4, 20) // King-ring attacks
                    +  3 * ei.king_zone_attacks_count[C_] // King-zone attacks
                    +  3 * (undefended != U64(0) ? more_than_one (undefended) ? pop_count<MAX15> (undefended) : 1 : 0) // King-zone undefended pieces
                    +  2 * (ei.pinneds[C] != U64(0)) // King pinned piece
                    - 15 * (pos.count<QUEN>(C_) == 0)
                    - i32(value) / 32;

                // Undefended squares around king not occupied by enemy's
                undefended &= ~pos.pieces (C_);
                if (undefended != U64(0))
                {
                    Bitboard undefended_attacked;
                    if (pos.count<QUEN> (C_) > 0)
                    {
                        // Analyse enemy's safe queen contact checks.
                        // Undefended squares around the king attacked by enemy queen...
                        undefended_attacked = undefended & ei.pin_attacked_by[C_][QUEN];
                        Bitboard unsafe = ei.ful_attacked_by[C_][PAWN]|ei.ful_attacked_by[C_][NIHT]|ei.ful_attacked_by[C_][BSHP]|ei.ful_attacked_by[C_][ROOK]|ei.ful_attacked_by[C_][KING];
                        while (undefended_attacked != U64(0))
                        {
                            Square sq = pop_lsq (undefended_attacked);
                            Bitboard attackers = U64(0);
                            if (  (unsafe & sq)
                               || (  pos.count<QUEN> (C_) > 1
                                  && (attackers = pos.pieces<QUEN> (C_) & (PIECE_ATTACKS[BSHP][sq]|PIECE_ATTACKS[ROOK][sq])) != U64(0)
                                  && more_than_one (attackers)
                                  && (attackers = attackers & (attacks_bb<BSHP> (sq, occ ^ pos.pieces<QUEN> (C_))|attacks_bb<ROOK> (sq, occ ^ pos.pieces<QUEN> (C_)))) != U64(0)
                                  && more_than_one (attackers)
                                  )
                               )
                            {
                                attack_units += CONTACT_CHECK_WEIGHT[QUEN];
                            }
                        }
                    }
                    if (pos.count<ROOK> (C_) > 0)
                    {
                        // Analyse enemy's safe rook contact checks.
                        // Undefended squares around the king attacked by enemy rooks...
                        undefended_attacked = undefended & ei.pin_attacked_by[C_][ROOK];
                        // Consider only squares where the enemy rook gives check
                        undefended_attacked &= PIECE_ATTACKS[ROOK][fk_sq];
                        Bitboard unsafe = ei.ful_attacked_by[C_][PAWN]|ei.ful_attacked_by[C_][NIHT]|ei.ful_attacked_by[C_][BSHP]|ei.ful_attacked_by[C_][QUEN]|ei.ful_attacked_by[C_][KING];
                        while (undefended_attacked != U64(0))
                        {
                            Square sq = pop_lsq (undefended_attacked);
                            Bitboard attackers = U64(0);
                            if (  (unsafe & sq)
                               || (  pos.count<ROOK> (C_) > 1
                                  && (attackers = pos.pieces<ROOK> (C_) & PIECE_ATTACKS[ROOK][sq]) != U64(0)
                                  && more_than_one (attackers)
                                  && (attackers = attackers & attacks_bb<ROOK> (sq, occ ^ pos.pieces<ROOK> (C_))) != U64(0)
                                  && more_than_one (attackers)
                                  )
                               )
                            {
                                attack_units += CONTACT_CHECK_WEIGHT[ROOK];
                            }
                        }
                    }
                    if (pos.count<BSHP> (C_) > 0)
                    {
                        // Analyse enemy's safe rook contact checks.
                        // Undefended squares around the king attacked by enemy bishop...
                        undefended_attacked = undefended & ei.pin_attacked_by[C_][BSHP];
                        // Consider only squares where the enemy bishop gives check
                        undefended_attacked &= PIECE_ATTACKS[BSHP][fk_sq];
                        Bitboard unsafe = ei.ful_attacked_by[C_][PAWN]|ei.ful_attacked_by[C_][NIHT]|ei.ful_attacked_by[C_][ROOK]|ei.ful_attacked_by[C_][QUEN]|ei.ful_attacked_by[C_][KING];
                        while (undefended_attacked != U64(0))
                        {
                            Square sq = pop_lsq (undefended_attacked);
                            Bitboard attackers = U64(0);
                            if (  (unsafe & sq)
                               || (  pos.count<BSHP> (C_) > 1
                                  && (attackers = pos.pieces<BSHP> (C_) & squares_of_color (sq)) != U64(0)
                                  && more_than_one (attackers)
                                  && (attackers = attackers & PIECE_ATTACKS[BSHP][sq]) != U64(0)
                                  && more_than_one (attackers)
                                  && (attackers = attackers & attacks_bb<BSHP> (sq, occ ^ pos.pieces<BSHP> (C_))) != U64(0)
                                  && more_than_one (attackers)
                                  )
                               )
                            {
                                attack_units += CONTACT_CHECK_WEIGHT[BSHP];
                            }
                        }
                    }
                    // Knight can't give contact check but safe distance check
                }

                // Analyse the enemies safe distance checks for sliders and knights
                const Bitboard safe_area = ~(pos.pieces (C_) | ei.pin_attacked_by[C][NONE]);
                const Bitboard rook_check = (ei.pin_attacked_by[C_][ROOK]|ei.pin_attacked_by[C_][QUEN]) != U64(0) ? attacks_bb<ROOK> (fk_sq, occ) & safe_area : U64(0);
                const Bitboard bshp_check = (ei.pin_attacked_by[C_][BSHP]|ei.pin_attacked_by[C_][QUEN]) != U64(0) ? attacks_bb<BSHP> (fk_sq, occ) & safe_area : U64(0);

                // Enemies safe-checks
                Bitboard safe_check;
                // Queens safe-checks
                safe_check = (rook_check | bshp_check) & ei.pin_attacked_by[C_][QUEN];
                if (safe_check != U64(0)) attack_units += SAFE_CHECK_WEIGHT[QUEN] * (more_than_one (safe_check) ? pop_count<MAX15> (safe_check) : 1);
                // Rooks safe-checks
                safe_check = rook_check & ei.pin_attacked_by[C_][ROOK];
                if (safe_check != U64(0)) attack_units += SAFE_CHECK_WEIGHT[ROOK] * (more_than_one (safe_check) ? pop_count<MAX15> (safe_check) : 1);
                // Bishops safe-checks
                safe_check = bshp_check & ei.pin_attacked_by[C_][BSHP];
                if (safe_check != U64(0)) attack_units += SAFE_CHECK_WEIGHT[BSHP] * (more_than_one (safe_check) ? pop_count<MAX15> (safe_check) : 1);
                // Knights safe-checks
                safe_check = PIECE_ATTACKS[NIHT][fk_sq] & safe_area & ei.pin_attacked_by[C_][NIHT];
                if (safe_check != U64(0)) attack_units += SAFE_CHECK_WEIGHT[NIHT] * (more_than_one (safe_check) ? pop_count<MAX15> (safe_check) : 1);

                //// Penalty for pinned pieces which not defended by a pawn
                //if (ei.pinned_pieces[C] & ~ei.pin_attacked_by[C][PAWN])
                //{
                //    attack_units += 1;
                //}

                // To index KING_DANGER[] attack_units must be in [0, MAX_ATTACK_UNITS-1] range
                attack_units = min (max (attack_units, 0), MAX_ATTACK_UNITS-1);

                // Finally, extract the king danger score from the KING_DANGER[]
                // array and subtract the score from evaluation.
                score -= KING_DANGER[attack_units];

                //if (ei.king_zone_attacks_count[C_] >= 3)
                //{
                //    // King mobility is good in the endgame
                //    Bitboard mobile = ei.ful_attacked_by[C][KING] & ~(pos.pieces<PAWN> (C) | ei.ful_attacked_by[C_][NONE]);
                //    u08 mob = mobile != U64(0) ? more_than_one (mobile) ? pop_count<MAX15> (mobile) : 1 : 0;
                //    if (mob < 3) score -= mk_score (0, 10 * (9 - mob*mob));
                //}
            }

            if (Trace)
            {
                Tracer::Terms[C][KING] = score;
            }

            return score;
        }

        template<Color C, bool Trace>
        // evaluate_threats<>() assigns bonuses according to the type of attacking piece
        // and the type of attacked one.
        inline Score evaluate_threats (const Position &pos, const EvalInfo &ei)
        {
            const Color C_ = WHITE == C ? BLACK : WHITE;

            Bitboard enemies = pos.pieces (C_);

            // Enemies protected by pawn and attacked by minors
            Bitboard protected_pieces = 
                   (enemies ^ pos.pieces<PAWN> (C_))
                &  ei.pin_attacked_by[C_][PAWN]
                & (ei.pin_attacked_by[C ][NIHT]|ei.pin_attacked_by[C ][BSHP]);

            // Enemies not defended by pawn and attacked by any piece
            Bitboard weak_pieces = 
                   enemies
                & ~ei.pin_attacked_by[C_][PAWN]
                &  ei.pin_attacked_by[C ][NONE];
            
            Score score = SCORE_ZERO;

            if (protected_pieces != U64(0))
            {
                score += ((protected_pieces & pos.pieces<QUEN> ()) ? THREAT_SCORE[MINOR][QUEN] :
                          (protected_pieces & pos.pieces<ROOK> ()) ? THREAT_SCORE[MINOR][ROOK] :
                          (protected_pieces & pos.pieces<BSHP> ()) ? THREAT_SCORE[MINOR][BSHP] :
                                                                      THREAT_SCORE[MINOR][NIHT]);
            }

            // Add a bonus according if the attacking pieces are minor or major
            if (weak_pieces != U64(0))
            {
                // Threaten pieces
                Bitboard threaten_pieces;
                // Threaten pieces by Minor
                threaten_pieces = weak_pieces & (ei.pin_attacked_by[C][NIHT]|ei.pin_attacked_by[C][BSHP]);
                if (threaten_pieces != U64(0))
                {
                    score += ((threaten_pieces & pos.pieces<QUEN> ()) ? THREAT_SCORE[MINOR][QUEN] :
                              (threaten_pieces & pos.pieces<ROOK> ()) ? THREAT_SCORE[MINOR][ROOK] :
                              (threaten_pieces & pos.pieces<BSHP> ()) ? THREAT_SCORE[MINOR][BSHP] :
                              (threaten_pieces & pos.pieces<NIHT> ()) ? THREAT_SCORE[MINOR][NIHT] :
                                                                        THREAT_SCORE[MINOR][PAWN]);
                }
                // Threaten pieces by Major
                threaten_pieces = weak_pieces & (ei.pin_attacked_by[C][ROOK]|ei.pin_attacked_by[C][QUEN]);
                if (threaten_pieces != U64(0))
                {
                    score += ((threaten_pieces & pos.pieces<QUEN> ()) ? THREAT_SCORE[MAJOR][QUEN] :
                              (threaten_pieces & pos.pieces<ROOK> ()) ? THREAT_SCORE[MAJOR][ROOK] :
                              (threaten_pieces & pos.pieces<BSHP> ()) ? THREAT_SCORE[MAJOR][BSHP] :
                              (threaten_pieces & pos.pieces<NIHT> ()) ? THREAT_SCORE[MAJOR][NIHT] :
                                                                        THREAT_SCORE[MAJOR][PAWN]);
                }

                // Threaten pieces by King
                threaten_pieces = weak_pieces & ei.ful_attacked_by[C][KING];
                if (threaten_pieces != U64(0)) score += more_than_one (threaten_pieces) ? THREAT_SCORE[ROYAL][1] : THREAT_SCORE[ROYAL][0]; 

                // Hanging pieces
                Bitboard hanging_pieces = weak_pieces & ~ei.pin_attacked_by[C_][NONE];
                if (hanging_pieces != U64(0)) score += HANGING_SCORE * (more_than_one (hanging_pieces) ? pop_count<MAX15> (hanging_pieces) : 1);
            }

            if (Trace)
            {
                Tracer::Terms[C][Tracer::THREAT] = score;
            }

            return score;
        }

        template<Color C>
        // evaluate_passed_pawns<>() evaluates the passed pawns of the given color
        inline Score evaluate_passed_pawns (const Position &pos, const EvalInfo &ei)
        {
            const Color C_   = WHITE == C ? BLACK : WHITE;
            const Delta PUSH = WHITE == C ? DEL_N : DEL_S;

            const i32 nonpawn_count_sum = pos.count<NONPAWN> (C ) + pos.count<NONPAWN> (C_) + 1;

            Score score = SCORE_ZERO;

            Bitboard passed_pawns = ei.pi->passed_pawns[C];
            while (passed_pawns != U64(0))
            {
                const Square s = pop_lsq (passed_pawns);
                ASSERT (pos.passed_pawn (C, s));
                
                const Rank pr = rel_rank (C, s);

                i32 r = max (i32(pr) - i32(R_2), 1);
                i32 rr = r * (r - 1);

                // Base bonus depends on rank
                Value mg_value = Value(17 * (rr));
                Value eg_value = Value(07 * (rr + r + 1));

                if (rr != 0)
                {
                    Square fk_sq = pos.king_sq (C );
                    Square ek_sq = pos.king_sq (C_);
                    Square block_sq = s + PUSH;

                    // Adjust bonus based on kings proximity
                    eg_value += (5 * rr * SQR_DIST[ek_sq][block_sq])
                             -  (2 * rr * SQR_DIST[fk_sq][block_sq]);
                    // If block square is not the queening square then consider also a second push
                    if (rel_rank (C, block_sq) != R_8)
                    {
                        eg_value -= (1 * rr * SQR_DIST[fk_sq][block_sq + PUSH]);
                    }

                    bool not_pinned = true;
                    if (ei.pinneds[C] & s)
                    {
                        // Only one real pinner exist other are fake pinner
                        Bitboard pawn_pinners =
                            ( (PIECE_ATTACKS[ROOK][fk_sq] & pos.pieces (QUEN, ROOK))
                            | (PIECE_ATTACKS[BSHP][fk_sq] & pos.pieces (QUEN, BSHP))
                            ) &  pos.pieces (C_) & RAY_LINE_bb[fk_sq][s];

                        not_pinned = BETWEEN_SQRS_bb[fk_sq][scan_lsq (pawn_pinners)] & block_sq;
                    }

                    if (not_pinned)
                    {
                        Bitboard pawnR7_capture = U64(0);

                        // If the pawn is free to advance, increase bonus
                        if (  pos.empty (block_sq)
                           || (pr == R_7 && (pawnR7_capture = pos.pieces (C_) & PAWN_ATTACKS[C][s]) != 0)
                           )
                        {
                            // Squares to queen
                            const Bitboard front_squares = FRONT_SQRS_bb[C ][s];
                            const Bitboard queen_squares = pr == R_7 ? front_squares | pawnR7_capture : front_squares;
                            const Bitboard back_squares  = FRONT_SQRS_bb[C_][s];
                            const Bitboard occ           = pos.pieces ();

                            Bitboard unsafe_squares;
                            // If there is an enemy rook or queen attacking the pawn from behind,
                            // add all X-ray attacks by the rook or queen. Otherwise consider only
                            // the squares in the pawn's path attacked or occupied by the enemy.
                            if (  (  (back_squares & pos.pieces<ROOK> (C_) && ei.pin_attacked_by[C_][ROOK] & s)
                                  || (back_squares & pos.pieces<QUEN> (C_) && ei.pin_attacked_by[C_][QUEN] & s)
                                  )
                               && back_squares & pos.pieces (C_, ROOK, QUEN) & attacks_bb<ROOK> (s, occ)
                               )
                            {
                                unsafe_squares = pr == R_7 ?
                                                 front_squares | (queen_squares & ei.pin_attacked_by[C_][NONE]) :
                                                 front_squares;
                            }
                            else
                            {
                                unsafe_squares = pr == R_7 ?
                                                 (front_squares & occ) | (queen_squares & ei.pin_attacked_by[C_][NONE]) :
                                                 front_squares & (occ | ei.pin_attacked_by[C_][NONE]);
                            }

                            Bitboard safe_squares;
                            if (  (  (back_squares & pos.pieces<ROOK> (C ) && ei.pin_attacked_by[C ][ROOK] & s)
                                  || (back_squares & pos.pieces<QUEN> (C ) && ei.pin_attacked_by[C ][QUEN] & s)
                                  )
                               && back_squares & pos.pieces (C , ROOK, QUEN) & attacks_bb<ROOK> (s, occ)
                               )
                            {
                                safe_squares = front_squares;
                            }
                            else
                            {
                                if (pr == R_7)
                                {
                                    // Pawn on Rank-7 attacks except the current one's
                                    Bitboard pawns_on_R7    = (pos.pieces<PAWN> (C) - s) & rel_rank_bb (C, R_7);
                                    Bitboard pawnR7_attacks = pawns_on_R7 != U64(0) ? 
                                                                shift_del<WHITE == C ? DEL_NE : DEL_SW> (pawns_on_R7) |
                                                                shift_del<WHITE == C ? DEL_NW : DEL_SE> (pawns_on_R7) :
                                                                U64(0);

                                    safe_squares = queen_squares & ( pawnR7_attacks
                                                                   | ei.pin_attacked_by[C ][NIHT]
                                                                   | ei.pin_attacked_by[C ][BSHP]
                                                                   | ei.pin_attacked_by[C ][ROOK]
                                                                   | ei.pin_attacked_by[C ][QUEN]
                                                                   | ei.pin_attacked_by[C ][KING]);
                                }
                                else
                                {
                                    safe_squares = front_squares & ei.pin_attacked_by[C ][NONE];
                                }
                            }

                            // Give a big bonus if there aren't enemy attacks, otherwise
                            // a smaller bonus if block square is not attacked.
                            i32 k = pr == R_7 ?
                                     unsafe_squares != queen_squares ? 15 : 0 :
                                     unsafe_squares != U64(0) ?
                                     unsafe_squares & block_sq ? 0 : 9 :
                                     15;

                            if (safe_squares != U64(0))
                            {
                                // Give a big bonus if the path to queen is fully defended,
                                // a smaller bonus if at least block square is defended.
                                k += pr == R_7 ?
                                     safe_squares & queen_squares ? 6 : 0 :
                                     safe_squares == front_squares ? 6 :
                                     safe_squares & block_sq ? 4 : 0;

                                // If the block square is defended by a pawn add more small bonus.
                                if (ei.pin_attacked_by[C][PAWN] & block_sq) k += 1;
                            }

                            if (k != 0)
                            {
                                mg_value += k * rr;
                                eg_value += k * rr;
                            }
                        }
                        else
                        {
                            if (pos.pieces (C) & block_sq)
                            {
                                mg_value += 3 * rr + 2 * r + 3;
                                eg_value += 1 * rr + 2 * r + 0;
                            }
                        }
                    }
                }

                // Increase the bonus if the passed pawn is supported by a friendly pawn
                // on the same rank and a bit smaller if it's on the previous rank.
                Bitboard supporting_pawns = pos.pieces<PAWN> (C) & ADJ_FILE_bb[_file (s)];
                if (supporting_pawns & rank_bb (s))
                {
                    eg_value += Value(20 * r);
                }
                else
                if (supporting_pawns & rank_bb (s - PUSH))
                {
                    eg_value += Value(12 * r);
                }

                // Rook pawns are a special case: They are sometimes worse, and
                // sometimes better than other passed pawns. It is difficult to find
                // good rules for determining whether they are good or bad. For now,
                // we try the following: Increase the value for rook pawns if the
                // other side has no pieces apart from a knight, and decrease the
                // value if the other side has a rook or queen.
                if (FILE_EDGE_bb & s)
                {
                    if (pos.non_pawn_material (C_) <= VALUE_MG_NIHT)
                    {
                        eg_value += eg_value / 4;
                    }
                    else if (pos.pieces (C_, ROOK, QUEN))
                    {
                        eg_value -= eg_value / 4;
                    }
                }

                // Increase the bonus if non-pawn pieces count decreased
                eg_value += eg_value / (2*nonpawn_count_sum);

                score += mk_score (mg_value, eg_value);
            }
            
            return score;
        }

        template<Color C>
        // evaluate_space<>() computes the space evaluation for a given side. The
        // space evaluation is a simple bonus based on the number of safe squares
        // available for minor pieces on the central four files on ranks 2--4. Safe
        // squares one, two or three squares behind a friendly pawn are counted
        // twice. Finally, the space bonus is scaled by a weight taken from the
        // material hash table. The aim is to improve play on game opening.
        inline i32 evaluate_space (const Position &pos, const EvalInfo &ei)
        {
            const Color C_ = WHITE == C ? BLACK : WHITE;

            // Find the safe squares for our pieces inside the area defined by
            // SPACE_MASK[]. A square is unsafe if it is attacked by an enemy
            // pawn, or if it is undefended and attacked by an enemy piece.
            Bitboard safe_space =
                  SPACE_MASK[C]
                & ~pos.pieces<PAWN> (C)//~ei.pi->blocked_pawns[C]
                & ~ei.pin_attacked_by[C_][PAWN]
                & (ei.pin_attacked_by[C ][NONE]|~ei.pin_attacked_by[C_][NONE]);

            // Since SPACE_MASK[C] is fully on our half of the board
            ASSERT (u32(safe_space >> (WHITE == C ? 32 : 0)) == 0);

            // Find all squares which are at most three squares behind some friendly pawn
            Bitboard behind = pos.pieces<PAWN> (C);
            behind |= shift_del<WHITE == C ? DEL_S  : DEL_N > (behind);
            behind |= shift_del<WHITE == C ? DEL_SS : DEL_NN> (behind);

            // Count safe_space + (behind & safe_space) with a single pop_count
            return pop_count<FULL> ((WHITE == C ? safe_space << 32 : safe_space >> 32) | (behind & safe_space));
        }

        template<bool Trace>
        // evaluate<>()
        inline Value evaluate (const Position &pos)
        {
            ASSERT (pos.checkers () == U64(0));

            Thread *thread = pos.thread ();

            EvalInfo ei;
            // Probe the material hash table
            ei.mi  = Material::probe (pos, thread->material_table);

            // If have a specialized evaluation function for the current material
            // configuration, call it and return.
            if (ei.mi->specialized_eval_exists ())
            {
                return ei.mi->evaluate (pos);
            }

            // Score is computed from the point of view of white.
            Score score;

            // Initialize score by reading the incrementally updated scores included
            // in the position object (material + piece square tables) and adding Tempo bonus. 
            score  = pos.psq_score ();
            score += ei.mi->matl_score;

            // Probe the pawn hash table
            ei.pi  = Pawns::probe (pos, thread->pawns_table);
            score += apply_weight (ei.pi->pawn_score, Weights[PAWN_STRUCTURE]);

            ei.ful_attacked_by[WHITE][NONE] = ei.pin_attacked_by[WHITE][NONE] = U64(0);
            ei.ful_attacked_by[BLACK][NONE] = ei.pin_attacked_by[BLACK][NONE] = U64(0);
            // Initialize attack and king safety bitboards
            init_evaluation<WHITE> (pos, ei);
            init_evaluation<BLACK> (pos, ei);

            // Evaluate pieces and mobility
            Score mobility[CLR_NO] = { SCORE_ZERO, SCORE_ZERO };
            
            // Do not include in mobility squares occupied by our pawns or king or protected by enemy pawns 
            const Bitboard mobility_area[CLR_NO] =
            {
                ~(pos.pieces (WHITE, PAWN, KING)|ei.pin_attacked_by[BLACK][PAWN]),
                ~(pos.pieces (BLACK, PAWN, KING)|ei.pin_attacked_by[WHITE][PAWN])
            };

            score += 
              + evaluate_pieces<WHITE, NIHT, Trace> (pos, ei, mobility_area[WHITE], mobility[WHITE])
              - evaluate_pieces<BLACK, NIHT, Trace> (pos, ei, mobility_area[BLACK], mobility[BLACK]);
            ei.ful_attacked_by[WHITE][NONE] |= ei.ful_attacked_by[WHITE][NIHT];
            ei.ful_attacked_by[BLACK][NONE] |= ei.ful_attacked_by[BLACK][NIHT];
            ei.pin_attacked_by[WHITE][NONE] |= ei.pin_attacked_by[WHITE][NIHT];
            ei.pin_attacked_by[BLACK][NONE] |= ei.pin_attacked_by[BLACK][NIHT];

            score += 
              + evaluate_pieces<WHITE, BSHP, Trace> (pos, ei, mobility_area[WHITE], mobility[WHITE])
              - evaluate_pieces<BLACK, BSHP, Trace> (pos, ei, mobility_area[BLACK], mobility[BLACK]);
            ei.ful_attacked_by[WHITE][NONE] |= ei.ful_attacked_by[WHITE][BSHP];
            ei.ful_attacked_by[BLACK][NONE] |= ei.ful_attacked_by[BLACK][BSHP];
            ei.pin_attacked_by[WHITE][NONE] |= ei.pin_attacked_by[WHITE][BSHP];
            ei.pin_attacked_by[BLACK][NONE] |= ei.pin_attacked_by[BLACK][BSHP];

            score += 
              + evaluate_pieces<WHITE, ROOK, Trace> (pos, ei, mobility_area[WHITE], mobility[WHITE])
              - evaluate_pieces<BLACK, ROOK, Trace> (pos, ei, mobility_area[BLACK], mobility[BLACK]);
            ei.ful_attacked_by[WHITE][NONE] |= ei.ful_attacked_by[WHITE][ROOK];
            ei.ful_attacked_by[BLACK][NONE] |= ei.ful_attacked_by[BLACK][ROOK];
            ei.pin_attacked_by[WHITE][NONE] |= ei.pin_attacked_by[WHITE][ROOK];
            ei.pin_attacked_by[BLACK][NONE] |= ei.pin_attacked_by[BLACK][ROOK];

            score += 
              + evaluate_pieces<WHITE, QUEN, Trace> (pos, ei, mobility_area[WHITE], mobility[WHITE])
              - evaluate_pieces<BLACK, QUEN, Trace> (pos, ei, mobility_area[BLACK], mobility[BLACK]);
            ei.ful_attacked_by[WHITE][NONE] |= ei.ful_attacked_by[WHITE][QUEN];
            ei.ful_attacked_by[BLACK][NONE] |= ei.ful_attacked_by[BLACK][QUEN];
            ei.pin_attacked_by[WHITE][NONE] |= ei.pin_attacked_by[WHITE][QUEN];
            ei.pin_attacked_by[BLACK][NONE] |= ei.pin_attacked_by[BLACK][QUEN];

            ei.ful_attacked_by[WHITE][NONE] |= ei.ful_attacked_by[WHITE][KING];
            ei.ful_attacked_by[BLACK][NONE] |= ei.ful_attacked_by[BLACK][KING];
            ei.pin_attacked_by[WHITE][NONE] |= ei.pin_attacked_by[WHITE][KING];
            ei.pin_attacked_by[BLACK][NONE] |= ei.pin_attacked_by[BLACK][KING];

            // Weight mobility
            score += apply_weight (mobility[WHITE] - mobility[BLACK], Weights[PIECE_MOBILITY]);

            // Evaluate kings after all other pieces because needed complete attack
            // information when computing the king safety evaluation.
            score += evaluate_king<WHITE, Trace> (pos, ei)
                  -  evaluate_king<BLACK, Trace> (pos, ei);

            // Evaluate tactical threats, needed full attack information including king
            score += evaluate_threats<WHITE, Trace> (pos, ei)
                  -  evaluate_threats<BLACK, Trace> (pos, ei);

            // Evaluate passed pawns, needed full attack information including king
            const Score passed_pawn[CLR_NO] =
            {
                evaluate_passed_pawns<WHITE> (pos, ei),
                evaluate_passed_pawns<BLACK> (pos, ei)
            };
            // Weight passed pawns
            score += apply_weight (passed_pawn[WHITE] - passed_pawn[BLACK], Weights[PASSED_PAWN]);

            const Value npm[CLR_NO] =
            {
                pos.non_pawn_material (WHITE),
                pos.non_pawn_material (BLACK)
            };

            // If one side has only a king, score for potential unstoppable pawns
            if (npm[BLACK] == VALUE_ZERO)
            {
                score += ei.pi->evaluate_unstoppable_pawns<WHITE> ();
            }
            if (npm[WHITE] == VALUE_ZERO)
            {
                score -= ei.pi->evaluate_unstoppable_pawns<BLACK> ();
            }

            Phase game_phase = ei.mi->game_phase;
            ASSERT (PHASE_ENDGAME <= game_phase && game_phase <= PHASE_MIDGAME);

            // Evaluate space for both sides, only in middle-game.
            i32 space[CLR_NO] = { SCORE_ZERO, SCORE_ZERO };
            Score space_weight = ei.mi->space_weight;
            if (space_weight != SCORE_ZERO)
            {
                space[WHITE] = evaluate_space<WHITE> (pos, ei);
                space[BLACK] = evaluate_space<BLACK> (pos, ei);

                score += apply_weight ((space[WHITE] - space[BLACK])*space_weight, Weights[SPACE_ACTIVITY]);
            }

            // In case of tracing add each evaluation contributions for both white and black
            if (Trace)
            {
                Tracer::add_term (PAWN             , ei.pi->pawn_score);
                Tracer::add_term (Tracer::MATERIAL , pos.psq_score ());
                Tracer::add_term (Tracer::IMBALANCE, ei.mi->matl_score);

                Tracer::add_term (Tracer::MOBILITY
                    , apply_weight (mobility[WHITE], Weights[PIECE_MOBILITY])
                    , apply_weight (mobility[BLACK], Weights[PIECE_MOBILITY]));

                Tracer::add_term (Tracer::PASSED_PAWN
                    , apply_weight (passed_pawn[WHITE], Weights[PASSED_PAWN])
                    , apply_weight (passed_pawn[BLACK], Weights[PASSED_PAWN]));

                Tracer::add_term (Tracer::SPACE
                    , apply_weight (space[WHITE] != 0 ? space[WHITE] * space_weight : SCORE_ZERO, Weights[SPACE_ACTIVITY])
                    , apply_weight (space[BLACK] != 0 ? space[BLACK] * space_weight : SCORE_ZERO, Weights[SPACE_ACTIVITY]));

                Tracer::add_term (Tracer::TOTAL    , score);

            }

            // --------------------------------------------------

            i32 mg = i32(mg_value (score));
            i32 eg = i32(eg_value (score));
            ASSERT (-VALUE_INFINITE < mg && mg < +VALUE_INFINITE);
            ASSERT (-VALUE_INFINITE < eg && eg < +VALUE_INFINITE);

            Color strong_side = (eg > VALUE_DRAW) ? WHITE : BLACK;
            // Scale winning side if position is more drawish than it appears
            ScaleFactor scale_fac = (strong_side == WHITE) ?
                ei.mi->scale_factor<WHITE> (pos) :
                ei.mi->scale_factor<BLACK> (pos);

            // If don't already have an unusual scale factor, check for opposite
            // colored bishop endgames, and use a lower scale for those.
            if (  game_phase < PHASE_MIDGAME
               && (scale_fac == SCALE_FACTOR_NORMAL || scale_fac == SCALE_FACTOR_PAWNS)
               )
            {
                if (pos.opposite_bishops ())
                {
                    // Both sides with opposite-colored bishops only ignoring any pawns.
                    if (  npm[WHITE] == VALUE_MG_BSHP
                       && npm[BLACK] == VALUE_MG_BSHP
                       )
                    {
                        // It is almost certainly a draw even with pawns.
                        i32 pawn_diff = abs (pos.count<PAWN> (WHITE) - pos.count<PAWN> (BLACK));
                        scale_fac = pawn_diff == 0 ? ScaleFactor (4) : ScaleFactor (8 * pawn_diff);
                    }
                    // Both sides with opposite-colored bishops, but also other pieces. 
                    else
                    {
                        // Still a bit drawish, but not as drawish as with only the two bishops.
                        scale_fac = ScaleFactor (50 * i32(scale_fac) / i32(SCALE_FACTOR_NORMAL));
                    }
                }
                else
                if (  abs (eg) <= VALUE_EG_BSHP
                   && ei.pi->pawn_span[strong_side] <= 1
                   && !pos.passed_pawn (~strong_side, pos.king_sq (~strong_side))
                   )
                {
                    // Endings where weaker side can place his king in front of the strong side pawns are drawish.
                    scale_fac = PAWN_SPAN_SCALE[ei.pi->pawn_span[strong_side]];
                }
            }

            // Interpolates between a middle game and a (scaled by 'scale_fac') endgame score, based on game phase.
            eg = eg * i32(scale_fac) / i32(SCALE_FACTOR_NORMAL);
            
            Value value = Value(((mg * i32(game_phase)) + (eg * i32(PHASE_MIDGAME - game_phase))) / i32(PHASE_MIDGAME));

            return WHITE == pos.active () ? +value : -value;
        }

        namespace Tracer {

            string trace (const Position &pos)
            {
                fill (*Terms, *Terms + sizeof (Terms) / sizeof (**Terms), SCORE_ZERO);

                Value value = evaluate<true> (pos);// + TempoBonus;    // Tempo bonus = 0.07
                value = WHITE == pos.active () ? +value : -value; // White's point of view

                stringstream ss;

                ss  << showpoint << showpos << setprecision (2) << fixed
                    << "      Eval term |    White    |    Black    |     Total    \n"
                    << "                |   MG    EG  |   MG    EG  |   MG    EG   \n"
                    << "----------------+-------------+-------------+--------------\n";
                format_row (ss, "Material"      , MATERIAL);
                format_row (ss, "Imbalance"     , IMBALANCE);
                format_row (ss, "Pawn"          , PAWN);
                format_row (ss, "Knight"        , NIHT);
                format_row (ss, "Bishop"        , BSHP);
                format_row (ss, "Rook"          , ROOK);
                format_row (ss, "Queen"         , QUEN);
                format_row (ss, "King Safety"   , KING);
                format_row (ss, "Piece Mobility", MOBILITY);
                format_row (ss, "Piece Threat"  , THREAT);
                format_row (ss, "Passed Pawn"   , PASSED_PAWN);
                format_row (ss, "Space Activity", SPACE);
                ss  << "---------------------+-------------+-------------+--------------\n";
                format_row (ss, "Total"         , TOTAL);
                ss  << "\n"
                    << "Total evaluation: " << value_to_cp (value) << " (white side)\n";

                return ss.str ();
            }
        }

    }

    // evaluate() is the main evaluation function.
    // It always computes two values, an endgame value and a middle game value, in score
    // and interpolates between them based on the remaining material.
    Value evaluate  (const Position &pos)
    {
        return evaluate<false> (pos) + TempoBonus;
    }

    // trace() is like evaluate() but instead of a value returns a string suitable
    // to be print on stdout with the detailed descriptions and values of each
    // evaluation term. Used mainly for debugging.
    string trace    (const Position &pos)
    {
        return Tracer::trace (pos);
    }

    // initialize() computes evaluation weights from the corresponding UCI parameters
    // and setup king danger tables.
    void configure (const Option &)
    {
        Weights[PIECE_MOBILITY] = weight_option (0                             , INTERNAL_WEIGHTS[PIECE_MOBILITY ]);
        Weights[PAWN_STRUCTURE] = weight_option (0                             , INTERNAL_WEIGHTS[PAWN_STRUCTURE ]);
        Weights[PASSED_PAWN   ] = weight_option (0                             , INTERNAL_WEIGHTS[PASSED_PAWN    ]);
        Weights[SPACE_ACTIVITY] = weight_option (i32(Options["Space Activity"]), INTERNAL_WEIGHTS[SPACE_ACTIVITY]);
        Weights[KING_SAFETY   ] = weight_option (i32(Options["King Safety"   ]), INTERNAL_WEIGHTS[KING_SAFETY   ]);

        const i32 MAX_SLOPE  =   30;
        const i32 PEAK_VALUE = 1280;

        KING_DANGER[0] = SCORE_ZERO;
        i32 mg = 0;
        for (u08 i = 1; i < MAX_ATTACK_UNITS; ++i)
        {
            mg = min (min (i32(0.4*i*i), mg + MAX_SLOPE), PEAK_VALUE);
            KING_DANGER[i] = apply_weight (mk_score (mg, 0), Weights[KING_SAFETY]);
        }
    }

}
