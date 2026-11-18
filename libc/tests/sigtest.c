/* sigtest: signals as a program sees them
 *
 * Built against musl for the ARM64 kernel and against glibc on the
 * machine the tests run on, and run on both: every line it prints has
 * to be the same (scripts/tests/t_arm64libc.py). What it does:
 *
 *   raise and kill to itself, with an SA_SIGINFO handler: the signal,
 *   si_code (SI_TKILL, SI_USER) and si_pid as Linux has them; the
 *   signal and sa_mask held back inside the handler, not after it
 *
 *   a signal held back waits, and is handled once when let through
 *
 *   pthread_kill to a thread computing: the handler runs on it, and the
 *   ucontext_t it gets has the registers it was interrupted with - one
 *   it holds a known value in, and its stack pointer, in its stack -
 *   and the FP/SIMD register the handler changed is as it was after
 *
 *   WebKit's way of stopping a thread to look at its stack
 *   (Source/WTF/wtf/posix/ThreadingPOSIX.cpp): a signal whose handler
 *   posts a semaphore and waits in sigsuspend, a second one to let it
 *   go on - fifty times, the thread counting; while stopped its count
 *   stands still and the stack pointer in its frame is in its stack
 *
 *   a thread waiting in read on a pipe, and one on a condition
 *   variable, takes a handler (SA_RESTART) and goes on waiting
 *
 *   sigsuspend ends with EINTR after the handler, the old mask back
 *
 *   kill to the program while this thread holds the signal back: the
 *   other thread takes it
 *
 *   a handler interrupted by another signal; SA_RESETHAND; a forked
 *   child keeps the handler; waitpid goes on after a handler
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAGIC  0x0123456789ABCDEFUL
#define FMAGIC 0x400921FB54442D18UL      /* pi, as a double */

static void ms_sleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) && errno == EINTR)
		;
}

static int wait_for(volatile int *flag, int ms)
{
	for (int i = 0; i < ms && !__atomic_load_n(flag, __ATOMIC_ACQUIRE); i++)
		ms_sleep(1);
	return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

static const char *code_name(int code)
{
	return code == SI_TKILL ? "SI_TKILL" : code == SI_USER ? "SI_USER" :
	       "other";
}

static void handle(int sig, void (*fn)(int, siginfo_t *, void *), int extra,
                   int flags)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fn;
	sa.sa_flags = SA_SIGINFO | flags;
	sigemptyset(&sa.sa_mask);
	if (extra)
		sigaddset(&sa.sa_mask, extra);
	sigaction(sig, &sa, 0);
}

static int held(int sig)
{
	sigset_t now;
	pthread_sigmask(SIG_SETMASK, 0, &now);
	return sigismember(&now, sig);
}

/* --- raise, kill, the mask in the handler ------------------------------- */

static volatile int got_count, got_sig, got_code, got_pid, in_self, in_extra;

static void info_handler(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	got_sig = sig;
	got_code = si->si_code;
	got_pid = si->si_pid;
	in_self = held(sig);
	in_extra = held(SIGUSR2);
	got_count++;
}

static void to_itself(void)
{
	handle(SIGUSR1, info_handler, SIGUSR2, SA_RESTART);
	raise(SIGUSR1);
	printf("raise: ran %d, signal %d, %s, from this program %d\n",
	       got_count, got_sig, code_name(got_code), got_pid == getpid());
	printf("in the handler the signal held back %d and sa_mask's %d; "
	       "after it %d %d\n", in_self, in_extra, held(SIGUSR1),
	       held(SIGUSR2));
	got_count = 0;
	kill(getpid(), SIGUSR1);
	printf("kill: ran %d, %s, from this program %d\n", got_count,
	       code_name(got_code), got_pid == getpid());

	sigset_t one;
	sigemptyset(&one);
	sigaddset(&one, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &one, 0);
	got_count = 0;
	raise(SIGUSR1);
	int while_held = got_count, says = held(SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &one, 0);
	printf("held back: ran %d times meanwhile, the mask says so %d; "
	       "let through: ran %d time\n", while_held, says, got_count);
}

/* --- a thread computing ------------------------------------------------- */

static volatile int spin_stop, spin_ready, spin_hit, spin_on_it, spin_code;
static volatile uint64_t spin_reg, spin_sp;
static pthread_t spinner_id;
static char *spin_lo, *spin_hi;

static void stack_of_this_thread(char **lo, char **hi)
{
	pthread_attr_t a;
	void *base;
	size_t size;
	pthread_getattr_np(pthread_self(), &a);
	pthread_attr_getstack(&a, &base, &size);
	pthread_attr_destroy(&a);
	*lo = base;
	*hi = (char *)base + size;
}

static void spin_handler(int sig, siginfo_t *si, void *v)
{
	(void)sig;
	ucontext_t *uc = v;
#if defined(__aarch64__)
	spin_reg = uc->uc_mcontext.regs[19];
	spin_sp = uc->uc_mcontext.sp;
	__asm__ volatile("movi v0.2d, #0" ::: "v0");
#elif defined(__x86_64__)
	spin_reg = (uint64_t)uc->uc_mcontext.gregs[REG_RBX];
	spin_sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
	__asm__ volatile("pxor %%xmm0, %%xmm0" ::: "xmm0");
#endif
	spin_on_it = pthread_equal(pthread_self(), spinner_id);
	spin_code = si->si_code;
	__atomic_store_n(&spin_hit, 1, __ATOMIC_RELEASE);
}

static void *spinner(void *arg)
{
	(void)arg;
	uint64_t out = 0;
	stack_of_this_thread(&spin_lo, &spin_hi);
	__atomic_store_n(&spin_ready, 1, __ATOMIC_RELEASE);
	/* A known value in a register the compiler does not touch, and one
	   in the first FP/SIMD register, until told to stop */
#if defined(__aarch64__)
	__asm__ volatile(
		"mov x19, %[m]\n"
		"fmov d0, %[f]\n"
		"1: ldr w9, [%[stop]]\n"
		"cbz w9, 1b\n"
		"fmov %[out], d0\n"
		: [out] "=&r"(out)
		: [m] "r"(MAGIC), [f] "r"(FMAGIC), [stop] "r"(&spin_stop)
		: "x19", "x9", "v0", "memory");
#elif defined(__x86_64__)
	__asm__ volatile(
		"mov %[m], %%rbx\n"
		"movq %[f], %%xmm0\n"
		"1: movl (%[stop]), %%eax\n"
		"testl %%eax, %%eax\n"
		"jz 1b\n"
		"movq %%xmm0, %[out]\n"
		: [out] "=&r"(out)
		: [m] "r"(MAGIC), [f] "r"(FMAGIC), [stop] "r"(&spin_stop)
		: "rbx", "rax", "xmm0", "memory");
#endif
	return (void *)(uintptr_t)(out == FMAGIC);
}

static void computing(void)
{
	handle(SIGUSR2, spin_handler, 0, SA_RESTART);
	pthread_create(&spinner_id, 0, spinner, 0);
	wait_for(&spin_ready, 5000);
	int regs = 0;
	/* Into its loop first: a few tries, should the first find it on
	   its way in */
	for (int i = 0; i < 10 && !regs; i++) {
		ms_sleep(20);
		spin_hit = 0;
		pthread_kill(spinner_id, SIGUSR2);
		wait_for(&spin_hit, 5000);
		regs = spin_reg == MAGIC;
	}
	int in_stack = (char *)spin_sp >= spin_lo && (char *)spin_sp < spin_hi;
	__atomic_store_n(&spin_stop, 1, __ATOMIC_RELEASE);
	void *fp_kept;
	pthread_join(spinner_id, &fp_kept);
	printf("a thread computing: the handler ran on it %d, %s; the frame "
	       "has the register it was interrupted with %d and its stack "
	       "pointer, in its stack %d; the FP register the handler changed "
	       "is as it was %d\n", spin_on_it, code_name(spin_code), regs,
	       in_stack, fp_kept != 0);
}

/* --- WebKit's suspend and resume ---------------------------------------- */

static sem_t wtf_sem;
static volatile int suspend_count;
static mcontext_t *volatile regs_at;
static volatile unsigned long counter;
static volatile int count_stop, count_ready;
static char *count_lo, *count_hi;

static void wtf_handler(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)si;
	if (suspend_count)
		return;              /* the one that resumes */
	regs_at = &((ucontext_t *)uc)->uc_mcontext;
	sem_post(&wtf_sem);
	sigset_t m;
	sigfillset(&m);
	sigdelset(&m, SIGUSR1);
	sigsuspend(&m);
	regs_at = 0;
	sem_post(&wtf_sem);
}

static void *counting(void *arg)
{
	(void)arg;
	stack_of_this_thread(&count_lo, &count_hi);
	__atomic_store_n(&count_ready, 1, __ATOMIC_RELEASE);
	while (!count_stop)
		counter++;
	return 0;
}

static uint64_t sp_of(const mcontext_t *m)
{
#if defined(__aarch64__)
	return m->sp;
#elif defined(__x86_64__)
	return (uint64_t)m->gregs[REG_RSP];
#endif
}

static void webkit_way(void)
{
	sem_init(&wtf_sem, 0, 0);
	handle(SIGUSR1, wtf_handler, SIGUSR1, SA_RESTART);
	pthread_t t;
	pthread_create(&t, 0, counting, 0);
	wait_for(&count_ready, 5000);
	int rounds = 50, stopped = 1, regs = 1, again = 1;
	for (int i = 0; i < rounds; i++) {
		/* suspend */
		pthread_kill(t, SIGUSR1);
		sem_wait(&wtf_sem);
		mcontext_t copy = *regs_at;
		suspend_count = 1;
		char *sp = (char *)(uintptr_t)sp_of(&copy);
		regs &= sp >= count_lo && sp < count_hi;
		unsigned long a = counter;
		ms_sleep(3);
		stopped &= counter == a;
		/* resume */
		pthread_kill(t, SIGUSR1);
		sem_wait(&wtf_sem);
		suspend_count = 0;
		unsigned long b = counter;
		int moved = 0;
		for (int k = 0; k < 2000 && !moved; k++) {
			moved = counter != b;
			if (!moved)
				ms_sleep(1);
		}
		again &= moved;
	}
	count_stop = 1;
	pthread_join(t, 0);
	printf("WebKit's suspend and resume, %d times: the thread stood "
	       "still while suspended %d, the stack pointer in its frame in "
	       "its stack %d, it ran on after each %d\n", rounds, stopped,
	       regs, again);
}

/* --- waits that go on ---------------------------------------------------- */

static volatile int plain_count;

static void plain_handler(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)si;
	(void)uc;
	__atomic_add_fetch(&plain_count, 1, __ATOMIC_RELEASE);
}

static int pipe_fds[2];
static volatile int reader_ready;
static long reader_got;
static char reader_byte;

static void *reader(void *arg)
{
	(void)arg;
	__atomic_store_n(&reader_ready, 1, __ATOMIC_RELEASE);
	reader_got = read(pipe_fds[0], &reader_byte, 1);
	return 0;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int cv_go, cv_waiting, cv_woke;

static void *cond_waiter(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&mu);
	cv_waiting = 1;
	while (!cv_go)
		pthread_cond_wait(&cv, &mu);
	cv_woke = 1;
	pthread_mutex_unlock(&mu);
	return 0;
}

static void waits(void)
{
	handle(SIGUSR2, plain_handler, 0, SA_RESTART);

	if (pipe(pipe_fds)) {
		printf("no pipe\n");
		return;
	}
	pthread_t t;
	pthread_create(&t, 0, reader, 0);
	wait_for(&reader_ready, 5000);
	ms_sleep(30);                      /* well into its read */
	plain_count = 0;
	pthread_kill(t, SIGUSR2);
	int ran = wait_for(&plain_count, 5000);
	ms_sleep(20);
	if (write(pipe_fds[1], "x", 1) != 1)
		printf("the write failed\n");
	pthread_join(t, 0);
	printf("a thread waiting in read on a pipe: the handler ran %d, and "
	       "read went on: %ld byte, '%c'\n", ran, reader_got, reader_byte);

	pthread_create(&t, 0, cond_waiter, 0);
	for (int i = 0; i < 5000; i++) {
		pthread_mutex_lock(&mu);
		int w = cv_waiting;
		pthread_mutex_unlock(&mu);
		if (w)
			break;
		ms_sleep(1);
	}
	ms_sleep(30);
	plain_count = 0;
	pthread_kill(t, SIGUSR2);
	ran = wait_for(&plain_count, 5000);
	ms_sleep(20);
	pthread_mutex_lock(&mu);
	int early = cv_woke;
	cv_go = 1;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	pthread_join(t, 0);
	printf("a thread waiting on a condition variable: the handler ran %d, "
	       "it went on waiting %d until signalled %d\n", ran, !early,
	       cv_woke);
}

/* --- sigsuspend, and kill to the program -------------------------------- */

static pthread_t main_id;

static void *poke_main(void *arg)
{
	ms_sleep((int)(intptr_t)arg);
	pthread_kill(main_id, SIGUSR2);
	return 0;
}

static volatile int other_ready, other_stop, taken_by_other;
static pthread_t other_id;

static void taker_handler(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)si;
	(void)uc;
	taken_by_other = pthread_equal(pthread_self(), other_id);
	__atomic_add_fetch(&plain_count, 1, __ATOMIC_RELEASE);
}

static void *other(void *arg)
{
	(void)arg;
	__atomic_store_n(&other_ready, 1, __ATOMIC_RELEASE);
	while (!other_stop)
		ms_sleep(2);
	return 0;
}

static void suspending(void)
{
	main_id = pthread_self();
	handle(SIGUSR2, plain_handler, 0, SA_RESTART);
	sigset_t two, none;
	sigemptyset(&two);
	sigaddset(&two, SIGUSR2);
	sigemptyset(&none);
	pthread_sigmask(SIG_BLOCK, &two, 0);
	pthread_t t;
	plain_count = 0;
	pthread_create(&t, 0, poke_main, (void *)(intptr_t)30);
	errno = 0;
	int r = sigsuspend(&none);
	int e = errno;
	pthread_join(t, 0);
	printf("sigsuspend: %d, EINTR %d; the handler ran %d; the mask from "
	       "before is back %d\n", r, e == EINTR, plain_count, held(SIGUSR2));
	pthread_sigmask(SIG_UNBLOCK, &two, 0);

	/* kill to the program, which this thread holds back */
	handle(SIGUSR2, taker_handler, 0, SA_RESTART);
	pthread_create(&other_id, 0, other, 0);
	wait_for(&other_ready, 5000);
	pthread_sigmask(SIG_BLOCK, &two, 0);
	plain_count = 0;
	kill(getpid(), SIGUSR2);
	int ran = wait_for(&plain_count, 5000);
	other_stop = 1;
	pthread_join(other_id, 0);
	pthread_sigmask(SIG_UNBLOCK, &two, 0);
	printf("kill to the program while this thread holds it back: handled "
	       "%d, by the other thread %d\n", ran, taken_by_other);
}

/* --- nested, SA_RESETHAND, fork, waitpid -------------------------------- */

static char order[16];
static volatile int order_n;

static void outer(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)si;
	(void)uc;
	order[order_n++] = '(';
	raise(SIGUSR2);
	order[order_n++] = ')';
}

static void inner(int sig, siginfo_t *si, void *uc)
{
	(void)sig;
	(void)si;
	(void)uc;
	order[order_n++] = '2';
}

static void *poke_main_later(void *arg)
{
	return poke_main(arg);
}

static void the_rest(void)
{
	handle(SIGUSR1, outer, 0, SA_RESTART);
	handle(SIGUSR2, inner, 0, SA_RESTART);
	raise(SIGUSR1);
	order[order_n] = 0;
	printf("a handler interrupted by another signal: %s\n", order);

	plain_count = 0;
	handle(SIGUSR2, plain_handler, 0, SA_RESTART | SA_RESETHAND);
	raise(SIGUSR2);
	struct sigaction now;
	sigaction(SIGUSR2, 0, &now);
	printf("SA_RESETHAND: ran %d, and the signal does what it does by "
	       "itself again %d\n", plain_count, now.sa_handler == SIG_DFL);

	handle(SIGUSR2, plain_handler, 0, SA_RESTART);
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) {
		plain_count = 0;
		raise(SIGUSR2);
		_exit(plain_count == 1 ? 21 : 22);
	}
	int st = 0;
	waitpid(c, &st, 0);
	printf("fork: the child has the handler %d\n",
	       WIFEXITED(st) && WEXITSTATUS(st) == 21);

	c = fork();
	if (c == 0) {
		ms_sleep(200);
		_exit(7);
	}
	main_id = pthread_self();
	plain_count = 0;
	pthread_t t;
	pthread_create(&t, 0, poke_main_later, (void *)(intptr_t)50);
	pid_t w = waitpid(c, &st, 0);
	pthread_join(t, 0);
	printf("waitpid: the handler ran %d while it waited, and it went on to "
	       "the child's end %d, code %d\n", plain_count, w == c,
	       WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

int main(void)
{
	setvbuf(stdout, 0, _IOLBF, 0);
	to_itself();
	computing();
	webkit_way();
	waits();
	suspending();
	the_rest();
	printf("sigtest: done\n");
	return 0;
}
