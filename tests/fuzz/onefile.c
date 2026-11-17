/* One input file, one call: the driver for fuzzers written for
 * libFuzzer (LLVMFuzzerTestOneInput) - libpng's, libjpeg-turbo's,
 * libxml2's and ours for FreeType. scripts/fuzz_parsers.py does the
 * corrupting and calls this once per case; a crash or a sanitizer's
 * report ends the program with an error, which is what it looks for.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv) __attribute__((weak));

/* libFuzzer's own mutator, which libxml2's custom mutators call: the
   corrupting is fuzz_parsers.py's here, so a case is left as it is */
size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max)
{
    (void)data;
    (void)max;
    return size;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    if (LLVMFuzzerInitialize)
        LLVMFuzzerInitialize(&argc, &argv);
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? n : 1);
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    LLVMFuzzerTestOneInput(buf, got);
    free(buf);
    return 0;
}
