#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "d_net.h"
#include "g_game.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_video.h"
#include "m_misc.h"

int mb_used = 6;

static ULONGLONG basetime_ms;
static ticcmd_t emptycmd;

void I_Tactile(int on, int off, int total)
{
    on = off = total = 0;
}

ticcmd_t *I_BaseTiccmd(void)
{
    return &emptycmd;
}

int I_GetHeapSize(void)
{
    return mb_used * 1024 * 1024;
}

byte *I_ZoneBase(int *size)
{
    *size = mb_used * 1024 * 1024;
    return (byte *)malloc(*size);
}

int I_GetTime(void)
{
    ULONGLONG now_ms = GetTickCount64();

    if (!basetime_ms)
        basetime_ms = now_ms;

    return (int)(((now_ms - basetime_ms) * TICRATE) / 1000);
}

void I_Init(void)
{
    I_InitSound();
    I_InitMusic();
}

void I_Quit(void)
{
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}

void I_WaitVBL(int count)
{
    DWORD delay_ms = (DWORD)((count * 1000) / 70);

    if (!delay_ms)
        delay_ms = 1;

    Sleep(delay_ms);
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte *I_AllocLow(int length)
{
    byte *mem = (byte *)malloc(length);

    if (mem)
        memset(mem, 0, length);

    return mem;
}

extern boolean demorecording;

void I_Error(char *error, ...)
{
    va_list argptr;

    va_start(argptr, error);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, error, argptr);
    fprintf(stderr, "\n");
    va_end(argptr);

    fflush(stderr);

    if (demorecording)
        G_CheckDemoStatus();

    D_QuitNetGame();
    I_ShutdownGraphics();
    exit(-1);
}
