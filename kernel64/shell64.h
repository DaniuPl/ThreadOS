#ifndef THREADOS_SHELL64_H
#define THREADOS_SHELL64_H

/* Starts the shell as a separate task. From this moment the system can
   be driven from the keyboard instead of replaying a script written
   into kmain64. */
void shell64_start(void);

#endif
