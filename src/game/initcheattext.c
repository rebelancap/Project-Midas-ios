#include <ultra64.h>
#include "initcheattext.h"
#include "player.h"
#include "cheat.h"

void initCheatTextBuffer(void) {
    int i;
    
#ifndef TARGET_N64
    /* the whole array: the port's own cheats sit past Rare's 75 */
    for(i=0;i<(int) sizeof(g_CheatPlayerTextRelated);i++)
#else
    for(i=0;i<75;i++)
#endif
    {
        g_CheatPlayerTextRelated[i] = 0;
    }

}


void disableOnscreenCheatText(void)
{
  g_CurrentPlayer->cheatInputBufferIndex = 0;
  g_CurrentPlayer->cheatInputCount = 0;
}

