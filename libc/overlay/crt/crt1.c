/* musl on ThreadOS 64: where a program starts
 *
 * ThreadOS starts a program with argc in RDI and the address of argv
 * in RSI (kernel64/proc64.c), and nothing else: no environment, no
 * auxiliary vector. musl's start-up expects Linux's layout - argv,
 * then the environment, then the auxiliary vector, one after the
 * other - so that layout is built here, on this function's stack, and
 * handed on. The stack frame lives as long as the program does:
 * __libc_start_main never returns, it ends in exit.
 *
 * Of the program headers musl wants only the one for thread-local
 * storage, so that is the one made here, from what the linker script
 * (libc/libc64.ld) says about .tdata and .tbss.
 */

#include <features.h>
#include <elf.h>
#include "libc.h"

int main();
weak void _init();
weak void _fini();
int __libc_start_main(int (*)(), int, char **,
	void (*)(), void(*)(), void(*)());

extern char __tls_image[], __tls_filesz[], __tls_memsz[], __tls_align[];

/* The sizes are absolute symbols, small numbers. On ARM64 a program is
   at 256 GiB, and the compiler reaches a symbol by adrp - within 4 GiB
   of the code - so a number near zero is out of its reach; a literal
   holds it whole instead. */
#if defined(__aarch64__)
#define LINKED(sym) ({ unsigned long v_; \
	__asm__ ("ldr %0, =" #sym : "=r"(v_)); v_; })
#else
#define LINKED(sym) ((unsigned long)sym)
#endif

#define ARGS_MAX 64

__attribute__((__used__))
void _start_c(long argc, char **argv)
{
	/* argv, its NULL, an empty environment, then the auxiliary vector */
	char *block[ARGS_MAX + 1 + 1 + 12];
	Elf64_Phdr tls = {
		.p_type = PT_TLS,
		.p_vaddr = (unsigned long)__tls_image,
		.p_filesz = LINKED(__tls_filesz),
		.p_memsz = LINKED(__tls_memsz),
		.p_align = LINKED(__tls_align),
	};
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
	   The time stamp counter (on ARM64 the generic timer's, which the
	   kernel lets EL0 read) is no randomness worth the name, but the
	   canary is the only thing that reads it. */
#if defined(__aarch64__)
	unsigned long long x;
	__asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r"(x));
	x |= 1;
#else
	unsigned lo, hi;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	unsigned long long x = ((unsigned long long)hi << 32) | lo;
#endif
	for (int i = 0; i < 16; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		random[i] = (unsigned char)x;
	}
	long *aux = (long *)&block[n];
	aux[0] = AT_PAGESZ; aux[1] = 4096;
	aux[2] = AT_RANDOM; aux[3] = (long)random;
	int k = 4;
	if (tls.p_memsz) {
		aux[k++] = AT_PHDR;  aux[k++] = (long)&tls;
		aux[k++] = AT_PHNUM; aux[k++] = 1;
		aux[k++] = AT_PHENT; aux[k++] = sizeof(tls);
	}
	aux[k++] = AT_NULL;  aux[k++] = 0;

	__libc_start_main(main, (int)argc, block, _init, _fini, 0);
}

/* The kernel enters with the stack aligned the way it is right after a
   call (kernel64/proc64.c), the arguments already in the registers the
   C function wants them in. The ARM64 kernel enters with the stack on
   a sixteen-byte line, which is what a call leaves there. */
#if defined(__aarch64__)
__asm__(
".text \n"
".global _start \n"
".type _start,%function \n"
"_start: \n"
"	mov x29, #0 \n"
"	mov x30, #0 \n"
"	bl _start_c \n"
"	brk #0 \n"
);
#else
__asm__(
".text \n"
".global _start \n"
"_start: \n"
"	xor %rbp,%rbp \n"
"	andq $-16,%rsp \n"
"	call _start_c \n"
"	ud2 \n"
);
#endif
