#ifndef THREADOS_BOOTSPLASH_H
#define THREADOS_BOOTSPLASH_H

void boot_splash_start(int requested);
void boot_splash_update(int progress, const char *status);

#endif
