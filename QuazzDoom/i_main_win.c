#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_command)
{
    int argc;
    LPWSTR *argv_wide;
    char **argv_utf8;
    int i;
    wchar_t module_path[MAX_PATH];
    wchar_t *last_slash;

    instance = instance;
    previous = previous;
    command_line = command_line;
    show_command = show_command;

    if (GetModuleFileNameW(NULL, module_path, MAX_PATH))
    {
        last_slash = wcsrchr(module_path, L'\\');
        if (last_slash)
        {
            *last_slash = L'\0';
            SetCurrentDirectoryW(module_path);
        }
    }

    argv_wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv_wide)
        return 1;

    argv_utf8 = (char **)malloc(sizeof(*argv_utf8) * argc);
    if (!argv_utf8)
    {
        LocalFree(argv_wide);
        return 1;
    }

    for (i = 0; i < argc; i++)
    {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, argv_wide[i], -1, NULL, 0, NULL, NULL);
        argv_utf8[i] = (char *)malloc(bytes);
        if (!argv_utf8[i])
        {
            while (i-- > 0)
                free(argv_utf8[i]);
            free(argv_utf8);
            LocalFree(argv_wide);
            return 1;
        }

        WideCharToMultiByte(CP_UTF8, 0, argv_wide[i], -1, argv_utf8[i], bytes, NULL, NULL);
    }

    LocalFree(argv_wide);

    myargc = argc;
    myargv = argv_utf8;

    D_DoomMain();
    return 0;
}
