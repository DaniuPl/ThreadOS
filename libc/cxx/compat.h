/* The host's libstdc++ headers were configured for glibc; these are the
   few glibc names they expect that musl spells differently or lacks */
#define __GLIBC_PREREQ(a, b) 0
/* Threads are always there, as with glibc 2.34 and later. Otherwise
   libstdc++ asks a weak reference to pthread_cancel whether the program
   uses them - and a weak reference never brings a definition out of
   musl's libc.a, so the answer would be no: std::mutex would lock
   nothing and std::call_once would fail (bits/gthr-posix.h) */
#define _GLIBCXX_GTHREAD_USE_WEAK 0
#include <locale.h>
#include <pthread.h>
#include <time.h>
typedef locale_t __locale_t;
/* glibc's clock-taking timed waits. libstdc++ calls them with
   CLOCK_MONOTONIC and a steady_clock deadline - every wait_for and
   try_lock_for, and wait_until on steady_clock - while musl's timed
   calls read the deadline on CLOCK_REALTIME (a condition variable's
   clock, which libstdc++ leaves at the default). On ThreadOS the two
   clocks are decades apart, seconds since 1970 against ticks since
   boot, so a monotonic deadline passed through as it is has long gone
   and every such wait times out at once. It is moved onto
   CLOCK_REALTIME instead: the time left on clk, from now on the wall
   clock. A deadline musl would refuse is left for it to refuse. */
static inline const struct timespec *
__threados_realtime_deadline(clockid_t clk, const struct timespec *t,
                             struct timespec *rt)
{
    struct timespec now;
    if (clk == CLOCK_REALTIME || t->tv_nsec < 0 ||
        t->tv_nsec >= 1000000000L || clock_gettime(clk, &now) ||
        clock_gettime(CLOCK_REALTIME, rt))
        return t;
    /* Each nanosecond field is below a second, so their sum is within
       a second of the range and carries at most one either way */
    long nsec = rt->tv_nsec + t->tv_nsec - now.tv_nsec;
    time_t carry = 0, sec;
    if (nsec < 0) {
        nsec += 1000000000L;
        carry = -1;
    } else if (nsec >= 1000000000L) {
        nsec -= 1000000000L;
        carry = 1;
    }
    /* Both clocks read at or after 0, so only a deadline at either end
       of time_t overflows - and it stays at that end */
    if (__builtin_sub_overflow(t->tv_sec, now.tv_sec, &sec) ||
        __builtin_add_overflow(sec, rt->tv_sec + carry, &sec)) {
        sec = t->tv_sec < 0 ? 0 : (time_t)(~0ULL >> 1);
        nsec = 0;
    }
    rt->tv_sec = sec;
    rt->tv_nsec = nsec;
    return rt;
}
static inline int pthread_cond_clockwait(pthread_cond_t *c, pthread_mutex_t *m,
                                         clockid_t clk, const struct timespec *t)
{
    struct timespec rt;
    return pthread_cond_timedwait(c, m, __threados_realtime_deadline(clk, t, &rt));
}
static inline int pthread_mutex_clocklock(pthread_mutex_t *m, clockid_t clk,
                                          const struct timespec *t)
{
    struct timespec rt;
    return pthread_mutex_timedlock(m, __threados_realtime_deadline(clk, t, &rt));
}
/* std::shared_timed_mutex's try_lock_for and try_lock_shared_for */
static inline int pthread_rwlock_clockrdlock(pthread_rwlock_t *l, clockid_t clk,
                                             const struct timespec *t)
{
    struct timespec rt;
    return pthread_rwlock_timedrdlock(l, __threados_realtime_deadline(clk, t, &rt));
}
static inline int pthread_rwlock_clockwrlock(pthread_rwlock_t *l, clockid_t clk,
                                             const struct timespec *t)
{
    struct timespec rt;
    return pthread_rwlock_timedwrlock(l, __threados_realtime_deadline(clk, t, &rt));
}
/* The bits of glibc's character table, which libstdc++'s ctype<char>
   names its masks after (bits/ctype_base.h, pulled in by <locale>,
   <complex> and the streams). Only the values are needed to compile:
   the table itself is glibc's, and a program that classifies
   characters through std::ctype is not built here. */
enum {
    _ISupper = 0x100, _ISlower = 0x200, _ISalpha = 0x400, _ISdigit = 0x800,
    _ISxdigit = 0x1000, _ISspace = 0x2000, _ISprint = 0x4000,
    _ISgraph = 0x8000, _ISblank = 0x1, _IScntrl = 0x2, _ISpunct = 0x4,
    _ISalnum = 0x8
};
