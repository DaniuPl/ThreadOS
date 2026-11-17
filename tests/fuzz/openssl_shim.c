/* OpenSSL's fuzzers (fuzz/ in its archive) are written to its own
 * interface - FuzzerInitialize, FuzzerTestOneInput, FuzzerCleanup -
 * and reach libFuzzer's through its driver.c only in a build configured
 * for libFuzzer. This is that bridge, for tests/fuzz/onefile.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int FuzzerInitialize(int *argc, char ***argv);
int FuzzerTestOneInput(const uint8_t *buf, size_t len);
void FuzzerCleanup(void);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    /* what OpenSSL sets up once (its providers, the fuzzers' fixed
       random generator) would otherwise be reported as leaked */
    atexit(FuzzerCleanup);
    return FuzzerInitialize(argc, argv);
}

int LLVMFuzzerTestOneInput(const uint8_t *buf, size_t len)
{
    return FuzzerTestOneInput(buf, len);
}
