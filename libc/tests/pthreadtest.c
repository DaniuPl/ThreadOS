/* POSIX threads, as a library uses them
 *
 * Plain portable C: the test builds it again on the host against glibc
 * and the two outputs have to be the same, line for line. So nothing
 * printed here may depend on timing or on which thread came first -
 * only on whether the primitives did their job. Lines starting with '~'
 * are this machine's own (how many cores, how long) and are not
 * compared.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* --- A mutex, and atomics, under contention ------------------------ */

#define WORKERS 8
#define ROUNDS  20000

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static atomic_long atomic_counter;

static void *count_up(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&counter_lock);
        /* read, think, write: without the lock, increments get lost */
        long v = counter;
        if ((i & 255) == 0)
            sched_yield();
        counter = v + 1;
        pthread_mutex_unlock(&counter_lock);
        atomic_fetch_add(&atomic_counter, 1);
    }
    return NULL;
}

static void mutex_test(void)
{
    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        pthread_create(&t[i], NULL, count_up, NULL);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(t[i], NULL);
    printf("mutex: %ld of %d\n", counter, WORKERS * ROUNDS);
    printf("atomic: %ld of %d\n", (long)atomic_counter, WORKERS * ROUNDS);
}

/* --- Computing side by side ------------------------------------------
 *
 * Which cores the threads ran on depends on the machine, so it is a '~'
 * line; the test reads it on a machine with four cores. */

static atomic_uint cores_seen;

static void *compute(void *arg)
{
    volatile unsigned long x = (unsigned long)arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (ms_since(&t0) < 300) {
        for (int i = 0; i < 100000; i++)
            x = x * 6364136223846793005UL + 1;
        int c = sched_getcpu();
        if (c >= 0 && c < 32)
            atomic_fetch_or(&cores_seen, 1u << c);
    }
    return NULL;
}

static void spread_test(void)
{
    pthread_t t[4];
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, compute, (void *)i);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    printf("~ran on %d core(s)\n", __builtin_popcount(atomic_load(&cores_seen)));
}

/* --- What a thread returns, and its arguments ---------------------- */

static void *square(void *arg)
{
    long n = (long)arg;
    return (void *)(n * n);
}

static void join_test(void)
{
    pthread_t t[10];
    long sum = 0;
    for (long i = 0; i < 10; i++)
        pthread_create(&t[i], NULL, square, (void *)(i + 1));
    for (int i = 0; i < 10; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    printf("join: sum of squares 1..10 = %ld\n", sum);
}

/* --- Thread-local storage ------------------------------------------ */

static __thread int tls_value = 7;
static __thread char tls_name[16] = "main";

static void *tls_worker(void *arg)
{
    long id = (long)arg;
    int fresh = tls_value == 7 && !strcmp(tls_name, "main");
    tls_value = (int)id * 100;
    snprintf(tls_name, sizeof(tls_name), "thread%ld", id);
    for (int i = 0; i < 50; i++)
        sched_yield();
    char want[16];
    snprintf(want, sizeof(want), "thread%ld", id);
    return (void *)(long)(fresh && tls_value == id * 100 &&
                          !strcmp(tls_name, want));
}

static void tls_test(void)
{
    pthread_t t[6];
    int good = 0;
    for (long i = 0; i < 6; i++)
        pthread_create(&t[i], NULL, tls_worker, (void *)i);
    for (int i = 0; i < 6; i++) {
        void *r;
        pthread_join(t[i], &r);
        good += (int)(long)r;
    }
    printf("tls: %d of 6 threads saw their own copy, main still %d %s\n",
           good, tls_value, tls_name);
}

/* --- A bounded queue: condition variables -------------------------- */

#define SLOTS 4
#define ITEMS 500

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_not_full = PTHREAD_COND_INITIALIZER;
static int queue[SLOTS], q_head, q_count, q_producers_left;
static long consumed_sum, consumed_n;

static void *producer(void *arg)
{
    long base = (long)arg;
    for (int i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&q_lock);
        while (q_count == SLOTS)
            pthread_cond_wait(&q_not_full, &q_lock);
        queue[(q_head + q_count++) % SLOTS] = (int)(base + i);
        pthread_cond_signal(&q_not_empty);
        pthread_mutex_unlock(&q_lock);
    }
    pthread_mutex_lock(&q_lock);
    q_producers_left--;
    pthread_cond_broadcast(&q_not_empty);
    pthread_mutex_unlock(&q_lock);
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_count && q_producers_left)
            pthread_cond_wait(&q_not_empty, &q_lock);
        if (!q_count) {
            pthread_mutex_unlock(&q_lock);
            return NULL;
        }
        int v = queue[q_head];
        q_head = (q_head + 1) % SLOTS;
        q_count--;
        consumed_sum += v;
        consumed_n++;
        pthread_cond_signal(&q_not_full);
        pthread_mutex_unlock(&q_lock);
    }
}

static void queue_test(void)
{
    pthread_t p[2], c[3];
    q_producers_left = 2;
    pthread_create(&p[0], NULL, producer, (void *)0L);
    pthread_create(&p[1], NULL, producer, (void *)10000L);
    for (int i = 0; i < 3; i++)
        pthread_create(&c[i], NULL, consumer, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(p[i], NULL);
    for (int i = 0; i < 3; i++)
        pthread_join(c[i], NULL);
    printf("queue: %ld items, sum %ld\n", consumed_n, consumed_sum);
}

/* --- Semaphores: two threads taking turns -------------------------- */

static sem_t ping, pong;
static char turns[41];

static void *ponger(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20; i++) {
        sem_wait(&ping);
        turns[2 * i + 1] = 'o';
        sem_post(&pong);
    }
    return NULL;
}

static void sem_test(void)
{
    pthread_t t;
    sem_init(&ping, 0, 0);
    sem_init(&pong, 0, 0);
    pthread_create(&t, NULL, ponger, NULL);
    for (int i = 0; i < 20; i++) {
        turns[2 * i] = 'i';
        sem_post(&ping);
        sem_wait(&pong);
    }
    pthread_join(t, NULL);
    printf("semaphores: %s\n", turns);
}

/* --- pthread_once, a barrier, a read-write lock -------------------- */

static pthread_once_t once = PTHREAD_ONCE_INIT;
static atomic_int once_runs;
static pthread_barrier_t barrier;
static atomic_int arrived[3];
static int barrier_ok = 1;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static int rw_a, rw_b, rw_torn;

static void init_once(void)
{
    atomic_fetch_add(&once_runs, 1);
}

static void *together(void *arg)
{
    long id = (long)arg;
    pthread_once(&once, init_once);
    for (int round = 0; round < 3; round++) {
        atomic_fetch_add(&arrived[round], 1);
        pthread_barrier_wait(&barrier);
        /* past the barrier, everyone has arrived at this round */
        if (atomic_load(&arrived[round]) != 4)
            barrier_ok = 0;
    }
    for (int i = 0; i < 2000; i++) {
        if (id == 0) {
            pthread_rwlock_wrlock(&rw);
            rw_a++;
            sched_yield();
            rw_b++;
            pthread_rwlock_unlock(&rw);
        } else {
            pthread_rwlock_rdlock(&rw);
            if (rw_a != rw_b)
                rw_torn++;
            pthread_rwlock_unlock(&rw);
        }
    }
    return NULL;
}

static void sync_test(void)
{
    pthread_t t[4];
    pthread_barrier_init(&barrier, NULL, 4);
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, together, (void *)i);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    printf("once: ran %d time(s)\n", (int)once_runs);
    printf("barrier: %s\n", barrier_ok ? "nobody went ahead" : "BROKEN");
    printf("rwlock: %d writes, %d torn reads\n", rw_a, rw_torn);
}

/* --- Many threads, one after another, and detached ones ------------ */

static atomic_int detached_done;

static void *detached(void *arg)
{
    (void)arg;
    char big[32 * 1024];               /* uses the stack it was given */
    memset(big, 1, sizeof(big));
    atomic_fetch_add(&detached_done, big[100] + big[30000] - 1);
    return NULL;
}

static void *nothing(void *arg)
{
    return arg;
}

static void lifetime_test(void)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < 40; i++) {
        pthread_t t;
        if (pthread_create(&t, &a, detached, NULL))
            printf("detached thread %d not created\n", i);
    }
    for (int w = 0; atomic_load(&detached_done) < 40 && w < 2000; w++)
        usleep(10000);
    printf("detached: %d of 40 finished\n", (int)detached_done);

    int ok = 0;
    for (long i = 0; i < 300; i++) {
        pthread_t t;
        void *r = NULL;
        if (pthread_create(&t, NULL, nothing, (void *)i))
            break;
        pthread_join(t, &r);
        ok += (long)r == i;
    }
    printf("created and joined one after another: %d of 300\n", ok);
}

/* --- One process: files and its number ----------------------------- */

static int shared_fd = -1;
static char thread_read[32];
static pid_t pid_in_thread;

static void *file_worker(void *arg)
{
    (void)arg;
    pid_in_thread = getpid();
    /* a descriptor main opened... */
    ssize_t n = read(shared_fd, thread_read, sizeof(thread_read) - 1);
    if (n > 0)
        thread_read[n] = 0;
    /* ...and a file opened here, for main to read once we are gone */
    int fd = open("ptout.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return (void *)-1L;
    if (write(fd, "written by a thread\n", 20) != 20)
        return (void *)-1L;
    return (void *)(long)fd;
}

static void process_test(void)
{
    int fd = open("ptin.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (write(fd, "opened by main", 14) != 14)
        printf("cannot write ptin.txt\n");
    close(fd);
    shared_fd = open("ptin.txt", O_RDONLY);

    pthread_t t;
    void *r;
    pthread_create(&t, NULL, file_worker, NULL);
    pthread_join(t, &r);
    int thread_fd = (int)(long)r;
    printf("a thread reads main's descriptor: '%s'\n", thread_read);
    printf("getpid in a thread is the process's: %s\n",
           pid_in_thread == getpid() ? "yes" : "no");

    /* the thread's descriptor is still open: it belongs to the process */
    printf("the thread's descriptor outlives it: %s\n",
           thread_fd >= 0 && close(thread_fd) == 0 ? "yes" : "no");
    char buf[64] = {0};
    fd = open("ptout.txt", O_RDONLY);
    if (read(fd, buf, sizeof(buf) - 1) < 0)
        printf("cannot read ptout.txt\n");
    close(fd);
    close(shared_fd);
    printf("main reads the thread's file: %s", buf);
    unlink("ptin.txt");
    unlink("ptout.txt");
}

/* --- A timed wait -------------------------------------------------- */

static void timed_test(void)
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec t0, until;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 300 * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&m);
    int rc = pthread_cond_timedwait(&c, &m, &until);
    pthread_mutex_unlock(&m);
    long ms = ms_since(&t0);
    printf("timedwait: %s, %s\n", rc == ETIMEDOUT ? "ETIMEDOUT" : strerror(rc),
           ms >= 250 && ms < 3000 ? "after about 300 ms" : "at the wrong time");
    printf("~timedwait took %ld ms\n", ms);
}

/* --- exit() from a thread ends the whole program ------------------- */

static void *spin_forever(void *arg)
{
    volatile long *n = arg;
    for (;;)
        (*n)++;
    return NULL;
}

static void *sleep_forever(void *arg)
{
    (void)arg;
    for (;;)
        sleep(1);
    return NULL;
}

static void *exit_five(void *arg)
{
    (void)arg;
    usleep(100000);
    exit(5);
}

static void exit_test(void)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        static long spins;
        pthread_t a, b, c;
        pthread_create(&a, NULL, spin_forever, &spins);
        pthread_create(&b, NULL, sleep_forever, NULL);
        pthread_create(&c, NULL, exit_five, NULL);
        pthread_join(a, NULL);         /* never returns by itself */
        _exit(1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    printf("exit(5) in a thread: the program ends with %d\n",
           WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

int main(void)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* Never joined: when main returns, exit() has to end it too (the
       test then looks for threads left behind) */
    pthread_t sleeper;
    pthread_create(&sleeper, NULL, sleep_forever, NULL);
    pthread_detach(sleeper);
    printf("~cores %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "mutex", mutex_test }, { "join", join_test }, { "tls", tls_test },
        { "queue", queue_test }, { "sem", sem_test }, { "sync", sync_test },
        { "lifetime", lifetime_test }, { "process", process_test },
        { "timed", timed_test }, { "exit", exit_test },
        { "spread", spread_test },
    };
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        tests[i].fn();
        printf("~%s %ld ms\n", tests[i].name, ms_since(&t));
    }
    printf("~took %ld ms\n", ms_since(&t0));
    printf("done\n");
    return 0;
}
