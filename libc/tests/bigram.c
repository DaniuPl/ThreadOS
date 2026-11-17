/* bigram - a program's memory above the low gigabyte
 *
 * The kernel used to manage 512 MiB of RAM at most, whatever the
 * machine had: it reached every frame at its own address, and programs
 * lie at 1 GiB. It manages up to 4 GiB now, the frames above the low
 * gigabyte going to programs' pages (kernel/pmm.c), which the kernel
 * reaches through a second map of all memory (kernel64/paging64.h).
 * scripts/tests/t_bigram.py runs this on a machine with 3 GiB:
 *
 *   'mib' MiB of anonymous memory, every page written with words of its
 *   own and read back - more than the low gigabyte could hold, so most
 *   of it is high frames
 *
 *   a forked child sees all of it, and writes into every 32nd page: the
 *   copies on write are made from high frames into high frames, and the
 *   parent's memory stays as it was
 *
 *   a memfd of 256 MiB written through a mapping and read back through
 *   read(), which the kernel copies out of its frames itself
 *
 * and says how much memory the machine had in use meanwhile.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIB (1024UL * 1024)
#define PAGE 4096UL
#define WORDS 8                      /* written in each page, 512 bytes apart */

static uint64_t word(uint64_t page, int k)
{
    return page * 0x9E3779B97F4A7C15ULL + (uint64_t)k * 0x632BE59BD9B4E019ULL + 1;
}

static long used_mib(void)
{
    struct sysinfo si;
    if (sysinfo(&si) != 0)
        return -1;
    return (long)((si.totalram - si.freeram) * si.mem_unit / MIB);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* how many pages of 'mem' hold what 'fill' wrote (with 'bump' added
   to every 'every'-th page's first word) */
static unsigned long intact(const uint64_t *mem, unsigned long pages,
                            unsigned long every, uint64_t bump)
{
    unsigned long good = 0;
    for (unsigned long p = 0; p < pages; p++) {
        const uint64_t *w = mem + p * (PAGE / 8);
        int ok = 1;
        for (int k = 0; k < WORDS; k++) {
            uint64_t want = word(p, k) + (k == 0 && every && p % every == 0
                                          ? bump : 0);
            ok &= w[k * 64] == want;
        }
        good += ok;
    }
    return good;
}

int main(int argc, char **argv)
{
    unsigned long mib = argc > 1 ? strtoul(argv[1], 0, 10) : 2048;
    unsigned long pages = mib * MIB / PAGE;
    long base = used_mib();
    long t0 = now_ms();

    uint64_t *mem = mmap(0, mib * MIB, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        printf("mapping %lu MiB: refused\n", mib);
        return 1;
    }
    for (unsigned long p = 0; p < pages; p++) {
        uint64_t *w = mem + p * (PAGE / 8);
        for (int k = 0; k < WORDS; k++)
            w[k * 64] = word(p, k);
    }
    long during = used_mib();
    unsigned long good = intact(mem, pages, 0, 0);
    printf("%lu MiB written, every page read back intact: %s\n", mib,
           good == pages ? "yes" : "no");
    if (good != pages)
        printf("  (%lu of %lu pages intact)\n", good, pages);
    printf("~in use: %ld MiB before, %ld MiB with it all written\n",
           base, during);
    printf("the machine had it all in memory at once: %s\n",
           during - base >= (long)mib ? "yes" : "no");
    printf("~writing and reading took %ld ms\n", now_ms() - t0);

    /* fork: shared, then copied where the child writes */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (intact(mem, pages, 0, 0) != pages)
            _exit(2);
        for (unsigned long p = 0; p < pages; p += 32)
            mem[p * (PAGE / 8)] += 7;
        _exit(intact(mem, pages, 32, 7) == pages ? 0 : 3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    printf("a forked child read it all, and its writes (%lu pages copied) "
           "read back: %s\n", pages / 32,
           WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "yes" : "no");
    printf("and the parent's memory is as it was: %s\n",
           intact(mem, pages, 0, 0) == pages ? "yes" : "no");
    munmap(mem, mib * MIB);

    /* a memfd: written through a mapping, read with read() */
    unsigned long fdmib = 256, fdpages = fdmib * MIB / PAGE;
    int fd = memfd_create("bigram", 0);
    int sized = fd >= 0 && ftruncate(fd, fdmib * MIB) == 0;
    uint64_t *shared = sized ? mmap(0, fdmib * MIB, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0) : MAP_FAILED;
    int fine = shared != MAP_FAILED;
    if (fine) {
        for (unsigned long p = 0; p < fdpages; p++)
            shared[p * (PAGE / 8)] = word(p + 12345, 0);
        static uint64_t buf[PAGE / 8];
        for (unsigned long p = 0; p < fdpages && fine; p += 97) {
            fine &= pread(fd, buf, sizeof(buf), p * PAGE) == (ssize_t)sizeof(buf);
            fine &= buf[0] == word(p + 12345, 0);
        }
        munmap(shared, fdmib * MIB);
    }
    close(fd);
    printf("a memfd of %lu MiB written through a mapping reads back "
           "through read(): %s\n", fdmib, fine ? "yes" : "no");
    return 0;
}
