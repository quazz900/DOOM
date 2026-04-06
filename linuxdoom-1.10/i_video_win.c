#include <string.h>

#include "doomdef.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

static byte current_palette[256 * 3];

void I_ShutdownGraphics(void)
{
}

void I_StartFrame(void)
{
}

void I_StartTic(void)
{
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_SetPalette(byte *palette)
{
    memcpy(current_palette, palette, sizeof(current_palette));
}

void I_InitGraphics(void)
{
    if (!screens[0])
        screens[0] = I_AllocLow(SCREENWIDTH * SCREENHEIGHT);
}
