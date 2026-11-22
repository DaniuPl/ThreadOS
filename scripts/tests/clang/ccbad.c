/* A mistake on line 6, which clang on ThreadOS is to point at */
#include <stdio.h>

int main(void)
{
    printf("%d\n", undeclared_name);
    return 0;
}
