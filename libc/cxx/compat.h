/* The host's libstdc++ headers were configured for glibc; these are the
   few glibc names they expect that musl spells differently or lacks */
#define __GLIBC_PREREQ(a, b) 0
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
