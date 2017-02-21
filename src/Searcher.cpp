﻿#include "Searcher.h"

#include "UCI.h"
#include "PRNG.h"
#include "Transposition.h"
#include "Evaluator.h"
#include "Thread.h"
#include "Zobrist.h"
#include "TBsyzygy.h"
#include "Polyglot.h"
#include "Notation.h"
#include "Debugger.h"

using namespace std;
using namespace UCI;
using namespace BitBoard;
using namespace MoveGen;
using namespace Transposition;
using namespace Evaluator;
using namespace Threading;
using namespace Zobrists;
using namespace TBSyzygy;
using namespace Polyglot;
using namespace Notation;
using namespace Debugger;

// Extract ponder move from TT is called in case have no ponder move before exiting the search,
bool RootMove::extract_ponder_move_from_tt (Position &pos)
{
    assert(1 == size ());
    assert(MOVE_NONE != at (0));

    auto move = at (0);
    StateInfo si;
    pos.do_move (move, si);
    bool tt_hit;
    const auto *tte = TT.probe (pos.si->posi_key, tt_hit);
    Move ponder_move;
    if (   tt_hit
        && MOVE_NONE != (ponder_move = tte->move ()) // Local copy to be SMP safe
        && pos.pseudo_legal (ponder_move)
        && pos.legal (ponder_move))
    {
        assert(MoveList<LEGAL> (pos).contains (ponder_move));
        *this += ponder_move;
    }
    pos.undo_move (move);
    return 1 < size ();
}

RootMove::operator string () const
{
    ostringstream oss;
    for (auto move : *this)
    {
        assert(MOVE_NONE != move);
        oss << ' ' << move_to_can (move);
    }
    return oss.str ();
}

RootMoveVector::operator std::string () const
{
    std::ostringstream oss;
    for (const auto &rm : *this)
    {
        oss << rm << '\n';
    }
    return oss.str ();
}

namespace {

    const Value MaxValue = Value(1 << 28);

    enum Stage : u08
    {
        S_NATURAL_TT, S_NATURAL, S_GOOD_CAPTURE, S_QUIET, S_BAD_CAPTURE,
        S_EVASION_TT, S_EVASION, S_ALL_EVASION,
        S_PROBCUT_CAPTURE_TT, S_PROBCUT_CAPTURE, S_ALL_PROBCUT_CAPTURE,
        S_Q_CHECK_TT, S_Q_CHECK, S_Q_CHECK_CAPTURE, S_Q_CHECK_QUIET,
        S_Q_NO_CHECK_TT, S_Q_NO_CHECK, S_Q_NO_CHECK_CAPTURE,
        S_Q_RECAPTURE_TT, S_Q_RECAPTURE, S_Q_ALL_RECAPTURE,
    };
}

// Constructors of the MovePicker class. As arguments pass information to help
// it to return the (presumably) good moves first, to decide which moves to return
// (in the quiescence search, for instance, only want to search captures, promotions, and some checks)
// and about how important good move ordering is at the current node.

MovePicker::MovePicker (const Position &pos, Move ttm, const Stack *const &ss)
    : _pos (pos)
    , _ss (ss)
    , _tt_move (ttm)
{
    assert(ttm == MOVE_NONE
        || (pos.pseudo_legal (ttm)
         && pos.legal (ttm)));

    _stage =
        0 == pos.si->checkers ?
            S_NATURAL_TT :
            S_EVASION_TT;
    if (_tt_move == MOVE_NONE)
    {
        ++_stage;
    }
}
MovePicker::MovePicker (const Position &pos, Move ttm, const Stack *const &ss, i16 d, Move lm)
    : _pos (pos)
    , _ss (ss)
    , _tt_move (ttm)
{
    assert(d <= 0);
    assert(ttm == MOVE_NONE
        || (pos.pseudo_legal (ttm)
         && pos.legal (ttm)));

    if (0 != pos.si->checkers)
    {
        _stage = S_EVASION_TT;
    }
    else
    if (d >= 0)
    {
        _stage = S_Q_CHECK_TT;
    }
    else
    if (d > -5)
    {
        _stage = S_Q_NO_CHECK_TT;
    }
    else
    {
        assert(MOVE_NONE != lm);

        _stage = S_Q_RECAPTURE_TT;
        _recap_sq = dst_sq (lm);
        if (   MOVE_NONE != _tt_move
            && !(   pos.capture (_tt_move)
                 && dst_sq (_tt_move) == _recap_sq))
        {
            _tt_move = MOVE_NONE;
        }
    }
    if (_tt_move == MOVE_NONE)
    {
        ++_stage;
    }
}
MovePicker::MovePicker (const Position &pos, Move ttm, Value thr)
    : _pos (pos)
    , _tt_move (ttm)
    , _threshold (thr)
{
    assert(0 == pos.si->checkers);
    assert(ttm == MOVE_NONE
        || (pos.pseudo_legal (ttm)
         && pos.legal (ttm)));

    _stage = S_PROBCUT_CAPTURE_TT;

    // In ProbCut we generate captures with SEE greater than or equal to the given threshold
    if (   MOVE_NONE != _tt_move
        && !(   pos.capture (_tt_move)
             && pos.see_ge (_tt_move, _threshold)))
    {
        _tt_move = MOVE_NONE;
    }
    if (_tt_move == MOVE_NONE)
    {
        ++_stage;
    }
}

// Assigns a numerical move ordering score to each move in a move list.
// The moves with highest scores will be picked first.

// Winning and equal captures in the main search are ordered by MVV/LVA, preferring captures near our home rank.
// Surprisingly, this appears to perform slightly better than SEE-based move ordering,
// exchanging big pieces before capturing a hanging piece probably helps to reduce the subtree size.
// In the main search push captures with negative SEE values to the bad captures vector,
// but instead of doing it now we delay until the move has been picked up,
// saving some SEE calls in case of a cutoff.
template<> void MovePicker::value<CAPTURE> ()
{
    for (auto &vm : _moves)
    {
        assert(_pos.pseudo_legal (vm.move)
            && _pos.legal (vm.move));

        vm.value =
              PieceValues[MG][_pos.cap_type (vm.move)]
            - 200 * Value(rel_rank (_pos.active, dst_sq (vm.move)))
            - Value(ptype (_pos[org_sq (vm.move)]));
    }
}
template<> void MovePicker::value<QUIET> ()
{
    for (auto &vm : _moves)
    {
        assert(_pos.pseudo_legal (vm.move)
            && _pos.legal (vm.move));

        vm.value =
              _pos.thread->history(_pos.active, vm.move)
            + ((_ss-1)->m_history != nullptr ? (*(_ss-1)->m_history)(_pos[org_sq (vm.move)], dst_sq (vm.move)) : VALUE_ZERO)
            + ((_ss-2)->m_history != nullptr ? (*(_ss-2)->m_history)(_pos[org_sq (vm.move)], dst_sq (vm.move)) : VALUE_ZERO)
            + ((_ss-4)->m_history != nullptr ? (*(_ss-4)->m_history)(_pos[org_sq (vm.move)], dst_sq (vm.move)) : VALUE_ZERO);
    }
}
// First captures ordered by MVV/LVA, then non-captures ordered by stats heuristics
template<> void MovePicker::value<EVASION> ()
{
    for (auto &vm : _moves)
    {
        assert(_pos.pseudo_legal (vm.move)
            && _pos.legal (vm.move));
        
        if (_pos.capture (vm.move))
        {
            vm.value =
                  PieceValues[MG][_pos.cap_type (vm.move)]
                - Value(ptype (_pos[org_sq (vm.move)]))
                + MaxValue;
        }
        else
        {
            vm.value =
                  _pos.thread->history(_pos.active, vm.move);
                //+ ((_ss-1)->m_history != nullptr ? (*(_ss-1)->m_history)(_pos[org_sq (vm.move)], dst_sq (vm.move)) : VALUE_ZERO)
                //+ ((_ss-2)->m_history != nullptr ? (*(_ss-2)->m_history)(_pos[org_sq (vm.move)], dst_sq (vm.move)) : VALUE_ZERO)
                //+ ((_ss-4)->m_history != nullptr ? (*(_ss-4)->m_history)(_pos[org_sq (vm.move)], dst_sq (vm.move)) : VALUE_ZERO);
        }
    }
}

// Finds the best move in the range [beg, end) and moves it to front,
// it is faster than sorting all the moves in advance when there are few moves (e.g. the possible captures).
ValMove& MovePicker::pick_best_move (u08 i)
{
    auto itr = _moves.begin () + i;
    std::swap (*itr, *std::max_element (itr, _moves.end ()));
    return *itr;
}
// Returns a new legal move every time it is called, until there are no more moves left.
// It picks the move with the biggest value from a list of generated moves.
Move MovePicker::next_move ()
{
    switch (_stage)
    {

    case S_NATURAL_TT:
    case S_EVASION_TT:
    case S_PROBCUT_CAPTURE_TT:
    case S_Q_CHECK_TT:
    case S_Q_NO_CHECK_TT:
    case S_Q_RECAPTURE_TT:
        ++_stage;
        return _tt_move;

    case S_NATURAL:
        ++_stage;
        generate<CAPTURE> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<CAPTURE> ();
        }
    case S_GOOD_CAPTURE:
        while (_index < _moves.size ())
        {
            auto move = pick_best_move (_index++).move;
            if (_pos.see_ge (move, VALUE_ZERO))
            {
                return move;
            }
            // Losing capture, add it to the capture moves
            _capture_moves.push_back (move);
        }
        ++_stage;
        generate<QUIET> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<QUIET> ();
        }
        // Killers to top of quiet move
        {
            MoveVector killer_moves (_ss->killer_moves, _ss->killer_moves + MaxKillers);
            if ((_ss-1)->m_history != nullptr)
            {
                auto move = (_ss-1)->current_move;
                auto pc = _pos[fix_dst_sq (move)];

                auto cm = _pos.thread->counter_moves(pc, move);
                if (   MOVE_NONE != cm
                    && _tt_move != cm
                    && std::find (killer_moves.begin (), killer_moves.end (), cm) == killer_moves.end ())
                {
                    killer_moves.push_back (cm);
                }

                auto dst = dst_sq (move);
                auto pt  = ptype (pc);
                Bitboard attacks = pt != PAWN ?
                    PieceAttacks[pt][dst] ^ org_sq (move) : 0;
                while (0 != attacks)
                {
                    auto org = pop_lsq (attacks);
                    cm = _pos.thread->counter_moves(pc, mk_move<NORMAL> (org, dst));
                    if (   MOVE_NONE != cm
                        && _tt_move != cm
                        && std::find (killer_moves.begin (), killer_moves.end (), cm) == killer_moves.end ())
                    {
                        killer_moves.push_back (cm);
                    }
                }
            }
            killer_moves.erase (std::remove_if (killer_moves.begin (),
                                                killer_moves.end (),
                                                [&](Move m)
                                                {
                                                    return m == MOVE_NONE
                                                        || m == _tt_move;
                                                }),
                                killer_moves.end ());
            i32 k = 0;
            for (auto km : killer_moves)
            {
                auto itr = std::find (_moves.begin (), _moves.end (), km);
                if (itr != _moves.end ())
                {
                    assert(_pos.pseudo_legal (km)
                        && _pos.legal (km));
                    itr->value = MaxValue - k++;
                }
            }
        }
    case S_QUIET:
        while (_index < _moves.size ())
        {
            return pick_best_move (_index++).move;
        }
        ++_stage;
        _index = 0;
    case S_BAD_CAPTURE:
        while (_index < _capture_moves.size ())
        {
            return _capture_moves[_index++];
        }
        break; // BREAK

    case S_EVASION:
        assert(0 != _pos.si->checkers);
        ++_stage;
        generate<EVASION> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<EVASION> ();
        }
    case S_ALL_EVASION:
        while (_index < _moves.size ())
        {
            return pick_best_move (_index++).move;
        }
        break; // BREAK

    case S_PROBCUT_CAPTURE:
        ++_stage;
        generate<CAPTURE> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<CAPTURE> ();
        }
    case S_ALL_PROBCUT_CAPTURE:
        while (_index < _moves.size ())
        {
            auto move = pick_best_move (_index++).move;
            if (_pos.see_ge (move, _threshold))
            {
                return move;
            }
        }
        break; // BREAK

    case S_Q_CHECK:
        ++_stage;
        generate<CAPTURE> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<CAPTURE> ();
        }
    case S_Q_CHECK_CAPTURE:
        while (_index < _moves.size ())
        {
            return pick_best_move (_index++).move;
        }
        ++_stage;
        generate<QUIET_CHECK> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<QUIET> ();
        }
    case S_Q_CHECK_QUIET:
        while (_index < _moves.size ())
        {
            return pick_best_move (_index++).move;
        }
        break; // BREAK

    case S_Q_NO_CHECK:
        ++_stage;
        generate<CAPTURE> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<CAPTURE> ();
        }
    case S_Q_NO_CHECK_CAPTURE:
        while (_index < _moves.size ())
        {
            return pick_best_move (_index++).move;
        }
        break; // BREAK

    case S_Q_RECAPTURE:
        ++_stage;
        generate<CAPTURE> (_moves, _pos);
        filter_illegal (_moves, _pos);
        if (MOVE_NONE != _tt_move)
        {
            _moves.erase (std::remove (_moves.begin (), _moves.end (), _tt_move), _moves.end ());
        }
        _index = 0;
        if (_moves.size () > 1)
        {
            value<CAPTURE> ();
        }
    case S_Q_ALL_RECAPTURE:
        while (_index < _moves.size ())
        {
            auto move = pick_best_move (_index++).move;
            if (dst_sq (move) == _recap_sq)
            {
                return move;
            }
        }
        break; // BREAK

    default:
        assert(false);
        break;
    }
    return MOVE_NONE;
}

namespace Searcher {

    Limit  Limits;

    atomic_bool
           ForceStop     { false }  // Stop search on request
        ,  PonderhitStop { false }; // Stop search on ponder-hit

    u16    MultiPV       = 1;
    //i32    MultiPV_cp    = 0;

    i16    FixedContempt = 0
        ,  ContemptTime  = 30
        ,  ContemptValue = 50;

    string HashFile     = "Hash.dat";

    bool   OwnBook      = false;
    string BookFile     = "Book.bin";
    bool   BookMoveBest = true;
    i16    BookUptoMove = 20;

    i16    TBProbeDepth = 1;
    i32    TBLimitPiece = 6;
    bool   TBUseRule50  = true;
    bool   TBHasRoot    = false;
    Value  TBValue      = VALUE_ZERO;

    string OutputFile   = Empty;

    namespace {

// Preloads the given address in L1/L2 cache.
// This is a non-blocking function that doesn't stall
// the CPU waiting for data to be loaded from memory,
// which can be quite slow.
#if defined(PREFETCH)
#   if defined(_MSC_VER) || defined(__INTEL_COMPILER)

#       include <xmmintrin.h> // Intel and Microsoft header for _mm_prefetch()
        void prefetch (const void *addr)
        {
#       if defined(__INTEL_COMPILER)
            // This hack prevents prefetches from being optimized away by
            // Intel compiler. Both MSVC and gcc seem not be affected by this.
            __asm__ ("");
#       endif
            _mm_prefetch (reinterpret_cast<const char*> (addr), _MM_HINT_T0);
        }

#   else
        void prefetch (const void *addr)
        {
            __builtin_prefetch (addr);
        }
#   endif
#else
        void prefetch (const void *)
        {}
#endif

        const i16 MaxRazorDepth = 4;
        // RazorMargins[depth]
        Value RazorMargins[MaxRazorDepth] = { Value(0), Value(570), Value(602), Value(554) };

        const i16 MaxFutilityDepth = 16;
        // FutilityMoveCounts[improving][depth]
        u08 FutilityMoveCounts[2][MaxFutilityDepth];

        const i16 MaxReductionDepth = 64;
        const u08 MaxReductionMoveCount = 64;
        // ReductionDepths[pv][improving][depth][move_count]
        i16 ReductionDepths[2][2][MaxReductionDepth][MaxReductionMoveCount];
        i16 reduction_depth (bool pv, bool imp, i16 d, u08 mc)
        {
            return ReductionDepths[pv ? 1 : 0]
                                  [imp ? 1 : 0]
                                  [min (d, i16(MaxReductionDepth-1))]
                                  [min (mc, u08(MaxReductionMoveCount-1))];
        }

        Value DrawValue     [CLR_NO]
            , BaseContempt  [CLR_NO];

        ofstream OutputStream;
        bool     WriteOutput = false;

        // check_limits() is used to print debug info and, more importantly,
        // to detect when out of available limits and thus stop the search.
        void check_limits ()
        {
            auto elapsed_time = Threadpool.time_mgr.elapsed_time ();

            if (elapsed_time - Limits.elapsed_time >= MilliSec)
            {
                Limits.elapsed_time = elapsed_time;
                dbg_print ();
            }

            // Do not stop until told so by the GUI
            if (   Limits.infinite
                || Limits.ponder)
            {
                return;
            }

            if (   (   Limits.use_time_management ()
                    && elapsed_time > Threadpool.time_mgr.maximum_time - 10) // 2*TimerResolution
                || (   0 != Limits.movetime
                    && elapsed_time >= Limits.movetime)
                || (   0 != Limits.nodes
                    && Threadpool.nodes () >= Limits.nodes))
            {
                ForceStop = true;
            }
        }

        // History and stats update bonus, based on depth
        Value stat_bonus (i16 depth)
        {
            return Value (depth*(depth + 2) - 2);
        }

        // Updates countermoves and followupmoves history stats
        void update_cm_stats (Stack *const &ss, Piece pc, Square s, Value value)
        {
            assert(pc != NO_PIECE);
            assert(_ok (s));

            if ((ss-1)->m_history != nullptr)
            {
                (ss-1)->m_history->update (pc, s, value);
            }
            if ((ss-2)->m_history != nullptr)
            {
                (ss-2)->m_history->update (pc, s, value);
            }
            if ((ss-4)->m_history != nullptr)
            {
                (ss-4)->m_history->update (pc, s, value);
            }
        }
        // Updates move sorting heuristics
        void update_stats (Stack *const &ss, const Position &pos, Move move, Value value)
        {
            assert(!pos.empty (org_sq (move)));
            assert(value > VALUE_ZERO);

            if (ss->killer_moves[0] != move)
            {
                ss->killer_moves[1] = ss->killer_moves[0];
                ss->killer_moves[0] = move;
            }
            assert(1 == std::count (ss->killer_moves, ss->killer_moves + MaxKillers, move));

            if ((ss-1)->m_history != nullptr)
            {
                pos.thread->counter_moves.update (pos[fix_dst_sq ((ss-1)->current_move)], (ss-1)->current_move, move);
            }

            pos.thread->history.update (pos.active, move, value);
            update_cm_stats (ss, pos[org_sq (move)], dst_sq (move), value);
        }

        // Appends the move and child pv vector
        void update_pv (MoveVector &pv, Move move, const MoveVector &child_pv)
        {
            pv.clear ();
            pv.push_back (move);
            for (auto m : child_pv)
            {
                pv.push_back (m);
            }
        }

        // It adjusts a mate score from "plies to mate from the root" to "plies to mate from the current position".
        // Non-mate scores are unchanged.
        // The function is called before storing a value to the transposition table.
        Value value_to_tt (Value v, i32 ply)
        {
            assert(v != VALUE_NONE);
            return v >= +VALUE_MATE_IN_MAX_PLY ? v + ply :
                   v <= -VALUE_MATE_IN_MAX_PLY ? v - ply :
                   v;
        }
        // It adjusts a mate score from "plies to mate from the current position" to "plies to mate from the root".
        // Non-mate scores are unchanged.
        // The function is called after retrieving a value of the transposition table.
        Value value_of_tt (Value v, i32 ply)
        {
            return v == VALUE_NONE             ? VALUE_NONE :
                   v >= +VALUE_MATE_IN_MAX_PLY ? v - ply :
                   v <= -VALUE_MATE_IN_MAX_PLY ? v + ply :
                   v;
        }

        // Formats PV information according to UCI protocol.
        // UCI requires to send all the PV lines also if are still to be searched
        // and so refer to the previous search score.
        string multipv_info (Thread *const &th, Value alfa, Value beta)
        {
            auto elapsed_time   = std::max (Threadpool.time_mgr.elapsed_time (), TimePoint(1));
            auto total_nodes    = Threadpool.nodes ();
            auto tb_hits        = Threadpool.tb_hits () + (TBHasRoot ? th->root_moves.size () : 0);
            assert(elapsed_time > 0);

            ostringstream oss;
            for (u16 i = 0; i < Threadpool.pv_limit; ++i)
            {
                auto d = i <= th->pv_index ?
                    th->running_depth :
                    th->running_depth - 1;
                if (d <= 0)
                {
                    continue;
                }
                auto v = i <= th->pv_index ?
                    th->root_moves[i].new_value :
                    th->root_moves[i].old_value;
                bool tb =
                       TBHasRoot
                    && abs (v) < +VALUE_MATE - i32(MaxPlies);

                oss << "info"
                    << " multipv "  << i + 1
                    << " depth "    << d
                    << " seldepth " << th->max_ply
                    << " score "    << to_string (tb ? TBValue : v)
                    << (!tb && i == th->pv_index ?
                            beta <= v ? " lowerbound" :
                                v <= alfa ? " upperbound" : "" : "")
                    << " nodes "    << total_nodes
                    << " time "     << elapsed_time
                    << " nps "      << total_nodes * MilliSec / elapsed_time
                    << " hashfull " << (elapsed_time > MilliSec ? TT.hash_full () : 0)
                    << " tbhits "   << tb_hits
                    << " pv"        << th->root_moves[i];
                if (i+1 < Threadpool.pv_limit)
                {
                    oss << '\n';
                }
            }
            return oss.str ();
        }

        // The quiescence search function,
        // which is called by the main depth limited search function
        // when the remaining depth is less than equal to 0.
        template<bool PVNode>
        Value quien_search (Position &pos, Stack *const &ss, Value alfa, Value beta, i16 depth = 0)
        {
            assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
            assert(PVNode || (alfa == beta-1));
            assert(0 >= depth);
            assert(ss->ply > 1
                && ss->ply == (ss-1)->ply + 1
                && ss->ply <= MaxPlies);

            const bool in_check = 0 != pos.si->checkers;

            Value pv_alfa;

            if (PVNode)
            {
                pv_alfa = alfa; // To flag BOUND_EXACT when eval above alfa and no available moves

                ss->pv.clear ();
            }

            // Check for an immediate draw or maximum ply reached
            if (   ss->ply >= MaxPlies
                || pos.draw (ss->ply))
            {
                return ss->ply >= MaxPlies
                    && !in_check ?
                        evaluate (pos) :
                        DrawValue[pos.active];
            }

            auto *th = pos.thread;

            Move move;
            // Transposition table lookup
            auto posi_key = pos.si->posi_key;
            bool tt_hit;
            auto *tte = TT.probe (posi_key, tt_hit);
            auto tt_move =
                   tt_hit
                && MOVE_NONE != (move = tte->move ())
                && pos.pseudo_legal (move)
                && pos.legal (move) ?
                    move :
                    MOVE_NONE;
            assert(tt_move == MOVE_NONE
                || (pos.pseudo_legal (tt_move)
                 && pos.legal (tt_move)));

            ss->current_move = MOVE_NONE;

            // Decide whether or not to include checks.
            // Fixes also the type of TT entry depth that are going to use.
            // Note that in quien_search use only 2 types of depth: (0) or (-1).
            i16 qs_depth = in_check || 0 <= depth ? 0 : -1;
            auto tt_value = value_of_tt (tte->value (), ss->ply);

            if (   !PVNode
                && tt_hit
                && tt_value != VALUE_NONE // Only in case of TT access race
                && tte->depth () >= qs_depth
                && (tte->bound () & (tt_value >= beta ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
            {
                return tt_value;
            }

            Value best_value
                , futility_base;

            // Evaluate the position statically
            if (in_check)
            {
                ss->static_eval = VALUE_NONE;
                best_value =
                futility_base = -VALUE_INFINITE;
            }
            else
            {
                Value tt_eval;
                if (tt_hit)
                {
                    // Never assume anything on values stored in TT
                    ss->static_eval = tt_eval =
                        tte->eval () != VALUE_NONE ?
                            tte->eval () :
                            evaluate (pos);
                    // Can tt_value be used as a better position evaluation?
                    if (   tt_value != VALUE_NONE
                        && (tte->bound () & (tt_value > tt_eval ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
                    {
                        tt_eval = tt_value;
                    }
                }
                else
                {
                    ss->static_eval = tt_eval =
                        (ss-1)->current_move != MOVE_NULL ?
                            evaluate (pos) :
                            -(ss-1)->static_eval + 2*Tempo;

                    //tte->save (posi_key,
                    //           MOVE_NONE,
                    //           VALUE_NONE,
                    //           ss->static_eval,
                    //           -6,
                    //           BOUND_NONE);
                }

                if (alfa < tt_eval)
                {
                    // Stand pat. Return immediately if static value is at least beta
                    if (tt_eval >= beta)
                    {
                        if (!tt_hit)
                        {
                            tte->save (posi_key,
                                       MOVE_NONE,
                                       value_to_tt (tt_eval, ss->ply),
                                       ss->static_eval,
                                       -6,
                                       BOUND_LOWER);
                        }

                        assert(-VALUE_INFINITE < tt_eval && tt_eval < +VALUE_INFINITE);
                        return tt_eval;
                    }

                    assert(tt_eval < beta);
                    // Update alfa! Always alfa < beta
                    if (PVNode)
                    {
                        alfa = tt_eval;
                    }
                }

                best_value = tt_eval;
                futility_base = best_value + 128;
            }

            auto best_move = MOVE_NONE;

            // Initialize move picker (2) for the current position.
            MovePicker mp (pos, tt_move, ss, depth, (ss-1)->current_move);
            StateInfo si;
            // Loop through the moves until no moves remain or a beta cutoff occurs.
            while (MOVE_NONE != (move = mp.next_move ()))
            {
                assert(pos.pseudo_legal (move)
                    && pos.legal (move));

                bool gives_check = pos.gives_check (move);

                // Futility pruning
                if (   !in_check
                    && futility_base <= alfa
                    && futility_base > -VALUE_KNOWN_WIN
                    && !gives_check
                    //&& 0 == Limits.mate
                        // Advance pawn push
                    && !(   PAWN == ptype (pos[org_sq (move)])
                         && rel_rank (pos.active, org_sq (move)) > R_4))
                {
                    // Futility pruning parent node
                    auto futility_value = futility_base + PieceValues[EG][ptype (pos[dst_sq (move)])];
                    if (futility_value <= alfa)
                    {
                        if (best_value < futility_value)
                        {
                            best_value = futility_value;
                        }
                        continue;
                    }
                    // Prune moves with negative or zero SEE
                    if (!pos.see_ge (move, VALUE_ONE))
                    {
                        if (best_value < futility_base)
                        {
                            best_value = futility_base;
                        }
                        continue;
                    }
                }

                // Don't search moves with negative SEE values
                if (   (   !in_check
                        // Evasion Prunable: Detect non-capture evasions that are candidate to be pruned
                        || (   best_value > -VALUE_MATE_IN_MAX_PLY
                            && !pos.capture (move)))
                    && PROMOTE != mtype (move)
                    //&& 0 == Limits.mate
                    && !pos.see_ge (move, VALUE_ZERO))
                {
                    continue;
                }

                ss->current_move = move;

                bool capture_or_promotion = pos.capture_or_promotion (move);

                // Speculative prefetch as early as possible
                prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                // Make the move
                pos.do_move (move, si, gives_check);

                if (   PAWN == ptype (pos[dst_sq (move)])
                    || PAWN == pos.si->capture)
                {
                    prefetch (th->pawn_table[pos.si->pawn_key]);
                }
                if (capture_or_promotion)
                {
                    prefetch (th->matl_table[pos.si->matl_key]);
                }

                auto value = -quien_search<PVNode> (pos, ss+1, -beta, -alfa, depth - 1);

                // Undo the move
                pos.undo_move (move);

                assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Check for new best move
                if (best_value < value)
                {
                    best_value = value;

                    if (alfa < value)
                    {
                        // Update pv even in fail-high case
                        if (PVNode)
                        {
                            update_pv (ss->pv, move, (ss+1)->pv);
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            tte->save (posi_key,
                                       move,
                                       value_to_tt (value, ss->ply),
                                       ss->static_eval,
                                       qs_depth,
                                       BOUND_LOWER);

                            assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);
                            return value;
                        }
                        assert(value < beta);

                        // Update alfa! Always alfa < beta
                        if (PVNode)
                        {
                            alfa = value;
                            best_move = move;
                        }
                    }
                }
            }

            // All legal moves have been searched.
            // A special case: If in check and no legal moves were found, it is checkmate.
            if (   in_check
                && best_value == -VALUE_INFINITE)
            {
                // Plies to mate from the root
                return mated_in (ss->ply);
            }

            tte->save (posi_key,
                       best_move,
                       value_to_tt (best_value, ss->ply),
                       ss->static_eval,
                       qs_depth,
                          PVNode
                       && pv_alfa < best_value ?
                           BOUND_EXACT :
                           BOUND_UPPER);

            assert(-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }
        // The main depth limited search function.
        template<bool PVNode>
        Value depth_search (Position &pos, Stack *const &ss, Value alfa, Value beta, i16 depth, bool cut_node, bool prun_node, Move exclude_move = MOVE_NONE)
        {
            const bool root_node = 1 == ss->ply;
            assert((root_node && PVNode) || !root_node);
            assert(!(PVNode && cut_node));
            assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
            assert(PVNode || (alfa == beta-1));
            assert(0 < depth && depth < MaxPlies);
            assert(ss->ply >= 1
                && ss->ply == (ss-1)->ply + 1
                && ss->ply <= MaxPlies);

            const bool in_check = 0 != pos.si->checkers;

            ss->move_count = 0;
            ss->history_val = VALUE_ZERO;

            // Step 1. Initialize node
            auto *th = pos.thread;

            // Check count reset
            if (th->count_reset.load (memory_order_relaxed))
            {
                th->count_reset = false;
                // At low node count increase the checking rate otherwise use a default value
                th->check_count = u16(0 != Limits.nodes ? std::min (std::max (i32(std::round ((double) Limits.nodes / 0x1000)), 1), 0x1000) : 0x1000);
            }
            assert(0 != th->check_count);
            // Check for the available remaining limit
            if (0 == --th->check_count)
            {
                Threadpool.reset ();
                check_limits ();
            }

            if (PVNode)
            {
                if (th->max_ply < ss->ply)
                {
                    th->max_ply = ss->ply;
                }
            }

            if (!root_node)
            {
                // Step 2. Check end condition
                // Check for aborted search, immediate draw or maximum ply reached
                if (   ForceStop.load (memory_order_relaxed)
                    || ss->ply >= MaxPlies
                    || pos.draw (ss->ply))
                {
                    return ss->ply >= MaxPlies
                        && !in_check ?
                            evaluate (pos) :
                            DrawValue[pos.active];
                }

                // Step 3. Mate distance pruning.
                // Even if mate at the next move our score would be at best mates_in(ss->ply+1),
                // but if alfa is already bigger because a shorter mate was found upward in the tree
                // then there is no need to search further, will never beat current alfa.
                // Same logic but with reversed signs applies also in the opposite condition of
                // being mated instead of giving mate, in this case return a fail-high score.
                alfa = std::max (mated_in (ss->ply+0), alfa);
                beta = std::min (mates_in (ss->ply+1), beta);
                if (alfa >= beta)
                {
                    return alfa;
                }
            }

            ss->current_move = MOVE_NONE;
            ss->m_history = nullptr;
            std::fill_n ((ss+2)->killer_moves, MaxKillers, MOVE_NONE);

            Move move;
            // Step 4. Transposition table lookup
            // Don't want the score of a partial search to overwrite a previous full search
            // TT value, so use a different position key in case of an excluded move.
            auto posi_key = pos.si->posi_key ^ Key(exclude_move);
            bool tt_hit;
            auto *tte = TT.probe (posi_key, tt_hit);
            auto tt_move =
                root_node ?
                    th->root_moves[th->pv_index][0] :
                       tt_hit
                    && MOVE_NONE != (move = tte->move ())
                    && pos.pseudo_legal (move)
                    && pos.legal (move) ?
                        move :
                        MOVE_NONE;
            assert(tt_move == MOVE_NONE
                || (pos.pseudo_legal (tt_move)
                 && pos.legal (tt_move)));

            auto tt_value = value_of_tt (tte->value (), ss->ply);

            // At non-PV nodes we check for an early TT cutoff
            if (   !PVNode
                && tt_hit
                && tt_value != VALUE_NONE // Only in case of TT access race
                && tte->depth () >= depth
                && (tte->bound () & (tt_value >= beta ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
            {
                // Update move sorting heuristics on tt_move
                if (MOVE_NONE != tt_move)
                {
                    if (tt_value >= beta)
                    {
                        // Bonus for a quiet tt_move
                        if (!pos.capture_or_promotion (tt_move))
                        {
                            update_stats (ss, pos, tt_move, stat_bonus (depth));
                        }
                        // Extra penalty for a quiet tt_move in previous ply when it gets refuted
                        if (   1 == (ss-1)->move_count
                            && NONE == pos.si->capture)
                        {
                            update_cm_stats (ss-1, pos[fix_dst_sq ((ss-1)->current_move)], dst_sq ((ss-1)->current_move), -stat_bonus (depth + 1));
                        }
                    }
                    else
                    // Penalty for a quiet tt_move that fails low
                    if (!pos.capture_or_promotion (tt_move))
                    {
                        auto penalty = -stat_bonus (depth + 1);
                        th->history.update (pos.active, tt_move, penalty);
                        update_cm_stats (ss, pos[org_sq (tt_move)], dst_sq (tt_move), penalty);
                    }
                }
                return tt_value;
            }

            // Step 4A. Tablebase probe
            if (   !root_node
                && 0 != TBLimitPiece)
            {
                auto piece_count = pos.count<NONE> ();

                if (   (   piece_count < TBLimitPiece
                        || (   piece_count == TBLimitPiece
                            && depth >= TBProbeDepth))
                    && 0 == pos.si->clock_ply
                    && !pos.has_castleright (CR_ANY))
                {
                    ProbeState state;
                    WDLScore v = probe_wdl (pos, state);

                    if (state != FAIL)
                    {
                        ++th->tb_hits;

                        auto draw_v = TBUseRule50 ? 1 : 0;

                        auto value = v < -draw_v ? -VALUE_MATE + i32(MaxPlies + ss->ply) :
                                     v > +draw_v ? +VALUE_MATE - i32(MaxPlies + ss->ply) : VALUE_ZERO + 2 * draw_v * v;

                        tte->save (posi_key,
                                   MOVE_NONE,
                                   value_to_tt (value, ss->ply),
                                   //0 == pos.si->checkers ?
                                   //    evaluate (pos) :
                                       VALUE_NONE,
                                   i16(std::min (depth + 6, MaxPlies - 1)),
                                   BOUND_EXACT);

                        return value;
                    }
                }
            }

            StateInfo si;

            // Step 5. Evaluate the position statically
            if (in_check)
            {
                ss->static_eval = VALUE_NONE;
            }
            else
            {
                Value tt_eval;
                if (tt_hit)
                {
                    // Never assume anything on values stored in TT
                    ss->static_eval = tt_eval =
                        tte->eval () != VALUE_NONE ?
                            tte->eval () :
                            evaluate (pos);
                    // Can tt_value be used as a better position evaluation?
                    if (   tt_value != VALUE_NONE
                        && (tte->bound () & (tt_value > tt_eval ? BOUND_LOWER : BOUND_UPPER)) != BOUND_NONE)
                    {
                        tt_eval = tt_value;
                    }
                }
                else
                {
                    ss->static_eval = tt_eval =
                        (ss-1)->current_move != MOVE_NULL ?
                            evaluate (pos) :
                            -(ss-1)->static_eval + 2*Tempo;

                    tte->save (posi_key,
                               MOVE_NONE,
                               VALUE_NONE,
                               ss->static_eval,
                               -6,
                               BOUND_NONE);
                }

                if (prun_node)
                {
                    // Step 6. Razoring sort of forward pruning where rather than
                    // skipping an entire subtree, search it to a reduced depth.
                    if (   !PVNode
                        && MaxRazorDepth > depth
                        //&& 0 == Limits.mate
                        && tt_eval + RazorMargins[depth] <= alfa)
                    {
                        if (1 >= depth)
                        {
                            return quien_search<false> (pos, ss, alfa, alfa+1);
                        }
                        auto alfa_margin = alfa - RazorMargins[depth];
                        assert(alfa_margin >= -VALUE_INFINITE);
                        auto value = quien_search<false> (pos, ss, alfa_margin, alfa_margin+1);
                        if (value <= alfa_margin)
                        {
                            return value;
                        }
                    }

                    // Step 7. Futility pruning: child node
                    // Betting that the opponent doesn't have a move that will reduce
                    // the score by more than futility margins [depth] if do a null move.
                    if (   !root_node
                        && 7 > depth
                        && tt_eval < +VALUE_KNOWN_WIN
                        && tt_eval - 150*depth >= beta
                        //&& 0 == Limits.mate
                        && pos.si->non_pawn_matl[pos.active] > VALUE_ZERO)
                    {
                        return tt_eval;
                    }

                    // Step 8. Null move search with verification search
                    if (   !PVNode
                        && tt_eval >= beta
                        //&& 0 == Limits.mate
                        && (   12 < depth
                            || ss->static_eval >= beta - 35*(depth - 6))
                        && pos.si->non_pawn_matl[pos.active] > VALUE_ZERO)
                    {
                        assert(exclude_move == MOVE_NONE);

                        ss->current_move = MOVE_NULL;
                        ss->m_history = nullptr;

                        // Null move dynamic reduction based on depth and static evaluation
                        auto reduced_depth = i16(depth - (67*depth + 823) / 256 + std::min (i16(tt_eval - beta)/VALUE_MG_PAWN, 3));

                        // Speculative prefetch as early as possible
                        prefetch (TT.cluster_entry (  pos.si->posi_key
                                                    ^ RandZob.color_key
                                                    ^ (pos.si->en_passant_sq != SQ_NO ? RandZob.en_passant_keys[_file (pos.si->en_passant_sq)] : 0)));

                        pos.do_null_move (si);
                        auto null_value =
                            reduced_depth <= 0 ?
                                -quien_search<false> (pos, ss+1, -beta, -beta+1) :
                                -depth_search<false> (pos, ss+1, -beta, -beta+1, reduced_depth, !cut_node, false);
                        pos.undo_null_move ();

                        if (null_value >= beta)
                        {
                            // Don't do verification search at low depths
                            if (   12 > depth
                                && abs (beta) < +VALUE_KNOWN_WIN)
                            {
                                // Don't return unproven mates
                                return null_value < +VALUE_MATE_IN_MAX_PLY ?
                                        null_value : beta;
                            }

                            // Do verification search at high depths
                            auto value =
                                reduced_depth <= 0 ?
                                    quien_search<false> (pos, ss, beta-1, beta) :
                                    depth_search<false> (pos, ss, beta-1, beta, reduced_depth, false, false);

                            if (value >= beta)
                            {
                                // Don't return unproven mates
                                return null_value < +VALUE_MATE_IN_MAX_PLY ?
                                        null_value : beta;
                            }
                        }
                    }

                    // Step 9. ProbCut
                    // If good enough capture and a reduced search returns a value much above beta,
                    // then can (almost) safely prune the previous move.
                    if (   !PVNode
                        && 4 < depth
                        //&& 0 == Limits.mate
                        && abs (beta) < +VALUE_MATE_IN_MAX_PLY)
                    {
                        auto reduced_depth = i16(depth - 4);
                        assert(reduced_depth > 0);
                        auto beta_margin = beta + 200;
                        assert(beta_margin <= +VALUE_INFINITE);

                        assert(MOVE_NONE != (ss-1)->current_move
                            && MOVE_NULL != (ss-1)->current_move
                            && (ss-1)->m_history != nullptr);

                        // Initialize move picker (3) for the current position.
                        MovePicker mp (pos, tt_move, beta_margin - ss->static_eval);
                        // Loop through all legal moves until no moves remain or a beta cutoff occurs.
                        while (MOVE_NONE != (move = mp.next_move ()))
                        {
                            assert(pos.pseudo_legal (move)
                                && pos.legal (move));

                            ss->current_move = move;
                            ss->m_history = &th->cm_history(pos[org_sq (move)], dst_sq (move));

                            assert(pos.capture_or_promotion (move));

                            // Speculative prefetch as early as possible
                            prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                            pos.do_move (move, si);

                            if (   PAWN == ptype (pos[dst_sq (move)])
                                || PAWN == pos.si->capture)
                            {
                                prefetch (th->pawn_table[pos.si->pawn_key]);
                            }
                            // NOTE:: All moves are capture_or_promotion
                            prefetch (th->matl_table[pos.si->matl_key]);

                            auto value = -depth_search<false> (pos, ss+1, -beta_margin, -beta_margin+1, reduced_depth, !cut_node, true);

                            pos.undo_move (move);

                            if (value >= beta_margin)
                            {
                                return value;
                            }
                        }
                    }

                    // Step 10. Internal iterative deepening (IID)
                    if (   tt_move == MOVE_NONE
                        && 4 < depth
                        && (   PVNode
                            || ss->static_eval + 256 >= beta))
                    {
                        depth_search<PVNode> (pos, ss, alfa, beta, 3*depth/4 - 2, cut_node, false);

                        tte = TT.probe (posi_key, tt_hit);
                        tt_move =
                               tt_hit
                            && MOVE_NONE != (move = tte->move ())
                            && pos.pseudo_legal (move)
                            && pos.legal (move) ?
                                move :
                                MOVE_NONE;
                    }
                }
            }

            // When in check search starts from here
            auto value      = -VALUE_INFINITE
               , best_value = -VALUE_INFINITE;

            auto best_move  = MOVE_NONE;

            bool singular_ext_node =
                   !root_node
                && MOVE_NONE != tt_move
                && MOVE_NONE == exclude_move // Recursive singular search is not allowed
                && 7 < depth
                && tte->depth () + 4 > depth
                && tt_value != VALUE_NONE
                && (tte->bound () & BOUND_LOWER) != BOUND_NONE;

            bool improving =
                   (ss-2)->static_eval <= (ss-0)->static_eval
                || (ss-2)->static_eval == VALUE_NONE;

            u08 move_count = 0;

            MoveVector quiet_moves;
            quiet_moves.reserve (16);

            // Initialize move picker (1) for the current position.
            MovePicker mp (pos, tt_move, ss);
            // Step 11. Loop through moves
            // Loop through all legal moves until no moves remain or a beta cutoff occurs.
            while (MOVE_NONE != (move = mp.next_move ()))
            {
                assert(pos.pseudo_legal (move)
                    && pos.legal (move));

                if (    // Skip exclusion move
                        move == exclude_move
                        // At root obey the "searchmoves" option and skip moves not listed in
                       // RootMove list, as a consequence any illegal move is also skipped.
                       // In MultiPV mode also skip PV moves which have been already searched.
                    || (   root_node
                        && std::find (th->root_moves.begin () + th->pv_index,
                                      th->root_moves.end (), move) ==
                                      th->root_moves.end ()))
                {
                    continue;
                }

                ss->move_count = ++move_count;

                auto mpc = pos[org_sq (move)];
                assert(mpc != NO_PIECE);

                if (   root_node
                    && Threadpool.main_thread () == th)
                {
                    auto elapsed_time = Threadpool.time_mgr.elapsed_time ();
                    if (elapsed_time > 3*MilliSec)
                    {
                        sync_cout
                            << "info"
                            << " depth "          << depth
                            << " currmovenumber " << th->pv_index + move_count
                            << " currmove "       << move_to_can (move)
                            << " time "           << elapsed_time
                            << sync_endl;
                    }
                }

                if (PVNode)
                {
                    (ss+1)->pv.clear ();
                }

                bool gives_check = pos.gives_check (move);
                bool capture_or_promotion = pos.capture_or_promotion (move);

                bool move_count_pruning =
                       MaxFutilityDepth > depth
                    && FutilityMoveCounts[improving][depth] <= move_count;

                // Calculate new depth for this move
                i16 new_depth = depth - 1;
                // Step 12. Extensions
                // Extend the move which gives check.
                if (   gives_check
                    && !move_count_pruning
                    && pos.see_ge (move, VALUE_ZERO))
                {
                    new_depth += 1;
                }

                // Singular extensions (SE).
                // We extend the TT move if its value is much better than its siblings.
                // If all moves but one fail low on a search of (alfa-s, beta-s),
                // and just one fails high on (alfa, beta), then that move is singular and should be extended.
                // To verify this do a reduced search on all the other moves but the tt_move,
                // if result is lower than tt_value minus a margin then extend tt_move.
                if (   singular_ext_node
                    && move == tt_move
                    && new_depth < depth)
                {
                    auto beta_margin = std::max (tt_value - 2*depth, -VALUE_MATE);
                    value = depth_search<false> (pos, ss, beta_margin-1, beta_margin, depth/2, cut_node, false, move);

                    if (value < beta_margin)
                    {
                        new_depth += 1;
                    }
                }

                // Step 13. Pruning at shallow depth
                if (   !root_node
                    //&& 0 == Limits.mate
                    && best_value > -VALUE_MATE_IN_MAX_PLY)
                {
                    if (   !capture_or_promotion
                        && !gives_check
                            // Advance pawn push
                        && !(   PAWN == ptype (pos[org_sq (move)])
                             && rel_rank (pos.active, org_sq (move)) > R_4))
                    {
                        // Move count based pruning
                        if (move_count_pruning)
                        {
                            continue;
                        }

                        // Reduced depth of the next LMR search
                        auto lmr_depth = i16(std::max (new_depth - reduction_depth (PVNode, improving, depth, move_count), 0));
                        if (    // Counter moves value based pruning
                               (   3 > lmr_depth
                                && ((ss-1)->m_history == nullptr || (*(ss-1)->m_history)(mpc, dst_sq (move)) < VALUE_ZERO)
                                && ((ss-2)->m_history == nullptr || (*(ss-2)->m_history)(mpc, dst_sq (move)) < VALUE_ZERO)
                                && (  ((ss-1)->m_history != nullptr && (ss-2)->m_history != nullptr)
                                    || (ss-4)->m_history == nullptr || (*(ss-4)->m_history)(mpc, dst_sq (move)) < VALUE_ZERO))
                                // Futility pruning: parent node
                            || (   7 > lmr_depth
                                && !in_check
                                && ss->static_eval + 200*lmr_depth + 256 <= alfa)
                                // SEE based pruning
                            || (   8 > lmr_depth
                                && !pos.see_ge (move, -Value(35*lmr_depth*lmr_depth))))
                        {
                            continue;
                        }
                    }
                    else
                    // SEE based pruning
                    if (   7 > depth
                        && new_depth < depth
                        && !pos.see_ge (move, -VALUE_EG_PAWN*(depth+0)))
                    {
                        continue;
                    }
                }

                // Update the current move (this must be done after singular extension search)
                ss->current_move = move;
                ss->m_history = &th->cm_history(mpc, dst_sq (move));

                // Speculative prefetch as early as possible
                prefetch (TT.cluster_entry (pos.move_posi_key (move)));

                // Step 14. Make the move
                pos.do_move (move, si, gives_check);

                if (   PAWN == ptype (pos[dst_sq (move)])
                    || PAWN == pos.si->capture)
                {
                    prefetch (th->pawn_table[pos.si->pawn_key]);
                }
                if (capture_or_promotion)
                {
                    prefetch (th->matl_table[pos.si->matl_key]);
                }

                bool full_depth_search;
                // Step 15. Reduced depth search (LMR).
                // If the move fails high will be re-searched at full depth.
                if (   2 < depth
                    && 1 < move_count
                    && (   move_count_pruning
                        || !capture_or_promotion))
                {
                    auto reduce_depth = reduction_depth (PVNode, improving, depth, move_count);
                    
                    if (capture_or_promotion)
                    {
                        reduce_depth -= 1;
                    }
                    else
                    {
                        assert(PROMOTE != mtype (move));
                        // Increase reduction for cut nodes
                        if (cut_node)
                        {
                            reduce_depth += 2;
                        }
                        else
                        // Decrease reduction for moves that escape a capture in no-cut nodes.
                        // Filter out castling moves, because they are coded as "king captures rook" and hence break mk_move().
                        if (   NORMAL == mtype (move)
                            && !pos.see_ge (mk_move<NORMAL> (dst_sq (move), org_sq (move)), VALUE_ZERO))
                        {
                            reduce_depth -= 2;
                        }

                        ss->history_val =
                              th->history(~pos.active, move)
                            + ((ss-1)->m_history != nullptr ? (*(ss-1)->m_history)(mpc, dst_sq (move)) : VALUE_ZERO)
                            + ((ss-2)->m_history != nullptr ? (*(ss-2)->m_history)(mpc, dst_sq (move)) : VALUE_ZERO)
                            + ((ss-4)->m_history != nullptr ? (*(ss-4)->m_history)(mpc, dst_sq (move)) : VALUE_ZERO)
                            - 4000; // Correction factor

                        // Decrease/Increase reduction by comparing opponent's stat score
                        if (   (ss)->history_val > VALUE_ZERO
                            && (ss-1)->history_val < VALUE_ZERO)
                        {
                            reduce_depth -= 1;
                        }
                        else
                        if (   (ss)->history_val < VALUE_ZERO
                            && (ss-1)->history_val > VALUE_ZERO)
                        {
                            reduce_depth += 1;
                        }

                        // Decrease/Increase reduction for moves with +ve/-ve history
                        reduce_depth -= i16(i32((ss)->history_val) / 20000);
                    }

                    if (reduce_depth < 0)
                    {
                        reduce_depth = 0;
                    }
                    else
                    if (reduce_depth > new_depth - 1)
                    {
                        reduce_depth = new_depth - 1;
                    }

                    value = -depth_search<false> (pos, ss+1, -(alfa+1), -alfa, new_depth - reduce_depth, true, true);

                    full_depth_search = alfa < value
                                     && reduce_depth > 0;
                }
                else
                {
                    full_depth_search = !PVNode
                                     || 1 < move_count;
                }

                // Step 16. Full depth search when LMR is skipped or fails high
                if (full_depth_search)
                {
                    value =
                        new_depth <= 0 ?
                            -quien_search<false> (pos, ss+1, -(alfa+1), -alfa) :
                            -depth_search<false> (pos, ss+1, -(alfa+1), -alfa, new_depth, !cut_node, true);
                }

                // Do a full PV search on:
                // - 'full depth move count' move
                // - 'fail high' move (search only if value < beta)
                // otherwise let the parent node fail low with alfa >= value and try another move.
                if (   PVNode
                    && (   1 == move_count
                        || (   alfa < value
                            && (   root_node
                                || value < beta))))
                {
                    (ss+1)->pv.clear ();

                    value =
                        new_depth <= 0 ?
                            -quien_search<true> (pos, ss+1, -beta, -alfa) :
                            -depth_search<true> (pos, ss+1, -beta, -alfa, new_depth, false, true);
                }

                // Step 17. Undo move
                pos.undo_move (move);

                assert(-VALUE_INFINITE < value && value < +VALUE_INFINITE);

                // Step 18. Check for the new best move
                // Finished searching the move. If a stop or a cutoff occurred,
                // the return value of the search cannot be trusted,
                // and return immediately without updating best move, PV and TT.
                if (ForceStop.load (memory_order_relaxed))
                {
                    return VALUE_ZERO;
                }

                if (root_node)
                {
                    auto &root_move = *std::find (th->root_moves.begin (), th->root_moves.end (), move);
                    // First PV move or new best move?
                    if (   1 == move_count
                        || alfa < value)
                    {
                        root_move.resize (1);
                        for (auto m : (ss+1)->pv)
                        {
                            root_move.push_back (m);
                        }
                        root_move.new_value = value;

                        // Record how often the best move has been changed in each iteration.
                        // This information is used for time management:
                        // When the best move changes frequently, allocate some more time.
                        if (   1 < move_count
                            && Limits.use_time_management ()
                            && Threadpool.main_thread () == th)
                        {
                            Threadpool.best_move_change++;
                        }
                    }
                    else
                    {
                        // All other moves but the PV are set to the lowest value, this
                        // is not a problem when sorting becuase sort is stable and move
                        // position in the list is preserved, just the PV is pushed up.
                        root_move.new_value = -VALUE_INFINITE;
                    }
                }

                // Step 19. Check best value
                if (best_value < value)
                {
                    best_value = value;

                    if (alfa < value)
                    {
                        best_move = move;

                        // Update pv even in fail-high case
                        if (   PVNode
                            && !root_node)
                        {
                            update_pv (ss->pv, move, (ss+1)->pv);
                        }
                        // Fail high
                        if (value >= beta)
                        {
                            break;
                        }
                        assert(value < beta);

                        // Update alfa! Always alfa < beta
                        if (PVNode)
                        {
                            alfa = value;
                        }
                    }
                }

                if (   move != best_move
                    && !capture_or_promotion)
                {
                    quiet_moves.push_back (move);
                }
            }

            assert(!in_check
                || 0 != move_count
                || MOVE_NONE != exclude_move
                || 0 == MoveList<LEGAL> (pos).size ());

            // Step 20. Check for checkmate and stalemate
            // If all possible moves have been searched and if there are no legal moves,
            // If in a singular extension search then return a fail low score (alfa).
            // Otherwise it must be a checkmate or a stalemate, so return value accordingly.
            if (0 == move_count)
            {
                best_value =
                    MOVE_NONE != exclude_move ?
                        alfa :
                        in_check ?
                            mated_in (ss->ply) :
                            DrawValue[pos.active];
            }
            else
            // Quiet best move: update move sorting heuristics
            if (MOVE_NONE != best_move)
            {
                if (!pos.capture_or_promotion (best_move))
                {
                    auto bonus = stat_bonus (depth);
                    update_stats (ss, pos, best_move, bonus);
                    // Decrease all the other played quiet moves
                    assert(std::find (quiet_moves.begin (), quiet_moves.end (), best_move) == quiet_moves.end ());
                    for (auto m : quiet_moves)
                    {
                        pos.thread->history.update (pos.active, m, -bonus);
                        update_cm_stats (ss, pos[org_sq (m)], dst_sq (m), -bonus);
                    }
                }
                // Penalty for a quiet best move in previous ply when it gets refuted
                if (   1 == (ss-1)->move_count
                    && NONE == pos.si->capture)
                {
                    update_cm_stats (ss-1, pos[fix_dst_sq ((ss-1)->current_move)], dst_sq ((ss-1)->current_move), -stat_bonus (depth + 1));
                }
            }
            else
            // Bonus for prior countermove that caused the fail low
            if (   2 < depth
                && nullptr != (ss-1)->m_history
                && NONE == pos.si->capture)
            {
                update_cm_stats (ss-1, pos[fix_dst_sq ((ss-1)->current_move)], dst_sq ((ss-1)->current_move), stat_bonus (depth));
            }

            tte->save (posi_key,
                       best_move,
                       value_to_tt (best_value, ss->ply),
                       ss->static_eval,
                       depth,
                       best_value >= beta ?
                           BOUND_LOWER :
                              PVNode
                           && MOVE_NONE != best_move ?
                               BOUND_EXACT :
                               BOUND_UPPER);

            assert(-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);
            return best_value;
        }
    }

    // Utility to verify move generation.
    // All the leaf nodes up to the given depth are generated, and the sum is returned.
    template<bool RootNode>
    u64 perft (Position &pos, i16 depth)
    {
        u64 leaf_nodes = 0;
        i16 move_count = 0;
        for (const auto &vm : MoveList<LEGAL> (pos))
        {
            u64 cum_nodes;
            if (   RootNode
                && 1 >= depth)
            {
                cum_nodes = 1;
            }
            else
            {
                StateInfo si;
                pos.do_move (vm.move, si);
                cum_nodes =
                    2 < depth ?
                        perft<false> (pos, depth - 1) :
                        MoveList<LEGAL> (pos).size ();
                pos.undo_move (vm.move);
            }

            if (RootNode)
            {
                sync_cout
                    << std::right
                    << std::setfill ('0')
                    << std::setw (2)
                    << ++move_count << ' '
                    << std::left
                    << std::setfill (' ')
                    << std::setw (7)
                    << 
                        //move_to_can (vm.move)
                        move_to_san (vm.move, pos)
                    << std::right
                    << std::setfill ('.')
                    << std::setw (16)
                    << cum_nodes
                    << std::setfill (' ')
                    << std::left
                    << sync_endl;
            }

            leaf_nodes += cum_nodes;
        }
        return leaf_nodes;
    }
    // Explicit template instantiations
    template u64 perft<false> (Position&, i16);
    template u64 perft<true > (Position&, i16);

    // Initialize lookup tables during startup
    void initialize ()
    {
        for (i16 d = 0; d < MaxFutilityDepth; ++d)
        {
            FutilityMoveCounts[0][d] = u08(0.773 * std::pow (d + 0.00, 1.8) + 2.40);
            FutilityMoveCounts[1][d] = u08(1.045 * std::pow (d + 0.49, 1.8) + 2.90);
        }
        for (u08 imp = 0; imp < 2; ++imp)
        {
            for (i16 d = 1; d < MaxReductionDepth; ++d)
            {
                for (u08 mc = 1; mc < MaxReductionMoveCount; ++mc)
                {
                    auto r = log (d) * log (mc) / 2;
                    ReductionDepths[0][imp][d][mc] = i16(std::round (r));
                    ReductionDepths[1][imp][d][mc] = i16(std::max (ReductionDepths[0][imp][d][mc] - 1, 0));
                    if (   0 == imp
                        && ReductionDepths[0][imp][d][mc] >= 2)
                    {
                        ReductionDepths[0][imp][d][mc] += 1;
                    }
                }
            }
        }
    }
    // Resets search state, to obtain reproducible results
    void clear ()
    {
        TT.clear ();
        Threadpool.clear ();
    }
}

namespace Threading {

    using namespace Searcher;

    // Thread iterative deepening loop function.
    // It calls depth_search() repeatedly with increasing depth until
    // - the force stop requested.
    // - the allocated thinking time has been consumed.
    // - the maximum search depth is reached.
    void Thread::search ()
    {
        Stack stacks[MaxPlies + 7]; // To allow referencing (ss-4) and (ss+2)
        for (auto s = stacks; s < stacks + MaxPlies + 7; ++s)
        {
            s->ply          = i16(s - stacks - 3);
            s->current_move = MOVE_NONE;
            std::fill_n (s->killer_moves, MaxKillers, MOVE_NONE);
            s->static_eval  = VALUE_ZERO;
            s->history_val  = VALUE_ZERO;
            s->move_count   = 0;
            s->m_history    = nullptr;
        }

        max_ply = 0;
        tb_hits = 0;
        running_depth  = 0;
        finished_depth = 0;

        auto best_value = VALUE_ZERO
           , window     = VALUE_ZERO
           , alfa       = -VALUE_INFINITE
           , beta       = +VALUE_INFINITE;

        // Iterative deepening loop until requested to stop or the target depth is reached.
        while (   ++running_depth < MaxPlies
               && !ForceStop
               && (   0 == Limits.depth
                   || Threadpool.main_thread ()->running_depth <= Limits.depth))
        {
            if (Threadpool.main_thread () == this)
            {
                if (Limits.use_time_management ())
                {
                    Threadpool.failed_low = false;
                    // Age out PV variability metric
                    Threadpool.best_move_change *= 0.505;
                }
            }
            else
            {
                static const size_t HalfDensityMapSize = 30;
                // Rotating symmetric patterns with increasing skipsize.
                // Set of rows with half bits set to true and half to false.
                // It is used to allocate the search depths across the threads.
                static const vector<bool> HalfDensityMap[HalfDensityMapSize] =
                {
                    { false, true },
                    { true, false },

                    { false, false, true, true },
                    { false, true, true, false },
                    { true, true, false, false },
                    { true, false, false, true },

                    { false, false, false, true, true, true },
                    { false, false, true, true, true, false },
                    { false, true, true, true, false, false },
                    { true, true, true, false, false, false },
                    { true, true, false, false, false, true },
                    { true, false, false, false, true, true },

                    { false, false, false, false, true, true, true, true },
                    { false, false, false, true, true, true, true, false },
                    { false, false, true, true, true, true, false, false },
                    { false, true, true, true, true, false, false, false },
                    { true, true, true, true, false, false, false, false },
                    { true, true, true, false, false, false, false, true },
                    { true, true, false, false, false, false, true, true },
                    { true, false, false, false, false, true, true, true },

                    { false, false, false, false, false, true, true, true, true, true },
                    { false, false, false, false, true, true, true, true, true, false },
                    { false, false, false, true, true, true, true, true, false, false },
                    { false, false, true, true, true, true, true, false, false, false },
                    { false, true, true, true, true, true, false, false, false, false },
                    { true, true, true, true, true, false, false, false, false, false },
                    { true, true, true, true, false, false, false, false, false, true },
                    { true, true, true, false, false, false, false, false, true, true },
                    { true, true, false, false, false, false, false, true, true, true },
                    { true, false, false, false, false, false, true, true, true, true },
                };

                const auto &hdm = HalfDensityMap[(index - 1) % HalfDensityMapSize];
                if (hdm[(running_depth + root_pos.ply) % hdm.size ()])
                {
                    continue;
                }
            }

            // Save the last iteration's values before first PV line is searched
            for (auto &rm : root_moves)
            {
                rm.old_value = rm.new_value;
            }

            // MultiPV loop. Perform a full root search for each PV line
            for (   pv_index = 0;
                    !ForceStop
                 && pv_index < Threadpool.pv_limit;
                    ++pv_index)
            {
                // Reset aspiration window starting size.
                if (running_depth > 4)
                {
                    window = Value(18);
                    alfa = std::max (root_moves[pv_index].old_value - window, -VALUE_INFINITE);
                    beta = std::min (root_moves[pv_index].old_value + window, +VALUE_INFINITE);
                }

                // Start with a small aspiration window and, in case of fail high/low,
                // research with bigger window until not failing high/low anymore.
                do {
                    best_value = depth_search<true> (root_pos, stacks+4, alfa, beta, running_depth, false, true);

                    // Bring the best move to the front. It is critical that sorting is
                    // done with a stable algorithm because all the values but the first
                    // and eventually the new best one are set to -VALUE_INFINITE and
                    // want to keep the same order for all the moves but the new PV
                    // that goes to the front. Note that in case of MultiPV search
                    // the already searched PV lines are preserved.
                    std::stable_sort (root_moves.begin () + pv_index, root_moves.end ());

                    // If search has been stopped, break immediately.
                    // Sorting and writing PV back to TT is safe becuase
                    // root moves is still valid, although refers to the previous iteration.
                    if (ForceStop)
                    {
                        break;
                    }

                    if (Threadpool.main_thread () == this)
                    {
                        // Give some update before to re-search.
                        if (   Threadpool.pv_limit == 1
                            && (best_value <= alfa || beta <= best_value)
                            && Threadpool.time_mgr.elapsed_time () > 3*MilliSec)
                        {
                            sync_cout << multipv_info (this, alfa, beta) << sync_endl;
                        }
                    }
                    // If failing low/high set new bounds, otherwise exit the loop.
                    if (best_value <= alfa)
                    {
                        beta = (alfa + beta) / 2;
                        alfa = std::max (best_value - window, -VALUE_INFINITE);

                        if (Threadpool.main_thread () == this)
                        {
                            if (Limits.use_time_management ())
                            {
                                Threadpool.failed_low = true;
                            }
                            PonderhitStop = false;
                        }
                    }
                    else
                    if (beta <= best_value)
                    {
                        alfa = (alfa + beta) / 2;
                        beta = std::min (best_value + window, +VALUE_INFINITE);
                    }
                    else
                    {
                        break;
                    }

                    window += window / 4 + 5;

                    assert(-VALUE_INFINITE <= alfa && alfa < beta && beta <= +VALUE_INFINITE);
                } while (true);

                // Sort the PV lines searched so far and update the GUI
                std::stable_sort (root_moves.begin (), root_moves.begin () + pv_index + 1);

                if (Threadpool.main_thread () == this)
                {
                    if (   ForceStop
                        || Threadpool.pv_limit == pv_index + 1
                        || Threadpool.time_mgr.elapsed_time () > 3*MilliSec)
                    {
                        sync_cout << multipv_info (this, alfa, beta) << sync_endl;
                    }
                }
            }

            if (!ForceStop)
            {
                finished_depth = running_depth;
            }

            //if (0 != ContemptValue)
            //{
            //    auto valued_contempt = Value(i32(root_moves[0].new_value)/ContemptValue);
            //    DrawValue[ root_pos.active] = BaseContempt[ root_pos.active] - valued_contempt;
            //    DrawValue[~root_pos.active] = BaseContempt[~root_pos.active] + valued_contempt;
            //}

            if (Threadpool.main_thread () == this)
            {
                // If skill level is enabled and can pick move, pick a sub-optimal best move
                if (   Threadpool.skill_mgr.enabled ()
                    && Threadpool.skill_mgr.can_pick (running_depth))
                {
                    Threadpool.skill_mgr.clear ();
                    Threadpool.skill_mgr.pick_best_move (Threadpool.pv_limit);
                }

                if (WriteOutput)
                {
                    OutputStream << pretty_pv_info (this) << std::endl;
                }

                if (   !ForceStop
                    && !PonderhitStop)
                {
                    // Stop the search early:
                    bool stop = false;

                    // Have time for the next iteration? Can stop searching now?
                    if (Limits.use_time_management ())
                    {
                        auto &root_move = root_moves[0];
                        // Stop the search
                        // -If there is only one legal move available
                        // -If all of the available time has been used
                        // -If matched an easy move from the previous search and just did a fast verification.
                        if (   root_moves.size () == 1
                            || (  Threadpool.time_mgr.elapsed_time ()
                                > Threadpool.time_mgr.optimum_time
                                        // Unstable factor
                                        * (1.0 + Threadpool.best_move_change)
                                        // Improving factor
                                        * std::min (1.1385,
                                          std::max (0.3646,
                                                    0.5685
                                                  + 0.1895 * (Threadpool.failed_low ? 1 : 0)
                                                  - 0.0096 * (Threadpool.last_value != VALUE_NONE ? i32(best_value - Threadpool.last_value) : 0))))
                            || (Threadpool.easy_played =
                                    (   root_move == Threadpool.easy_move
                                     && Threadpool.best_move_change < 0.030
                                     && Threadpool.time_mgr.elapsed_time () > Threadpool.time_mgr.optimum_time * 0.1136), Threadpool.easy_played))
                        {
                            stop = true;
                        }

                        if (root_move.size () >= MoveManager::PVSize)
                        {
                            Threadpool.move_mgr.update (root_pos, root_move);
                        }
                        else
                        {
                            Threadpool.move_mgr.clear ();
                        }
                    }
                    else
                    // Have found a "mate in <x>"?
                    if (   0 != Limits.mate
                        && best_value >= +VALUE_MATE - 2*Limits.mate)
                    {
                        stop = true;
                    }

                    if (stop)
                    {
                        // If allowed to ponder do not stop the search now but
                        // keep pondering until GUI sends "ponderhit" or "stop".
                        if (Limits.ponder)
                        {
                            PonderhitStop = true;
                        }
                        else
                        {
                            ForceStop = true;
                        }
                    }
                }
            }
        }
    }
    // Main thread function.
    // It searches from root position and outputs the "bestmove"/"ponder".
    void MainThread::search ()
    {
        static Book book; // Defined static to initialize the PRNG only once
        assert(Threadpool.main_thread () == this);

        if (!white_spaces (OutputFile))
        {
            OutputStream.open (OutputFile, ios_base::out|ios_base::app);
            WriteOutput = OutputStream.is_open ();
            if (WriteOutput)
            {
                OutputStream
                    << boolalpha
                    << "RootPos  : " << root_pos.fen (true)                << '\n'
                    << "RootSize : " << root_moves.size ()                 << '\n'
                    << "Infinite : " << Limits.infinite                    << '\n'
                    << "Ponder   : " << Limits.ponder                      << '\n'
                    << "ClockTime: " << Limits.clock[root_pos.active].time << '\n'
                    << "Increment: " << Limits.clock[root_pos.active].inc  << '\n'
                    << "MoveTime : " << Limits.movetime                    << '\n'
                    << "MovesToGo: " << u16(Limits.movestogo)              << '\n'
                    << "Depth    : " << Limits.depth                       << '\n'
                    << " Depth Score    Time       Nodes  PV\n"
                    << "-----------------------------------------------------------"
                    << noboolalpha << std::endl;
            }
        }

        if (Limits.use_time_management ())
        {
            // Initialize the time manager before searching.
            Threadpool.time_mgr.initialize (root_pos.active, root_pos.ply);
        }

        Transposition::Entry::Generation = u08(((root_pos.ply + 1) << 2) & 0xFC);

        bool voting = false;

        if (root_moves.empty ())
        {
            root_moves += RootMove ();

            sync_cout
                << "info"
                << " depth " << 0
                << " score " << to_string (0 != root_pos.si->checkers ? -VALUE_MATE : VALUE_DRAW)
                << " time "  << 0
                << sync_endl;
        }
        else
        {
            // Check if can play with own book.
            if (   OwnBook
                && !white_spaces (BookFile)
                && (   0 == BookUptoMove
                    || root_pos.move_num () <= BookUptoMove)
                && 0 == Limits.mate
                && !Limits.infinite)
            {
                book.open (BookFile, ios_base::in);
                bool found = false;
                auto book_best_move = book.probe_move (root_pos, BookMoveBest);
                if (   MOVE_NONE != book_best_move
                    && std::find (root_moves.begin (), root_moves.end (), book_best_move) != root_moves.end ())
                {
                    auto &root_move = root_moves[0];
                    std::swap (root_move, *std::find (root_moves.begin (), root_moves.end (), book_best_move));
                    StateInfo si;
                    root_pos.do_move (book_best_move, si);
                    auto book_ponder_move = book.probe_move (root_pos, BookMoveBest);
                    root_move += book_ponder_move;
                    root_pos.undo_move (book_best_move);
                    found = true;
                }
                book.close ();
                if (found)
                {
                    goto finish;
                }
            }

            i16 timed_contempt = 0;
            i64 diff_time;
            if (   Limits.use_time_management ()
                && 0 != ContemptTime
                && (diff_time = i64(  Limits.clock[ root_pos.active].time
                                    - Limits.clock[~root_pos.active].time)/MilliSec) != 0)
            {
                timed_contempt = i16(diff_time/ContemptTime);
            }

            auto contempt = cp_to_value ((FixedContempt + timed_contempt) / 100.0);
            DrawValue[ root_pos.active] = BaseContempt[ root_pos.active] = VALUE_DRAW - contempt;
            DrawValue[~root_pos.active] = BaseContempt[~root_pos.active] = VALUE_DRAW + contempt;

            if (Limits.use_time_management ())
            {
                Threadpool.easy_move = Threadpool.move_mgr.easy_move (root_pos.si->posi_key);
                Threadpool.move_mgr.clear ();
                Threadpool.easy_played = false;
                Threadpool.failed_low  = false;
                Threadpool.best_move_change = 0.000;
            }
            if (Threadpool.skill_mgr.enabled ())
            {
                Threadpool.skill_mgr.clear ();
            }

            // Have to play with skill handicap?
            // In this case enable MultiPV search by skill pv size
            // that will use behind the scenes to get a set of possible moves.
            Threadpool.pv_limit = std::min (std::max (MultiPV, u16(Threadpool.skill_mgr.enabled () ? SkillManager::MinSkillPV : 0)), u16(root_moves.size ()));

            voting = true;

            for (auto *th : Threadpool)
            {
                if (th != this)
                {
                    th->start_searching (false);
                }
            }

            Thread::search (); // Let's start searching !

            // Clear any candidate easy move that wasn't stable for the last search iterations;
            // the second condition prevents consecutive fast moves.
            if (   Limits.use_time_management ()
                && (   Threadpool.easy_played
                    || Threadpool.move_mgr.stable_count < 6))
            {
                Threadpool.move_mgr.clear ();
            }
            // Swap best PV line with the sub-optimal one if skill level is enabled
            if (Threadpool.skill_mgr.enabled ())
            {
                std::swap (root_moves[0], *std::find (root_moves.begin (), root_moves.end (), Threadpool.skill_mgr.pick_best_move (Threadpool.pv_limit)));
            }
        }

    finish:
        if (Limits.use_time_management ())
        {
            // Update the time manager after searching.
            Threadpool.time_mgr.update (root_pos.active);
        }
        // When reach max depth arrive here even without Force Stop is raised,
        // but if are pondering or in infinite search, according to UCI protocol,
        // shouldn't print the best move before the GUI sends a "stop" or "ponderhit" command.
        // Simply wait here until GUI sends one of those commands (that raise Force Stop).
        if (   !ForceStop
            && (   Limits.infinite
                || Limits.ponder))
        {
            PonderhitStop = true;
            wait_until (ForceStop);
        }

        Thread *best_thread = this;
        if (voting)
        {
            // Stop the threads if not already stopped.
            ForceStop = true;
            // Wait until all threads have finished.
            for (auto *th : Threadpool)
            {
                if (th != this)
                {
                    th->wait_while_searching ();
                }
            }
            // Check if there are deeper thread than main thread.
            if (   1 == Threadpool.pv_limit
                && !Threadpool.easy_played
                //&& 0 == Limits.depth // Depth limit search don't use deeper thread
                && !Threadpool.skill_mgr.enabled ())
            {
                // If best thread is not main thread send new PV.
                if ((best_thread = Threadpool.best_thread ()) != this)
                {
                    sync_cout << multipv_info (best_thread, -VALUE_INFINITE, +VALUE_INFINITE) << sync_endl;
                }
            }
        }

        assert(!best_thread->root_moves.empty ()
            && !best_thread->root_moves[0].empty ());
        auto &root_move = best_thread->root_moves[0];

        if (Limits.use_time_management ())
        {
            Threadpool.last_value = root_move.new_value;
        }

        if (WriteOutput)
        {
            auto total_nodes  = Threadpool.nodes ();
            auto elapsed_time = std::max (Threadpool.time_mgr.elapsed_time (), TimePoint(1));
            OutputStream
                << "Nodes (N)  : " << total_nodes                           << '\n'
                << "Time (ms)  : " << elapsed_time                          << '\n'
                << "Speed (N/s): " << total_nodes*MilliSec / elapsed_time   << '\n'
                << "Hash-full  : " << TT.hash_full ()                       << '\n'
                << "Best Move  : " << move_to_san (root_move[0], root_pos)  << '\n';
            if (   MOVE_NONE != root_move[0]
                && (   root_move.size () > 1
                    || root_move.extract_ponder_move_from_tt (root_pos)))
            {
                StateInfo si;
                root_pos.do_move (root_move[0], si);
                OutputStream << "Ponder Move: " << move_to_san (root_move[1], root_pos) << '\n';
                root_pos.undo_move (root_move[0]);
            }
            OutputStream << std::endl;
            OutputStream.close ();
            WriteOutput = false;
        }

        // Best move could be MOVE_NONE when searching on a stalemate position.
        sync_cout << "bestmove " << move_to_can (root_move[0]);
        if (   MOVE_NONE != root_move[0]
            && (   root_move.size () > 1
                || root_move.extract_ponder_move_from_tt (root_pos)))
        {
            std::cout << " ponder " << move_to_can (root_move[1]);
        }
        std::cout << sync_endl;
    }
}
