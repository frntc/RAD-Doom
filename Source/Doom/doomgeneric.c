#include "doomgeneric.h"
#include "../mempool_c.h"

uint32_t* DG_ScreenBuffer = 0;


void dg_Create()
{
	DG_ScreenBuffer = Rmalloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);

	DG_Init();
}

