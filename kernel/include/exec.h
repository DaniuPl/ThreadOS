#ifndef THREADOS_EXEC_H
#define THREADOS_EXEC_H

/* Runs an ELF program (from the initrd or the disk) as a user process:
   ring 3, its own address space. The argument is a command line - the
   program plus arguments separated by spaces. Returns a pid or -1.
   The desktop starts applications through this (kernel64/compat64.c). */
int exec_spawn(const char *cmdline);

#endif
