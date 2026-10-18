/* musl on ThreadOS 64: system calls
 *
 * Upstream this file issues the syscall instruction with Linux's
 * numbers. ThreadOS has the same instruction but numbers and argument
 * shapes of its own (a path is a pointer and a length, a file is opened
 * by name relative to the working directory, and so on), so every call
 * goes to one C function instead - __tos_syscall in
 * src/threados/syscall.c - which speaks Linux to musl and ThreadOS to
 * the kernel. musl itself stays unchanged: it still thinks in Linux
 * system calls, and gets Linux answers (a result, or -errno).
 */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

__attribute__((__visibility__("hidden")))
long __tos_syscall(long n, long a1, long a2, long a3, long a4, long a5,
                   long a6);

static __inline long __syscall0(long n)
{
	return __tos_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
	return __tos_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return __tos_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __tos_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __tos_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __tos_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __tos_syscall(n, a1, a2, a3, a4, a5, a6);
}

/* No vDSO: there is no such page, and clock_gettime goes through the
   call like everything else */

#define IPC_64 0
