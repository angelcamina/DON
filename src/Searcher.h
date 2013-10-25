//#pragma once
#ifndef SEARCHER_H_
#define SEARCHER_H_

#include "Time.h"
#include <iomanip>

#include "Move.h"
#include "Position.h"

class PolyglotBook;

namespace Searcher {

    const uint16_t MAX_DEPTH = 64;
    const uint16_t MAX_MOVES = 192;
    const uint16_t MAX_PLY   = 100;
    const uint16_t MAX_PLY_2 = MAX_PLY + 2;

    const uint16_t FAIL_LOW_MARGIN = 50;        // => 20
    const uint16_t FUTILITY_CUT_LIMIT_PCT = 60; // => 60
    const uint16_t MAX_THREAT = 90;

    extern PolyglotBook book;

    // GameClock stores the available time and time-gain per move
    typedef struct GameClock
    {
        // unit: milli-seconds
        int32_t time;   // time left
        int32_t inc;    // time gain

        GameClock ()
        {
            time  = 5 * 60 * 1000; // 5 mins default time
            inc   = 0;
        }

        GameClock (int32_t t, int32_t i)
        {
            time  = t;
            inc   = i;
        }

    } GameClock;

    // Limits stores information sent by GUI about available time to search the current move.
    //  - Maximum time and increment.
    //  - Maximum depth.
    //  - Maximum nodes.
    //  - Search move list
    //  - if in analysis mode.
    //  - if have to ponder while is opponent's side to move.
    typedef struct Limits
    {
        GameClock game_clock[CLR_NO];

        uint32_t  move_time;      // search <x> time in milli-seconds
        uint8_t   moves_to_go;    // search <x> moves to the next time control
        uint8_t   depth;          // search <x> depth (plies) only
        uint16_t  nodes;          // search <x> nodes only
        uint8_t   mate_in;        // search mate in <x> moves
        MoveList  search_moves;   // search these moves only restrict
        bool      infinite;       // search until the "stop" command
        bool      ponder;         // search on ponder move

        Limits() { std::memset (this, 0, sizeof (Limits)); }

        bool use_time_management () const
        {
            return !(infinite || mate_in || move_time || depth || nodes);
        }

        // Determines how much time it should search
        int time_to_search ()
        {
            //int cpuTime = board->turn == WHITE ? wTime : bTime;
            //int humanTime = board->turn == WHITE ? bTime : wTime;

            //int cpuInc = board->turn == WHITE ? wInc : bInc;

            //if (moves_to_go > 0)
            //{
            //  return cpuTime / moves_to_go + cpuInc / 2 + (cpuTime - humanTime) / 2;
            //}

            //return cpuTime / 30 + cpuInc / 2;
        }

    } Limits;

    // Signals stores volatile flags updated during the search sent by the GUI
    // typically in an async fashion.
    //  - Stop search (to stop the search by the GUI).
    //  - Stop on ponderhit.
    //  - On first root move.
    //  - Falied low at root.
    typedef struct Signals
    {
        bool stop;
        bool stop_on_ponderhit;
        bool first_root_move;
        bool failed_low_at_root;

        Signals() { std::memset (this, 0, sizeof (Signals)); }

    } Signals;

    // RootMove is used for moves at the root of the tree.
    // For each root move stores:
    //  - Current value.
    //  - Last value.
    //  - Node count.
    //  - PV (really a refutation table in the case of moves which fail low).
    // Score is normally set at -VALUE_INFINITE for all non-pv moves.
    struct RootMove
    {
        Value curr_value;
        Value last_value;
        //uint64_t nodes;

        MoveList pv;

        RootMove(Move m)
            : curr_value(-VALUE_INFINITE)
            , last_value(-VALUE_INFINITE)
        {
            pv.emplace_back (m);
            pv.emplace_back (MOVE_NONE);
        }

        // Ascending Sort
        bool operator< (const RootMove &rm) const { return curr_value > rm.curr_value; }
        bool operator== (const Move &m) const { return m == pv[0]; }

        void extract_pv_from_tt (Position &pos);
        void insert_pv_into_tt (Position &pos);

    };

    extern Limits                limits;
    extern volatile Signals      signals;

    extern std::vector<RootMove> rootMoves;
    extern Position              rootPos;
    extern Color                 rootColor;
    extern StateInfoStackPtr     setupStates;

    extern Time::point           searchTime;


    extern void think ();


    //extern int nThreads;
    //extern int maxThreadsPerNode;

    ////typedef struct Node
    ////{
    ////    //Node* parent;
    ////    //Node* children[maxThreads];
    ////    Position pos;
    ////
    ////    //volatile bool _stopped;
    ////    //volatile int workers;
    ////    //bool used;
    ////    //int threadNumber;
    ////    //pthread_mutex_t mutex;
    ////    //Variation pv;
    ////    //int nodes;
    ////    //int qNodes;
    ////    //int failHighs;
    ////    //int failHighFirsts;
    ////    //int transRefProbes;
    ////    //int transRefHits;
    ////    //Move killer1[maxPly],killer2[maxPly];
    ////    //Score pValue;
    ////    //Score cValue;
    ////    //Score alpha;
    ////    //Score beta;
    ////    //Depth depth;
    ////    //int ply;
    ////    //Depth totalExtension;
    ////    //Moves moves[maxPly];
    ////    //int nextMove[maxPly];
    ////    //Move bestMove;
    ////
    ////} Node;
    ////
    ////
    ////extern void Think(Node &rootNode);
    ////
    ////Score search(Node &node, Score alpha, Score beta, Depth depth, int ply, bool nullMoveIsOK, int8_t totalExtension);

    //extern void search(const Position &pos, Depth depth);

}

//inline std::ostream& operator<< (std::ostream &os, const Score &score)
//{
//    if (abs (int16_t (score)) < VALUE_INFINITE - 300)
//    {
//        os.setf (std::ios_base::fixed, std::ios_base::floatfield);
//        os.setf (std::ios_base::right, std::ios_base::adjustfield);
//        os << std::setiosflags (std::ios_base::showpos);
//        os << std::setw (4) << std::setprecision (3) << (double) (score) / 1000;
//        os.unsetf (std::ios_base::showpos);
//    }
//    else
//    {
//        os << ((score > 0) ? "+" : "-");
//        int32_t value = VALUE_INFINITE - abs (int16_t (score));
//        os << "MAT" << value;
//        if (value < 10)
//        {
//            os << " ";
//        }
//    }
//    return os;
//}

#endif // SEARCHER_H_
