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

#define SHOW_INTRO

// required for C128
#define FORCE_RESET_VECTORS

#include "rad_doom.h"
#include "rad_doom_defs.h"
#include "c64screen.h"
#include "linux/kernel.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <circle/usertimer.h>

static const char DRIVE[] = "SD:";
static const char FILENAME_CONFIG[] = "SD:RAD/rad.cfg";

// REU (sort of legacy of the actual RAD software)
#include "rad_reu.h"
#define REU_MAX_SIZE_KB	(16384)
u8 mempool[ REU_MAX_SIZE_KB * 1024 + 8192 ] AAA = {0};

// low-level communication code
u64 armCycleCounter;
#include "lowlevel_dma.h"

static u16 resetVector = 0xFCE2;

// emulate GAME-cartridge to start C128 (also works on C64) with custom reset-vector => forces C128 in C64 mode
void startForcedResetVectors()
{
	register u32 g2, g3;

	const u8 romh[] = { 
		0x4c, 0x0a, 0xe5, 0x4c, 0x00, 0xe5, 0x52, 0x52,
		0x42, 0x59, 0x43, 0xfe, 0xe2, 0xfc, 0x48, 0xff };

	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)startForcedResetVectors, 1024 * 4 );
	CACHE_PRELOADL1STRM( romh );
	FORCE_READ_LINEARa( (void*)startForcedResetVectors, 1024 * 4, 65536 );
	FORCE_READ_LINEARa( (void*)romh, 16, 1024 );

	OUT_GPIO( DMA_OUT );
	OUT_GPIO( GAME_OUT );

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( 100 );
	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bOE_Dx | bRW_OUT | bDMA_OUT | bDIR_Dx );
	INP_GPIO_RW();
	INP_GPIO_IRQ();

	CLR_GPIO( bGAME_OUT );
	CLR_GPIO( bMPLEX_SEL );

	DELAY( 1 << 20 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );

	u32 nCycles = 0, nRead = 0;
	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( 50 );
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		if ( nCycles ++ > 100000 )
		{
			OUT_GPIO( RESET_OUT );
			CLR_GPIO( bRESET_OUT );
			DELAY( 1 << 18 );
			SET_GPIO( bRESET_OUT );
			INP_GPIO( RESET_OUT );
			nRead = nCycles = 0;
		}

		if ( ROMH_ACCESS && CPU_READS_FROM_BUS )
		{
			u8 d = 0;
			if ( ADDRESS0to7 == 0xfc || ADDRESS0to7 == 0xfd ) nRead ++;
			if ( ADDRESS0to7 == 0xfc ) d = resetVector & 255;
			if ( ADDRESS0to7 == 0xfd ) d = resetVector >> 8;

			{
				register u32 DD = ( ( d ) & 255 ) << D0;
				write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
				write32( ARM_GPIO_GPSET0, DD );
				SET_BANK2_OUTPUT
				WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ );
				SET_GPIO( bOE_Dx | bDIR_Dx );
			}

			if ( nRead >= 2 )
			{
				WAIT_FOR_VIC_HALFCYCLE
				SET_GPIO( bGAME_OUT );
				break;
			}
		}
		WAIT_FOR_VIC_HALFCYCLE
		RESET_CPU_CYCLE_COUNTER
	}
}


#include "rad_doom_hijack.h"

unsigned fiqRegOffset;
u32		 fiqRegMask;
u32		 temperature;

extern "C" void startDoom();

CTimer				*pTimer;

u64 lastTick = 0, firstTick = 0;
u32 firstTimer = 1;

#define CLOCKHZ	1000000

u64 globalTime = 0;

extern "C" uint64_t GetuSec()
{
	InstructionSyncBarrier ();
	u64 nCNTPCT, nCNTFRQ;
	asm volatile ("mrs %0, CNTPCT_EL0" : "=r" (nCNTPCT));
	asm volatile ("mrs %0, CNTFRQ_EL0" : "=r" (nCNTFRQ));
	return (unsigned) (nCNTPCT * CLOCKHZ / nCNTFRQ);
}

extern "C" uint64_t GetTickCount()
{
	if ( firstTimer )
	{
		firstTimer = 0;
		firstTick = lastTick = GetuSec();
		return 10;
	}
	u64 t = GetuSec();
	u64 r = t - firstTick;
	return r / 1000;
}

CUserTimer 			*sidtimer;
CInterruptSystem	*pInterrupt;

extern "C" void prepareC64()
{
	f_unmount( "SD:" );
	DisableIRQs();

	// only called to initialize timing values
	REU_SIZE_KB = 128;
	initREU( mempool );

	#ifdef FORCE_RESET_VECTORS
	resetVector = 0xfce2;
	#endif

#ifndef SHOW_INTRO
	checkIfMachineRunning();		
	DELAY( 1 << 27 );
	hijackDOOM( false );
#endif

	EnableIRQs();
}

#define SOUND_PLAY_IRQ
#define SAMPLE_RATE 22050
#define SOUND_RINGBUF_SIZE	16384

uint64_t soundFirstuSecTick = 0;

extern const unsigned char *mahoneyLUT;
extern u32 nWAVSamples;
u8 *wavMemory;
u32 wavPosition = 0;

#ifdef SOUND_PLAY_IRQ
#define PEEK( a, v ) { emuReadByteREU_p1( g2, a ); emuReadByteREU_p2( g2 ); emuReadByteREU_p3( g2, v, false ); }
#define POKE( a, v ) { emuWriteByteREU_p1( g2, a, v ); emuWriteByteREU_p2( g2, false ); }

static int nextMouseUpdate = 0, mouseReadStep = 0;
		extern "C" void handleMouseUpdate( uint8_t *mouseData );

void sidSamplePlayIRQ( CUserTimer *pSIDTimer, void *pParam )
{
	uint64_t curTickuSec = GetuSec();
	uint64_t curSoundPos = ( curTickuSec - soundFirstuSecTick ) * SAMPLE_RATE / 1000000ULL;

	extern unsigned char soundRingBuffer[ SOUND_RINGBUF_SIZE ];
	unsigned char s = soundRingBuffer[ curSoundPos & (SOUND_RINGBUF_SIZE-1) ];

	register u32 g2;
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

	POKE( DAC_ADDRESS, s );

	pSIDTimer->Start( USER_CLOCKHZ / 22050 );
}
#endif

extern "C" void startSIDSamplePlayer()
{
#ifdef SOUND_PLAY_IRQ
	sidtimer->Start( 100 );
#endif
}

extern "C" void stopSIDSamplePlayer()
{
#ifdef SOUND_PLAY_IRQ
	sidtimer->Stop();
#endif
}

#ifdef SHOW_INTRO

unsigned char logo[ 320 * 124 * 360 ];
unsigned char scroller[ 11264 * 22 ];
unsigned char loading[ 160 * 200 ];

const unsigned char ditherMatrix4x4[ 4 * 4 ] = {
	 0, 12,  3, 15,
	 8,  4, 11,  7,
	 2, 14,  1, 13,
	10,  6,  9,  5 };

const uint8_t ditherMatrix4x4_line[ 4 * 4 ] = {
	 0,  4,  2,  6,
	 8, 12, 10, 14,
	 3,  7,  1,  5,
	11, 15,  9, 13 };
	
extern int palette_pepto[ 16 ][ 3 ];

extern "C" int (*functionAddress[]) (void);

extern "C" { void printC64( const char *t, int x_, int y, unsigned int color ); }

void doIntro()
{
	wavMemory = new u8[ 8192 * 1024 ];
	
	FILE *f = fopen( "SD:RADDOOM/dazzler_ex.wav", "rb" );
	fseek( f, 0, SEEK_END );
	u32 s = ftell( f );
	fseek( f, 0, SEEK_SET );
	fread( wavMemory, 1, s, f );
	
	extern void convertWAV2RAW_inplace( u8 *_data );
	convertWAV2RAW_inplace( wavMemory );
	wavPosition = 0;

	f = fopen( "SD:RADDOOM/logo.ani", "rb" );
	fread( logo, 1, 320 * 124 * 360, f );
	fclose( f );

	f = fopen( "SD:RADDOOM/scroller.raw", "rb" );
	fread( scroller, 1, 11264 * 22, f );
	fclose( f );

	f = fopen( "SD:RADDOOM/loadingscreen.raw", "rb" );
	fread( loading, 1, 160 * 200, f );
	fclose( f );

	// only called to initialize timing values
	REU_SIZE_KB = 128;
	initREU( mempool );

	DisableIRQs();
	checkIfMachineRunning();		
	DELAY( 1 << 27 );
	hijackDOOM( false );

	extern unsigned char soundRingBuffer[ SOUND_RINGBUF_SIZE ];
	memset( soundRingBuffer, 0, SOUND_RINGBUF_SIZE );

	(*functionAddress[0])();

	soundFirstuSecTick = GetuSec();
	EnableIRQs();
	sidtimer->Start( 100 );

	u32 curRingBufferPos = 0;
	u32 soundFirstTick = GetTickCount(), framesSoFar = 0;

	int introFC = 0, fadeOut = -1;

	const u8 logoC64colors[8]  = { 0,  6, 11, 14,  3, 13,  1, 1 };
	const u8 logoC64colors4[8] = { 0, 11, 12, 15, 13,  1,  1 };
	//const u8 logoC64colors[8] = { 0, 11, 12, 15, 13,  7,  1 };
	const u8 logoC64colors2[8] = { 0,  9,  2,  8, 10,  7,  1 };
	const u8 logoC64colors3[8] = { 0,  6,  4, 14, 10,  7,  1 };
	const u8 loadingcolors[8] = { 0,  6,  4, 10, 7,  1,  1 };
	//const u8 logoC64colors[8] = { 0, 11, 12, 10, 15,  7,  1 };

	const u8 *logoColor = logoC64colors;
	int blink = 0;

	int tabSC[ 768 ];
	for ( int i = 0; i < 768; i++ )
		tabSC[ i ] = sin( i / 512.0f * 2.0f * 3.14159265359f ) * 255.0f + 256.0f;

	while ( 1 )
	{
		int globalFade = 256;
		if ( fadeOut >= 0 )
		{
			globalFade = max( 0, 256 - fadeOut );
			fadeOut += 2;
		}

		uint64_t curTick = GetuSec();
		uint64_t totalFrames = SAMPLE_RATE * (curTick - soundFirstuSecTick) / 1000000UL;
		totalFrames += SAMPLE_RATE * 200 / 1000;
		s32 num_frames = totalFrames - framesSoFar;

		for ( s32 i = 0; i < num_frames; i++ )
		{
			soundRingBuffer[ curRingBufferPos % SOUND_RINGBUF_SIZE ] = mahoneyLUT[ ( (int)wavMemory[ curRingBufferPos % nWAVSamples ] * globalFade ) >> 8 ];
//				soundRingBuffer[ curRingBufferPos % SOUND_RINGBUF_SIZE ] = mahoneyLUT[ 127+(( (int)wavMemory[ curRingBufferPos % nWAVSamples ] * globalFade ) >> 16) ];
			curRingBufferPos ++;
		}
		framesSoFar += num_frames;

		extern unsigned int *DG_ScreenBuffer;
		memset( DG_ScreenBuffer, 0, 320 * 200 * 4 );

		int logopos = 0;
		const u8 *dm;
		int dmask, dshift, dofs, dmul = 3;
		dm = ditherMatrix4x4;
		dmask = 3; 
		dshift = 2; 
		dofs = 7; 

		static int fr = 0;
		static int logoAngle = 0, logoSpeed = 0;

		if ( blink ) -- blink;

		static int scrollPos = -384;
		if ( introFC == 384 )
		{
			blink = 64;
			scrollPos = -200;
		}

		if ( introFC >= 384 )
		{
			scrollPos ++;
			logoSpeed ++;
			logoColor = logoC64colors4;
		}

		if ( logoSpeed > 128 ) logoSpeed = 128;
		logoAngle += logoSpeed;
		
		fr = logoAngle >> 7;

		fr %= 360;

		for ( int y = 0; y < 124; y ++ )
		{
			int lineFade = max( 0, min( 256, (y - 124) + introFC ) ) + blink * 4;

			for ( int x = 0; x < 320; x += 2 )
			//if ( x + logopos >= 0 && x + logopos < 320 )
			{
				int l = (int)logo[ (x>>1) + y * 160 + fr * 160 * 124 ];
				l += l;

				l = ( l * lineFade * globalFade ) >> 16;

				if ( fr & 1 )
					l += ( (int)dm[ ( (x>>1) & dmask ) + ( ( y & dmask ) << dshift ) ] - dofs ) * dmul; else
					l += ( -(int)dm[ ( (x>>1) & dmask ) + ( ( y & dmask ) << dshift ) ] + dofs ) * dmul;

				if ( l < 0 ) l = 0;
				l /= 64;
				if ( l > 6 ) l = 6;

				DG_ScreenBuffer[ x + logopos + y * 320 ] = ( logoColor[ l ] << 24 );
			}
		}


		//
		// simple scroller (bitmap copy/coloring + blurred reflection)
		//
		int mtime = scrollPos;

		u8 txt[ 320 * 50 ];
		u8 txt2[ 320 * 50 ];

		memset( txt, 0, 320 * 50 );
		memset( txt2, 0, 320 * 50 );

		for ( int i = 0; i < 160; i++ )
		{
			int x = i + ((mtime/1) % 11264);
			if ( x >= 0)
			{
				for ( int j = 0; j < 22; j++ )
				{
					int xofs = ( ( tabSC[ (8 * mtime+j*32)&511 ] - 256 ) * (21-j) ) >> 10;
					int yy = j;
					int l = 255-scroller[ x + yy * 5632 ];

					// coloring
					l *= ( tabSC[ ( tabSC[ ( ( i + mtime ) & 511 ) + 256 ] * 2 + i * 2 + 4 * j + 512 ) & 511 ] - 256 ) / 9 + 170;

					// fade to side borders
					l *= min( 32, min( i, 161 - i ) );
					l = ( l * globalFade ) >> 8;

					txt[ i + j * 320 ] = txt[ i + (xofs*(21-j+12)/33) + ( 45 - j ) * 320 ] = l >> 13;
				}
			}
		}

		u8 gauss[] = { 31, 126, 198, 126, 31 };

		// some passes of separable Gauss-blur
		for ( int pass = 0; pass < 10; pass ++ )
		{
			int yo = 2 + pass * 6 / 2;
			for ( int j = 0; j < min( yo, 23 ); j++ )
				for ( int i = 2; i < 160 - 2; i++ )
					txt2[ i + ( 23 + j ) * 320 ] = txt[ i + ( 23 + j ) * 320 ];

			for ( int j = yo; j < 23; j++ )
			{
				for ( int i = 2; i < 160 - 2; i++ )
				{
					unsigned int v;
					v  = gauss[ 0 ] * txt[ i - 2 + ( 24 + j ) * 320 ];
					v += gauss[ 1 ] * txt[ i - 1 + ( 24 + j ) * 320 ];
					v += gauss[ 2 ] * txt[ i + ( 24 + j ) * 320 ];
					v += gauss[ 3 ] * txt[ i + 1 + ( 24 + j ) * 320 ];
					v += gauss[ 4 ] * txt[ i + 2 + ( 24 + j ) * 320 ];

					txt2[ i + ( 23 + j ) * 320 ] = v >> 9;
				}
			}

			int yo2 = yo * 2 - 0;
			for ( int j = 0; j < min( yo2, 23 ); j++ )
				for ( int i = 2; i < 160 - 2; i++ )
					txt[ i + ( 24 + j ) * 320 ] = txt2[ i + ( 24 + j ) * 320 ];

			for ( int j = yo2; j < 23; j++ )
			{
				for ( int i = 2; i < 160 - 2; i++ )
				{
					unsigned int v;
					if ( j >= 2 ) v  = gauss[ 0 ] * txt2[ i + ( 24 + j - 2 ) * 320 ];
					if ( j >= 1 ) v += gauss[ 1 ] * txt2[ i + ( 24 + j - 1 ) * 320 ];
					v += gauss[ 2 ] * txt2[ i + ( 24 + j ) * 320 ];
					if ( 24 + j + 1 <= 45 ) v += gauss[ 3 ] * txt2[ i + ( 24 + j + 1 ) * 320 ];
					if ( 24 + j + 2 <= 45 ) v += gauss[ 4 ] * txt2[ i + ( 24 + j + 2 ) * 320 ];

					txt[ i + ( 24 + j ) * 320 ] = v >> 9;
				}
			}
		}

		for ( int j = 0; j < 44; j++ )
		{
			for ( int i = 0; i < 160; i++ )
			{
				int l = txt[ i + j * 320 ] << 1;

				if ( fr & 1 )
					l += ( (int)dm[ ( i & dmask ) + ( ( j & dmask ) << dshift ) ] - dofs ) * dmul; else
					l += ( -(int)dm[ ( i & dmask ) + ( ( j & dmask ) << dshift ) ] + dofs ) * dmul;

				l >>= 6;
				if ( l < 0 ) l = 0;
				if ( l > 6 ) l = 6;

				int aa = logoC64colors2[ l ];
				if ( j > 22 )
					aa = logoC64colors[ l ];

				DG_ScreenBuffer[ i*2 + ( j + 144 ) * 320 ] = 
				DG_ScreenBuffer[ i*2+1 + ( j + 144 ) * 320 ] = ( aa << 24 );
			}
		}

		// oh no, we're faster than 50 Hz, better wait :)
		uint64_t waitStart = curTick;
		do {
			curTick = GetuSec();
		} while ( curTick - waitStart < 1000 * 12 );

		int key = (*functionAddress[1])();

		if ( key ) fadeOut = 0;

		if ( fadeOut >= 256 )
		{
			for ( int y = 0; y < 200; y ++ )
			{
				for ( int x = 0; x < 320; x += 2 )
				{
					int l = (int)loading[ (x>>1) + y * 160 ];
					l += l;

					l += ( (int)dm[ ( (x>>1) & dmask ) + ( ( y & dmask ) << dshift ) ] - dofs ) * dmul; 

					if ( l < 0 ) l = 0;
					l /= 64;
					if ( l > 6 ) l = 6;

					DG_ScreenBuffer[ x + y * 320 ] = ( loadingcolors[ l ] << 24 );
				}

			}

			(*functionAddress[1])();
			(*functionAddress[1])();
			(*functionAddress[1])();
			break;
		}

		introFC ++;
	}
	memset( soundRingBuffer, 0, SOUND_RINGBUF_SIZE );

	EnableIRQs();
}

#endif

void  CRAD::Run( void )
{
	gpioInit();
	m_EMMC.Initialize();

	setDefaultTimings( AUTO_TIMING_RPI3PLUS_C64C128 );
	readConfig( logger, DRIVE, FILENAME_CONFIG );

	OUT_GPIO( RESET_OUT );
	CLR_GPIO( bRESET_OUT );
	DELAY( 1 << 25 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );

	pTimer = &m_Timer;
	pInterrupt = &m_Interrupt;

	u64 nCNTFRQ;
	asm volatile ( "mrs %0, CNTFRQ_EL0" : "=r" ( nCNTFRQ ) );
	assert( nCNTFRQ % HZ == 0 );
	u64 m_nClockTicksPerHZTick = nCNTFRQ / HZ;
	u64 nCNTPCT;
	asm volatile ( "mrs %0, CNTPCT_EL0" : "=r" ( nCNTPCT ) );
	asm volatile ( "msr CNTP_CVAL_EL0, %0" :: "r" ( nCNTPCT + m_nClockTicksPerHZTick ) );
	asm volatile ( "msr CNTP_CTL_EL0, %0" :: "r" ( 1 ) );

	FATFS mFileSystem;
	if ( f_mount( &mFileSystem, "SD:", 1 ) != FR_OK )
		logger->Write( "RD", LogError, "failed mounting partition 'SD:'" );

	m_CPUThrottle.SetSpeed( CPUSpeedMaximum );

	sidtimer = new CUserTimer( &m_Interrupt, sidSamplePlayIRQ, this, !true );
	sidtimer->Initialize();

#ifdef SHOW_INTRO
	doIntro();

	extern void restartIncrementalBlitter();
	restartIncrementalBlitter();
#endif

	startDoom();
}

extern "C" void radMountFileSystem()
{
	FATFS mFileSystem;
	if ( f_mount( &mFileSystem, "SD:", 1 ) != FR_OK )
		logger->Write( "RD", LogError, "failed mounting partition 'SD:'" );
}

extern "C" void radUnmountFileSystem()
{
	f_unmount( "SD:" );
}

__attribute__((optimize("align-functions=256"))) void CRAD::FIQHandler( void *pParam )
{
}

int main( void )
{
	CRAD kernel;
	if ( kernel.Initialize() )
		kernel.Run();

	halt();
	return EXIT_HALT;
}

