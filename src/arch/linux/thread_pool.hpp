#ifndef __LINUX_THREAD_POOL_HPP__
#define __LINUX_THREAD_POOL_HPP__

#include <pthread.h>
#include "config/args.hpp"
#include "arch/linux/event_queue.hpp"
#include "arch/linux/disk.hpp"
#include "arch/linux/system_event.hpp"
#include "arch/linux/message_hub.hpp"
#include "arch/linux/coroutines.hpp"
#include "arch/linux/blocker_pool.hpp"
#include "arch/timer.hpp"

class linux_thread_message_t;
class linux_thread_t;

/* A thread pool represents a group of threads, each of which is associated with an
event queue. There is one thread pool per server. It is responsible for starting up
and shutting down the threads and event queues. */

class linux_thread_pool_t {
public:
    explicit linux_thread_pool_t(int n_threads);
    
    // When the process receives a SIGINT or SIGTERM, interrupt_message will be delivered to the
    // same thread that initial_message was delivered to, and interrupt_message will be set to
    // NULL. If you want to receive notification of further SIGINTs or SIGTERMs, you must call
    // set_interrupt_message() again. Returns the previous value of interrupt_message.
    static linux_thread_message_t *set_interrupt_message(linux_thread_message_t *interrupt_message);
    
    // Blocks while threads are working. Only returns after shutdown() is called. initial_message
    // is a thread message that will be delivered to one of the threads after all of the event queues
    // have been started; it is used to start the server's activity.
    void run(linux_thread_message_t *initial_message);
    
    // Shut down all the threads. Can be called from any thread.
    void shutdown();
    
    ~linux_thread_pool_t();

private:
    static void *start_thread(void*);
    
    static void interrupt_handler(int);
    static void sigsegv_handler(int, siginfo_t *, void *);
    pthread_spinlock_t interrupt_message_lock;
    linux_thread_message_t *interrupt_message;
    
    // Used to signal the main thread for shutdown
    volatile bool do_shutdown;
    pthread_cond_t shutdown_cond;
    pthread_mutex_t shutdown_cond_mutex;

    // The number of threads to allocate for handling blocking calls
    static const int GENERIC_BLOCKER_THREAD_COUNT = 2;
    blocker_pool_t* generic_blocker_pool;

    template<class T>
    struct generic_job_t :
        public blocker_pool_t::job_t
    {
        void run() {
            retval = fn();
        }

        void done() {
            // Now that the function is done, resume execution of the suspended task
            suspended->notify();
        }

        boost::function<T()> fn;
        coro_t* suspended;
        T retval;
    }; 

public:
    pthread_t pthreads[MAX_THREADS];
    linux_thread_t *threads[MAX_THREADS];

    // Cooperatively run a blocking function call using the generic_blocker_pool
    template<class T>
    static T run_in_blocker_pool(boost::function<T()>);
    
    int n_threads;
    // The thread_pool that started the thread we are currently in
    static __thread linux_thread_pool_t *thread_pool;
    // The ID of the thread we are currently in
    static __thread int thread_id;
    // The event queue for the thread we are currently in (same as &thread_pool->threads[thread_id])
    static __thread linux_thread_t *thread;
};

// Function to handle blocking calls in a separate thread pool
// This should be used for any calls that cannot otherwise be made non-blocking
template<class T>
T linux_thread_pool_t::run_in_blocker_pool(boost::function<T()> fn)
{
    generic_job_t<T> job;
    job.fn = fn;
    job.suspended = coro_t::self();

    rassert(thread_pool->generic_blocker_pool != NULL,
            "thread_pool_t::run_in_blocker_pool called while generic_thread_pool uninitialized");
    thread_pool->generic_blocker_pool->do_job(&job);

    // Give up execution, to be resumed when the done callback is made
    coro_t::wait();

    return job.retval;
}

class linux_thread_t :
    public linux_event_callback_t,
    public linux_queue_parent_t
{
    timer_token_t *perfmon_stats_timer;
    
public:
    linux_thread_t(linux_thread_pool_t *parent_pool, int thread_id);
    ~linux_thread_t();
    
    linux_event_queue_t queue;
    linux_message_hub_t message_hub;
    timer_handler_t timer_handler;
    
    /* Never accessed; its constructor and destructor set up and tear down thread-local variables
    for coroutines. */
    coro_globals_t coro_globals;
    
    void pump();   // Called by the event queue
    bool should_shut_down();   // Called by the event queue
    void initiate_shut_down(); // Can be called from any thread
    void on_event(int events);

private:
    volatile bool do_shutdown;
    pthread_mutex_t do_shutdown_mutex;
    system_event_t shutdown_notify_event;
};

#endif /* __LINUX_THREAD_POOL_HPP__ */
