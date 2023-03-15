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
#include <circle/startup.h>
#include <circle/memio.h>
#include <circle/memory.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/types.h>
#include <circle/util.h>
#include <circle/bcm2835.h>
#include <circle/gpioclock.h>
#include <circle/gpiopin.h>
#include <circle/gpiopinfiq.h>
#include <circle/gpiomanager.h>

#include "lowlevel_arm64.h"
#include "gpio_defs.h"
#include "helpers.h"
#include "c64screen.h"

#define STATUS_MESSAGES

#define RUN_FLAGS	    0xff0000
#define RUN_REBOOT	    0x010000
#define RUN_MEMEXP	    0x020000
#define SAVE_IMAGE	    0x030000
#define RESET_DETECTED  0x040000

extern u8 isC128;
extern int meType; // 0 = REU, 1 = GEORAM, 2 = NONE
extern int meSize0, meSize1;

extern u16  getResetVector();

extern void setStatusMessage( char *msg, const char *tmp );
extern int  hijackC64( bool alreadyInDMA = false );
extern int  hijackDOOM( bool alreadyInDMA = false );
extern void initHijack();
extern u8   checkIfMachineRunning();
