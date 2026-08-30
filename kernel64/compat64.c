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
#include "proc64.h"
#include "keyboard64.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/keyboard.h"

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

struct task *task_create_wszedzie(const char *name, void (*entry)(void *),
                                  void *arg)
{
    return (struct task *)task64_create_wszedzie(name, entry, arg);
}

/* Shell jadra 64-bitowego nie przerysowuje promptu po cudzym wypisie -
   linia zostaje tam, gdzie byla, i nowy prompt pojawia sie sam przy
   nastepnym poleceniu */
void shell_refresh(void)
{
}

/* Konsole wirtualne pytaja zadanie, na ktorej ma pisac - tak samo jak
   w jadrze 32-bitowym. Nazwa jest umowa, reszta sprawa jadra. */
int task_vc(void)
{
    return task64_vc();
}

/* Szukanie programu pyta o katalog roboczy zadania - program z dysku
   szuka sie tam, gdzie stoi ten, kto go uruchamia */
int task_cwd(void)
{
    return task64_cwd();
}

/* Wymiana stron pyta jadro, kto ma oddac pamiec na dysk */
int task_reclaim_page(void)
{
    return task64_reclaim_page();
}

/* --- Pulpit --------------------------------------------------------
 *
 * Pulpit uruchamia aplikacje po nazwie i przejmuje klawiature. Jedno
 * i drugie ma juz w tym jadrze swoja postac - tu jest tylko przeklad
 * nazw, tak samo jak przy sieci.
 */
int exec_spawn(const char *cmdline)
{
    return proc64_spawn(cmdline, 0, 1);
}

/* Klawiatura USB podaje gotowe znaki i zadania przelaczenia konsoli.
   Dalsza droga jest ta sama, co przy przerwaniu PS/2 - sterownik USB
   nie musi wiedziec, ze kolejek jest kilka. */
void keyboard_inject(char c)
{
    keyboard64_inject(c);
}

void keyboard_inject_switch(int vc)
{
    keyboard64_inject_switch(vc);
}

void keyboard_set_override(keyboard_char_handler_t handler)
{
    keyboard64_set_override(handler);
}

/* Shell jadra 64-bitowego czyta znaki z kolejki, a nie przez funkcje
   zwrotna - nie ma wiec czego oddac pulpitowi na przechowanie. Gdy
   pulpit sie skonczy, klawisze same wracaja do kolejki. */
keyboard_char_handler_t keyboard_console_handler(void)
{
    return (keyboard_char_handler_t)0;
}
