#include <stdio.h>

#include "doomdef.h"
#include "i_sound.h"
#include "w_wad.h"

FILE *sndserver = 0;
char *sndserver_filename = "";

static int next_handle = 1;

void I_SetChannels(void)
{
}

void I_SetSfxVolume(int volume)
{
    volume = volume;
}

void I_SetMusicVolume(int volume)
{
    volume = volume;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    id = vol = sep = pitch = priority;
    return next_handle++;
}

void I_StopSound(int handle)
{
    handle = 0;
}

int I_SoundIsPlaying(int handle)
{
    handle = 0;
    return 0;
}

void I_UpdateSound(void)
{
}

void I_SubmitSound(void)
{
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    handle = vol = sep = pitch = 0;
}

void I_ShutdownSound(void)
{
}

void I_InitSound(void)
{
}

void I_InitMusic(void)
{
}

void I_ShutdownMusic(void)
{
}

void I_PlaySong(int handle, int looping)
{
    handle = looping = 0;
}

void I_PauseSong(int handle)
{
    handle = 0;
}

void I_ResumeSong(int handle)
{
    handle = 0;
}

void I_StopSong(int handle)
{
    handle = 0;
}

void I_UnRegisterSong(int handle)
{
    handle = 0;
}

int I_RegisterSong(void *data)
{
    data = NULL;
    return 1;
}

int I_QrySongPlaying(int handle)
{
    handle = 0;
    return 0;
}
