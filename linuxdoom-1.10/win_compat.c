#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>

int access(const char *path, int mode)
{
    return _access(path, mode);
}

int close(int fd)
{
    return _close(fd);
}

int dup(int fd)
{
    return _dup(fd);
}

char *getcwd(char *buffer, int maxlen)
{
    return _getcwd(buffer, maxlen);
}

long lseek(int fd, long offset, int origin)
{
    return _lseek(fd, offset, origin);
}

int mkdir(const char *path, int mode)
{
    mode = mode;
    return _mkdir(path);
}

int open(const char *filename, int flags, ...)
{
    int mode = 0;
    va_list args;

    va_start(args, flags);
    if (flags & _O_CREAT)
        mode = va_arg(args, int);
    va_end(args);

    return _open(filename, flags, mode);
}

int read(int fd, void *buffer, unsigned int count)
{
    return _read(fd, buffer, count);
}

int write(int fd, const void *buffer, unsigned int count)
{
    return _write(fd, buffer, count);
}
