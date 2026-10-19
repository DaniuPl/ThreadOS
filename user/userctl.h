/* Shared account system call contract for both kernels and user programs. */

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

struct user_ctl {
    int32_t op, index, uid, level;
    char name[USER_NAME_MAX + 1];
    char password[USER_PASSWORD_MAX + 1];
    char admin_password[USER_PASSWORD_MAX + 1];
};

#endif
