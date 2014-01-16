﻿#include "Searcher.h"

#include <iostream>
#include <cfloat>

#include "UCI.h"
#include "Time.h"
#include "TimeManager.h"
#include "Position.h"
#include "PolyglotBook.h"
#include "Transposition.h"
#include "MoveGenerator.h"
#include "MovePicker.h"
#include "Material.h"
#include "Pawns.h"
#include "Evaluator.h"
#include "Thread.h"
#include "Notation.h"
#include "atomicstream.h"
#include "Log.h"

using namespace std;
using namespace BitBoard;
using namespace MoveGenerator;
using namespace Searcher;
using namespace Evaluator;

namespace {

    // Set to true to force running with one thread. Used for debugging
    const bool FakeSplit            = false;

    // Different node types, used as template parameter
    enum NodeType { Root, PV, NonPV, SplitPointRoot, SplitPointPV, SplitPointNonPV };

    // Futility lookup tables (initialized at startup) and their access functions
    int32_t FutilityMoveCounts[2][32];  // [improving][depth]

    // Reduction lookup tables (initialized at startup) and their access function
    int8_t Reductions[2][2][64][64]; // [pv][improving][depth][move_number]

    inline Value futility_margin (Depth d)
    {
        return Value (100 * int32_t (d));
    }

    template<bool PVNode>
    inline Depth reduction (bool imp, Depth d, int32_t mn)
    {
        return Depth (Reductions[PVNode][imp][min (int32_t (d) / ONE_MOVE, 63)][min (mn, 63)]);
    }

    // Dynamic razoring margin based on depth
    inline Value razor_margin (Depth d)
    {
        return Value (512 + 16 * int32_t (d));
    }

    TimeManager TimeMgr;
    Value       DrawValue[CLR_NO];

    double      BestMoveChanges;

    uint32_t
        PVSize,
        PVIdx;

    GainsStats          Gains;
    HistoryStats        History;
    MovesStats          CounterMoves;
    MovesStats          FollowupMoves;

    // Duration of iteration
    uint64_t            Iterated;


    // update_stats() updates killers, history, countermoves and followupmoves stats
    // after a fail-high of a quiet move.
    inline void update_stats (Position &pos, Stack ss[], Move move, int32_t depth, Move quiets[], int32_t quiets_count)
    {
        if (ss->killers[0] != move)
        {
            ss->killers[1] = ss->killers[0];
            ss->killers[0] = move;
        }

        // Increase history value of the cut-off move and decrease all the other played quiet moves.
        Value bonus = Value (depth * depth);
        History.update (pos[org_sq (move)], dst_sq (move), bonus);
        //if (quiets)
        {
            for (int32_t i = 0; i < quiets_count; ++i)
            {
                Move m = quiets[i];
                History.update (pos[org_sq (m)], dst_sq (m), -bonus);
            }
        }

        Move prev_move = (ss-1)->current_move;
        if (_ok (prev_move))
        {
            Square prev_move_sq = dst_sq (prev_move);
            CounterMoves.update (pos[prev_move_sq], prev_move_sq, move);
        }

        Move prev_own_move = (ss-2)->current_move;
        if (_ok (prev_own_move) && (ss-1)->current_move == (ss-1)->tt_move)
        {
            Square prev_own_move_sq = dst_sq (prev_own_move);
            FollowupMoves.update (pos[prev_own_move_sq], prev_own_move_sq, move);
        }
    }

    void iter_deep_loop (Position &pos);

    template <NodeType NT>
    Value search        (Position &pos, Stack ss[], Value alpha, Value beta, Depth depth, bool cut_node);

    template <NodeType NT, bool IN_CHECK>
    Value search_quien  (Position &pos, Stack ss[], Value alpha, Value beta, Depth depth);

    Value value_to_tt (Value v, int32_t ply);
    Value value_fr_tt (Value v, int32_t ply);

    string pv_info_uci  (const Position &pos, int16_t depth, Value alpha, Value beta);

    typedef struct Skill
    {
        int32_t level;
        Move    move;

        Skill (int32_t lvl)
            : level(lvl)
            , move(MOVE_NONE)
        {}

        ~Skill ()
        {
            if (enabled ()) // Swap best PV line with the sub-optimal one
            {
                //iter_swap (RootMoves.begin (), find (RootMoves.begin (), RootMoves.end (), move ? move : pick_move ()));
                swap (RootMoves[0], *std::find(RootMoves.begin (), RootMoves.end(), move ? move : pick_move ()));
            }
        }

        bool enabled ()                   const { return (level < 20); }
        bool time_to_pick (int16_t depth) const { return depth == (1 + level); }

        Move pick_move ();

    } Skill;


    // _perft() is our utility to verify move generation. All the leaf nodes
    // up to the given depth are generated and counted and the sum returned.
    size_t _perft (Position &pos, Depth depth)
    {
        size_t cnt = 0;
        const bool leaf = (depth == ONE_MOVE);

        StateInfo si;
        CheckInfo ci (pos);
        MoveList mov_lst = generate<LEGAL> (pos);

        for_each (mov_lst.cbegin (), mov_lst.cend (), [&] (Move m)
        {
            pos.do_move (m, si, pos.check (m, ci) ? &ci : NULL);
            cnt += leaf ? generate<LEGAL> (pos).size () : _perft (pos, depth - ONE_MOVE);
            pos.undo_move ();
        });

        return cnt;
    }

    // Debug functions used mainly to collect run-time statistics
    uint64_t
        hits [2] = { U64(0), U64(0), },
        means[2] = { U64(0), U64(0), };

    void dbg_hit_on  (bool b)           { ++hits[0]; if (b) ++hits[1]; }
    void dbg_hit_on_c(bool c, bool b)   { if (c) dbg_hit_on (b);       }
    void dbg_mean_of (int32_t v)        { ++means[0]; means[1] += v;   }

    inline void dbg_print()
    {
        if (hits[0])
        {
            cerr 
                << "Total " << (hits[0])
                << " Hits " << (hits[1])
                << " Hit-rate (%) " << (100 * hits[1] / hits[0])
                << endl;
        }
        if (means[0])
        {
            cerr 
                << "Total " << (means[0])
                << " Mean " << double (means[1]) / double (means[0])
                << endl;
        }
    }

} // namespace

namespace Searcher {

    Limits_t            Limits;
    volatile Signals_t  Signals;

    vector<RootMove>    RootMoves;
    Position            RootPos;
    StateInfoStackPtr   SetupStates;

    Time::point         SearchTime;

    // initialize the PRNG only once
    PolyglotBook book;

#pragma region Root Move

    // RootMove::extract_pv_from_tt() builds a PV by adding moves from the TT table.
    // We consider also failing high nodes and not only EXACT nodes so to
    // allow to always have a ponder move even when we fail high at root, and a
    // long PV to print that is important for position analysis.
    void RootMove::extract_pv_from_tt (Position &pos)
    {
        uint16_t ply = 0;
        Move m = pv[ply];
        pv.clear ();

        const TranspositionEntry *te;
        StateInfo states[MAX_PLY_6], *si = states;

        do
        {
            pv.emplace_back (m);

#ifndef NDEBUG
            MoveList mov_lst = generate<LEGAL> (pos);
            ASSERT (find (mov_lst.cbegin (), mov_lst.cend (), pv[ply]) != mov_lst.cend ());
#endif

            pos.do_move (pv[ply++], *si++);
            te = TT.retrieve (pos.posi_key ());

            //// Local copy, TT could change
            //if (!te || MOVE_NONE == (m = te->move ()) ||
            //    !pos.pseudo_legal (m) || !pos.legal (m) ||
            //    !(ply < MAX_PLY) || (pos.draw () && ply >= 1))
            //{
            //    break;
            //}
        }
        while (te // Local copy, TT could change
            && (m = te->move ())
            && pos.pseudo_legal (m)
            && pos.legal (m)
            && (ply < MAX_PLY)
            && (!pos.draw () || ply < 2));

        pv.emplace_back (MOVE_NONE); // Must be zero-terminating

        while (ply)
        {
            pos.undo_move ();
            --ply;
        }
    }

    // RootMove::insert_pv_in_tt() is called at the end of a search iteration, and
    // inserts the PV back into the TT. This makes sure the old PV moves are searched
    // first, even if the old TT entries have been overwritten.
    void RootMove::insert_pv_into_tt (Position &pos)
    {
        uint16_t ply = 0;

        const TranspositionEntry *te;
        StateInfo states[MAX_PLY_6], *si = states;

        do
        {
            te = TT.retrieve (pos.posi_key ());
            // Don't overwrite correct entries
            if (!te || te->move() != pv[ply])
            {
                TT.store (pos.posi_key (), pv[ply], DEPTH_NONE, BND_NONE, pos.game_nodes (), VALUE_NONE, VALUE_NONE);
            }

#ifndef NDEBUG
            MoveList mov_lst = generate<LEGAL> (pos);
            ASSERT (find (mov_lst.cbegin (), mov_lst.cend (), pv[ply]) != mov_lst.cend ());
#endif

            pos.do_move (pv[ply++], *si++);

        }
        while (MOVE_NONE != pv[ply]);

        while (ply)
        {
            pos.undo_move ();
            --ply;
        }
    }

#pragma endregion

    size_t perft (Position &pos, Depth depth)
    {
        return (depth > ONE_MOVE) ? _perft (pos, depth) : generate<LEGAL> (pos).size();
    }

    void think ()
    {
        TimeMgr.initialize (Limits, RootPos.game_ply (), RootPos.active ());

        bool write_search_log = *(Options["Write Search Log"]);
        string fn_search_log  = *(Options["Search Log File"]);

        if (RootMoves.empty ())
        {
            RootMoves.push_back (RootMove (MOVE_NONE));
            ats ()
                << "info depth 0 score "
                << score_uci (RootPos.checkers () ? -VALUE_MATE : VALUE_DRAW)
                << endl;

            goto finish;
        }

        if (*(Options["Own Book"]) && !Limits.infinite && !Limits.mate_in)
        {
            if (!book.is_open ()) book.open (*(Options["Book File"]), ios_base::in);
            Move book_move = book.probe_move (RootPos, *(Options["Best Book Move"]));
            if (book_move && count (RootMoves.begin (), RootMoves.end (), book_move))
            {
                swap (RootMoves[0], *find (RootMoves.begin (), RootMoves.end (), book_move));
                goto finish;
            }
        }

        int32_t cf = *(Options["Contempt Factor"]);
        if (cf && !*(Options["UCI_AnalyseMode"]))
        {
            cf = cf * VALUE_MG_PAWN / 100;                              // From centipawns
            cf = cf * Material::game_phase (RootPos) / PHASE_MIDGAME;   // Scale down with phase
            DrawValue[ RootPos.active ()] = VALUE_DRAW - Value (cf);
            DrawValue[~RootPos.active ()] = VALUE_DRAW + Value (cf);
        }
        else
        {
            DrawValue[WHITE] = VALUE_DRAW;
            DrawValue[BLACK] = VALUE_DRAW;
        }

        if (write_search_log)
        {
            Log log (fn_search_log);

            log << "---------\n" << boolalpha
                << "Searching: "    << RootPos.fen () << '\n'
                << " infinite: "    << Limits.infinite
                << " ponder: "      << Limits.ponder
                << " time: "        << Limits.game_clock[RootPos.active ()].time
                << " increment: "   << Limits.game_clock[RootPos.active ()].inc
                << " moves to go: " << uint32_t (Limits.moves_to_go)
                << endl;
        }

        // Reset the threads, still sleeping: will wake up at split time
        for (size_t i = 0; i < Threads.size (); ++i)
        {
            Threads[i]->max_ply = 0;
        }

        Threads.sleep_while_idle = *(Options["Idle Threads Sleep"]);
        Threads.timer->run = true;
        Threads.timer->notify_one();     // Wake up the recurring timer

        iter_deep_loop (RootPos);        // Let's start searching !

        Threads.timer->run = false;      // Stop the timer
        Threads.sleep_while_idle = true; // Send idle threads to sleep

        if (write_search_log)
        {
            uint64_t elapsed = Time::now () - SearchTime + 1;

            Log log (fn_search_log);
            log << "Nodes: "        << RootPos.game_nodes ()                  << '\n'
                << "Nodes/second: " << RootPos.game_nodes () * 1000 / elapsed << '\n'
                << "Best move: "    << move_to_san (RootMoves[0].pv[0], RootPos)
                << endl;

            StateInfo si;
            RootPos.do_move (RootMoves[0].pv[0], si);
            log << "Ponder move: " << move_to_san (RootMoves[0].pv[1], RootPos)
                << endl;
            RootPos.undo_move ();
        }

finish:

        // When search is stopped this info is not printed
        ats ()
            << "info"
            << " time "  << Time::now () - SearchTime + 1
            << " nodes " << RootPos.game_nodes ()
            << endl;

        // When we reach max depth we arrive here even without Signals.stop is raised,
        // but if we are pondering or in infinite search, according to UCI protocol,
        // we shouldn't print the best move before the GUI sends a "stop" or "ponderhit" command.
        // We simply wait here until GUI sends one of those commands (that raise Signals.stop).
        if (!Signals.stop && (Limits.ponder || Limits.infinite))
        {
            Signals.stop_on_ponderhit = true;
            RootPos.thread ()->wait_for (Signals.stop);
        }

        // Best move could be MOVE_NONE when searching on a stalemate position
        ats () << "bestmove " << move_to_can (RootMoves[0].pv[0], RootPos.chess960 ());
        if (RootMoves[0].pv[0])
        {
            ats () << " ponder " << move_to_can (RootMoves[0].pv[1], RootPos.chess960 ());
        }
        ats () << endl;

    }

    // initialize() is called during startup to initialize various lookup tables
    void initialize ()
    {
        // Init reductions array
        for (int32_t hd = 1; hd < 64; ++hd) // half-depth (ONE_PLY == 1)
        {
            for (int32_t mc = 1; mc < 64; ++mc) // move count
            {
                double     pv_red = 0.00 + log (double (hd)) * log (double (mc)) / 3.00;
                double non_pv_red = 0.33 + log (double (hd)) * log (double (mc)) / 2.25;
                Reductions[1][1][hd][mc] = int8_t (    pv_red >= 1.0 ? floor(    pv_red * int32_t (ONE_MOVE)) : 0);
                Reductions[0][1][hd][mc] = int8_t (non_pv_red >= 1.0 ? floor(non_pv_red * int32_t (ONE_MOVE)) : 0);

                Reductions[1][0][hd][mc] = Reductions[1][1][hd][mc];
                Reductions[0][0][hd][mc] = Reductions[0][1][hd][mc];

                if (false);
                else if (Reductions[0][0][hd][mc] > 2 * ONE_MOVE)
                {
                    Reductions[0][0][hd][mc] += ONE_MOVE;
                }
                else if (Reductions[0][0][hd][mc] > 1 * ONE_MOVE)
                {
                    Reductions[0][0][hd][mc] += ONE_MOVE / 2;
                }
            }
        }
        // Init futility move count array
        for (int32_t d = 0; d < 32; ++d)    // depth (ONE_MOVE == 2)
        {
            FutilityMoveCounts[0][d] = int32_t (2.4 + 0.222 * pow (d + 0.00, 1.8));
            FutilityMoveCounts[1][d] = int32_t (3.0 + 0.300 * pow (d + 0.98, 1.8));
        }
    }

}

namespace {

    // iter_deep_loop () is the main iterative deepening loop. It calls search() repeatedly
    // with increasing depth until the allocated thinking time has been consumed,
    // user stops the search, or the maximum search depth is reached.
    void iter_deep_loop (Position &pos)
    {
        Stack stack[MAX_PLY_6], *ss = stack+2; // To allow referencing (ss-2)

        memset (ss-2, 0, 5 * sizeof (Stack));
        (ss-1)->current_move = MOVE_NULL; // Hack to skip update gains

        TT.new_gen ();

        Gains.clear ();
        History.clear();
        CounterMoves.clear();
        FollowupMoves.clear();

        BestMoveChanges  = 0.0;

        Value best_value = -VALUE_INFINITE;
        Value alpha      = -VALUE_INFINITE;
        Value beta       = +VALUE_INFINITE;
        Value delta      = -VALUE_INFINITE;
        Depth depth      = DEPTH_ZERO;

        PVSize = int32_t (*(Options["MultiPV"]));
        Skill skill (*(Options["Skill Level"]));

        // Do we have to play with skill handicap? In this case enable MultiPV search
        // that we will use behind the scenes to retrieve a set of possible moves.
        if (skill.enabled () && PVSize < 4) PVSize = 4;

        if (RootMoves.size () < PVSize) PVSize = RootMoves.size ();

        // Iterative deepening loop until requested to stop or target depth reached
        while (++depth <= MAX_PLY && !Signals.stop && (!Limits.depth || depth <= Limits.depth))
        {
            // Age out PV variability metric
            BestMoveChanges *= 0.8;

            // Save last iteration's scores before first PV line is searched and all
            // the move scores but the (new) PV are set to -VALUE_INFINITE.
            for (uint32_t i = 0; i < RootMoves.size (); ++i)
            {
                RootMoves[i].last_value = RootMoves[i].curr_value;
            }

            // MultiPV loop. We perform a full root search for each PV line
            for (PVIdx = 0; PVIdx < PVSize && !Signals.stop; ++PVIdx)
            {
                // Reset aspiration window starting size
                if (depth >= 5)
                {
                    delta = Value (16);
                    alpha = max (RootMoves[PVIdx].last_value - delta, -VALUE_INFINITE);
                    beta  = min (RootMoves[PVIdx].last_value + delta, +VALUE_INFINITE);
                }

                // Start with a small aspiration window and, in case of fail high/low,
                // research with bigger window until not failing high/low anymore.
                while (true)
                {
                    best_value = search<Root> (pos, ss, alpha, beta, depth * int32_t (ONE_MOVE), false);

                    // Bring to front the best move. It is critical that sorting is
                    // done with a stable algorithm because all the values but the first
                    // and eventually the new best one are set to -VALUE_INFINITE and
                    // we want to keep the same order for all the moves but the new
                    // PV that goes to the front. Note that in case of MultiPV search
                    // the already searched PV lines are preserved.
                    stable_sort (RootMoves.begin () + PVIdx, RootMoves.end ());

                    // Write PV back to transposition table in case the relevant
                    // entries have been overwritten during the search.
                    for (uint32_t i = 0; i <= PVIdx; ++i)
                    {
                        RootMoves[i].insert_pv_into_tt (pos);
                    }

                    // If search has been stopped break immediately. Sorting and
                    // writing PV back to TT is safe becuase RootMoves is still
                    // valid, although refers to previous iteration.
                    if (Signals.stop) break;

                    // When failing high/low give some update
                    // (without cluttering the UI) before to research.
                    if ((alpha >= best_value || best_value >= beta) &&
                        (Time::now () - SearchTime) > 3000)
                    {
                        ats () << pv_info_uci (pos, depth, alpha, beta) << endl;
                    }

                    // In case of failing low/high increase aspiration window and
                    // research, otherwise exit the loop.
                    if (false);
                    else if (best_value <= alpha)
                    {
                        alpha = max (best_value - delta, -VALUE_INFINITE);

                        Signals.failed_low_at_root  = true;
                        Signals.stop_on_ponderhit   = false;
                    }
                    else if (best_value >= beta)
                    {
                        beta = min (best_value + delta, +VALUE_INFINITE);
                    }
                    else
                    {
                        break;
                    }

                    delta += delta / 2;

                    ASSERT (-VALUE_INFINITE <= alpha && beta <= +VALUE_INFINITE);
                }

                // Sort the PV lines searched so far and update the GUI
                stable_sort (RootMoves.begin (), RootMoves.begin () + PVIdx + 1);

                if (PVIdx + 1 == PVSize || (Time::now () - SearchTime) > 3000)
                {
                    ats () << pv_info_uci (pos, depth, alpha, beta) << endl;
                }
            }

            Iterated = Time::now () - SearchTime;

            // If skill levels are enabled and time is up, pick a sub-optimal best move
            if (skill.enabled () && skill.time_to_pick (depth))
            {
                skill.pick_move ();
            }

            bool write_search_log = *(Options["Write Search Log"]);
            if (write_search_log)
            {
                RootMove &rm = RootMoves[0];
                if (MOVE_NONE != skill.move)
                {
                    rm = *find (RootMoves.begin (), RootMoves.end (), skill.move);
                }

                string fn_search_log  = *(Options["Search Log File"]);
                Log log (fn_search_log);
                log << pretty_pv (pos, depth, rm.curr_value, (Time::now () - SearchTime), rm.pv)
                    << endl;
            }

            // Have found a "mate in x"?
            if (Limits.mate_in &&
                best_value >= VALUE_MATES_IN_MAX_PLY &&
                VALUE_MATE - best_value <= 2 * int32_t (Limits.mate_in))
            {
                Signals.stop = true;
            }

            // Do we have time for the next iteration? Can we stop searching now?
            if (Limits.use_time_management () &&
                !Signals.stop &&
                !Signals.stop_on_ponderhit)
            {
                bool stop = false; // Local variable, not the volatile Signals.stop

                // Take in account some extra time if the best move has changed
                if (4 < depth && depth < 50 &&  1 == PVSize)
                {
                    TimeMgr.pv_instability (BestMoveChanges);
                }

                // Stop the search if there is only one legal move available or 
                // most of the available time has been used.
                // We probably don't have enough time to search the first move at the next iteration anyway.
                if (RootMoves.size () == 1 ||
                    Iterated > TimeMgr.available_time () * 62 / 100)
                {
                    stop = true;
                }

                //// Stop the search early if one move seems to be much better than others
                //if (!stop && depth >= 12 &&
                //    BestMoveChanges <= DBL_EPSILON &&
                //    PVSize == 1 &&
                //    best_value > VALUE_MATED_IN_MAX_PLY &&
                //    (RootMoves.size () == 1 ||
                //    (Time::now () - SearchTime) > TimeMgr.available_time() * 20 / 100))
                //{
                //    Value rbeta = best_value - 2 * VALUE_MG_PAWN;
                //
                //    ss->excluded_move  = RootMoves[0].pv[0];
                //    ss->skip_null_move = true;
                //    Value v = search<NonPV> (pos, ss, rbeta - 1, rbeta, (depth - 3) * int32_t (ONE_MOVE), true);
                //    ss->skip_null_move = false;
                //    ss->excluded_move  = MOVE_NONE;
                //
                //    if (v < rbeta) stop = true;
                //}

                if (stop)
                {
                    // If we are allowed to ponder do not stop the search now but
                    // keep pondering until GUI sends "ponderhit" or "stop".
                    if (Limits.ponder)
                        Signals.stop_on_ponderhit = true;
                    else
                        Signals.stop              = true;
                }
            }
        }
    }

    template <NodeType NT>
    // search<> () is the main search function for both PV and non-PV nodes and for
    // normal and SplitPoint nodes. When called just after a split point the search
    // is simpler because we have already probed the hash table, done a null move
    // search, and searched the first move before splitting, we don't have to repeat
    // all this work again. We also don't need to store anything to the hash table
    // here: This is taken care of after we return from the split point.
    Value search (Position &pos, Stack ss[], Value alpha, Value beta, Depth depth, bool cut_node)
    {
        const bool   PVNode = (NT == PV || NT == Root || NT == SplitPointPV || NT == SplitPointRoot);
        const bool   SPNode = (NT == SplitPointPV  || NT == SplitPointNonPV || NT == SplitPointRoot);
        const bool RootNode = (NT == Root || NT == SplitPointRoot);

        ASSERT (-VALUE_INFINITE <= alpha && alpha < beta && beta <= +VALUE_INFINITE);
        ASSERT (PVNode || (alpha == beta - 1));
        ASSERT (depth > DEPTH_ZERO);

        StateInfo   si;

        SplitPoint *split_point;
        Key         posi_key;

        const TranspositionEntry *te;

        Move        best_move
            ,       tt_move
            ,       excluded_move
            ,       move;

        Value       best_value
            ,       tt_value
            ,       value;

        int32_t     moves_count
            ,       quiets_count;

        Move quiets_searched[64] = {MOVE_NONE};

        // Step 1. Initialize node
        Thread *thread      = pos.thread ();
        bool    in_check    = pos.checkers ();

        if (SPNode)
        {
            split_point = ss->split_point;
            best_move   = split_point->best_move;
            best_value  = split_point->best_value;

            te       = NULL;
            tt_move  = excluded_move = MOVE_NONE;
            tt_value = VALUE_NONE;

            ASSERT (split_point->best_value > -VALUE_INFINITE && split_point->moves_count > 0);

            goto moves_loop;
        }

        moves_count  = 0;
        quiets_count = 0;
        best_value  = -VALUE_INFINITE;
        best_move   = ss->current_move = ss->tt_move = (ss+1)->excluded_move = MOVE_NONE;
        (ss)->ply   = (ss-1)->ply + 1;
        (ss+1)->skip_null_move = false;
        (ss+1)->reduction      = DEPTH_ZERO;
        (ss+2)->killers[0] = (ss+2)->killers[1] = MOVE_NONE;

        // Used to send sel_depth info to GUI
        if (PVNode && thread->max_ply < ss->ply) thread->max_ply = ss->ply;

        if (!RootNode)
        {
            // Step 2. Check for aborted search and immediate draw
            if (Signals.stop || pos.draw () || ss->ply > MAX_PLY)
            {
                return DrawValue[pos.active ()];
            }

            // Step 3. Mate distance pruning. Even if we mate at the next move our score
            // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
            // a shorter mate was found upward in the tree then there is no need to search
            // further, we will never beat current alpha. Same logic but with reversed signs
            // applies also in the opposite condition of being mated instead of giving mate,
            // in this case return a fail-high score.
            alpha = max (mated_in (ss->ply)  , alpha);
            beta  = min (mates_in (ss->ply +1), beta);

            if (alpha >= beta) return alpha;
        }

        // Step 4. Transposition table lookup
        // We don't want the score of a partial search to overwrite a previous full search
        // TT value, so we use a different position key in case of an excluded move.
        excluded_move = ss->excluded_move;
        posi_key = excluded_move ? pos.posi_key_exclusion () : pos.posi_key ();
        te       = TT.retrieve (posi_key);
        tt_move  = ss->tt_move = RootNode ? RootMoves[PVIdx].pv[0]
        :          te ?              te->move ()            : MOVE_NONE;
        tt_value = te ? value_fr_tt (te->value (), ss->ply) : VALUE_NONE;

        // At PV nodes we check for exact scores, while at non-PV nodes we check for
        // a fail high/low. Biggest advantage at probing at PV nodes is to have a
        // smooth experience in analysis mode. We don't probe at Root nodes otherwise
        // we should also update RootMoveList to avoid bogus output.
        if (!RootNode && te &&
            te->depth () >= depth &&
            tt_value != VALUE_NONE && // Only in case of TT access race
            (           PVNode ?  te->bound () == BND_EXACT
            : tt_value >= beta ? (te->bound () &  BND_LOWER)
            /**/               : (te->bound () &  BND_UPPER)))
        {
            TT.refresh (te);
            ss->current_move = tt_move; // Can be MOVE_NONE

            // If tt_move is quiet, update killers, history, counter move and followup move on TT hit
            if ( tt_value >= beta && tt_move &&
                !pos.capture_or_promotion (tt_move) && !in_check)
            {
                update_stats (pos, ss, tt_move, depth, NULL, 0);
            }
            return tt_value;
        }

        Value eval_value;

        // Step 5. Evaluate the position statically and update parent's gain statistics
        if (in_check)
        {
            eval_value = ss->static_eval = VALUE_NONE;
            goto moves_loop;
        }
        else if (te)
        {
            // Never assume anything on values stored in TT
            Value e_value = te->eval_value ();
            if (VALUE_NONE == e_value) e_value = evaluate (pos);
            eval_value = ss->static_eval = e_value;

            // Can tt_value be used as a better position evaluation?
            if (VALUE_NONE != tt_value)
            {
                if (te->bound () & (tt_value > eval_value ? BND_LOWER : BND_UPPER))
                {
                    eval_value = tt_value;
                }
            }
        }
        else
        {
            eval_value = ss->static_eval = evaluate (pos);
            TT.store (posi_key, MOVE_NONE, DEPTH_NONE, BND_NONE, pos.game_nodes (), VALUE_NONE, ss->static_eval);
        }

        if (pos.cap_type () != PT_NO &&
            ss->static_eval != VALUE_NONE &&
            (ss-1)->static_eval != VALUE_NONE &&
            (move = (ss-1)->current_move) != MOVE_NULL &&
            m_type (move) == NORMAL)
        {
            Square dst = dst_sq (move);
            Gains.update (pos[dst], dst, -((ss-1)->static_eval + ss->static_eval));
        }

        // Step 6. Razoring (skipped when in check)
        if (!PVNode && depth < 4 * ONE_MOVE &&
            eval_value + razor_margin (depth) < beta &&
            abs (int32_t (beta)) < VALUE_MATES_IN_MAX_PLY &&
            !tt_move && !pos.pawn_on_7thR (pos.active ()))
        {
            Value rbeta = beta - razor_margin (depth);
            Value v = search_quien<NonPV, false> (pos, ss, rbeta-1, rbeta, DEPTH_ZERO);
            if (v < rbeta)
            {
                // Logically we should return (value + razor_margin (depth)), but
                // surprisingly this did slightly weaker in tests.
                return v;
            }
        }

        // Step 7. Futility pruning: child node (skipped when in check)
        /// We're betting that the opponent doesn't have a move that will reduce
        /// the score by more than futility_margin (depth) if we do a null move.
        if (!PVNode && !ss->skip_null_move &&
            depth < 7 * ONE_MOVE &&
            eval_value - futility_margin (depth) >= beta &&
            abs (int32_t (beta)) < VALUE_MATES_IN_MAX_PLY &&
            abs (int32_t (eval_value)) < VALUE_KNOWN_WIN &&
            pos.non_pawn_material (pos.active ()))
        {
            return eval_value - futility_margin (depth);
        }

        // Step 8. Null move search with verification search (is omitted in PV nodes)
        if (!PVNode && !ss->skip_null_move &&
            depth >= 2 * ONE_MOVE &&
            eval_value >= beta &&
            abs (int32_t (beta)) < VALUE_MATES_IN_MAX_PLY &&
            pos.non_pawn_material (pos.active ()))
        {
            ss->current_move = MOVE_NULL;

            // Null move dynamic reduction based on depth
            Depth red_depth = 3 * ONE_MOVE + depth / 4;

            // Null move dynamic reduction based on value
            if (eval_value - VALUE_MG_PAWN > beta) red_depth += ONE_MOVE;

            // Make null move
            pos.do_null_move (si);
            (ss+1)->skip_null_move = true;
            Value null_value = 
                (depth-red_depth < ONE_MOVE)
                ? -search_quien<NonPV, false> (pos, ss+1, -beta, -alpha, DEPTH_ZERO)
                : -search      <NonPV>        (pos, ss+1, -beta, -alpha, depth-red_depth, !cut_node);
            (ss+1)->skip_null_move = false;
            // Unmake null move
            pos.undo_null_move ();

            if (null_value >= beta)
            {
                // Do not return unproven mate scores
                if (null_value >= VALUE_MATES_IN_MAX_PLY)
                {
                    null_value = beta;
                }

                if (depth < 12 * ONE_MOVE) return null_value;

                // Do verification search at high depths
                ss->skip_null_move = true;
                Value v = search<NonPV> (pos, ss, alpha, beta, depth-red_depth, false);
                ss->skip_null_move = false;

                if (v >= beta) return null_value;
            }
        }

        // Step 9. ProbCut (skipped when in check)
        // If we have a very good capture (i.e. SEE > seeValues[captured_piece_type])
        // and a reduced search returns a value much above beta, we can (almost) safely
        // prune the previous move.
        if (!PVNode && depth >= 5 * ONE_MOVE &&
            !ss->skip_null_move &&
            abs (int32_t (beta)) < VALUE_MATES_IN_MAX_PLY)
        {
            Value rbeta  = beta + 200;
            Depth rdepth = depth - ONE_MOVE - 3 * ONE_MOVE;

            ASSERT (rdepth >= ONE_MOVE);
            ASSERT ((ss-1)->current_move != MOVE_NONE);
            ASSERT ((ss-1)->current_move != MOVE_NULL);

            // Initialize a MovePicker object for the current position,
            // and prepare to search the moves.
            MovePicker mp (pos, tt_move, History, pos.cap_type ());
            CheckInfo  ci (pos);

            while ((move = mp.next_move<false> ()) != MOVE_NONE)
            {
                if (!pos.legal (move, ci.pinneds)) continue;

                ss->current_move = move;

                pos.do_move (move, si, pos.check (move, ci) ? &ci : NULL);

                value = -search<NonPV> (pos, ss+1, -rbeta, -rbeta+1, rdepth, !cut_node);

                pos.undo_move ();

                if (value >= rbeta) return value;
            }
        }

        // Step 10. Internal iterative deepening (skipped when in check)
        if (depth >= (PVNode ? 5 * ONE_MOVE : 8 * ONE_MOVE) &&
            !tt_move && (PVNode || ss->static_eval + Value (256) >= beta))
        {
            Depth d = depth - 2 * ONE_MOVE - (PVNode ? DEPTH_ZERO : depth / 4);

            ss->skip_null_move = true;
            search<PVNode ? PV : NonPV> (pos, ss, alpha, beta, d, true);
            ss->skip_null_move = false;

            te = TT.retrieve (posi_key);
            tt_move = te ? te->move() : MOVE_NONE;
        }

moves_loop: // When in check and at SPNode search starts from here

        Square prev_move_sq = dst_sq ((ss-1)->current_move);
        Move cm[CLR_NO] = 
        {
            CounterMoves[pos[prev_move_sq]][prev_move_sq].first,
            CounterMoves[pos[prev_move_sq]][prev_move_sq].second,
        };

        Square prev_own_move_sq = dst_sq ((ss-2)->current_move);
        Move fm[CLR_NO] =
        { 
            FollowupMoves[pos[prev_own_move_sq]][prev_own_move_sq].first,
            FollowupMoves[pos[prev_own_move_sq]][prev_own_move_sq].second,
        };


        MovePicker mp (pos, tt_move, depth, History, cm, fm, ss);
        CheckInfo  ci (pos);

        value = best_value; // Workaround a bogus 'uninitialized' warning under gcc
        bool improving = 
            ss->static_eval >= (ss-2)->static_eval ||
            ss->static_eval == VALUE_NONE          ||
            (ss-2)->static_eval == VALUE_NONE;

        bool singular_ext_node =
            !RootNode && !SPNode  &&
            depth >= 8 * ONE_MOVE &&
            tt_move != MOVE_NONE  &&
            !excluded_move        && // Recursive singular search is not allowed
            (te->bound () & BND_LOWER) &&
            (te->depth () >= depth - 3 * ONE_MOVE);

        // Step 11. Loop through moves
        // Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs
        while ((move = mp.next_move<SPNode> ()) != MOVE_NONE)
        {
            ASSERT (_ok (move));
            if (move == excluded_move) continue;

            // At root obey the "searchmoves" option and skip moves not listed in Root
            // Move List, as a consequence any illegal move is also skipped. In MultiPV
            // mode we also skip PV moves which have been already searched.
            if (RootNode && !count (RootMoves.begin () + PVIdx, RootMoves.end (), move)) continue;

            if (SPNode)
            {
                // Shared counter cannot be decremented later if move turns out to be illegal
                if (!pos.legal (move, ci.pinneds)) continue;

                moves_count = ++split_point->moves_count;
                split_point->mutex.unlock ();
            }
            else
            {
                ++moves_count;
            }

            if (RootNode)
            {
                Signals.first_root_move = (1 == moves_count);

                if (thread == Threads.main () && 
                    Time::now () - SearchTime > 3000)
                {
                    ats ()
                        << "info"
                        << " depth "          << int32_t (depth / ONE_MOVE)
                        << " currmove "       << move_to_can (move, pos.chess960 ())
                        << " currmovenumber " << moves_count + PVIdx
                        << endl;
                }
            }

            Depth ext = DEPTH_ZERO;
            bool capture_or_promotion = pos.capture_or_promotion (move);
            bool gives_check          = pos.check (move, ci);

            bool dangerous = gives_check || NORMAL != m_type (move) || pos.advanced_pawn_push (move);

            // Step 12. Extend checks
            if (gives_check && pos.see_sign (move) >= 0) ext = ONE_MOVE;

            // Singular extension search. If all moves but one fail low on a search of
            // (alpha-s, beta-s), and just one fails high on (alpha, beta), then that move
            // is singular and should be extended. To verify this we do a reduced search
            // on all the other moves but the tt_move, if result is lower than tt_value minus
            // a margin then we extend tt_move.
            if (singular_ext_node &&
                move == tt_move &&
                ext != DEPTH_ZERO &&
                pos.legal (move, ci.pinneds) &&
                abs (int32_t (tt_value)) < VALUE_KNOWN_WIN)
            {
                ASSERT (tt_value != VALUE_NONE);

                Value rbeta = tt_value - int32_t (depth);
                ss->excluded_move  = move;
                ss->skip_null_move = true;
                value = search<NonPV> (pos, ss, rbeta - 1, rbeta, depth / 2, cut_node);
                ss->skip_null_move = false;
                ss->excluded_move  = MOVE_NONE;

                if (value < rbeta) ext = ONE_MOVE;
            }

            // Update current move (this must be done after singular extension search)
            Depth new_depth = depth - ONE_MOVE + ext;

            // Step 13. Pruning at shallow depth (exclude PV nodes)
            if (!PVNode &&
                !capture_or_promotion &&
                !in_check &&
                !dangerous &&
                // move != tt_move &&  // Already implicit in the next condition 
                best_value > VALUE_MATED_IN_MAX_PLY)
            {
                // Move count based pruning
                if (depth < 16 * ONE_MOVE &&
                    moves_count >= FutilityMoveCounts[improving][depth])
                {
                    if (SPNode) split_point->mutex.lock ();
                    continue;
                }

                // Value based pruning
                // We illogically ignore reduction condition depth >= 3*ONE_MOVE for predicted depth,
                // but fixing this made program slightly weaker.
                Depth predicted_depth = new_depth - reduction<PVNode> (improving, depth, moves_count);

                // Futility pruning: parent node
                if (predicted_depth < 7 * ONE_MOVE)
                {
                    Value futility_value = ss->static_eval + futility_margin (predicted_depth)
                        + Value (128) + Gains[pos[org_sq (move)]][dst_sq (move)];

                    if (futility_value <= alpha)
                    {
                        if (futility_value > best_value) best_value = futility_value;

                        if (SPNode)
                        {
                            split_point->mutex.lock ();
                            if (best_value > split_point->best_value) split_point->best_value = best_value;
                        }
                        continue;
                    }
                }

                // Prune moves with negative SEE at low depths
                if (predicted_depth < 4 * ONE_MOVE && pos.see_sign (move) < 0)
                {
                    if (SPNode) split_point->mutex.lock ();
                    continue;
                }
            }

            // Check for legality only before to do the move
            if (!RootNode && !SPNode && !pos.legal (move, ci.pinneds))
            {
                --moves_count;
                continue;
            }

            bool move_pv = PVNode && (1 == moves_count);
            ss->current_move = move;

            if (!SPNode && !capture_or_promotion)
            {
                if (quiets_count < 64) quiets_searched[quiets_count++] = move;
            }

            // Step 14. Make the move
            pos.do_move (move, si, gives_check ? &ci : NULL);

            bool full_depth_search;

            // Step 15. Reduced depth search (LMR).
            // If the move fails high will be re-searched at full depth.
            if (!move_pv &&
                depth >= 3 * ONE_MOVE  && 
                !capture_or_promotion  &&
                move != tt_move        &&
                move != ss->killers[0] &&
                move != ss->killers[1])
            {
                ss->reduction = reduction<PVNode> (improving, depth, moves_count);

                if (!PVNode && cut_node)
                {
                    ss->reduction += ONE_MOVE;
                }
                else if (History[pos[dst_sq (move)]][dst_sq (move)] < 0)
                {
                    ss->reduction += ONE_MOVE / 2;
                }

                if (move == cm[0] || move == cm[1])
                {
                    ss->reduction = max (DEPTH_ZERO, ss->reduction - ONE_MOVE);
                }

                Depth d = max (new_depth - ss->reduction, ONE_MOVE);

                if (SPNode) alpha = split_point->alpha;

                value = -search<NonPV> (pos, ss+1, -(alpha+1), -alpha, d, true);

                // Research at intermediate depth if reduction is very high
                if (value > alpha && ss->reduction >= 4 * ONE_MOVE)
                {
                    Depth inter_depth = max (new_depth - 2 * ONE_MOVE, ONE_MOVE);
                    value = -search<NonPV> (pos, ss+1, -(alpha+1), -alpha, inter_depth, true);
                }

                full_depth_search = (value > alpha && ss->reduction != DEPTH_ZERO);
                ss->reduction = DEPTH_ZERO;
            }
            else
            {
                full_depth_search = !move_pv;
            }

            // Step 16. Full depth search, when LMR is skipped or fails high
            if (full_depth_search)
            {
                if (SPNode) alpha = split_point->alpha;

                value = 
                    new_depth < ONE_MOVE
                    ? (gives_check
                    ? -search_quien<NonPV,  true> (pos, ss+1, -(alpha+1), -alpha, DEPTH_ZERO)
                    : -search_quien<NonPV, false> (pos, ss+1, -(alpha+1), -alpha, DEPTH_ZERO))
                    : -search      <NonPV>        (pos, ss+1, -(alpha+1), -alpha, new_depth, !cut_node);
            }

            // For PV nodes only, do a full PV search on the first move or after a fail
            // high (in the latter case search only if value < beta), otherwise let the
            // parent node fail low with value <= alpha and to try another move.
            if (PVNode && (move_pv || (value > alpha && (RootNode || value < beta))))
            {
                value =
                    new_depth < ONE_MOVE
                    ? (gives_check
                    ? -search_quien<PV,  true> (pos, ss+1, -beta, -alpha, DEPTH_ZERO)
                    : -search_quien<PV, false> (pos, ss+1, -beta, -alpha, DEPTH_ZERO))
                    : -search      <PV>        (pos, ss+1, -beta, -alpha, new_depth, false);
            }

            // Step 17. Undo move
            pos.undo_move ();

            ASSERT (-VALUE_INFINITE < value && value < +VALUE_INFINITE);

            // Step 18. Check for new best move
            if (SPNode)
            {
                split_point->mutex.lock ();
                best_value = split_point->best_value;
                alpha = split_point->alpha;
            }

            // Finished searching the move. If Signals.stop is true, the search
            // was aborted because the user interrupted the search or because we
            // ran out of time. In this case, the return value of the search cannot
            // be trusted, and we don't update the best move and/or PV.
            if (Signals.stop || thread->cutoff_occurred ())
            {
                return value; // To avoid returning VALUE_INFINITE
            }

            if (RootNode)
            {
                RootMove &rm = *find (RootMoves.begin (), RootMoves.end (), move);

                // PV move or new best move ?
                if (move_pv || value > alpha)
                {
                    rm.curr_value = value;
                    rm.extract_pv_from_tt (pos);

                    // We record how often the best move has been changed in each
                    // iteration. This information is used for time management: When
                    // the best move changes frequently, we allocate some more time.
                    if (!move_pv) ++BestMoveChanges;
                }
                else
                {
                    // All other moves but the PV are set to the lowest value, this
                    // is not a problem when sorting becuase sort is stable and move
                    // position in the list is preserved, just the PV is pushed up.
                    rm.curr_value = -VALUE_INFINITE;
                }
            }

            if (value > best_value)
            {
                if (SPNode) split_point->best_value = value;
                best_value = value;

                if (value > alpha)
                {
                    if (SPNode) split_point->best_move = move;
                    best_move = move;

                    if (PVNode && value < beta) // Update alpha! Always alpha < beta
                    {
                        if (SPNode) split_point->alpha = value;
                        alpha = value;
                    }
                    else
                    {
                        ASSERT (value >= beta); // Fail high
                        if (SPNode) split_point->cut_off = true;
                        break;
                    }
                }
            }

            // Step 19. Check for splitting the search
            if (!SPNode && depth >= Threads.min_split_depth &&
                Threads.available_slave (thread) &&
                thread->split_points_size < MAX_SPLITPOINTS_PER_THREAD)
            {
                ASSERT (best_value < beta);

                thread->split<FakeSplit> (pos, ss, alpha, beta, &best_value, &best_move, depth, moves_count, &mp, NT, cut_node);

                if (best_value >= beta) break;
            }
        }

        if (SPNode) return best_value;

        // Step 20. Check for mate and stalemate
        // All legal moves have been searched and if there are no legal moves, it
        // must be mate or stalemate. Note that we can have a false positive in
        // case of Signals.stop or thread.cutoff_occurred() are set, but this is
        // harmless because return value is discarded anyhow in the parent nodes.
        // If we are in a singular extension search then return a fail low score.
        // A split node has at least one move, the one tried before to be splitted.
        if (0 == moves_count)
        {
            return excluded_move ? alpha : in_check ? mated_in (ss->ply) : DrawValue[pos.active ()];
        }

        // If we have pruned all the moves without searching return a fail-low score
        if (best_value == -VALUE_INFINITE) best_value = alpha;

        TT.store (
            posi_key,
            best_move,
            depth, 
            best_value >= beta  ? BND_LOWER : PVNode && best_move ? BND_EXACT : BND_UPPER,
            pos.game_nodes (),
            value_to_tt (best_value, ss->ply),
            ss->static_eval);

        // Quiet best move: update killers, history, counter moves and followup moves
        if (best_value >= beta && !pos.capture_or_promotion (best_move) && !in_check)
        {
            update_stats (pos, ss, best_move, depth, quiets_searched, quiets_count);
        }

        ASSERT (-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);

        return best_value;
    }

    template <NodeType NT, bool IN_CHECK>
    // search_quien() is the quiescence search function, which is called by the main search function
    // when the remaining depth is zero (or, to be more precise, less than ONE_MOVE).
    Value search_quien (Position &pos, Stack ss[], Value alpha, Value beta, Depth depth)
    {
        const bool PVNode = (NT == PV);

        ASSERT (NT == PV || NT == NonPV);
        ASSERT (IN_CHECK == !!pos.checkers ());
        ASSERT (alpha >= -VALUE_INFINITE && alpha < beta && beta <= +VALUE_INFINITE);
        ASSERT (PVNode || (alpha == beta - 1));
        ASSERT (depth <= DEPTH_ZERO);

        Move best_move = ss->current_move = MOVE_NONE;
        ss->ply = (ss-1)->ply + 1;

        // Check for an instant draw or maximum ply reached
        if (pos.draw () || ss->ply > MAX_PLY)
        {
            return DrawValue[pos.active ()];
        }

        Value       best_value;
        Value       old_alpha;
        StateInfo   si;

        // To flag EXACT a node with eval_value above alpha and no available moves
        if (PVNode) old_alpha = alpha;

        // Decide whether or not to include checks, this fixes also the type of
        // TT entry depth that we are going to use. Note that in search_quien we use
        // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
        Depth tt_depth = IN_CHECK || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS : DEPTH_QS_NO_CHECKS;

        Key posi_key = pos.posi_key ();

        // Transposition table lookup
        const TranspositionEntry *te;
        Move  tt_move;
        Value tt_value;

        te       = TT.retrieve (posi_key);
        tt_move  = te ?              te->move()             : MOVE_NONE;
        tt_value = te ? value_fr_tt (te->value (), ss->ply) : VALUE_NONE;

        if (   te
            && te->depth() >= tt_depth
            && tt_value != VALUE_NONE // Only in case of TT access race
            && (        PVNode ?  te->bound () == BND_EXACT
            : tt_value >= beta ? (te->bound () &  BND_LOWER)
            /**/               : (te->bound () &  BND_UPPER)))
        {
            ss->current_move = tt_move; // Can be MOVE_NONE
            return tt_value;
        }

        Value futility_base;

        // Evaluate the position statically
        if (IN_CHECK)
        {
            ss->static_eval = VALUE_NONE;
            best_value = futility_base = -VALUE_INFINITE;
        }
        else
        {
            if (te)
            {
                // Never assume anything on values stored in TT
                Value e_value = te->eval_value ();
                if (VALUE_NONE == e_value) e_value = evaluate (pos);
                best_value = ss->static_eval = e_value;

                // Can tt_value be used as a better position evaluation?
                if (VALUE_NONE != tt_value)
                {
                    if (te->bound () & (tt_value > best_value ? BND_LOWER : BND_UPPER))
                    {
                        best_value = tt_value;
                    }
                }
            }
            else
            {
                best_value = ss->static_eval = evaluate (pos);
            }

            // Stand pat. Return immediately if static value is at least beta
            if (best_value >= beta)
            {
                if (!te)
                {
                    TT.store (pos.posi_key (), MOVE_NONE, DEPTH_NONE, BND_LOWER, pos.game_nodes (), value_to_tt (best_value, ss->ply), ss->static_eval);
                }
                return best_value;
            }

            if (PVNode && best_value > alpha) alpha = best_value;

            futility_base = best_value + Value (128);
        }

        // Initialize a MovePicker object for the current position, and prepare
        // to search the moves. Because the depth is <= 0 here, only captures,
        // queen promotions and checks (only if depth >= DEPTH_QS_CHECKS) will
        // be generated.
        MovePicker mp (pos, tt_move, depth, History, dst_sq ((ss-1)->current_move));
        CheckInfo  ci (pos);

        Move move;
        // Loop through the moves until no moves remain or a beta cutoff occurs
        while ((move = mp.next_move<false> ()) != MOVE_NONE)
        {
            ASSERT (_ok (move));

            bool gives_check = pos.check (move, ci);

            // Futility pruning
            if (!PVNode && !IN_CHECK &&
                !gives_check && move != tt_move &&
                !pos.advanced_pawn_push (move) &&
                futility_base > -VALUE_KNOWN_WIN)
            {
                ASSERT (m_type (move) != ENPASSANT); // Due to !pos.advanced_pawn_push

                Value futility_value = futility_base + PieceValue[EG][_type (pos[dst_sq (move)])];

                if (futility_value < beta)
                {
                    if (futility_value > best_value) best_value = futility_value;
                    continue;
                }
                // Prune moves with negative or equal SEE and also moves with positive
                // SEE where capturing piece loses a tempo and SEE < beta - futility_base.
                if (futility_base < beta && pos.see (move) <= 0)
                {
                    if (futility_base > best_value) best_value = futility_base;
                    continue;
                }
            }

            // Detect non-capture evasions that are candidate to be pruned
            bool evasion_prunable = IN_CHECK &&
                best_value > VALUE_MATED_IN_MAX_PLY &&
                !pos.capture (move) &&
                !pos.can_castle (pos.active ());

            // Don't search moves with negative SEE values
            if (!PVNode && (!IN_CHECK || evasion_prunable) &&
                move != tt_move &&
                m_type (move) != PROMOTE &&
                pos.see_sign (move) < 0)
            {
                continue;
            }

            // Check for legality just before making the move
            if (!pos.legal (move, ci.pinneds)) continue;

            ss->current_move = move;

            // Make and search the move
            pos.do_move (move, si, gives_check ? &ci : NULL);

            Value value = 
                gives_check
                ? -search_quien<NT,  true> (pos, ss+1, -beta, -alpha, depth - ONE_MOVE)
                : -search_quien<NT, false> (pos, ss+1, -beta, -alpha, depth - ONE_MOVE);

            pos.undo_move ();

            ASSERT (-VALUE_INFINITE < value && value < +VALUE_INFINITE);

            // Check for new best move
            if (value > best_value)
            {
                best_value = value;

                if (value > alpha)
                {
                    if (PVNode && value < beta) // Update alpha here! Always alpha < beta
                    {
                        alpha = value;
                        best_move = move;
                    }
                    else // Fail high
                    {
                        TT.store (posi_key, move, tt_depth, BND_LOWER, pos.game_nodes (), value_to_tt(value, ss->ply), ss->static_eval);
                        return value;
                    }
                }
            }
        }

        // All legal moves have been searched. A special case: If we're in check
        // and no legal moves were found, it is checkmate.
        if (IN_CHECK && best_value == -VALUE_INFINITE)
        {
            return mated_in (ss->ply); // Plies to mate from the root
        }

        TT.store (
            posi_key,
            best_move,
            tt_depth,
            PVNode && (best_value > old_alpha) ? BND_EXACT : BND_UPPER,
            pos.game_nodes (),
            value_to_tt (best_value, ss->ply),
            ss->static_eval);

        ASSERT (-VALUE_INFINITE < best_value && best_value < +VALUE_INFINITE);

        return best_value;
    }

    // value_to_tt () adjusts a mate score from "plies to mate from the root" to
    // "plies to mate from the current position". Non-mate scores are unchanged.
    // The function is called before storing a value to the transposition table.
    inline Value value_to_tt (Value v, int32_t ply)
    {
        //ASSERT (v != VALUE_NONE);
        
        return
            v >= VALUE_MATES_IN_MAX_PLY ? v + ply :
            v <= VALUE_MATED_IN_MAX_PLY ? v - ply : v;
    }

    // value_fr_tt () is the inverse of value_to_tt ():
    // It adjusts a mate score from the transposition table
    // (where refers to the plies to mate/be mated from current position)
    // to "plies to mate/be mated from the root".
    inline Value value_fr_tt (Value v, int32_t ply)
    {
        return
            v == VALUE_NONE             ? VALUE_NONE :
            v >= VALUE_MATES_IN_MAX_PLY ? v - ply :
            v <= VALUE_MATED_IN_MAX_PLY ? v + ply : v;
    }

    // When playing with strength handicap choose best move among the MultiPV set
    // using a statistical rule dependent on 'level'. Idea by Heinz van Saanen.
    Move Skill::pick_move ()
    {
        static RKISS rk;
        // PRNG sequence should be not deterministic
        for (int32_t i = int32_t (Time::now ()) % 50; i > 0; --i) rk.rand64 ();

        // RootMoves are already sorted by score in descending order
        int32_t variance = min (RootMoves[0].curr_value - RootMoves[PVSize - 1].curr_value, VALUE_MG_PAWN);
        int32_t weakness = 120 - 2 * level;
        int32_t max_v    = -VALUE_INFINITE;
        move = MOVE_NONE;

        // Choose best move. For each move score we add two terms both dependent on
        // weakness, one deterministic and bigger for weaker moves, and one random,
        // then we choose the move with the resulting highest score.
        for (size_t i = 0; i < PVSize; ++i)
        {
            int32_t v = RootMoves[i].curr_value;

            // Don't allow crazy blunders even at very low skills
            if ((i > 0) && (RootMoves[i-1].curr_value > (v + 2 * VALUE_MG_PAWN))) break;

            // This is our magic formula
            v += (weakness * int32_t (RootMoves[0].curr_value - v)
                + variance * (rk.randX<uint32_t> () % weakness)) / 128;

            if (v > max_v)
            {
                max_v = v;
                move = RootMoves[i].pv[0];
            }
        }
        return move;
    }

    // pv_info_uci () formats PV information according to UCI protocol.
    // UCI requires to send all the PV lines also if are still to be searched
    // and so refer to the previous search score.
    inline string pv_info_uci (const Position &pos, int16_t depth, Value alpha, Value beta)
    {
        stringstream spv;

        uint64_t elapsed = Time::now () - SearchTime + 1;
        uint32_t pv_size = min<int32_t> (*(Options["MultiPV"]), RootMoves.size ());

        int32_t sel_depth = 0;
        for (uint32_t i = 0; i < Threads.size (); ++i)
        {
            if (Threads[i]->max_ply > sel_depth)
            {
                sel_depth = Threads[i]->max_ply;
            }
        }

        for (uint32_t i = 0; i < pv_size; ++i)
        {
            bool updated = (i <= PVIdx);

            if ((1 == depth) && !updated) continue;

            int32_t d = updated ? depth : depth - 1;
            Value   v = updated ? RootMoves[i].curr_value : RootMoves[i].last_value;

            // Not at first line
            if (spv.rdbuf ()->in_avail ()) spv << endl;

            spv << "info"
                << " depth "    << d
                << " seldepth " << sel_depth
                << " score "    << (i == PVIdx ? score_uci (v, alpha, beta) : score_uci (v))
                << " time "     << elapsed
                << " nodes "    << pos.game_nodes ()
                << " nps "      << pos.game_nodes () * 1000 / elapsed
                << " multipv "  << i + 1
                << " pv";
            for (size_t j = 0; RootMoves[i].pv[j] != MOVE_NONE; ++j)
            {
                spv << ' ' << move_to_can (RootMoves[i].pv[j], pos.chess960 ());
            }
        }

        return spv.str ();
    }

} // namespace

// check_time () is called by the timer thread when the timer triggers.
// It is used to print debug info and, more important,
// to detect when we are out of available time and so stop the search.
void check_time ()
{
    static Time::point last_time = Time::now ();
    int64_t nodes = 0; // Workaround silly 'uninitialized' gcc warning

    Time::point now_time = Time::now ();
    if (now_time - last_time >= 1000)
    {
        last_time = now_time;
        dbg_print ();
    }

    if (Limits.ponder) return;

    if (Limits.nodes)
    {
        Threads.mutex.lock ();

        nodes = RootPos.game_nodes ();

        // Loop across all split points and sum accumulated SplitPoint nodes plus
        // all the currently active positions nodes.
        for (size_t i = 0; i < Threads.size (); ++i)
        {
            for (int32_t j = 0; j < Threads[i]->split_points_size; ++j)
            {
                SplitPoint &sp = Threads[i]->split_points[j];
                sp.mutex.lock ();
                nodes += sp.nodes;
                Bitboard sm = sp.slaves_mask;
                while (sm)
                {
                    Position *pos = Threads[pop_lsq (sm)]->active_pos;
                    if (pos) nodes += pos->game_nodes();
                }
                sp.mutex.unlock ();
            }
        }
        Threads.mutex.unlock ();
    }

    uint64_t elapsed = now_time - SearchTime;

    bool still_at_first_move = 
        Signals.first_root_move     &&
        !Signals.failed_low_at_root &&
        (elapsed > TimeMgr.available_time ()
        || (   elapsed > TimeMgr.available_time() * 62 / 100
        &&     elapsed > Iterated * 1.4));

    bool no_more_time = 
        elapsed > TimeMgr.maximum_time () - 2 * TimerThread::Resolution ||
        still_at_first_move;

    if ((Limits.use_time_management () && no_more_time)   ||
        (Limits.move_time && elapsed >= Limits.move_time) ||
        (Limits.nodes && nodes >= Limits.nodes))
    {
        Signals.stop = true;
    }

}

// Thread::idle_loop () is where the thread is parked when it has no work to do
void Thread::idle_loop ()
{
    // Pointer 'this_sp' is not null only if we are called from split(), and not
    // at the thread creation. So it means we are the split point's master.
    SplitPoint *this_sp = split_points_size ? active_split_point : NULL;

    ASSERT (!this_sp || (this_sp->master_thread == this && searching));

    while (true)
    {
        // If we are not searching, wait for a condition to be signaled instead of
        // wasting CPU time polling for work.
        while ((!searching && Threads.sleep_while_idle) || exit)
        {
            if (exit)
            {
                ASSERT (!this_sp);
                return;
            }

            // Grab the lock to avoid races with Thread::notify_one ()
            mutex.lock ();

            // If we are master and all slaves have finished then exit idle_loop
            if (this_sp && !this_sp->slaves_mask)
            {
                mutex.unlock ();
                break;
            }

            // Do sleep after retesting sleep conditions under lock protection, in
            // particular we need to avoid a deadlock in case a master thread has,
            // in the meanwhile, allocated us and sent the notify_one () call before
            // we had the chance to grab the lock.
            if (!searching && !exit) sleep_condition.wait (mutex);

            mutex.unlock ();
        }

        // If this thread has been assigned work, launch a search
        if (searching)
        {
            ASSERT (!exit);

            Threads.mutex.lock ();

            ASSERT (searching);
            ASSERT (active_split_point);
            SplitPoint *sp = active_split_point;

            Threads.mutex.unlock ();

            Stack stack[MAX_PLY_6], *ss = stack+2; // To allow referencing (ss-2)
            Position pos (*sp->pos, this);

            memcpy (ss-2, sp->ss-2, 5 * sizeof (Stack));
            ss->split_point = sp;

            sp->mutex.lock ();

            ASSERT (active_pos == NULL);

            active_pos = &pos;

            switch (sp->node_type)
            {
            case Root : search<SplitPointRoot>  (pos, ss, sp->alpha, sp->beta, sp->depth, sp->cut_node); break;
            case PV   : search<SplitPointPV>    (pos, ss, sp->alpha, sp->beta, sp->depth, sp->cut_node); break;
            case NonPV: search<SplitPointNonPV> (pos, ss, sp->alpha, sp->beta, sp->depth, sp->cut_node); break;
            default   : ASSERT (false);
            }

            ASSERT (searching);

            searching  = false;
            active_pos = NULL;
            sp->slaves_mask &= ~(1ULL << idx);
            sp->nodes += pos.game_nodes ();

            // Wake up master thread so to allow it to return from the idle loop
            // in case we are the last slave of the split point.
            if (Threads.sleep_while_idle &&
                this != sp->master_thread &&
                !sp->slaves_mask)
            {
                ASSERT (!sp->master_thread->searching);
                sp->master_thread->notify_one ();
            }

            // After releasing the lock we cannot access anymore any SplitPoint
            // related data in a safe way becuase it could have been released under
            // our feet by the sp master. Also accessing other Thread objects is
            // unsafe because if we are exiting there is a chance are already freed.
            sp->mutex.unlock ();
        }

        // If this thread is the master of a split point and all slaves have finished
        // their work at this split point, return from the idle loop.
        if (this_sp && !this_sp->slaves_mask)
        {
            this_sp->mutex.lock ();
            bool finished = !this_sp->slaves_mask; // Retest under lock protection
            this_sp->mutex.unlock ();

            if (finished) return;
        }
    }
}
