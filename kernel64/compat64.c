/* A dictionary of task calls for the shared code
 *
 * The network stack, and everything that comes after it, talks to the
 * kernel through a handful of functions: put me to sleep, am I to die,
 * is that task still alive, make a new thread. The names of these
 * functions are the same in both kernels, because it is they that are
 * the contract - what stands behind them is the kernel's business.
 *
 * Thanks to that net.c, dhcp.c and tcp.c cross over to long mode
 * without a single change: they do not have to know that the scheduler
 * is called something else here. This file is the whole difference.
 */

#include <stddef.h>
#include <stdint.h>
#include "task64.h"
#include "proc64.h"
#include "keyboard64.h"
#include "signal64.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/keyboard.h"

struct task;

void task_sleep(uint32_t ms)
{
    task64_sleep(ms);
}

/* "Am I to die" means here: is a signal waiting for me. Loops waiting
   inside the kernel ask about that themselves, because a signal is
   delivered only on the return to ring 3 - and they do not return
   there. */
/* Killing a process by number. The desktop reaches for this when an
   application does not answer a request to close its window - a window
   nobody can close must not exist. In the 32-bit kernel the name is
   task_kill; here the same thing is an uncatchable signal. */
int task_kill(int id)
{
    return task64_signal(id, SIGKILL);
}

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

struct task *task_create_anycore(const char *name, void (*entry)(void *),
                                  void *arg)
{
    return (struct task *)task64_create_anycore(name, entry, arg);
}

/* The 64-bit kernel's shell does not redraw the prompt after somebody
   else's print - the line stays where it was and a new prompt appears
   by itself with the next command */
void shell_refresh(void)
{
}

/* The virtual consoles ask a task which one it is to write on - just
   as in the 32-bit kernel. The name is the contract, the rest is the
   kernel's business. */
int task_vc(void)
{
    return task64_vc();
}

/* Looking for a program asks about the task's working directory - a
   program on disk is looked for where whoever starts it stands */
int task_cwd(void)
{
    return task64_cwd();
}

/* Page swapping asks the kernel who is to hand memory over to disk */
int task_reclaim_page(void)
{
    return task64_reclaim_page();
}

/* --- The desktop ---------------------------------------------------
 *
 * The desktop starts applications by name and takes over the keyboard.
 * Both of these already have their own shape in this kernel - here
 * there is only a translation of names, just as with the network.
 */
int exec_spawn(const char *cmdline)
{
    return proc64_spawn(cmdline, 0, 1);
}

/* A USB keyboard supplies finished characters and console switch
   requests. The rest of the path is the same as with a PS/2 interrupt
   - the USB driver does not have to know there are several queues. */
void keyboard_inject(char c)
{
    keyboard64_inject(c);
}

void keyboard_inject_switch(int vc)
{
    keyboard64_inject_switch(vc);
}

/* The desktop asks how many tasks are running - it puts the number on
   the bar. One word, the same question on both sides. */
int task_count(void)
{
    return task64_count();
}

void keyboard_set_fkey_override(keyboard_fkey_handler_t handler)
{
    keyboard64_set_fkey_override(handler);
}

void keyboard_set_override(keyboard_char_handler_t handler)
{
    keyboard64_set_override(handler);
}

/* The 64-bit kernel's shell reads characters from a queue rather than
   through a callback - so there is nothing to hand the desktop for
   safekeeping. When the desktop finishes, the keys return to the queue
   by themselves. */
/* What the desktop is to do with a key that no window wants. In
   protected mode this is the handler of the visible console - its
   shell; here the same thing means putting the character into that
   console's queue, which is where its shell reads from. Without it a
   64-bit desktop was a screen one could not type at unless a window
   was listening. */
keyboard_char_handler_t keyboard_console_handler(void)
{
    return keyboard64_to_console;
}

/* task_name_of() for the shared desktop: the same rows task64_ps hands
   a program */
int task_name_of(int pid, char *out, int max)
{
    struct psinfo64 ps;
    for (int i = 0; task64_ps(i, &ps) == 0; i++) {
        if (ps.pid != pid)
            continue;
        int k = 0;
        for (; ps.name[k] && k < max - 1; k++)
            out[k] = ps.name[k];
        out[k] = '\0';
        return 1;
    }
    return 0;
}
