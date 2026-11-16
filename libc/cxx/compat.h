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
static inline int pthread_cond_clockwait(pthread_cond_t *c, pthread_mutex_t *m,
                                         clockid_t clk, const struct timespec *t)
{
    (void)clk;
    return pthread_cond_timedwait(c, m, t);
}
static inline int pthread_mutex_clocklock(pthread_mutex_t *m, clockid_t clk,
                                          const struct timespec *t)
{
    (void)clk;
    return pthread_mutex_timedlock(m, t);
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
