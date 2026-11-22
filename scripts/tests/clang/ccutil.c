/* The second of two files (ccmain.c) */
unsigned long long cc_collatz_steps(unsigned long long n)
{
    unsigned long long steps = 0;
    while (n != 1) {
        n = (n & 1) ? 3 * n + 1 : n / 2;
        steps++;
    }
    return steps;
}
