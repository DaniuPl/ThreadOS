/* bigprog: a program larger than the kernel would read into its heap
 *
 * A program on the disk used to be read into the kernel's heap whole
 * before it was loaded, which held such programs to 2 MiB; WebKit's
 * are tens of megabytes. The kernel now reads a larger one straight
 * into its own pages (kernel64/elf64.c, elf64_load_file). This program
 * is several megabytes of known bytes, in each kind of segment:
 *
 *   - read-only data (5 MiB), as code and constants are;
 *   - writable data (1 MiB), which the program changes and which has
 *     to have arrived as the file has it;
 *   - zeroed data (3 MiB, .bss), which is not in the file at all.
 *
 * It says the SHA-256 of the first, of the second after inverting every
 * byte, and whether the third is all zeros; scripts/tests/t_bigprog.py
 * makes the same bytes on the host (scripts/mkblob.py) and holds the
 * answers to hashlib's.
 *
 * `bigprog exec` then replaces itself with itself (execv), which is the
 * other way the kernel loads a program (kernel64/proc64.c) - the way
 * WebKit's processes are started.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <openssl/sha.h>

/* The bytes, from BUILD/gen (the assembler's -I) */
__asm__(".section .rodata\n"
        ".balign 64\n"
        ".global ro_blob\n"
        "ro_blob:\n"
        ".incbin \"bigprog-ro.bin\"\n"
        ".global ro_blob_end\n"
        "ro_blob_end:\n"
        ".section .data\n"
        ".balign 64\n"
        ".global rw_blob\n"
        "rw_blob:\n"
        ".incbin \"bigprog-rw.bin\"\n"
        ".global rw_blob_end\n"
        "rw_blob_end:\n"
        ".previous\n");

extern const unsigned char ro_blob[], ro_blob_end[];
extern unsigned char rw_blob[], rw_blob_end[];

static unsigned char zeros[3 << 20];

static void say(const char *what, const unsigned char *p, size_t n)
{
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256(p, n, h);
    printf("%s %zu ", what, n);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        printf("%02x", h[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    say("rodata", ro_blob, (size_t)(ro_blob_end - ro_blob));

    size_t n = (size_t)(rw_blob_end - rw_blob);
    for (size_t i = 0; i < n; i++)
        rw_blob[i] ^= 0xFF;
    say("data", rw_blob, n);

    size_t bad = sizeof(zeros);
    for (size_t i = 0; i < sizeof(zeros); i++)
        if (zeros[i]) {
            bad = i;
            break;
        }
    if (bad == sizeof(zeros))
        printf("bss %zu zero\n", sizeof(zeros));
    else
        printf("bss not zero at %zu\n", bad);
    /* and it is memory the program can use */
    memset(zeros, 0x5A, sizeof(zeros));
    printf("bss written %d\n", zeros[sizeof(zeros) - 1]);
    if (argc > 1 && !strcmp(argv[1], "exec")) {
        printf("exec\n");
        fflush(stdout);
        execl("bigprog", "bigprog", (char *)0);
        printf("exec failed\n");
        return 1;
    }
    printf("done\n");
    return 0;
}
