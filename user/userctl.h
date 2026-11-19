/* Shared account system call contract between the kernel and user programs. */

#ifndef THREADOS_USERCTL_H
#define THREADOS_USERCTL_H

#include <stdint.h>

#define USER_ADMIN 0
#define USER_PLAIN 1
#define USER_NAME_MAX 15
#define USER_DISPLAY_NAME_MAX 31
#define USER_PASSWORD_MAX 128
#define USER_ACCOUNT_MAX 24

#define USER_CTL_LIST 0
#define USER_CTL_ADD 1
#define USER_CTL_DELETE 2
#define USER_CTL_PASSWORD 3
#define USER_CTL_ROLE 4
/* How one's own locked screen opens. INFO: level = the way
   (0 password, 1 PIN, 2 swipe), index = 1 when a PIN is set. SET:
   level = the way, password = a new PIN or "", admin_password = one's
   own password. -> 0, -2 wrong password, -3 not a PIN. */
#define USER_CTL_UNLOCK_INFO 5
#define USER_CTL_UNLOCK 6
/* Who is signed in: name = the account's name, index = 1 when its home
   folder (/home/<name>) is there to keep things in - a program that
   remembers something for the person (Weft, its cookies) keeps it
   there, and with 0 keeps nothing. */
#define USER_CTL_WHOAMI 7

struct user_ctl {
    int32_t op, index, uid, level;
    char name[USER_NAME_MAX + 1];
    char password[USER_PASSWORD_MAX + 1];
    char admin_password[USER_PASSWORD_MAX + 1];
};

#endif
