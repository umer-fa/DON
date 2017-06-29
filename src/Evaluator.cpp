#include "Evaluator.h"

#include <ostream>

#include "Material.h"
#include "MoveGenerator.h"
#include "Pawns.h"
#include "Thread.h"

namespace Evaluator {

    using namespace std;
    using namespace BitBoard;
    using namespace MoveGen;
    using namespace EndGame;

    namespace {

        namespace Tracer {

            enum Term : u08
            {
                // The first 6 entries are for PieceType
                MATERIAL = NONE,
                IMBALANCE,
                MOBILITY,
                THREAT,
                PASSER,
                SPACE,
                TOTAL,
                TERM_NO
            };

            double cp[TERM_NO][CLR_NO][2];

            void write (u08 term, Color c, Score score)
            {
                cp[term][c][MG] = value_to_cp (mg_value (score));
                cp[term][c][EG] = value_to_cp (eg_value (score));
            }
            void write (u08 term, Score wscore, Score bscore = SCORE_ZERO)
            {
                write (term, WHITE, wscore);
                write (term, BLACK, bscore);
            }

            ostream& operator<< (ostream &os, Term term)
            {
                switch (u08(term))
                {
                case PAWN:
                case MATERIAL:
                case IMBALANCE:
                case TOTAL:
                    os << " | ----- ----- | ----- ----- | ";
                    break;
                default:
                    os << " | " << std::setw (5) << cp[term][WHITE][MG]
                       << " "   << std::setw (5) << cp[term][WHITE][EG]
                       << " | " << std::setw (5) << cp[term][BLACK][MG]
                       << " "   << std::setw (5) << cp[term][BLACK][EG]
                       << " | ";
                    break;
                }
                os << std::setw (5) << cp[term][WHITE][MG] - cp[term][BLACK][MG] << " "
                   << std::setw (5) << cp[term][WHITE][EG] - cp[term][BLACK][EG]
                   << std::endl;
                return os;
            }
        }

        using namespace Tracer;

        // Evaluation class contains various information computed and collected
        // by the evaluation functions.
        template<bool Trace>
        class Evaluation
        {
        private:
            const Position &_pos;
            Material::Entry *_me = nullptr;
            Pawns   ::Entry *_pe = nullptr;

            Bitboard mob_area[CLR_NO];
            Score    mobility[CLR_NO];

            // Contains all squares attacked by the color and piece type.
            Bitboard ful_attacked_by[CLR_NO];
            // Contains all squares attacked by the color and piece type with pinned removed.
            Bitboard pin_attacked_by[CLR_NO][MAX_PTYPE];
            // Squares attacked by more than one pieces of a color, possibly via x-ray or by one pawn and one piece.
            // Diagonal x-ray through pawn or squares attacked by 2 pawns are not explicitly added.
            Bitboard dbl_attacked[CLR_NO];

            // Zone around the king which is considered by the king safety evaluation.
            // This consists of the squares directly adjacent to the king, and the three (or two, for a king on an edge file) squares two ranks in front of the king.
            // For instance, if black's king is on g8, king_ring[BLACK] is a bitboard containing the squares f8, h8, f7, g7, h7, f6, g6 and h6.
            Bitboard king_ring[CLR_NO];
            // Number of pieces of the color, which attack a square in the king_ring of the enemy king.
            u08      king_ring_attackers_count[CLR_NO];
            // Sum of the "weight" of the pieces of the color which attack a square in the king_ring of the enemy king.
            // The weights of the individual piece types are given by the PieceKingAttacks[piece-type]
            i32      king_ring_attackers_weight[CLR_NO];
            // Number of attacks by the color to squares directly adjacent to the enemy king.
            // Pieces which attack more than one square are counted multiple times.
            u08      king_zone_attacks_count[CLR_NO];

            // Evaluation helpers
            template<Color Own> void initialize ();
            template<Color Own, PieceType PT> Score evaluate_pieces ();
            template<Color Own> Score evaluate_king ();
            template<Color Own> Score evaluate_threats ();
            template<Color Own> Score evaluate_passers ();
            template<Color Own> Score evaluate_space ();

            Score evaluate_initiative (Value eg);
            Scale evaluate_scale (Value eg);

        public:
            Evaluation () = delete;
            Evaluation (const Evaluation&) = delete;
            Evaluation& operator= (const Evaluation&) = delete;

            Evaluation (const Position &pos)
                : _pos (pos)
            {};
            
            Value value ();
        };

    #define V(v) Value(v)
    #define S(mg, eg) mk_score (mg, eg)

        // PieceMobility[piece-type][attacks] contains bonuses for mobility,
        // indexed by piece type and number of attacked squares in the mobility area
        const Score PieceMobility[][28] =
        {
            {},
            { // Knight
                S(-75,-76), S(-57,-54), S( -9,-28), S( -2,-10), S(  6,  5), S( 14, 12),
                S( 22, 26), S( 29, 29), S( 36, 29)
            },
            { // Bishop
                S(-48,-59), S(-20,-23), S( 16, -3), S( 26, 13), S( 38, 24), S( 51, 42),
                S( 55, 54), S( 63, 57), S( 63, 65), S( 68, 73), S( 81, 78), S( 81, 86),
                S( 91, 88), S( 98, 97)
            },
            { // Rook
                S(-58,-76), S(-27,-18), S(-15, 28), S(-10, 55), S( -5, 69), S( -2, 82),
                S(  9,112), S( 16,118), S( 30,132), S( 29,142), S( 32,155), S( 38,165),
                S( 46,166), S( 48,169), S( 58,171)
            },
            { // Queen
                S(-39,-36), S(-21,-15), S(  3,  8), S(  3, 18), S( 14, 34), S( 22, 54),
                S( 28, 61), S( 41, 73), S( 43, 79), S( 48, 92), S( 56, 94), S( 60,104),
                S( 60,113), S( 66,120), S( 67,123), S( 70,126), S( 71,133), S( 73,136),
                S( 79,140), S( 88,143), S( 88,148), S( 99,166), S(102,170), S(102,175),
                S(106,184), S(109,191), S(113,206), S(116,212)
            }
        };

        // PieceCloseness[piece-type] * "distance to own king" determines a bonus for each piece.
        const Score PieceCloseness[] =
        {
            S( 0, 0), S(-3, -5), S(-4, -3), S(-3, 0), S(-1, 1)
        };

        // PieceOutpost[piece-type][supported by pawn] contains bonuses for piece outposts
        // If they can reach an outpost square, bigger if that square is supported by a pawn
        // If the minor piece occupies an outpost square then score is doubled
        const Score PieceOutpost[][2] =
        {
            {},
            { S(22, 6), S(33, 9) },
            { S( 9, 2), S(14, 4) }
        };

        // RookOnFile[semiopen/open] contains bonuses for rooks
        // when there is no friend pawn on the rook file
        const Score RookOnFile[] =
        {
            S(20, 7), S(45,20)
        };
        // Bonus for minor behind a pawn
        const Score MinorBehindPawn = S(16, 0);
        // Penalty for bishop with pawns on same color
        const Score BishopPawns     = S( 8,12);
        // Penalty for bishop trapped with pawns (Chess960)
        const Score BishopTrapped   = S(50,50);

        // Bonus for rook on pawns
        const Score RookOnPawns     = S( 8,24);
        // Penalty for rook trapped
        const Score RookTrapped     = S(92, 0);
        // Penalty for queen weaken
        const Score QueenWeaken     = S(50,10);

        const Score ProbChecked     = S(10,10);
        // King tropism
        const Score EnemyInFlank    = S( 7, 0);
        const Score PawnlessFlank   = S(20,80);

        // Bonus for each hanged piece
        const Score PieceHanged     = S(48,27);

        const Score PawnPushThreat  = S(38,22);

        const Score HangPawnThreat  = S( 71,  61);
        const Score SafePawnThreat  = S(182, 175);

        // PieceThreat[piece-type][piece-type] contains bonus according to piece type
        const Score PieceThreat[][NONE] =
        {
            {},
            { S( 0,33), S(45,43), S(46,47), S(47,107), S(48,118), S( 0, 0) },
            { S( 0,33), S(45,43), S(46,47), S(47,107), S(48,118), S( 0, 0) },
            { S( 0,25), S(40,62), S(40,59), S( 0, 34), S(35, 48), S( 0, 0) }
        };

        const Score PieceRankThreat = S(16, 3);

        // KingThreat[one/more] contains bonus for king attacks on pawns or pieces which are not pawn-defended
        const Score KingThreat[] =
        {
            S( 3, 62), S( 9,138)
        };

        const Score PawnPassHinder = S( 7, 0);

        // PawnPassFile[file] contains bonus for passed pawns according to distance from edge
        const Score PawnPassFile[] = { S( 9, 10), S( 2, 10), S( 1, -8), S(-20,-12) };
        // PawnPassRank[rank] contains bonus for passed pawns according to the rank of the pawn
        const Value PawnPassRank[][R_NO] =
        {
            { V(  0), V(  5), V(  5), V( 31), V( 73), V(166), V(252), V(  0) },
            { V(  0), V(  7), V( 14), V( 38), V( 73), V(166), V(252), V(  0) }
        };

        // Threshold for lazy evaluation
        const Value LazyThreshold   = V(1500);
        const Value SpaceThreshold  = V(12222);

    #undef S
    #undef V

        // Bonus for king attack by piece type
        const i32 PieceKingAttacks[] = {  0, 78, 56, 45, 11,  0 };


        // initialize() computes king and pawn attacks, and the king ring bitboard for the color.
        // This is done at the beginning of the evaluation.
        template<bool Trace> template<Color Own>
        void Evaluation<Trace>::initialize ()
        {
            const auto Opp  = Own == WHITE ? BLACK : WHITE;
            const auto LCap = Own == WHITE ? DEL_NW : DEL_SE;
            const auto RCap = Own == WHITE ? DEL_NE : DEL_SW;
            const auto Push = Own == WHITE ? DEL_N : DEL_S;
            const auto Pull = Own == WHITE ? DEL_S : DEL_N;
            const Bitboard UpRanks = Own == WHITE ?
                                        R7_bb|R6_bb :
                                        R2_bb|R3_bb;

            Bitboard pinned_pawns = _pos.abs_blockers (Own) & _pos.pieces (Own, PAWN);
            if (0 != pinned_pawns)
            {
                Bitboard loosed_pawns = _pos.pieces (Own, PAWN) & ~pinned_pawns;
                pin_attacked_by[Own][PAWN] =
                      (  shift<LCap> (loosed_pawns)
                       | shift<RCap> (loosed_pawns))
                    | (  (  shift<LCap> (pinned_pawns)
                          | shift<RCap> (pinned_pawns))
                       & PieceAttacks[BSHP][_pos.square<KING> (Own)]);
            }
            else
            {
                pin_attacked_by[Own][PAWN] = _pe->any_attacks[Own];
            }

            pin_attacked_by[Own][KING] = PieceAttacks[KING][_pos.square<KING> (Own)];
            ful_attacked_by[Own]       = pin_attacked_by[Own][KING] | _pe->any_attacks[Own];
            pin_attacked_by[Own][NONE] = pin_attacked_by[Own][KING] | pin_attacked_by[Own][PAWN];
            dbl_attacked[Own]          = _pe->dbl_attacks[Own]
                                       | (pin_attacked_by[Own][KING] & pin_attacked_by[Own][PAWN]);

            // Do not include in mobility area
            // - squares protected by enemy pawns
            // - squares occupied by block pawns (pawns blocked or on ranks 2-3)
            // - square occupied by friend king
            Bitboard b = pin_attacked_by[Own][PAWN]
                       | (  _pos.pieces (Opp, PAWN)
                          & (shift<Push> (_pos.pieces ()) | UpRanks));
            mob_area[Opp] = ~(b | _pos.square<KING> (Opp));
            
            king_ring_attackers_weight[Own] = 0;
            king_zone_attacks_count[Own] = 0;

            if (_pos.si->non_pawn_material (Own) >= VALUE_MG_ROOK + VALUE_MG_NIHT)
            {
                b = PieceAttacks[KING][_pos.square<KING> (Opp)];
                king_ring[Opp] = b;
                switch (rel_rank (Opp, _pos.square<KING> (Opp)))
                {
                case R_1: king_ring[Opp] |= shift<Pull> (b); break;
                case R_8: king_ring[Opp] |= shift<Push> (b); break;
                default:                                     break;
                }
                king_ring_attackers_count[Own] = u08(pop_count (b & pin_attacked_by[Own][PAWN]));
            }
            else
            {
                king_ring[Opp] = 0;
                king_ring_attackers_count[Own] = 0;
            }
        }

        // Evaluates bonuses and penalties of the pieces of the color and type
        template<bool Trace> template<Color Own, PieceType PT>
        Score Evaluation<Trace>::evaluate_pieces ()
        {
            const auto Opp  = Own == WHITE ? BLACK : WHITE;
            const auto Push = Own == WHITE ? DEL_N : DEL_S;
            const auto LCap = Own == WHITE ? DEL_NW : DEL_SE;
            const auto RCap = Own == WHITE ? DEL_NE : DEL_SW;
            // Mask of allowed outpost squares
            const Bitboard Outposts = Own == WHITE ?
                                                R4_bb|R5_bb|R6_bb :
                                                R5_bb|R4_bb|R3_bb;

            assert(NIHT <= PT && PT <= QUEN);

            auto score = SCORE_ZERO;
            pin_attacked_by[Own][PT] = 0;

            for (auto s : _pos.squares[Own][PT])
            {
                assert(_pos[s] == (Own|PT));
                // Find attacked squares, including x-ray attacks for bishops and rooks
                Bitboard attacks;
                switch (PT)
                {
                case NIHT: attacks = PieceAttacks[NIHT][s];                                                                             break;
                case BSHP: attacks = attacks_bb<BSHP> (s, _pos.pieces () ^ (_pos.pieces (Own, BSHP, QUEN) & ~_pos.abs_blockers (Own))); break;
                case ROOK: attacks = attacks_bb<ROOK> (s, _pos.pieces () ^ (_pos.pieces (Own, ROOK, QUEN) & ~_pos.abs_blockers (Own))); break;
                case QUEN: attacks = attacks_bb<QUEN> (s, _pos.pieces () ^ (_pos.pieces (Own,       QUEN) & ~_pos.abs_blockers (Own))); break;
                default:   assert(false);
                           attacks = 0;
                }

                ful_attacked_by[Own] |= attacks;

                if (contains (_pos.abs_blockers (Own), s))
                {
                    attacks &= strline_bb (_pos.square<KING> (Own), s);
                }

                if (QUEN == PT)
                {
                    Bitboard att = attacks & ~_pos.abs_blockers (Own);
                    Bitboard qb = _pos.pieces (Own, BSHP) & PieceAttacks[BSHP][s] & att;
                    Bitboard qr = _pos.pieces (Own, ROOK) & PieceAttacks[ROOK][s] & att;
                    dbl_attacked[Own] |= pin_attacked_by[Own][NONE]
                                          & (  attacks
                                             | (0 != qb ? attacks_bb<BSHP> (s, _pos.pieces () ^ qb) : 0)
                                             | (0 != qr ? attacks_bb<ROOK> (s, _pos.pieces () ^ qr) : 0));
                }
                else
                {
                    dbl_attacked[Own] |= pin_attacked_by[Own][NONE] & attacks;
                }
                
                pin_attacked_by[Own][NONE] |=
                pin_attacked_by[Own][PT]   |= attacks;

                if (0 != (king_ring[Opp] & attacks))
                {
                    king_ring_attackers_count[Own]++;
                    king_ring_attackers_weight[Own] += PieceKingAttacks[PT];
                    king_zone_attacks_count[Own] += u08(pop_count (pin_attacked_by[Opp][KING] & attacks));
                }

                auto mob = pop_count (mob_area[Own] & attacks);
                assert(0 <= mob && mob <= 27);
                // Bonus for piece mobility
                mobility[Own] += PieceMobility[PT][mob];

                // Bonus for piece closeness to King
                score += PieceCloseness[PT] * dist (s, _pos.square<KING> (Own));
                //score += PieceCloseness[PT] * dist (s, _pos.square<KING> (Opp));

                Bitboard b;
                // Special extra evaluation for pieces
                switch (PT)
                {
                case NIHT:
                case BSHP:
                {
                    // Bonus for minors when behind a pawn
                    if (   rel_rank (Own, s) < R_5
                        && contains (_pos.pieces (PAWN), s+Push))
                    {
                        score += MinorBehindPawn;
                    }

                    b = Outposts
                      & ~_pe->attack_span[Opp];
                    // Bonus for minors outpost squares
                    if (contains (b, s))
                    {
                        score += PieceOutpost[PT][contains (pin_attacked_by[Own][PAWN], s) ? 1 : 0] * 2;
                    }
                    else
                    {
                        b &= attacks
                           & ~_pos.pieces (Own);
                        if (0 != b)
                        {
                            score += PieceOutpost[PT][0 != (pin_attacked_by[Own][PAWN] & b)] * 1;
                        }
                    }

                    if (BSHP == PT)
                    {
                        // Penalty for pawns on the same color square as the bishop
                        score -= BishopPawns * i32(_pe->color_count[Own][color (s)]);

                        if (   mob <= 2
                            && contains (FA_bb|FH_bb,  s)
                            && rel_rank (Own, s) >= R_4)
                        {
                            auto del = (F_A == _file (s) ? DEL_E : DEL_W)-Push;
                            if (_pos[s+del] == (Own|PAWN))
                            {
                                score -= BishopTrapped * (_pos.empty (s+del+Push) ? 1 : 2);
                            }
                        }

                        if (Position::Chess960)
                        {
                            // An important Chess960 pattern: A cornered bishop blocked by a friend pawn diagonally in front of it.
                            // It is a very serious problem, especially when that pawn is also blocked.
                            // Bishop (white or black) on a1/h1 or a8/h8 which is trapped by own pawn on b2/g2 or b7/g7.
                            if (   contains (FA_bb|FH_bb, s)
                                && rel_rank (Own, s) == R_1)
                            {
                                auto del = (F_A == _file (s) ? DEL_E : DEL_W)+Push;
                                if (_pos[s+del] == (Own|PAWN))
                                {
                                    score -= BishopTrapped * (_pos.empty (s+del+Push) ?
                                                                  _pos[s+del+del] != (Own|PAWN) ?
                                                                      1 : 2 : 4);
                                }
                            }
                        }
                    }
                }
                    break;

                case ROOK:
                {
                    // Bonus for rook aligning with enemy pawns on the same rank/file
                    if (rel_rank (Own, s) > R_4)
                    {
                        score += RookOnPawns * pop_count (_pos.pieces (Opp, PAWN) & PieceAttacks[ROOK][s]);
                    }

                    // Bonus for rook when on an open or semi-open file
                    if (_pe->file_semiopen (Own, _file (s)))
                    {
                        score += RookOnFile[_pe->file_semiopen (Opp, _file (s)) ? 1 : 0];
                    }
                    else
                    if (   mob <= 3
                        && rel_rank (Own, s) < R_5
                        && 0 != (front_sqrs_bb (Own, s) & _pos.pieces (Own, PAWN))
                        && 0 == (front_sqrs_bb (Opp, s) & _pos.pieces (Own, PAWN)))
                    {
                        // Penalty for rook when trapped by the king, even more if the king can't castle
                        auto kf = _file (_pos.square<KING> (Own));
                        if (   ((kf < F_E) == (_file (s) < kf))
                            && !_pe->side_semiopen (Own, kf, kf < F_E))
                        {
                            score -= (RookTrapped - mk_score (22 * mob, 0)) * (_pos.can_castle (Own) ? 1 : 2);
                        }
                    }
                }
                    break;

                case QUEN:
                {
                    // Penalty for pin or discover attack on the queen
                    if (0 != (_pos.slider_blockers<Own> (s, _pos.pieces (Opp, QUEN), b, b) & ~(  (_pos.pieces (Opp, PAWN) & file_bb (s) & ~(  shift<LCap> (_pos.pieces (Own))
                                                                                                                                            | shift<RCap> (_pos.pieces (Own))))
                                                                                               | _pos.abs_blockers (Opp))))
                    {
                        score -= QueenWeaken;
                    }
                }
                    break;
                }
            }

            if (Trace)
            {
                Tracer::write (PT, Own, score);
            }

            return score;
        }

        // Evaluates bonuses and penalties of the king of the color
        template<bool Trace> template<Color Own>
        Score Evaluation<Trace>::evaluate_king ()
        {
            const auto Opp  = Own == WHITE ? BLACK : WHITE;
            const auto Push = Own == WHITE ? DEL_N : DEL_S;
            //const auto Pull = Own == WHITE ? DEL_S : DEL_N;
            //const auto LCap = Own == WHITE ? DEL_NW : DEL_SE;
            //const auto RCap = Own == WHITE ? DEL_NE : DEL_SW;
            const Bitboard Camp = Own == WHITE ?
                                             R1_bb|R2_bb|R3_bb|R4_bb|R5_bb :
                                             R8_bb|R7_bb|R6_bb|R5_bb|R4_bb;

            auto fk_sq = _pos.square<KING> (Own);

            // King Safety: friend pawns shelter and enemy pawns storm
            auto index = _pe->do_king_safety<Own> (_pos, fk_sq);
            auto value = _pe->king_safety[Own][index];
            if (   rel_rank (Own, fk_sq) == R_1
                && _pos.can_castle (Own))
            {
                if (   value < _pe->king_safety[Own][0]
                    && _pos.can_castle (Own, CS_KING)
                    && _pos.expeded_castle (Own, CS_KING)
                    && 0 == (_pos.king_path[Own][CS_KING] & ful_attacked_by[Opp]))
                {
                    value = _pe->king_safety[Own][0];
                }
                if (   value < _pe->king_safety[Own][1]
                    && _pos.can_castle (Own, CS_QUEN)
                    && _pos.expeded_castle (Own, CS_QUEN)
                    && 0 == (_pos.king_path[Own][CS_QUEN] & ful_attacked_by[Opp]))
                {
                    value = _pe->king_safety[Own][1];
                }
            }

            auto score = mk_score (value, -16 * _pe->king_pawn_dist[Own][index]);

            Bitboard b;
            // Main king safety evaluation
            if (king_ring_attackers_count[Opp] + _pos.count<QUEN> (Opp) > 1)
            {
                // Find the attacked squares which are defended only by the king in the king zone...
                Bitboard king_zone_undef =
                       pin_attacked_by[Own][KING]
                    &  pin_attacked_by[Opp][NONE]
                    & ~dbl_attacked[Own];
                // ... and those which are not defended at all in the king ring.
                Bitboard king_ring_undef =
                       king_ring[Own]
                    & ~_pos.pieces (Opp)
                    &  pin_attacked_by[Opp][NONE]
                    & ~pin_attacked_by[Own][NONE];
                // Initialize the king danger, which will be transformed later into a king danger score.
                // The initial value is based on the
                // - the number and types of the enemy's attacking pieces,
                // - the number of attacked and undefended squares around our king,
                // - the quality of the pawn shelter ('mg score' value).
                i32 king_danger =
                        1 * king_ring_attackers_count[Opp]*king_ring_attackers_weight[Opp]
                    + 102 * king_zone_attacks_count[Opp]
                    + 201 * pop_count (king_zone_undef)
                    + 143 * pop_count (king_ring_undef | _pos.abs_blockers (Own))
                    //+ 143 * pop_count (_pos.dsc_blockers (Opp) & ~(  (_pos.pieces (Opp, PAWN) & (  (file_bb (fk_sq) & ~(  shift<LCap> (_pos.pieces (Own))
                    //                                                                                                    | shift<RCap> (_pos.pieces (Own))))
                    //                                                                             | shift<Pull> (_pos.pieces ())))
                    //                                               | _pos.abs_blockers (Opp)))
                    - 848 * (0 == _pos.count<QUEN>(Opp))
                    -   9 * value / 8
                    +  40;

                Bitboard rook_attack = attacks_bb<ROOK> (fk_sq, _pos.pieces ());
                Bitboard bshp_attack = attacks_bb<BSHP> (fk_sq, _pos.pieces ());
                assert(0 == (rook_attack & _pos.pieces (Opp, ROOK, QUEN)));
                assert(0 == (bshp_attack & _pos.pieces (Opp, BSHP, QUEN)));

                // For safe enemy's checks on the safe square which are possible on next move ...
                Bitboard safe_area =
                      ~_pos.pieces (Opp)
                    &  (  ~pin_attacked_by[Own][NONE]
                        |  (  king_zone_undef
                            & dbl_attacked[Opp]));
                // ... and for some other probable potential checks, 
                // the square to be safe from pawn-attacks and not being occupied by a blocked pawns.
                Bitboard prob_area =
                      ~(  pin_attacked_by[Own][PAWN]
                        | (  _pos.pieces (Opp, PAWN)
                           & shift<Push> (_pos.pieces (PAWN))));

                // Enemy queens safe checks
                b =    (rook_attack | bshp_attack)
                    &  pin_attacked_by[Opp][QUEN];
                if (0 != (b & safe_area))
                {
                    king_danger += 780;
                }

                // For other pieces, the safe square also if attacked twice and only defended by a queen.
                safe_area |=
                       dbl_attacked[Opp]
                    & ~(  dbl_attacked[Own]
                        | _pos.pieces (Opp))
                    &  pin_attacked_by[Own][QUEN];

                // Enemy rooks safe and other checks
                b =    rook_attack
                    &  pin_attacked_by[Opp][ROOK];
                if (0 != (b & safe_area))
                {
                    king_danger += 880;
                }
                else
                if (0 != (b & prob_area))
                {
                    score -= ProbChecked;
                }
                // Enemy bishops safe and other checks
                b =    bshp_attack
                    &  pin_attacked_by[Opp][BSHP];
                if (0 != (b & safe_area))
                {
                    king_danger += 435;
                }
                else
                if (0 != (b & prob_area))
                {
                    score -= ProbChecked;
                }
                // Enemy knights safe and other checks
                b =    PieceAttacks[NIHT][fk_sq]
                    &  pin_attacked_by[Opp][NIHT];
                if (0 != (b & safe_area))
                {
                    king_danger += 790;
                }
                else
                if (0 != (b & prob_area))
                {
                    score -= ProbChecked;
                }

                // Transform the king units into a score, and substract it from the evaluation
                if (king_danger > 0)
                {
                    score -= mk_score (king_danger*king_danger / 0x1000, king_danger / 0x10);
                }
            }

            // King tropism: Find squares that enemy attacks in the friend king flank
            auto kf = _file (fk_sq);
            b =    Camp
                &  KingFlank[kf]
                &  pin_attacked_by[Opp][NONE];
            assert(0 == ((Own == WHITE ? b << 4 : b >> 4) & b));
            assert(pop_count (Own == WHITE ? b << 4 : b >> 4) == pop_count (b));
            // Add the squares which are attacked twice in that flank and are not protected by a friend pawn.
            b =   (Own == WHITE ? b << 4 : b >> 4)
                | (   b
                   &  dbl_attacked[Opp]
                   & ~pin_attacked_by[Own][PAWN]);
            score -= EnemyInFlank * pop_count (b);

            // Penalty when our king is on a pawnless flank
            if (0 == (KingFlank[kf] & _pos.pieces (PAWN)))
            {
                score -= PawnlessFlank;
            }

            if (Trace)
            {
                Tracer::write (KING, Own, score);
            }

            return score;
        }

        // Evaluates the threats of the color
        template<bool Trace> template<Color Own>
        Score Evaluation<Trace>::evaluate_threats ()
        {
            const auto Opp  = Own == WHITE ? BLACK : WHITE;
            const auto Push = Own == WHITE ? DEL_N  : DEL_S;
            const auto LCap = Own == WHITE ? DEL_NW : DEL_SE;
            const auto RCap = Own == WHITE ? DEL_NE : DEL_SW;
            const Bitboard R2BB = Own == WHITE ? R2_bb : R7_bb;
            const Bitboard R7BB = Own == WHITE ? R7_bb : R2_bb;

            auto score = SCORE_ZERO;

            Bitboard b;
            
            // Enemy non-pawns
            Bitboard nonpawns =
                  _pos.pieces (Opp)
                ^ _pos.pieces (Opp, PAWN);

            // Squares defended by the opponent,
            // - attack the square with a pawn
            // - attack the square twice and not defended twice.
            Bitboard defended =
                   pin_attacked_by[Opp][PAWN]
                | (   dbl_attacked[Opp]
                   & ~dbl_attacked[Own]);

            // Enemy not defended and attacked by any friend piece
            Bitboard weak_pieces =
                   _pos.pieces (Opp)
                & ~defended
                &  pin_attacked_by[Own][NONE];

            // Add a bonus according to the type of attacking pieces

            // Enemies attacked by knights
            b =   (  weak_pieces
                    // Rooks or Queens
                   | _pos.pieces (Opp, ROOK, QUEN)
                    // Enemy defended non-pawns
                   | (  nonpawns
                      & defended))
                & pin_attacked_by[Own][NIHT];
            while (0 != b)
            {
                auto s = pop_lsq (b);
                auto pt = ptype (_pos[s]);
                score += PieceThreat[NIHT][pt];
                if (pt != PAWN)
                {
                    score += PieceRankThreat * rel_rank (Opp, s);
                }
            }
            // Enemies attacked by bishops
            b =   (  weak_pieces
                    // Rooks or Queens
                   | _pos.pieces (Opp, ROOK, QUEN)
                    // Enemy defended non-pawns
                   | (  nonpawns
                      & defended))
                & pin_attacked_by[Own][BSHP];
            while (0 != b)
            {
                auto s = pop_lsq (b);
                auto pt = ptype (_pos[s]);
                score += PieceThreat[BSHP][pt];
                if (pt != PAWN)
                {
                    score += PieceRankThreat * rel_rank (Opp, s);
                }
            }
            // Enemies attacked by rooks
            b =   (  weak_pieces
                    // Queens
                   | _pos.pieces (Opp, QUEN))
                & pin_attacked_by[Own][ROOK];
            while (0 != b)
            {
                auto s = pop_lsq (b);
                auto pt = ptype (_pos[s]);
                score += PieceThreat[ROOK][pt];
                if (pt != PAWN)
                {
                    score += PieceRankThreat * rel_rank (Opp, s);
                }
            }
            // Enemies attacked by king
            b =    weak_pieces
                &  pin_attacked_by[Own][KING];
            if (0 != b)
            {
                score += KingThreat[more_than_one (b) ? 1 : 0];
            }
            // Enemies attacked by friend are hanging
            b =    weak_pieces
                & ~pin_attacked_by[Opp][NONE];
            score += PieceHanged * pop_count (b);

            Bitboard safe =
                  ~pin_attacked_by[Opp][NONE]
                |  pin_attacked_by[Own][NONE];

            // Enemy non-pawns attacked by any friend pawn
            Bitboard weak_nonpawns =
                   nonpawns
                &  pin_attacked_by[Own][PAWN];
            if (0 != weak_nonpawns)
            {
                // Safe friend pawns
                b =   safe
                    & _pos.pieces (Own, PAWN);
                b =   (  shift<LCap> (b)
                       | shift<RCap> (b))
                    & weak_nonpawns;

                score += SafePawnThreat * pop_count (b);
                
                if (0 != (weak_nonpawns ^ b))
                {
                    score += HangPawnThreat;
                }
            }

            // Bonus if some friend pawns safely push can attack an enemy piece
            b =    _pos.pieces (Own, PAWN)
                & ~(  R7BB
                    | _pos.abs_blockers (Own));
            // Friend pawns push
            b =   shift<Push> (b | (  shift<Push> (b & R2BB)
                                    & ~_pos.pieces ()))
                & ~_pos.pieces ();
            // Friend pawns safe push
            b &=   safe
                & ~pin_attacked_by[Opp][PAWN];
            // Friend pawns safe push attacks an enemy piece not already attacked by pawn
            b =    (  shift<LCap> (b)
                    | shift<RCap> (b))
                &  _pos.pieces (Opp)
                & ~pin_attacked_by[Own][PAWN];
            score += PawnPushThreat * pop_count (b);

            if (Trace)
            {
                Tracer::write (THREAT, Own, score);
            }

            return score;
        }

        // Evaluates the passed pawns of the color
        template<bool Trace> template<Color Own>
        Score Evaluation<Trace>::evaluate_passers ()
        {
            const auto Opp  = Own == WHITE ? BLACK : WHITE;
            const auto Push = Own == WHITE ? DEL_N : DEL_S;

            auto score = SCORE_ZERO;

            Bitboard passers = _pe->passers[Own];
            while (0 != passers)
            {
                auto s = pop_lsq (passers);
                assert(0 == (_pos.pieces (Opp, PAWN) & front_sqrs_bb (Own, s+Push)));

                auto rank = rel_rank (Own, s);
                // Base bonus depending on rank.
                auto mg_value = PawnPassRank[MG][rank];
                auto eg_value = PawnPassRank[EG][rank];

                auto r  = dist (rank, R_2);
                auto rr = r*(r-1);

                if (0 != rr)
                {
                    auto push_sq = s+Push;

                    // Adjust bonus based on kings proximity.
                    if (!contains (pawn_pass_span (Own, s), _pos.square<KING> (Opp)))
                    {
                        eg_value += 5*rr*dist (_pos.square<KING> (Opp), push_sq);
                    }
                    eg_value -= 2*rr*dist (_pos.square<KING> (Own), push_sq);
                    // If block square is not the queening square then consider also a second push.
                    if (rel_rank (Own, push_sq) != R_8)
                    {
                        eg_value -= 1*rr*dist (_pos.square<KING> (Own), push_sq+Push);
                    }
                    
                    // If the pawn is free to advance.
                    if (_pos.empty (push_sq))
                    {
                        // Squares to queen
                        Bitboard front_squares = front_sqrs_bb (Own, s);
                        Bitboard safe_front_squares = front_squares
                            ,  unsafe_front_squares = front_squares;
                        // If there is a rook or queen attacking/defending the pawn from behind, consider front squares.
                        // Otherwise consider only the squares in the pawn's path attacked or occupied by the enemy.
                        Bitboard behind_majors = _pos.pieces (ROOK, QUEN) & front_sqrs_bb (Opp, s);
                        if (0 != behind_majors)
                        {
                            behind_majors &= attacks_bb<ROOK> (s, _pos.pieces ());
                        }
                        // If there is an enemy rook or queen attacking the pawn from behind, add all X-ray attacks by the rook or queen.
                        // Otherwise consider only the squares in the pawn's path attacked or occupied by the enemy.
                        if (0 == (behind_majors & _pos.pieces (Opp)))
                        {
                            unsafe_front_squares &= pin_attacked_by[Opp][NONE] | _pos.pieces (Opp);
                        }
                        if (0 == (behind_majors & _pos.pieces (Own)))
                        {
                            safe_front_squares   &= pin_attacked_by[Own][NONE];
                        }

                        // Give a big bonus if the path to the queen is not attacked,
                        // a smaller bonus if the block square is not attacked.
                        i32 k = 0 != unsafe_front_squares ?
                                 contains (unsafe_front_squares, push_sq) ?
                                    0 : 8 : 18;
                        // Give a big bonus if the path to the queen is fully defended,
                        // a smaller bonus if the block square is defended.
                        k += safe_front_squares != front_squares ?
                                !contains (safe_front_squares, push_sq) ?
                                    0 : 4 : 6;

                        mg_value += k*rr;
                        eg_value += k*rr;
                    }
                    else
                    // If the pawn is blocked by own pieces.
                    if (contains (_pos.pieces (Own), push_sq))
                    {
                        mg_value += 1*rr + 2*r;
                        eg_value += 1*rr + 2*r;
                    }
                }

                // Scale down bonus for candidate passers which need more than one 
                // pawn push to become passed or have a pawn in front of them.
                if (   !_pos.pawn_passed_at (Own, s+Push)
                    || 0 != (_pos.pieces (PAWN) & front_sqrs_bb (Own, s)))
                {
                    mg_value /= 2;
                    eg_value /= 2;
                }

                score += mk_score (mg_value, eg_value)
                       + PawnPassFile[std::min (_file (s), F_H - _file (s))]
                       - PawnPassHinder * pop_count (front_sqrs_bb (Own, s) & (pin_attacked_by[Opp][NONE] | _pos.pieces (Opp)));
            }

            if (Trace)
            {
                Tracer::write (PASSER, Own, score);
            }

            return score;
        }

        // Evaluates the space of the color
        // The space evaluation is a simple bonus based on the number of safe squares
        // available for minor pieces on the central four files on ranks 2-4
        // Safe squares one, two or three squares behind a friend pawn are counted twice
        // The aim is to improve play on opening
        template<bool Trace> template<Color Own>
        Score Evaluation<Trace>::evaluate_space ()
        {
            const auto Opp  = Own == WHITE ? BLACK : WHITE;
            const auto Pull = Own == WHITE ? DEL_S : DEL_N;
            const auto Dull = Own == WHITE ? DEL_SS : DEL_NN;
            // SpaceArea contains the area of the board which is considered by the space evaluation.
            // Bonus based on how many squares inside this area are safe.
            const Bitboard SpaceArea = Own == WHITE ?
                                                R2_bb|R3_bb|R4_bb :
                                                R7_bb|R6_bb|R5_bb;
            // Find the safe squares for our pieces inside the area defined by SpaceArea.
            // A square is safe:
            // - if not occupied by friend pawns
            // - if not attacked by an enemy pawns
            // - if defended or not attacked by an enemy pieces.
            Bitboard safe_space =
                   SpaceArea
                &  Side_bb[CS_NO]
                & ~_pos.pieces (Own, PAWN)
                & ~pin_attacked_by[Opp][PAWN]
                & (   pin_attacked_by[Own][NONE]
                   | ~pin_attacked_by[Opp][NONE]);

            // Since SpaceArea is fully on our half of the board
            assert((Own == WHITE ?
                        safe_space & U64(0xFFFFFFFF00000000) :
                        safe_space & U64(0x00000000FFFFFFFF)) == 0);

            // Find all squares which are at most three squares behind some friend pawn
            Bitboard behind = _pos.pieces (Own, PAWN);
            behind |= shift<Pull> (behind);
            behind |= shift<Dull> (behind);
            i32 count = pop_count (  (behind & safe_space)
                                   | (Own == WHITE ?
                                       safe_space << 32 :
                                       safe_space >> 32));
            i32 weight = _pos.count<NONE> (Own) - 2 * _pe->open_count;
            auto score = mk_score (count * weight * weight / 16, 0);

            if (Trace)
            {
                Tracer::write (SPACE, Own, score);
            }

            return score;
        }

        // Evaluates the initiative correction value for the position
        // i.e. second order bonus/malus based on the known attacking/defending status of the players
        template<bool Trace>
        Score Evaluation<Trace>::evaluate_initiative (Value eg)
        {
            i32 king_dist = dist<File> (_pos.square<KING> (WHITE), _pos.square<KING> (BLACK))
                          - dist<Rank> (_pos.square<KING> (WHITE), _pos.square<KING> (BLACK));

            // Compute the initiative bonus for the attacking side
            i32 initiative =  8 * (king_dist + _pe->asymmetry - 17)
                           + 12 * _pos.count<PAWN> ()
                             // Pawn on both flanks
                           + 16 * (   0 != (_pos.pieces (PAWN) & Side_bb[CS_KING])
                                   && 0 != (_pos.pieces (PAWN) & Side_bb[CS_QUEN]) ? 1 : 0);
            // Now apply the bonus: note that we find the attacking side by extracting
            // the sign of the endgame value, and that we carefully cap the bonus so
            // that the endgame score will never change sign after the bonus.
            return mk_score (0, sign (eg) * initiative);
        }

        // Evaluates the scale for the position
        template<bool Trace>
        Scale Evaluation<Trace>::evaluate_scale (Value eg)
        {
            assert(PHASE_ENDGAME <= _pos.phase () && _pos.phase () <= PHASE_MIDGAME);

            auto strong_color = eg >= VALUE_ZERO ? WHITE : BLACK;
            Scale scale;
            if (   nullptr == _me->scale_func[strong_color]
                || (scale = (*_me->scale_func[strong_color])(_pos)) == SCALE_NONE)
            {
                scale = _me->scale[strong_color];
            }
            assert(scale != SCALE_NONE);

            // If don't already have an unusual scale, check for certain types of endgames.
            switch (scale)
            {
            case SCALE_NORMAL:
            case SCALE_ONEPAWN:
                if (_pos.opposite_bishops ())
                {
                    return
                        // Endgame with opposite-colored bishops and no other pieces (ignoring pawns)
                           VALUE_MG_BSHP == _pos.si->non_pawn_material (WHITE) && 1 == _pos.count<BSHP> (WHITE)
                        && VALUE_MG_BSHP == _pos.si->non_pawn_material (BLACK) && 1 == _pos.count<BSHP> (BLACK) ?
                                1 >= _pos.count<PAWN> () ?
                                    Scale( 9) :
                                    Scale(31) :
                        // Endgame with opposite-colored bishops but also other pieces
                        // is still a bit drawish, but not as drawish as with only the two bishops. 
                                Scale(46);
                }
                // Endings where weaker side can place his king in front of the strong side pawns are drawish.
                if (   VALUE_EG_BSHP >= abs (eg)
                    && 2 >= _pos.count<PAWN> (strong_color)
                    && !_pos.pawn_passed_at (~strong_color, _pos.square<KING> (~strong_color)))
                {
                    return Scale(37 + 7 * _pos.count<PAWN> (strong_color));
                }
                break;
            default:
                break;
            }
            return scale;
        }

        // value() is the main function of the class. It computes the various parts of
        // the evaluation and returns the value of the position from the point of view
        // of the side to move.
        template<bool Trace>
        Value Evaluation<Trace>::value ()
        {
            assert(0 == _pos.si->checkers);

            // Probe the material hash table
            _me = Material::probe (_pos);
            // If have a specialized evaluation function for the material configuration
            if (nullptr != _me->value_func)
            {
                return (*_me->value_func) (_pos);
            }

            // Probe the pawn hash table
            _pe = Pawns::probe (_pos);

            // Score is computed internally from the white point of view, initialize by
            // - the incrementally updated scores (material + piece square tables).
            // - the material imbalance.
            // - the pawn score
            auto score =
                  _pos.si->psq_score
                + _me->imbalance
                + _pe->score;

            // Early exit if score is high
            auto v = (mg_value (score) + eg_value (score)) / 2;
            if (abs (v) > LazyThreshold)
            {
                return WHITE == _pos.active ? +v : -v;
            }

            initialize<WHITE> ();
            initialize<BLACK> ();
            
            mobility[WHITE] =
            mobility[BLACK] = SCORE_ZERO;

            // Evaluate all pieces except pawns and king
            score += evaluate_pieces<WHITE, NIHT> ()
                  -  evaluate_pieces<BLACK, NIHT> ();
            score += evaluate_pieces<WHITE, BSHP> ()
                  -  evaluate_pieces<BLACK, BSHP> ();
            score += evaluate_pieces<WHITE, ROOK> ()
                  -  evaluate_pieces<BLACK, ROOK> ();
            score += evaluate_pieces<WHITE, QUEN> ()
                  -  evaluate_pieces<BLACK, QUEN> ();
            // Evaluate piece mobility
            score += mobility[WHITE]
                  -  mobility[BLACK];

            // Full attack information needed including king
            // Evaluate kings
            score += evaluate_king<WHITE> ()
                  -  evaluate_king<BLACK> ();
            // Evaluate threats
            score += evaluate_threats<WHITE> ()
                  -  evaluate_threats<BLACK> ();
            // Evaluate passers
            score += evaluate_passers<WHITE> ()
                  -  evaluate_passers<BLACK> ();
            
            // Evaluate space, if in the opening phase
            if (_pos.si->non_pawn_material () >= SpaceThreshold)
            {
                score += evaluate_space<WHITE> ()
                      -  evaluate_space<BLACK> ();
            }

            // Evaluate potential for the position
            score += evaluate_initiative (eg_value (score));

            assert(-VALUE_INFINITE < mg_value (score) && mg_value (score) < +VALUE_INFINITE);
            assert(-VALUE_INFINITE < eg_value (score) && eg_value (score) < +VALUE_INFINITE);

            auto phase = _pos.phase ();
            // Interpolates between a midgame and a endgame score, scaled based on game phase.
            v = Value(  (  mg_value (score) * i32(phase)
                         + eg_value (score) * i32(PHASE_MIDGAME - phase)
                                                    // Evaluate scale for the position
                                            * i32(evaluate_scale (eg_value (score)))/SCALE_NORMAL)
                      / PHASE_MIDGAME);

            if (Trace)
            {
                // Write remaining evaluation terms
                Tracer::write (PAWN, _pe->score);
                Tracer::write (MATERIAL, _pos.si->psq_score);
                Tracer::write (IMBALANCE, _me->imbalance);
                Tracer::write (MOBILITY, mobility[WHITE], mobility[BLACK]);
                Tracer::write (TOTAL, score);
            }

            return (WHITE == _pos.active ? +v : -v) + Tempo; // Side to move point of view
        }
    }

    // evaluate() is the evaluator for the outer world. It returns a static evaluation
    // of the position from the point of view of the side to move.
    Value evaluate (const Position &pos)
    {
        return Evaluation<false> (pos).value ();
    }

    // trace_eval() returns a string (suitable for outputting to stdout) that contains
    // the detailed descriptions and values of each evaluation term.
    string trace_eval (const Position &pos)
    {
        std::memset (Tracer::cp, 0x00, sizeof (Tracer::cp));
        // White's point of view
        auto value = WHITE == pos.active ?
                        +Evaluation<true> (pos).value () :
                        -Evaluation<true> (pos).value ();

        ostringstream oss;
        oss << std::showpos << std::showpoint << std::setprecision (2) << std::fixed
            << "      Eval Term |    White    |    Black    |     Total    \n"
            << "                |   MG    EG  |   MG    EG  |   MG    EG   \n"
            << "----------------+-------------+-------------+--------------\n"
            << "       Material" << Term(MATERIAL)
            << "      Imbalance" << Term(IMBALANCE)
            << "           Pawn" << Term(PAWN)
            << "         Knight" << Term(NIHT)
            << "         Bishop" << Term(BSHP)
            << "           Rook" << Term(ROOK)
            << "          Queen" << Term(QUEN)
            << "       Mobility" << Term(MOBILITY)
            << "           King" << Term(KING)
            << "         Threat" << Term(THREAT)
            << "    Pawn Passer" << Term(PASSER)
            << "          Space" << Term(SPACE)
            << "----------------+-------------+-------------+--------------\n"
            << "          Total" << Term(TOTAL)
            << "\nEvaluation: " << value_to_cp (value) << " (white side)\n"
            << std::noshowpoint << std::noshowpos;
        return oss.str ();
    }
}
