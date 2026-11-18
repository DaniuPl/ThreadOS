/* musl on ThreadOS: one ThreadOS system call, from the C library
 *
 * The kernel's own numbers and arguments (kernel64/syscall64.h), the
 * same on both machines: on the PC the syscall instruction, number in
 * RAX and arguments in RDI, RSI, RDX, R10; on ARM64 svc, number in x8
 * and arguments in x0-x3 (kernelarm64/proc.c). Every register but the
 * answer comes back as it went in, on both.
 */

#ifndef THREADOS_TOS_H
#define THREADOS_TOS_H

static inline long tos(long n, long a1, long a2, long a3, long a4)
{
#if defined(__aarch64__)
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a1;
	register long x1 __asm__("x1") = a2;
	register long x2 __asm__("x2") = a3;
	register long x3 __asm__("x3") = a4;
	__asm__ __volatile__ ("svc #0" : "+r"(x0)
		: "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
#else
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ __volatile__ ("syscall" : "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
		: "rcx", "r11", "memory");
	return ret;
#endif
}

#endif
