// The timed waits of C++'s thread library, each timed on steady_clock:
// condition variables, timed mutexes and shared timed mutexes through
// wait_for, wait_until and try_lock_for, and glibc's clock-taking calls
// beneath them called straight. libstdc++ gives those calls a deadline
// on CLOCK_MONOTONIC, musl reads deadlines on CLOCK_REALTIME, and on
// ThreadOS the two clocks are decades apart - so a deadline that is
// not moved from one to the other (libc/cxx/compat.h) has passed
// before the wait begins, and every wait returns at once.
//
// Each line is "what: outcome (N ms)". scripts/tests/t_clockwait.py
// holds the outcomes to those of this file built against glibc on the
// host, and the times to what was asked for.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

using namespace std::chrono;
using namespace std::chrono_literals;

static steady_clock::time_point t0;

static void start()
{
    t0 = steady_clock::now();
}

static long ms_since_start()
{
    return (long)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

static void report(const char *what, const char *outcome)
{
    long ms = ms_since_start();
    printf("%s: %s (%ld ms)\n", what, outcome, ms);
    fflush(stdout);
}

static const char *err_name(int e)
{
    switch (e) {
    case 0: return "0";
    case ETIMEDOUT: return "ETIMEDOUT";
    case EINVAL: return "EINVAL";
    case EBUSY: return "EBUSY";
    }
    return "another error";
}

// A deadline on clk, ms from now (negative: already gone)
static struct timespec deadline(clockid_t clk, long ms)
{
    struct timespec t;
    clock_gettime(clk, &t);
    long long ns = (long long)t.tv_nsec + ms * 1000000LL;
    t.tv_sec += ns / 1000000000LL;
    t.tv_nsec = ns % 1000000000LL;
    if (t.tv_nsec < 0) {
        t.tv_nsec += 1000000000L;
        t.tv_sec--;
    }
    return t;
}

static void condition_variables()
{
    std::mutex m;
    std::condition_variable cv;

    // Nobody notifies: the predicate keeps a spurious wakeup from
    // ending the wait early, so only the deadline can
    {
        std::unique_lock<std::mutex> lk(m);
        start();
        bool r = cv.wait_for(lk, 300ms, [] { return false; });
        report("condition_variable wait_for 300 ms", r ? "woken" : "timeout");
    }
    {
        std::unique_lock<std::mutex> lk(m);
        start();
        bool r = cv.wait_until(lk, steady_clock::now() + 300ms,
                               [] { return false; });
        report("condition_variable wait_until steady_clock +300 ms",
               r ? "woken" : "timeout");
    }

    // Notified before the deadline: the wait ends when it is told to
    bool flag = false;
    start();
    std::thread n([&] {
        std::this_thread::sleep_for(100ms);
        {
            std::lock_guard<std::mutex> g(m);
            flag = true;
        }
        cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> lk(m);
        bool r = cv.wait_for(lk, 5s, [&] { return flag; });
        report("condition_variable wait_for 5 s, notified at 100 ms",
               r ? "woken" : "timeout");
    }
    n.join();

    // A worker that sleeps between looks at its queue: about ten
    // wakeups a second, not a loop that never sleeps
    {
        std::unique_lock<std::mutex> lk(m);
        long wakeups = 0;
        start();
        while (ms_since_start() < 1000) {
            cv.wait_for(lk, 100ms);
            wakeups++;
        }
        char what[64];
        snprintf(what, sizeof what, "%ld wakeups", wakeups);
        report("worker in wait_for 100 ms steps for 1 s", what);
    }
}

static void timed_mutexes()
{
    std::timed_mutex tm;

    tm.lock();
    std::thread a([&] {
        start();
        bool r = tm.try_lock_for(300ms);
        report("timed_mutex try_lock_for 300 ms, held",
               r ? "taken" : "not taken");
        if (r)
            tm.unlock();
    });
    a.join();

    std::thread b([&] {
        start();
        bool r = tm.try_lock_until(steady_clock::now() + 300ms);
        report("timed_mutex try_lock_until steady_clock +300 ms, held",
               r ? "taken" : "not taken");
        if (r)
            tm.unlock();
    });
    b.join();

    start();
    std::thread c([&] {
        bool r = tm.try_lock_for(5s);
        report("timed_mutex try_lock_for 5 s, released at 100 ms",
               r ? "taken" : "not taken");
        if (r)
            tm.unlock();
    });
    std::this_thread::sleep_for(100ms);
    tm.unlock();
    c.join();
}

static void shared_timed_mutexes()
{
    std::shared_timed_mutex sm;
    auto attempt = [&](const char *what, bool shared, auto wait) {
        std::thread t([&] {
            start();
            bool r = shared ? sm.try_lock_shared_for(wait)
                            : sm.try_lock_for(wait);
            report(what, r ? "taken" : "not taken");
            if (r) {
                if (shared)
                    sm.unlock_shared();
                else
                    sm.unlock();
            }
        });
        t.join();
    };

    sm.lock();
    attempt("shared_timed_mutex try_lock_shared_for 300 ms, written",
            true, 300ms);
    attempt("shared_timed_mutex try_lock_for 300 ms, written", false, 300ms);
    sm.unlock();

    sm.lock_shared();
    attempt("shared_timed_mutex try_lock_for 300 ms, read", false, 300ms);
    attempt("shared_timed_mutex try_lock_shared_for 300 ms, read",
            true, 300ms);
    sm.unlock_shared();

    sm.lock();
    start();
    std::thread w([&] {
        bool r = sm.try_lock_shared_for(5s);
        report("shared_timed_mutex try_lock_shared_for 5 s, "
               "released at 100 ms", r ? "taken" : "not taken");
        if (r)
            sm.unlock_shared();
    });
    std::this_thread::sleep_for(100ms);
    sm.unlock();
    w.join();
}

// glibc's calls themselves, on the clocks and deadlines libstdc++
// never passes
static void clock_calls()
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec t;
    int r;

    pthread_mutex_lock(&m);
    t = deadline(CLOCK_REALTIME, 300);
    start();
    do
        r = pthread_cond_clockwait(&c, &m, CLOCK_REALTIME, &t);
    while (r == 0);
    report("pthread_cond_clockwait CLOCK_REALTIME +300 ms", err_name(r));

    t = deadline(CLOCK_MONOTONIC, -1000);
    start();
    do
        r = pthread_cond_clockwait(&c, &m, CLOCK_MONOTONIC, &t);
    while (r == 0);
    report("pthread_cond_clockwait CLOCK_MONOTONIC -1 s", err_name(r));

    t = deadline(CLOCK_MONOTONIC, 300);
    t.tv_nsec = 1000000000L;
    start();
    r = pthread_cond_clockwait(&c, &m, CLOCK_MONOTONIC, &t);
    report("pthread_cond_clockwait CLOCK_MONOTONIC, tv_nsec 1000000000",
           err_name(r));
    pthread_mutex_unlock(&m);

    // A deadline at the end of time_t: there is nothing to add to it
    // without overflowing, and it must not come out in the past
    bool flag = false;
    t.tv_sec = (time_t)(~0ULL >> 1);
    t.tv_nsec = 0;
    start();
    std::thread n([&] {
        std::this_thread::sleep_for(100ms);
        pthread_mutex_lock(&m);
        flag = true;
        pthread_cond_signal(&c);
        pthread_mutex_unlock(&m);
    });
    pthread_mutex_lock(&m);
    r = 0;
    while (!flag && r == 0)
        r = pthread_cond_clockwait(&c, &m, CLOCK_MONOTONIC, &t);
    pthread_mutex_unlock(&m);
    report("pthread_cond_clockwait CLOCK_MONOTONIC at the end of time, "
           "signalled at 100 ms", err_name(r));
    n.join();

    pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&held);
    std::thread l([&] {
        struct timespec d = deadline(CLOCK_MONOTONIC, 300);
        start();
        int e = pthread_mutex_clocklock(&held, CLOCK_MONOTONIC, &d);
        report("pthread_mutex_clocklock CLOCK_MONOTONIC +300 ms, held",
               err_name(e));
        if (!e)
            pthread_mutex_unlock(&held);
    });
    l.join();
    pthread_mutex_unlock(&held);

    pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
    pthread_rwlock_wrlock(&rw);
    std::thread k([&] {
        struct timespec d = deadline(CLOCK_MONOTONIC, 300);
        start();
        int e = pthread_rwlock_clockrdlock(&rw, CLOCK_MONOTONIC, &d);
        report("pthread_rwlock_clockrdlock CLOCK_MONOTONIC +300 ms, written",
               err_name(e));
        if (!e)
            pthread_rwlock_unlock(&rw);
        d = deadline(CLOCK_MONOTONIC, 300);
        start();
        e = pthread_rwlock_clockwrlock(&rw, CLOCK_MONOTONIC, &d);
        report("pthread_rwlock_clockwrlock CLOCK_MONOTONIC +300 ms, written",
               err_name(e));
        if (!e)
            pthread_rwlock_unlock(&rw);
    });
    k.join();
    pthread_rwlock_unlock(&rw);
}

int main()
{
    condition_variables();
    timed_mutexes();
    shared_timed_mutexes();
    clock_calls();
    printf("done\n");
    return 0;
}
