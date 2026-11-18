/* notetest: a program's notification
 *
 * SYS64_NOTIFY, the PC's call, on the phone: the title and the body
 * given on the command line (or the ones below) go to the shell, which
 * lists them in the shade and shows a banner unless do not disturb is
 * on (kernelarm64/mobile.c). scripts/tests/t_arm64apps.py starts it
 * from a package's tile and reads what the screen shows.
 */

#include <stdio.h>
#include "usys64.h"

int main(int argc, char **argv)
{
	const char *title = argc > 2 ? argv[1] : "Test";
	const char *body = argc > 2 ? argv[2] : "Hello from a program";
	long r = syscall64(SYS64_NOTIFY, (int64_t)(uintptr_t)title,
	                   (int64_t)(uintptr_t)body);
	printf("[notetest] posted: %ld\n", r);
	return r == 0 ? 0 : 1;
}
