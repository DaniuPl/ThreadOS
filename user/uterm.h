#ifndef THREADOS_UTERM_H
#define THREADOS_UTERM_H

#include "uapi.h"

#define UTERM_ARGS_MAX 8
#define UTERM_ARG_SIZE 32

struct uterm_args {
    char words[UTERM_ARGS_MAX][UTERM_ARG_SIZE];
    char *argv[UTERM_ARGS_MAX + 1];
    int argc;
};

static inline int uterm_split_args(const char *line, struct uterm_args *out)
{
    int i = 0;
    out->argc = 0;
    while (line[i]) {
        while (line[i] == ' ')
            i++;
        if (!line[i])
            break;
        if (out->argc == UTERM_ARGS_MAX)
            return -1;
        int n = 0;
        while (line[i] && line[i] != ' ') {
            if (n == UTERM_ARG_SIZE - 1)
                return -1;
            out->words[out->argc][n++] = line[i++];
        }
        out->words[out->argc][n] = '\0';
        out->argv[out->argc] = out->words[out->argc];
        out->argc++;
    }
    out->argv[out->argc] = (char *)0;
    return out->argc;
}

static inline int32_t uterm_start_one(const char *cmdline,
                                      const char *outfile,
                                      const char *errfile,
                                      int *input_fd)
{
    if (!input_fd || !outfile || !outfile[0])
        return -1;
    *input_fd = -1;
#ifdef __x86_64__
    sys_unlink(outfile);
    int out = (int)sys_open(outfile, O_WRITE | O_CREATE | O_TRUNC);
    if (out < 0)
        return -1;
    int err = -1;
    if (errfile && errfile[0]) {
        sys_unlink(errfile);
        err = (int)sys_open(errfile, O_WRITE | O_CREATE | O_TRUNC);
        if (err < 0) {
            sys_close(out);
            return -1;
        }
    }

    int pin[2];
    if (sys_pipe(pin) < 0) {
        sys_close(out);
        if (err >= 0)
            sys_close(err);
        return -1;
    }

    int64_t pid = sys_fork();
    if (pid == 0) {
        sys_close(pin[1]);
        if (sys_dup2(out, FD_STDOUT) < 0 ||
            (err >= 0 && sys_dup2(err, 2) < 0) ||
            sys_dup2(pin[0], FD_STDIN) < 0)
            sys_exit(127);
        sys_close(out);
        if (err >= 0)
            sys_close(err);
        sys_close(pin[0]);
        struct uterm_args args = { 0 };
        if (uterm_split_args(cmdline, &args) <= 0)
            sys_exit(127);
        sys_exec(args.argv[0], args.argv);
        sys_exit(127);
    }
    sys_close(out);
    if (err >= 0)
        sys_close(err);
    sys_close(pin[0]);
    if (pid < 0) {
        sys_close(pin[1]);
        return -1;
    }
    *input_fd = pin[1];
    return (int32_t)pid;
#else
    (void)errfile;
    int out = (int)sys_open(outfile, O_WRITE | O_CREATE | O_TRUNC);
    if (out < 0)
        return -1;
    sys_close(out);
    return sys_spawn_talk(cmdline, outfile, input_fd);
#endif
}

static inline int32_t uterm_write_input(int fd, const void *buf, uint32_t len)
{
    return fd < 0 ? -1 : sys_write(fd, buf, len);
}

static inline int32_t uterm_read_new(const char *path, uint32_t *shown,
                                     char *buf, uint32_t cap)
{
    if (!path || !path[0] || !shown || !buf || !cap)
        return -1;
    int fd = (int)sys_open(path, O_READ);
    if (fd < 0)
        return -1;
    if (*shown)
        sys_seek(fd, *shown);
    int32_t n = sys_read(fd, buf, cap);
    if (n > 0)
        *shown += (uint32_t)n;
    sys_close(fd);
    return n;
}

#endif
