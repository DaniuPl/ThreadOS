/* A dictionary of task calls for the shared code
 *
 * The network stack, and everything that comes after it, talks to the
 * kernel through a handful of functions: put me to sleep, am I to die,
 * is that task still alive, make a new thread (kernel/include/task.h).
 * Those names are the contract - what stands behind them is the
 * scheduler's business.
 *
 * Thanks to that net.c, dhcp.c and tcp.c do not have to know that the
 * scheduler calls things something else. This file is the whole
 * translation.
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
   nobody can close must not exist. It is an uncatchable signal. */
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

int task_end_of(int id, int *code, const char **why, uint64_t *addr)
{
    return task64_end_of(id, code, why, addr);
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

/* The virtual consoles ask a task which one it is to write on */
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
    struct args64 args = { 0 };
    int i = 0;

    if (!cmdline) {
        printk("[desktop] no command to start\n");
        return -1;
    }
    while (cmdline[i]) {
        while (cmdline[i] == ' ')
            i++;
        if (!cmdline[i])
            break;
        if (args.argc == ARG_MAX) {
            printk("[desktop] too many command arguments\n");
            return -1;
        }
        int n = 0;
        while (cmdline[i] && cmdline[i] != ' ') {
            if (n == ARG_LEN - 1) {
                printk("[desktop] command argument is too long\n");
                return -1;
            }
            args.v[args.argc][n++] = cmdline[i++];
        }
        args.v[args.argc][n] = '\0';
        args.argc++;
    }
    if (!args.argc) {
        printk("[desktop] empty command\n");
        return -1;
    }
    for (int n = 0; args.v[0][n]; n++)
        if (n >= 15) {
            printk("[desktop] program name is too long\n");
            return -1;
        }
    int id = proc64_spawn(args.v[0], &args, 1);
    if (id < 0)
        printk("[desktop] could not start '%s'\n", args.v[0]);
    return id;
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
   the bar */
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

/* What the desktop is to do with a key that no window wants: put it
   into the visible console's queue, which is where that console's
   shell reads from. Without it the desktop was a screen one could not
   type at unless a window was listening. The shell reads from a queue
   rather than through a callback, so there is nothing else to hand the
   desktop for safekeeping: when it finishes, the keys go back to the
   queue by themselves. */
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
