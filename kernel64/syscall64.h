#ifndef PROTONOS_SYSCALL64_H
#define PROTONOS_SYSCALL64_H

#include <stdint.h>

/* Numery wywolan - musza zgadzac sie z user64/usys64.h */
#define SYS64_WRITE 1
#define SYS64_EXIT  2
#define SYS64_RING  3
#define SYS64_SLEEP 4
#define SYS64_TICKS 5

/* Wejscie do jadra przez instrukcje syscall (zamiast bramki przerwania) */
void syscall64_init(void);

/* Stos jadra dla wywolan z ring 3 - ustawiany przed wejsciem w ring 3 */
void syscall64_set_kernel_stack(uint64_t rsp);

/* Skok do ring 3; nie wraca */
void enter_ring3_64(uint64_t rip, uint64_t rsp);

/* Co program zdazyl powiedziec jadru - do sprawdzenia po jego koncu */
int syscall64_user_ran(void);
uint32_t syscall64_user_cs(void);
int syscall64_user_exited(void);
int syscall64_user_status(void);
void syscall64_forget_user(void);

#endif
