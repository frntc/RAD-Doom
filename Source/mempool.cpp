/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
        - RAD-DOOM an acceleRADor for running Doom on a C64/C128
 Copyright (c) 2022, 2023 Carsten Dachsbacher <frenetic@dachsbacher.de>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
#include <circle/util.h>
#include <stdlib.h>
#include "linux/kernel.h"
#include "mempool.h"

#define STD_MALLOC

#ifndef STD_MALLOC
static unsigned char mempool[ 64 * 1024 * 1024 ];
static unsigned int cnt = 0;
#endif

extern "C" void *Rmalloc (size_t nSize)
{
#ifdef STD_MALLOC
	return malloc( nSize );
#else
	void *ptr = (void*)&mempool[ cnt ];
	cnt += nSize;
	return ptr;
#endif
}

extern "C" void Rfree (void *pBlock)
{
}

extern "C" void *Rcalloc (size_t nBlocks, size_t nSize)
{
#ifdef STD_MALLOC
	return calloc( nBlocks, nSize );
#else
	nSize *= nBlocks;
	if (nSize == 0)
	{
		nSize = 1;
	}
	assert (nSize >= nBlocks);

	void *pNewBlock = (void*)&mempool[ cnt ];
	cnt += nSize;

	if (pNewBlock != 0)
	{
		memset (pNewBlock, 0, nSize);
	}

	return pNewBlock;
#endif
}

extern "C" void *Rrealloc (void *pBlock, size_t nSize)
{
#ifdef STD_MALLOC
	return realloc( pBlock, nSize );
#else
	void *ptr = (void*)&mempool[ cnt ];
	cnt += nSize;
	return ptr;
#endif
}
