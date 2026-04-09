#ifndef DOOM_WIN32_COMPAT_H
#define DOOM_WIN32_COMPAT_H

#ifdef _WIN32

#include <ctype.h>
#include <malloc.h>

#define alloca _alloca
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strdup _strdup

#endif

#endif
