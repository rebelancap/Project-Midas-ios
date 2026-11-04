#ifndef _INITBONDDATADEFAULTS_H_
#define _INITBONDDATADEFAULTS_H_
#include <ultra64.h>

void sets_a_bunch_of_BONDdata_values_to_default(void);
#if defined(LEFTOVERDEBUG) && !defined(PORT) /* round 2: donor lv.c includes <stdlib.h> on 64-bit */
extern void exit(void);
#endif

#endif
