/* Two files built into one program: this one and ccutil.c */
#include <stdio.h>

unsigned long long cc_collatz_steps(unsigned long long n);

int main(void)
{
    unsigned long long best = 0, at = 0;
    for (unsigned long long n = 1; n < 100000; n++) {
        unsigned long long s = cc_collatz_steps(n);
        if (s > best) {
            best = s;
            at = n;
        }
    }
    printf("two files: the longest Collatz run below 100000 starts at %llu "
           "and takes %llu steps\n", at, best);
    return 0;
}
