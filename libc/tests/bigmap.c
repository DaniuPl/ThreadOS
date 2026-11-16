/* bigmap - the mapping area, where a program's large memory goes
 *
 * Mappings used to share the program's one gigabyte with its image,
 * heap and stack, and had a quarter of it: a web engine reserves more
 * than that before it reads a page. They lie at 512 GiB now, 64 GiB of
 * room (kernel64/elf64.h). scripts/tests/t_bigmap.py runs this and
 * reads what it says:
 *
 *   three mappings of a gigabyte each - twelve times what fitted before
 *   - all of them above 512 GiB, a byte written every 16 MiB and read
 *   back intact, only the touched pages becoming real
 *
 *   madvise(MADV_DONTNEED) gives pages back and they read as zeros, as
 *   Linux promises and WebKit counts on
 *
 *   a forked child sees the parent's memory there, and what each then
 *   writes stays its own (the kernel copies the area's tables on fork)
 *
 *   threads, whose stacks are mappings too, run and come back
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define GIB (1024UL * 1024 * 1024)
#define STEP (16UL * 1024 * 1024)

static void *work(void *arg)
{
    /* a few pages of its own stack and a mapping of its own */
    volatile char deep[64 * 1024];
    memset((char *)deep, (int)(uintptr_t)arg, sizeof(deep));
    char *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return (void *)0;
    for (int i = 0; i < (1 << 20); i += 4096)
        m[i] = (char)(uintptr_t)arg;
    long sum = deep[100] + m[4096 * 7];
    munmap(m, 1 << 20);
    return (void *)sum;
}

int main(void)
{
    char *map[3];
    int high = 1, kept = 1;
    for (int k = 0; k < 3; k++) {
        map[k] = mmap(0, GIB, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map[k] == MAP_FAILED) {
            printf("mapping %d of a gigabyte: refused\n", k + 1);
            return 1;
        }
        high &= (uintptr_t)map[k] >= (512UL << 30);
        for (unsigned long off = 0; off < GIB; off += STEP)
            map[k][off] = (char)(k * 64 + off / STEP);
    }
    for (int k = 0; k < 3; k++)
        for (unsigned long off = 0; off < GIB; off += STEP)
            kept &= map[k][off] == (char)(k * 64 + off / STEP);
    printf("three gigabytes mapped, above 512 GiB: %s\n", high ? "yes" : "no");
    printf("a byte every 16 MiB read back intact: %s\n", kept ? "yes" : "no");

    /* given back, and zeros afterwards */
    memset(map[0], 0x5A, 64 * 4096);
    madvise(map[0], 64 * 4096, MADV_DONTNEED);
    int zero = 1;
    for (int i = 0; i < 64 * 4096; i++)
        zero &= map[0][i] == 0;
    printf("madvise(MADV_DONTNEED) reads back zeros: %s\n", zero ? "yes" : "no");

    /* fork: the child sees the parent's memory, and writes its own */
    map[1][STEP * 3] = 'P';
    pid_t pid = fork();
    if (pid == 0) {
        int saw = map[1][STEP * 3] == 'P';
        map[1][STEP * 3] = 'C';
        _exit(saw && map[1][STEP * 3] == 'C' ? 7 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    printf("a forked child sees the parent's memory there: %s\n",
           WIFEXITED(status) && WEXITSTATUS(status) == 7 ? "yes" : "no");
    printf("and what it writes stays its own: %s\n",
           map[1][STEP * 3] == 'P' ? "yes" : "no");

    pthread_t t[4];
    long total = 0;
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], 0, work, (void *)(i + 1));
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        total += (long)r;
    }
    printf("four threads ran and came back: %s\n", total == 20 ? "yes" : "no");

    for (int k = 0; k < 3; k++)
        munmap(map[k], GIB);
    return 0;
}
