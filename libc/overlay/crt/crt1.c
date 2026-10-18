/* musl on ThreadOS 64: where a program starts
 *
 * ThreadOS starts a program with argc in RDI and the address of argv
 * in RSI (kernel64/proc64.c), and nothing else: no environment, no
 * auxiliary vector. musl's start-up expects Linux's layout - argv,
 * then the environment, then the auxiliary vector, one after the
 * other - so that layout is built here, on this function's stack, and
 * handed on. The stack frame lives as long as the program does:
 * __libc_start_main never returns, it ends in exit.
 */

#include <features.h>
#include <elf.h>
#include "libc.h"

int main();
weak void _init();
weak void _fini();
int __libc_start_main(int (*)(), int, char **,
	void (*)(), void(*)(), void(*)());

#define ARGS_MAX 64

__attribute__((__used__))
void _start_c(long argc, char **argv)
{
	/* argv, its NULL, an empty environment, then the auxiliary vector */
	char *block[ARGS_MAX + 1 + 1 + 6];
	unsigned char random[16];
	int n = 0;

	if (argc < 0 || !argv)
		argc = 0;
	if (argc > ARGS_MAX)
		argc = ARGS_MAX;
	for (long i = 0; i < argc; i++)
		block[n++] = argv[i];
	block[n++] = 0;                  /* the end of argv */
	block[n++] = 0;                  /* and of the environment */

	/* AT_RANDOM is what musl seeds the stack protector's canary from.
	   The time stamp counter is no randomness worth the name, but the
	   canary is the only thing that reads it. */
	unsigned lo, hi;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	unsigned long long x = ((unsigned long long)hi << 32) | lo;
	for (int i = 0; i < 16; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		random[i] = (unsigned char)x;
	}
	long *aux = (long *)&block[n];
	aux[0] = AT_PAGESZ; aux[1] = 4096;
	aux[2] = AT_RANDOM; aux[3] = (long)random;
	aux[4] = AT_NULL;   aux[5] = 0;

	__libc_start_main(main, (int)argc, block, _init, _fini, 0);
}

/* The kernel enters with the stack aligned the way it is right after a
   call (kernel64/proc64.c), the arguments already in the registers the
   C function wants them in */
__asm__(
".text \n"
".global _start \n"
"_start: \n"
"	xor %rbp,%rbp \n"
"	andq $-16,%rsp \n"
"	call _start_c \n"
"	ud2 \n"
);
