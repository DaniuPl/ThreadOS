/* Program użytkownika (ring 3)
 *
 * Kod i dane leżą w sekcjach .user_text/.user_data - jedynych stronach
 * jądra z bitem USER. Program nie może wołać żadnych funkcji jądra ani
 * sięgać po jego dane (string literals lądują w .rodata jądra, więc
 * wszystkie napisy muszą być statycznymi tablicami w USER_DATA!).
 * Ze światem rozmawia wyłącznie syscallami int 0x80.
 *
 * Na końcu celowo czyta pamięć jądra, żeby zademonstrować ochronę:
 * jądro ubija proces, system działa dalej.
 */

#include <stdint.h>
#include "include/syscall.h"

#define USER_TEXT __attribute__((section(".user_text"), noinline))
#define USER_DATA __attribute__((section(".user_data")))

static inline int32_t sys_write(const char *s, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WRITE), "b"(s), "c"(len)
                     : "memory");
    return ret;
}

static inline void sys_sleep(uint32_t ms)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SLEEP), "b"(ms)
                     : "memory");
}

static inline void sys_exit(int code)
{
    __asm__ volatile("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
}

static char msg_hello[] USER_DATA = "[ring3] czesc z trybu uzytkownika!\n";
static char msg_count[] USER_DATA = "[ring3] licze: 0\n";
static char msg_probe[] USER_DATA = "[ring3] a teraz czytam pamiec jadra...\n";
static char msg_never[] USER_DATA = "[ring3] TEGO NIE POWINNO BYC WIDAC\n";

USER_TEXT static uint32_t u_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

USER_TEXT void user_main(void)
{
    sys_write(msg_hello, u_strlen(msg_hello));

    for (int i = 1; i <= 3; i++) {
        msg_count[15] = (char)('0' + i);
        sys_write(msg_count, u_strlen(msg_count));
        sys_sleep(300);
    }

    sys_write(msg_probe, u_strlen(msg_probe));
    volatile uint32_t probe = *(volatile uint32_t *)0x100000; /* jadro! */
    (void)probe;

    /* Tu nie docieramy - page fault ubija proces */
    sys_write(msg_never, u_strlen(msg_never));
    sys_exit(0);
}
