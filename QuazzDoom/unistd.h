#ifndef DOOM_UNISTD_H
#define DOOM_UNISTD_H

#include <stdio.h>

#ifndef R_OK
#define R_OK 4
#endif

int access(const char *path, int mode);
int close(int fd);
int dup(int fd);
char *getcwd(char *buffer, int maxlen);
long lseek(int fd, long offset, int origin);
int mkdir(const char *path, int mode);
int open(const char *filename, int flags, ...);
int read(int fd, void *buffer, unsigned int count);
int write(int fd, const void *buffer, unsigned int count);

#endif
