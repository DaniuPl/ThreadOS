/* Putting the ARM64 kernel's device tree reader through damaged trees
 *
 * kernelarm64/fdt.c is compiled here as it is, with the sanitizers
 * watching. The tree is read from a buffer of exactly the file's
 * length, and that length is what the reader is told it may read - so
 * a header that claims more than is there, and anything that believes
 * it, shows up as a read past an allocation.
 *
 * It prints what it found, one line, so the fuzzing script can hold the
 * undamaged trees to the memory and cores they were made with.
 *
 * Usage: fdt_harness <tree file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fdt.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tree file>\n", argv[0]);
        return 2;
    }
    static unsigned char scratch[1 << 20];
    FILE *fh = fopen(argv[1], "rb");
    if (!fh) {
        perror(argv[1]);
        return 2;
    }
    size_t len = fread(scratch, 1, sizeof(scratch), fh);
    fclose(fh);

    /* At least the four bytes of the magic, which fdt_valid reads
       before anything is known about the size */
    size_t alloc = len < 4 ? 4 : len;
    unsigned char *tree = calloc(1, alloc);
    memcpy(tree, scratch, len);

    struct fdt_info fi;
    int r = fdt_scan(tree, (uint32_t)len, &fi);
    /* The command line is a pointer into the tree: it has to point
       inside it, and end there */
    const char *args = fi.bootargs ? fi.bootargs : "-";
    if (fi.bootargs && ((const unsigned char *)fi.bootargs < tree ||
                        (const unsigned char *)fi.bootargs >= tree + len ||
                        !memchr(fi.bootargs, 0,
                                tree + len - (const unsigned char *)fi.bootargs)))
        args = "(outside the tree)";
    printf("r=%d mem=%llu MiB base=0x%llx cpus=%d gic=v%d dist=0x%llx "
           "cpu=0x%llx args=%s psci=%s ids=", r,
           (unsigned long long)(fi.mem_total >> 20),
           (unsigned long long)fi.mem_base, fi.cpus, fi.gic_version,
           (unsigned long long)fi.gic_dist, (unsigned long long)fi.gic_cpu,
           args, fi.psci == 1 ? "hvc" : fi.psci == 2 ? "smc" : "-");
    for (int i = 0; i < fi.cpus && i < 8; i++)
        printf("%s%llx", i ? "," : "", (unsigned long long)fi.cpu_id[i]);
    printf("\n");
    free(tree);
    return 0;
}
