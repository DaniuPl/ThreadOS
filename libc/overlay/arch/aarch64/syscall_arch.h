/* musl on ThreadOS for ARM64: system calls
 *
 * The same as arch/x86_64/syscall_arch.h. Upstream this file issues svc
 * with Linux's numbers for AArch64 - which are not even x86-64's
 * numbers; here every call goes to __tos_syscall
 * (src/threados/syscall.c), which speaks Linux to musl and ThreadOS to
 * the kernel, and the ARM64 kernel takes the PC's ThreadOS numbers
 * (kernelarm64/proc.c). What musl asks for by Linux's AArch64 numbers
 * the layer answers by name (SYS_openat, SYS_read), so one C file
 * serves both machines.
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
