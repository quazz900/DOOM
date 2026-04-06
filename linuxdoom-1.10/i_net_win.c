#include <stdlib.h>
#include <string.h>

#include "d_event.h"
#include "d_net.h"
#include "doomstat.h"
#include "i_net.h"
#include "i_system.h"
#include "m_argv.h"

void I_InitNetwork(void)
{
    doomcom = malloc(sizeof(*doomcom));

    if (!doomcom)
        I_Error("I_InitNetwork: failed to allocate doomcom");

    memset(doomcom, 0, sizeof(*doomcom));

    if (M_CheckParm("-net"))
        I_Error("This Windows build does not support network play yet");

    netgame = false;
    doomcom->id = DOOMCOM_ID;
    doomcom->ticdup = 1;
    doomcom->numplayers = 1;
    doomcom->numnodes = 1;
    doomcom->deathmatch = false;
    doomcom->consoleplayer = 0;
}

void I_NetCmd(void)
{
    doomcom->remotenode = -1;
}
