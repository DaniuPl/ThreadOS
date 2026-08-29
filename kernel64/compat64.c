/* Slownik zadan dla kodu wspolnego
 *
 * Siec, i wszystko, co po niej przyjdzie, mowi do jadra kilkoma
 * funkcjami: uspij mnie, czy mam zginac, czy tamto zadanie jeszcze
 * zyje, zrob nowy watek. Nazwy tych funkcji sa te same w obu jadrach,
 * bo to one sa umowa - a co za nimi stoi, jest juz sprawa jadra.
 *
 * Dzieki temu net.c, dhcp.c i tcp.c przechodza do trybu dlugiego bez
 * jednej zmiany: nie musza wiedziec, ze scheduler nazywa sie tu
 * inaczej. Ten plik jest cala roznica.
 */

#include <stddef.h>
#include <stdint.h>
#include "task64.h"
#include "../kernel/include/printk.h"

struct task;

void task_sleep(uint32_t ms)
{
    task64_sleep(ms);
}

/* "Czy mam zginac" znaczy tu: czy czeka na mnie sygnal. Petle
   czekajace w jadrze pytaja o to same, bo sygnal dostarcza sie
   dopiero przy powrocie do pierscienia 3 - a one tam nie wracaja. */
int task_killed(void)
{
    return task64_signal_pending();
}

int task_alive(int id)
{
    return task64_alive(id);
}

int task_current_id(void)
{
    return task64_id_current();
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    return (struct task *)task64_create(name, entry, arg);
}

/* Shell jadra 64-bitowego nie przerysowuje promptu po cudzym wypisie -
   linia zostaje tam, gdzie byla, i nowy prompt pojawia sie sam przy
   nastepnym poleceniu */
void shell_refresh(void)
{
}
