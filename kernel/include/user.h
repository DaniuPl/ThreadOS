#ifndef PROTONOS_USER_H
#define PROTONOS_USER_H

#include <stdint.h>

/* Who is at the machine, and what that lets them do
 *
 * Until now everything that ran could do everything: change the
 * settings the system reads at boot, overwrite the swap file, delete
 * the file the desktop keeps its icon positions in. There was nobody
 * to ask, because there was nobody - one machine, one set of powers.
 *
 * There are now two: an ADMIN, who may change the system, and a plain
 * user, who may do everything else. That is the whole of it - no
 * groups, no owners per file, no list of rights. Two levels are enough
 * to say the thing worth saying: that changing the system is a
 * different act from using it.
 *
 * A process carries its level and passes it to its children. The
 * kernel shell carries its own, because it is not a process. The way
 * up is a password, and the way back down is free - going down is
 * never something to check.
 */

#define USER_ADMIN 0
#define USER_PLAIN 1

/* The files the system itself reads. A plain user may look into them;
   writing to them or deleting them is changing the system. */
int user_is_system_file(const char *name);

/* Is that password the admin's? Wrong answers are counted, and after a
   few of them the machine stops answering for a while: a password of
   this length would otherwise be a matter of seconds for a program in
   ring 3. */
int user_password_ok(const char *password);

/* Changes the password. Only for someone who is already an admin -
   which the caller checks, because only the caller knows who it is. */
int user_set_password(const char *password);

/* The kernel shell's own level (it is not a process, so it has no
   task to carry it) */
int user_shell_level(void);
void user_shell_set(int level);

#endif
