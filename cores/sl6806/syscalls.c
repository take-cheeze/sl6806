/*
 * syscalls.c - the newlib bottom end.
 *
 * Enough to make malloc, snprintf and the C++ runtime work in a freestanding
 * build. stdout/stderr are wired to the console ring so printf() from plain C
 * code lands in the same place as Serial.print().
 */

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sl6806_console.h"

extern char __heap_start__, __heap_end__;

/* Bounded so a runaway allocation reports failure instead of quietly walking
 * into the stack. */
void *_sbrk(ptrdiff_t incr)
{
    static char *brk;
    char *prev;

    if (!brk)
        brk = &__heap_start__;

    if (brk + incr > &__heap_end__) {
        errno = ENOMEM;
        return (void *)-1;
    }

    prev = brk;
    brk += incr;
    return prev;
}

int _write(int fd, const void *buf, size_t len)
{
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        sl6806_console_write((const unsigned char *)buf, len);
        return (int)len;
    }
    errno = EBADF;
    return -1;
}

int _read(int fd, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    size_t n = 0;

    if (fd != STDIN_FILENO) {
        errno = EBADF;
        return -1;
    }
    while (n < len && sl6806_console_available())
        p[n++] = (unsigned char)sl6806_console_read();
    return (int)n;
}

int _close(int fd)                    { (void)fd; errno = EBADF; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _isatty(int fd)                   { (void)fd; return 1; }
int _getpid(void)                     { return 1; }
int _kill(int pid, int sig)           { (void)pid; (void)sig; errno = EINVAL; return -1; }

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

void _exit(int code)
{
    (void)code;
    for (;;)
        ;
}
