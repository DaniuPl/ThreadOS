#ifndef THREADOS_USER_H
#define THREADOS_USER_H

#include <stdint.h>
#include "../../user/userctl.h"

/* Who is at the machine, and what that lets them do
 *
 * Until now everything that ran could do everything: change the
 * settings the system reads at boot, overwrite the swap file, delete
 * the file the desktop keeps its icon positions in. There was nobody
 * to ask, because there was nobody - one machine, one set of powers.
 *
 * Accounts have two roles: an ADMIN may change the system, and a plain
 * user may use it. Each account has a UID and private home directory;
 * TFS checks ownership on every path and inode operation. There are no
 * groups or per-file permission bits.
 *
 * A process carries its level and passes it to its children. The
 * kernel shell carries its own, because it is not a process. The way
 * up is a password, and the way back down is free - going down is
 * never something to check.
 */

/* The account file is read after the filesystem and machine settings. */
void user_accounts_load(void);
int user_setup_required(void);
int user_account_setup(const char *name, const char *display_name,
                       const char *password, const char *theme);
int user_auto_login(void);
int user_current_passwordless(void);
int user_account_count(void);
int user_account_info(int index, char *name, int name_size,
                      int *uid, int *level);
int user_account_add(const char *name, const char *password, int level,
                     const char *admin_password);
int user_account_delete(const char *name, const char *admin_password);
int user_account_set_level(const char *name, int level,
                           const char *admin_password);
int user_login(const char *name, const char *password);
void user_session_guest(void);
int user_reauth(const char *password);
int user_password_current_ok(const char *password);
const char *user_current_name(void);
const char *user_current_display_name(void);

/* The files the system itself reads. A plain user may look into them;
   writing to them or deleting them is changing the system. */
int user_is_system_file(const char *name);

/* Is that password the admin's? Wrong answers are counted, and after a
   few of them the machine stops answering for a while: a password of
   this length would otherwise be a matter of seconds for a program in
   ring 3. */
int user_password_ok(const char *password);

/* Changes the current account's password after the caller verifies it. */
int user_set_password(const char *password);
int user_set_password_for(const char *name, const char *password,
                          const char *admin_password);

/* The kernel shell's own level (it is not a process, so it has no
   task to carry it) */
int user_shell_level(void);
void user_shell_set(int level);
int user_shell_uid(void);
void user_shell_set_uid(int uid);

#endif
