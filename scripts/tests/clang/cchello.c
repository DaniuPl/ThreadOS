/* The first program clang builds on ThreadOS (scripts/tests/t_clang.py) */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("hello from clang on ThreadOS: argc=%d argv[1]=%s\n",
           argc, argc > 1 ? argv[1] : "-");
    return 0;
}
