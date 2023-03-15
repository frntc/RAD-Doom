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
#ifndef _raddoomdefs_h
#define _raddoomdefs_h

// use Digimax (userport) instead of SID
//#define USE_DIGIMAX

// play music via MIDI (works together with SID and Digimax)
//#define USE_MIDI
// address of MIDI device ($de00 or $de04)
#define MIDI_ADDRESS    0xDE00

// dont't change anything below this line

#define RENDER_SOUND

#ifdef USE_DIGIMAX
#define DAC_ADDRESS     0xDD01
#else
#define DAC_ADDRESS     0xD418
#endif

#define MIDI_BUF_SIZE   8192

#define SAMPLE_RATE     22050
#define NSOUNDFX        128
#define NCHANNELS       8
#define SOUND_BUF_SIZE  65536
#define SOUND_RINGBUF_SIZE	16384

#endif
 