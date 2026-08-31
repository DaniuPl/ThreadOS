/* spawn64 - the classic way of starting programs: fork, exec, wait
 *
 * The program duplicates itself, the child replaces its image with
 * another program, and the parent waits for its end and collects the
 * exit code.
 *
 * None of these three things demands anything of the kernel beyond
 * what it already knows: fork gives a second process, exec replaces its
 * memory, and wait collects the trace left behind it. Only together do
 * they give what every shell in the world does.
 */

#include "usys64.h"

static char buf[96];

static int put(int n, const char *s)
{
    while (*s)
        buf[n++] = *s++;
    return n;
}

static int putn(int n, int64_t v)
{
    char tmp[24];
    utoa((uint64_t)v, tmp);
    return put(n, tmp);
}

void _start(void)
{

    print("[spawn64] powielam sie i uruchamiam w dziecku 'hello64'\n");

    int64_t child = sys_fork();
    if (child < 0) {
        print("[spawn64] BLAD: fork sie nie udal\n");
        sys_exit(1);
    }

    if (child == 0) {
        /* The child stops being itself: from the next instruction on a
           completely different program is running. If exec returns, that
           means it failed. */
        sys_exec("hello64", 0);
        print("[spawn64] BLAD: exec wrocil\n");
        sys_exit(1);
    }

    int code = -1;
    int64_t who = sys_wait(&code);

    int n = put(0, "[spawn64] doczekalem dziecka ");
    n = putn(n, who);
    n = put(n, ", kod wyjscia ");
    n = putn(n, code);
    buf[n++] = '\n';
    buf[n] = 0;
    print(buf);

    /* hello64 ends with a seven - if we see that, it went the whole
       way: fork, exec, wait */
    int ok = (who == child && code == 7);
    print(ok ? "[spawn64] fork + exec + wait dzialaja\n"
             : "[spawn64] BLAD: nie ten numer albo nie ten kod\n");

    /* There is nothing left to wait for - the second wait has to say so */
    int64_t none = sys_wait(0);
    if (none != -1) {
        print("[spawn64] BLAD: wait wymyslil sobie dziecko\n");
        ok = 0;
    }

    sys_exit(ok ? 0 : 1);
}
