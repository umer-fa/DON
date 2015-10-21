#ifndef _THREAD_H_INC_
#define _THREAD_H_INC_

#include <atomic>
#include <bitset>
#include <thread>
#include <vector>

#include "thread_win32.h"

#include "Position.h"
#include "Pawns.h"
#include "Material.h"
#include "MovePicker.h"
#include "Searcher.h"

namespace Threading {

    using namespace Searcher;
    using namespace MovePick;

    const u16 MAX_THREADS               = 128; // Maximum Threads

    // ThreadBase class is the base of the hierarchy from where
    // derive all the specialized thread classes.
    class ThreadBase
        : public std::thread
    {
    public:
        Mutex               mutex;
        ConditionVariable   sleep_condition;
        volatile bool       alive = true;

        virtual ~ThreadBase() = default;

        void notify_one ();
        void wait_for   (volatile const bool &condition);
        void wait_while (volatile const bool &condition);

        virtual void idle_loop () = 0;
    };

    // Thread struct keeps together all the thread related stuff like locks, state
    // and especially split points. We also use per-thread pawn and material hash
    // tables so that once we get a pointer to an entry its life time is unlimited
    // and we don't have to care about someone changing the entry under our feet.
    class Thread
        : public ThreadBase
    {
    public:
        Pawns   ::Table pawn_table;
        Material::Table matl_table;

        size_t  index, pv_index;
        i32     max_ply = 0;

        Position       root_pos;
        RootMoveVector root_moves;
        Stack       stack[MAX_DEPTH+4];
        ValueStats  history_value;
        MoveStats   counter_moves;
        Depth       depth;

        volatile bool searching = false;

        Thread ();
        
        void idle_loop () override;
        void search (bool is_main_thread = false);

    };

    // MainThread struct is derived struct used for the main one
    class MainThread
        : public Thread
    {
    public:
        volatile bool thinking = true; // Avoid a race with start_thinking()
       
        void idle_loop () override;
        void join ();
        void think ();
    };

    const i32 TIMER_RESOLUTION = 5; // Millisec between two check_time() calls

    // TimerThread struct is derived struct used for the recurring timer.
    class TimerThread
        : public ThreadBase
    {
    private:
        bool _running = false;

    public:
        
        i32 resolution; // Millisec between two task() calls
        void (*task) () = nullptr;
        
        void start () { _running = true ; }
        void stop  () { _running = false; }

        void idle_loop () override;

    };

    // ThreadPool struct handles all the threads related stuff like
    // - initializing
    // - starting
    // - parking
    // - launching a slave thread at a split point (most important).
    // All the access to shared thread data is done through this.
    class ThreadPool
        : public std::vector<Thread*>
    {
    public:

        TimerThread *check_limits_th = nullptr;
        TimerThread *save_hash_th    = nullptr;
        
        MainThread* main () const { return static_cast<MainThread*> (at (0)); }

        // No c'tor and d'tor, threadpool rely on globals that should
        // be initialized and valid during the whole thread lifetime.
        void initialize ();
        void exit ();

        void start_main (const Position &pos, const LimitsT &limit, StateStackPtr &states);
        u64  game_nodes ();

        void configure ();

    };

    template<class T>
    extern T* new_thread ();

    extern void delete_thread (ThreadBase *th);

    extern void check_limits ();
    extern void save_hash ();

}

enum SyncT { IO_LOCK, IO_UNLOCK };

// Used to serialize access to std::cout to avoid multiple threads writing at the same time.
inline std::ostream& operator<< (std::ostream &os, SyncT sync)
{
    static Mutex io_mutex;

    if (sync == IO_LOCK)
        io_mutex.lock ();
    else
    if (sync == IO_UNLOCK)
        io_mutex.unlock ();

    return os;
}

#define sync_cout std::cout << IO_LOCK
#define sync_endl std::endl << IO_UNLOCK


extern Threading::ThreadPool  Threadpool;

#endif // _THREAD_H_INC_
