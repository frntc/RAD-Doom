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
#include "rad_doom_defs.h"
#include "rad_doom_hijack.h"
#include "rad_reu.h"
#include "linux/kernel.h"
#include <circle/machineinfo.h>

#define PLAY_MUSIC

static u64 armCycleCounter;

u8 isC128 = 0;
u8 isC64 = 0;	// only set once we're sure

// isNTSC == 0 => PAL: 312 rasterlines, 63 cycles
// isNTSC == 1 => NTSC: 262 (0..261) rasterlines, 64 cycles, 6567R56A
// isNTSC == 2 => NTSC: 263 (0..262) rasterlines, 65 cycles, 6567R8
u8 isNTSC = 0;
u8 isRPiZero2 = 0;

u8 justBooted = 0;
char SIDKickVersion[ 64 ] = {0};

extern void *pFIQ;
extern void warmCache( void *fiqh );

#include "lowlevel_dma.h"

u8 SIDType;

const unsigned char *mahoneyLUT;

unsigned char SwinSIDLUT[ 256 ];

#ifdef PLAY_MUSIC
u32 nWAVSamples = 0;
void convertWAV2RAW_inplace( u8 *_data );
static u32 wavPosition = 0;
static u8 *wavMemory;
#endif

#include "mahoney_lut.h"
#include "font.h"

static u32 g2, g3;

#define WAIT_FOR_RASTERLINE( r )					\
	do {											\
		emuReadByteREU( y, 0xd012, false, {} );		\
		emuReadByteREU( x, 0xd011, false, {} );		\
		t = ( x & 128 ) << 1; t |= y;				\
	} while ( t != r );

#define BUS_RESYNC {			\
	WAIT_FOR_CPU_HALFCYCLE		\
	WAIT_FOR_VIC_HALFCYCLE		\
	RESTART_CYCLE_COUNTER }

#define POKE( a, v ) { BUS_RESYNC; emuWriteByteREU_p1( g2, a, v ); emuWriteByteREU_p2( g2, false ); }
#define PEEK( a, v ) { emuReadByteREU_p1( g2, a ); emuReadByteREU_p2( g2 ); emuReadByteREU_p3( g2, v, false ); }

#define FASTBLIT

#ifdef FASTBLIT
#define MPOKE( a, v ) { emuWriteByteMany_p1( g2, a, v ); emuWriteByteMany_p2( g2, false ); }
#else
#define MPOKE( a, v ) { emuWriteByteREU_p1( g2, a, v ); emuWriteByteREU_p2( g2, false ); }
#endif

volatile u8 forceRead;

void SPOKE( u16 a, u8 v )
{
	BUS_RESYNC
	POKE( a, v );
}

void SPEEK( u16 a, u8 &v )
{
	BUS_RESYNC
	PEEK( a, v )
}

void NOP( u32 nCycles )
{
	for ( u32 i = 0; i < nCycles; i++ )
		BUS_RESYNC
}

u8 detectSID()
{
	u8 y;
	BUS_RESYNC
	POKE( 0xd412, 0xff );
	POKE( 0xd40e, 0xff );
	POKE( 0xd40f, 0xff );
	POKE( 0xd412, 0x20 );
	NOP( 3 );
	PEEK( 0xd41b, y );
	// (y==2) -> 8580 // (y==3) -> 6581
	if ( y == 2 ) return 8580 & 255;
	if ( y == 3 ) return 6581 & 255;
	return 0;
}

const unsigned char keyTable[ 64 ] = 
{
	VK_DELETE, '3',        '5', '7', '9', '+', '?', '1',
	VK_RETURN, 'W',        'R', 'Y', 'I', 'P', '*', 95,
	VK_RIGHT,  'A',        'D', 'G', 'J', 'L', ';', 0,
	VK_F7,     '4',        '6', '8', '0', '-', VK_HOME, '2',
	VK_F1,     'Z',        'C', 'B', 'M', '.', VK_SHIFT_R, VK_SPACE,
	VK_F3,     'S',        'F', 'H', 'K', ':', '=', VK_COMMODORE,
	VK_F5,     'E',        'T', 'U', 'O', '@', '^', 'Q',
	VK_DOWN,   VK_SHIFT_L, 'X', 'V', 'N', ',', '/', 0, 
};

#include "C64Side/ultimax_init.h"

u8 checkIfMachineRunning()
{
	justBooted = 1;

	bool running = false;

	RESET_CPU_CYCLE_COUNTER
	u64 start, duration;
	do {
		WAIT_FOR_CPU_HALFCYCLE
		WAIT_FOR_VIC_HALFCYCLE
		READ_CYCLE_COUNTER( start );
		for ( u32 i = 0; i < 1000; i++ )
		{
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
		}
		READ_CYCLE_COUNTER( duration );
		duration -= start;

		// target value about 1000 * 1400 (+/- depending on PAL/NTSC, RPi clock speed)
		if ( duration > 1200000 && duration < 1600000)
			running = true;
	} while ( !running );

	return 1;
}


void checkForC128()
{
	// check if we're running on a C128
	isC128 = 0;
	u8 x, y;
	SPEEK( 0xd030, y );
	if ( y == 0xff )
	{
		SPOKE( 0xd030, 0xfc );
		SPEEK( 0xd030, x );
		if ( x == 0xfc )
		{
			SPOKE( 0xd030, 0xff );
			isC128 = 1;
		}
	} else
		isC128 = 1;

	SPOKE( 0xd030, y );

	if ( !isC128 ) isC64 = 1;

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
}

void checkForNTSC()
{
	isNTSC = 0;

	BUS_RESYNC

	u8 y;
	u16 curRasterLine;
	u16 maxRasterLine = 0;
	u16 lastRasterLine = 9999;

	for ( int i = 0; i < 313; i++ )
	{
		do {
			SPEEK( 0xd012, y );
			curRasterLine = y;
		} while ( curRasterLine == lastRasterLine );
		lastRasterLine = curRasterLine;

		SPEEK( 0xd011, y );
		if ( y & 128 ) curRasterLine += 256;

		if ( curRasterLine > maxRasterLine )
			maxRasterLine = curRasterLine;
	}

	if ( maxRasterLine < 300 )
		isNTSC = maxRasterLine - 260;

	// isNTSC == 0 => PAL: 312 rasterlines, 63 cycles
	// isNTSC == 1 => NTSC: 262 (0..261) rasterlines, 64 cycles, 6567R56A
	// isNTSC == 2 => NTSC: 263 (0..262) rasterlines, 65 cycles, 6567R8
}

void checkForRPiZero()
{
	isRPiZero2 = 0;
	if ( CMachineInfo::Get()->GetMachineModel() == MachineModelZero2W )
	{
		//rpiHasAudioJack = false;
		isRPiZero2 = 1;
	}
}


void waitAndHijack( register u32 &g2 )
{
	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER
	WAIT_FOR_VIC_HALFCYCLE

	u32 cycles = 0;
	do
	{
		emuWAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( TIMING_BA_SIGNAL_AVAIL );
		g2 = read32( ARM_GPIO_GPLEV0 );
		cycles ++;
	} while ( ( g2 & bBA ) && cycles < 25000 );

	emuWAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	// now we are in a badline ...
	// ... and it is safe to assert DMA ...
	WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA ); // 80ns after falling Phi2
	OUT_GPIO( DMA_OUT );
	CLR_GPIO( bDMA_OUT );


	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

	checkForC128();
	checkForNTSC();
}

#include "C64Side/ultimax_memcfg.h"

void startWithUltimax( bool doReset = true )
{
	register u32 g2, g3;
	u8 nNOPs = 0;

	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bOE_Dx | bRW_OUT );
	INP_GPIO( RW_OUT );
	INP_GPIO( IRQ_OUT );
	OUT_GPIO( RESET_OUT );
	OUT_GPIO( GAME_OUT );
	CLR_GPIO( bRESET_OUT | bGAME_OUT | bDMA_OUT );

	CACHE_PRELOAD_DATA_CACHE( &ultimax_memcfg[ 0 ], 256, CACHE_PRELOADL2KEEP )
	FORCE_READ_LINEAR32a( &ultimax_memcfg, 256, 256 * 8 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( && ultimaxCRTCFG, 1024 );

	DELAY( 1 << 20 );

ultimaxCRTCFG:
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER
	WAIT_FOR_VIC_HALFCYCLE
	SET_GPIO( bRESET_OUT | bDMA_OUT );
	INP_GPIO( RESET_OUT );

	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER						
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS+ TIMING_OFFSET_CBTD );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		if ( ADDRESS_FFxx && CPU_READS_FROM_BUS )
		{
			u8 addr = ADDRESS0to7;
			u8 D = ultimax_memcfg[ addr ];

			register u32 DD = ( ( D ) & 255 ) << D0;
			write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
			write32( ARM_GPIO_GPSET0, DD );
			SET_BANK2_OUTPUT
			WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ );
			SET_GPIO( bOE_Dx | bDIR_Dx );

			if ( D == 0xEA ) nNOPs ++;
		}

		WAIT_FOR_VIC_HALFCYCLE

		if ( nNOPs > 12 )
			return;
	}
}


void waitAndHijackMenu( register u32 &g2 )
{
	if ( !isC64 )
		startWithUltimax();
	SET_GPIO( bGAME_OUT );

	{
		OUT_GPIO( DMA_OUT );
		SET_GPIO( bDMA_OUT );

		CLR_GPIO( bMPLEX_SEL );
		WAIT_FOR_CPU_HALFCYCLE
		BEGIN_CYCLE_COUNTER
		WAIT_FOR_VIC_HALFCYCLE

		u32 cycles = 0;
		do
		{
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			RESTART_CYCLE_COUNTER
			WAIT_UP_TO_CYCLE( TIMING_BA_SIGNAL_AVAIL );
			g2 = read32( ARM_GPIO_GPLEV0 );
			cycles ++;
		} while ( ( g2 & bBA ) && cycles < 250000 );

		emuWAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA ); // 80ns after falling Phi2
		CLR_GPIO( bDMA_OUT );
	}

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

	checkForC128();
	checkForNTSC();
}

u16 getResetVector()
{
	register u32 g2;
	u8 x;
	u16 vec;

	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bGAME_OUT | bOE_Dx | bRW_OUT );
	INP_GPIO( RW_OUT );
	INP_GPIO( IRQ_OUT );
	OUT_GPIO( RESET_OUT );
	CLR_GPIO( bRESET_OUT );

	CLR_GPIO( bGAME_OUT );
	CLR_GPIO( bMPLEX_SEL );
	DELAY( 1 << 18 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );

	// wait until CPU runs
	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS + 10 );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		if ( ADDRESS_FFxx && CPU_READS_FROM_BUS && ADDRESS0to7 == 0xfc )
		{
			SET_GPIO( bGAME_OUT );
			break;
		}
	}

	// now hijack computer and get reset vector
	waitAndHijack( g2 );

	SPEEK( 0xfffc, x );	vec = x;
	SPEEK( 0xfffd, x );	vec |= (u16)x << 8;

	return vec;
}

void readKeyDoom( unsigned int *kbEvents, unsigned char *nEvents )
{
	static int firstKeyScan = 1;
	static u8 prevMatrix[ 8 ];

	*nEvents = 0;

	SPOKE( 0xdc02, 0xff );	// port a ddr (output)

	u8 matrix[ 8 ];
	u8 x, y, v1, v2, a = 1;
	for ( u8 i = 0; i < 8; i++ )
	{
		// this is just overly redundant to make sure all read values are correct
		SPOKE( 0xdc00, ~a );
		SPOKE( 0xdc00, ~a );
		do {
			SPEEK( 0xdc01, v1 );
			SPEEK( 0xdc01, v2 );
		} while ( v1 != v2 );
		matrix[ i ] = v1;
		a <<= 1;
	}

	SPOKE( 0xdc00, 0 );

	if ( firstKeyScan )
	{
		firstKeyScan = 0;
		memcpy( prevMatrix, matrix, 8 );
	}

	int k = 0;
	for ( int i = 0; i < 8; i++ )
		for ( a = 0; a < 8; a++ )
			if ( ( ( matrix[ a ] >> i ) & 1 ) != ( ( prevMatrix[ a ] >> i ) & 1 ) )
			{
				kbEvents[ *nEvents ] = keyTable[ i * 8 + a ];

				if ( ( ( matrix[ a ] >> i ) & 1 ) == 0 ) // key down
					kbEvents[ *nEvents ] |= 256;

				(*nEvents) ++;
			}

	memcpy( prevMatrix, matrix, 8 );
}

uint8_t supportDAC = 0;
uint8_t hasSIDKick = 0;


extern "C" unsigned char getCurrentSample();

static u32 nFIQSaveLock;

extern "C" void disableInterrupts()
{
	nFIQSaveLock = read32 (ARM_IC_FIQ_CONTROL);
	write32 (ARM_IC_FIQ_CONTROL, 0);
	DisableIRQs();
}

extern "C" void enableInterrupts()
{
	EnableIRQs();
	write32 (ARM_IC_FIQ_CONTROL, nFIQSaveLock);	
}

extern "C" uint64_t getCurrentSamplePos()
{
	extern u64 soundFirstuSecTick;
	InstructionSyncBarrier();
	u64 nCNTPCT, nCNTFRQ;
	asm volatile ("mrs %0, CNTPCT_EL0" : "=r" (nCNTPCT));
	asm volatile ("mrs %0, CNTFRQ_EL0" : "=r" (nCNTFRQ));
	u64 curTickuSec = (nCNTPCT * CLOCKHZ / nCNTFRQ);
	return ( curTickuSec - soundFirstuSecTick ) * SAMPLE_RATE / 1000000UL;
}

extern "C" void radPOKE( unsigned short a, unsigned char v )
{
	SPOKE( a, v );
}

extern "C" u8 writeMIDI( unsigned short a, unsigned char v )
{
	int attemps = 0;
	u8 x = 0;
	do {
		SPEEK( a + 0x02, x );
		attemps ++;
	} while ( !( x & 2 ) && attemps < 32 );
	if ( attemps >= 32 )
		return 0;
	SPOKE( a + 0x01, v );
	return 1;
}

extern "C" void waitNC64Cycles( int n )
{
	for ( int i = 0; i < n; i++ ) { BUS_RESYNC }
}

#define TRANSFER_NEXT_SAMPLE( POK ) { \
	InstructionSyncBarrier();																					\
	asm volatile ( "mrs %0, CNTPCT_EL0" : "=r" ( nCNTPCT ) );													\
	asm volatile ( "mrs %0, CNTFRQ_EL0" : "=r" ( nCNTFRQ ) );													\
	u64 realSoundPos = ( ( nCNTPCT * CLOCKHZ / nCNTFRQ ) - soundFirstuSecTick ) * SAMPLE_RATE / 1000000ULL;		\
	if ( realSoundPos != curSoundPos ) {																		\
		u8 v = soundRingBuffer[ ( realSoundPos ) & ( SOUND_RINGBUF_SIZE - 1 ) ];								\
		CACHE_PRELOADL1STRM( &soundRingBuffer[ ( realSoundPos + 1 ) & ( SOUND_RINGBUF_SIZE - 1 ) ] );			\
		BUS_RESYNC																								\
		POK( DAC_ADDRESS, v );																						\
		curSoundPos = realSoundPos;																				\
		supd += 2000;																							\
	} }

static int firstBlit = 2;
static u8 prevScreenBuffer[ 10000 * 2 ];

void restartIncrementalBlitter()
{
	firstBlit = 2;
}

extern "C" void handleMouseUpdate( uint8_t *mouseData );

extern "C" void blitScreenDOOM( unsigned char *koalaData, unsigned int *kbEvents, unsigned char *nEvents, unsigned char *mouseData )
{
	static u8 fc = 0;
	register u32 g2;

	u32 curAddr = 0x2000;
	s32 pos = 0;	// bitmap scan pos
	s32 spos = 0;	// screenRAM scan pos
	s32 cpos = 0;	// colorRAM scan pos
	s32 totalTransfer = 0;

	u32 spanIdx = 0;
	//u32 spanBeginColorRAM = 0; // only for debugging
	u32 spanList[ 20000 ];
	u32 prevScreenOfs = (fc & 1) ? 10000 : 0;


	static s32 invalidateRow = 0, invalidateRow2 = 21;
	invalidateRow = ( invalidateRow + 1 ) % 25;
	invalidateRow2 ++; if ( invalidateRow2 >= 25 ) invalidateRow2 = 21;
	#define INVALIDATE_RECT( x ) \
		( !( x % 320 ) || \
		  ( (x) > invalidateRow2 * 320 && (x) < ((invalidateRow2+1) * 320) ) || \
		  ( (x) > invalidateRow * 320 && (x) < ((invalidateRow+1) * 320) ) )

	if ( firstBlit )
	{
		firstBlit --;
		spanList[ 0 ] = ( 0 << 18 ) | ( 8000 << 2 ) | 0;
		spanList[ 1 ] = ( 8000 << 18 ) | ( 9000 << 2 ) | 1;
		spanList[ 2 ] = ( 9000 << 18 ) | ( 10000 << 2 ) | 2;
		spanIdx = 3;
	} else
	{
		// 320 byte of bitmap data corresponds to 40 byte of screen + color RAM data
		while ( pos < 8000 )
		{
			// search next byte that is different
			while ( pos < 8000 && prevScreenBuffer[ pos + prevScreenOfs ] == koalaData[ pos ] && !INVALIDATE_RECT( pos ) )	{ pos ++; }
			// search next byte that is equal
			s32 pos2 = pos + 1;
			while ( pos2 < 8000 && ( prevScreenBuffer[ pos2 + prevScreenOfs ] != koalaData[ pos2 ] || INVALIDATE_RECT( pos2 ) ) )	{ pos2 ++; }

			u32 flag = 0;

			if ( pos < 8000 )
			{
				spanList[ spanIdx++ ] = ( pos << 18 ) | ( pos2 << 2 ) | flag;
				totalTransfer += pos2 - pos;
			}

			pos = pos2;

			// ok, scanned some bitmap data, catch up with screen and color RAM data
			s32 end = pos / 8;
			flag = 1;
			while ( spos < end )
			{
				while ( spos < end && prevScreenBuffer[ 8000+spos + prevScreenOfs ] == koalaData[ 8000+spos ] && !INVALIDATE_RECT( spos * 8 ) ) { spos ++; }
				s32 pos2 = spos + 1;
				while ( pos2 < end && ( prevScreenBuffer[ 8000+pos2 + prevScreenOfs ] != koalaData[ 8000+pos2 ] || INVALIDATE_RECT( pos2 * 8 ) ) ) { pos2 ++; }
				spanList[ spanIdx++ ] = ( (8000+spos) << 18 ) | ( (8000+pos2) << 2 ) | flag;
				totalTransfer += pos2 - spos;
				spos = pos2;
			}
/*
			flag = 2;
			while ( cpos < end )
			{
				while ( cpos < end && prevScreenBuffer[ 9000+cpos ] == koalaData[ 9000+cpos ] && !INVALIDATE_RECT( cpos * 8 ) ) { cpos ++; }
				s32 pos2 = cpos + 1;
				while ( pos2 < end && ( prevScreenBuffer[ 9000+pos2 ] != koalaData[ 9000+pos2 ] || INVALIDATE_RECT( pos2 * 8 ) ) ) { pos2 ++; }
				spanList[ spanIdx++ ] = ( (9000+cpos) << 18 ) | ( (9000+pos2) << 2 ) | flag;
				totalTransfer += pos2 - cpos;
				cpos = pos2;
			}*/

			if ( pos >= 8000 ) break;
		}

		//spanBeginColorRAM = spanIdx;
		u32 flag = 2;
		while ( cpos < 1000 )
		{
			while ( cpos < 1000 && prevScreenBuffer[ 9000+cpos ] == koalaData[ 9000+cpos ] && !INVALIDATE_RECT( cpos * 8 ) ) { cpos ++; }
			s32 pos2 = cpos + 1;
			while ( pos2 < 1000 && ( prevScreenBuffer[ 9000+pos2 ] != koalaData[ 9000+pos2 ] || INVALIDATE_RECT( pos2 * 8 ) ) ) { pos2 ++; }
			spanList[ spanIdx++ ] = ( (9000+cpos) << 18 ) | ( (9000+pos2) << 2 ) | flag;
			totalTransfer += pos2 - cpos;
			cpos = pos2;
		}
	}

	u32 nFIQSave = read32 (ARM_IC_FIQ_CONTROL);
	write32 (ARM_IC_FIQ_CONTROL, 0);

	DisableIRQs();

	readKeyDoom( kbEvents, nEvents );

	// prepare reading mouse
	unsigned char tmp02, tmp00;
	SPEEK( 0xdc02, tmp02 );
	SPOKE( 0xdc02, 0xc0 );
	SPEEK( 0xdc00, tmp00 );
	tmp00 = ( tmp00 & 0b00111111 ) | 0b10000000;
	SPOKE( 0xdc00, tmp00 );


	for ( int i = 0; i < 16; i++ )
	{
		void *p = (u8*)( && blitterLoop ) + i * 16;
		CACHE_PRELOADIKEEP( p );
	}

	// copying = 10000 cycles (+ badlines etc)
	// 22050 Hz Sample Rate => output sample after 985240 / 22050 cycles = 44.68 cycles
	// say copying takes net 11500 cycles (very conservative estimate!), then we need to fetch ~257 samples
	extern u64 soundFirstuSecTick;
	InstructionSyncBarrier();
	u64 nCNTPCT, nCNTFRQ;
	asm volatile ("mrs %0, CNTPCT_EL0" : "=r" (nCNTPCT));
	asm volatile ("mrs %0, CNTFRQ_EL0" : "=r" (nCNTFRQ));
	u64 curTickuSec = (nCNTPCT * CLOCKHZ / nCNTFRQ);
	u64 curSoundPos = ( curTickuSec - soundFirstuSecTick ) * SAMPLE_RATE / 1000000UL;
	u64 preloadSoundPos = curSoundPos & ~63;

	extern unsigned char soundRingBuffer[ SOUND_RINGBUF_SIZE ];
	CACHE_PRELOADL1STRM( &soundRingBuffer[ (preloadSoundPos+  0) & (SOUND_RINGBUF_SIZE-1) ] );
	CACHE_PRELOADL1STRM( &soundRingBuffer[ (preloadSoundPos+ 64) & (SOUND_RINGBUF_SIZE-1) ] );
	CACHE_PRELOADL1STRM( &soundRingBuffer[ (preloadSoundPos+128) & (SOUND_RINGBUF_SIZE-1) ] );
	CACHE_PRELOADL1STRM( &soundRingBuffer[ (preloadSoundPos+192) & (SOUND_RINGBUF_SIZE-1) ] );
	CACHE_PRELOADL1STRM( &soundRingBuffer[ (preloadSoundPos+256) & (SOUND_RINGBUF_SIZE-1) ] );

	int supd = 0;

	armCycleCounter = 0;
	RESET_CPU_CYCLE_COUNTER 

	// very safe way of reading the current rasterline
	u8 y;
	readRasterLine:
	SPEEK( 0xd012, y );
	u16 curRasterLine = y;
	SPEEK( 0xd011, y );
	if ( y & 128 ) curRasterLine += 256;

	SPEEK( 0xd012, y );
	u16 curRasterLine2 = y;
	SPEEK( 0xd011, y );
	if ( y & 128 ) curRasterLine2 += 256;

	curRasterLine2 -= curRasterLine;

	supd += 4000;

	if ( curRasterLine2 >= 1 ) goto readRasterLine;

	// we transfer 'totalTransfer' bytes, this will take approx. ...
	// - cycles per 8 scanlines = 63*7+23*1=464
	// - totalTransfer*8/464 scanlines
	// we want to end up between 251..312 and 0..51
	u32 scanlinesNeeded = totalTransfer * 8 / 464;
	u32 endRasterLine = curRasterLine + scanlinesNeeded;
	if ( endRasterLine >= 312 ) endRasterLine -= 312;

	int updMouse = 0;

	if ( supd > 44682 )
	{
		supd -= 44682;
		TRANSFER_NEXT_SAMPLE( SPOKE )

#if 0
		//if ( ++ updMouse >= 200 )
		{
			uint8_t a, mouseData[ 3 ], tmp00;

			updMouse = 0;
			SPEEK( 0xd419, mouseData[ 0 ] );
			SPEEK( 0xd41a, mouseData[ 1 ] );
			SPEEK( 0xdc01, a );
			mouseData[ 2 ] = ( a >> 2 ) & 3;
			handleMouseUpdate( mouseData );
		}
#endif
	}

	if ( scanlinesNeeded < 110 )
	{
		if ( curRasterLine < 255 ) goto readRasterLine;
	} else
	{
		if ( !( endRasterLine < 51 || endRasterLine > 260 ) ) goto readRasterLine;
	}

	u16 base = ( fc & 1 ) ? 0x4000 : 0;

	blitterLoop:
#ifdef FASTBLIT
	emuPrepareManyWrites( g2 );
#endif

	BUS_RESYNC

	//MPOKE(0xd020, 14 );

	for ( register u32 s = 0; s < spanIdx; s++ )
	{
		/*if ( s == spanBeginColorRAM )
		{
			MPOKE(0xd020, 5);
		}*/

		u32 flag = spanList[ s ] & 3;
		u32 from = ( spanList[ s ] >> 18 ) & 16383;
		u32 to   = ( spanList[ s ] >> 2  ) & 16383;

		u32 addr = 0x2000 + base;
		if ( flag == 1 ) addr = 0x0400 - 8000 + base;
		if ( flag == 2 ) addr = 0xd800 - 9000;

		for ( register u32 i = from; i < to; i++ )
		{
			u8 d = koalaData[ i ];
			//if ( ( fc & 1 ) == 1 ) d = 0;
			MPOKE( addr + i, d );

			supd += 1020*4; // check regularly
			if ( supd > 44682 )
			{
				supd -= 44682;
				TRANSFER_NEXT_SAMPLE( MPOKE )
			}
		}
	}

/*	MPOKE(0xd020, 0);
	MPOKE(0xd020, 0);
	MPOKE(0xd020, 0);*/

#ifdef FASTBLIT
	emuEndManyWrites( g2 );
#endif

#ifdef USE_MIDI
	extern unsigned short midiAddr;
	extern unsigned short midiRingBuf[ 8192 ];
	extern unsigned short midiCur, midiLast;

	int nWrites = 0;
	while ( midiCur != midiLast && nWrites < 64 )
	{
		if ( writeMIDI( midiAddr, midiRingBuf[ midiCur ] ) )
		{
			midiCur ++;
			midiCur &= 8191;
			nWrites ++;
		} else
			break;
	}
#endif

	SPOKE( 0xd011, 0x3b );
	SPOKE( 0xd018, 0x18 );
	SPOKE( 0xd016, 0x18 /*+ (1-(fc&1))*/ );
	
	if ( fc & 1 )
		{SPOKE( 0xdd00, 0x02 + 4 );} else
		{SPOKE( 0xdd00, 0x03 + 4 );}

	SPOKE( 0xd021, koalaData[ 10000 ] );
	SPOKE( 0xd021, koalaData[ 10000 ] );

	uint8_t a, b;
/*	do {
		SPEEK( 0xd419, a );
		SPEEK( 0xd419, b );
	} while ( a != b );
	mouseData[ 0 ] = a;
	do {
		SPEEK( 0xd41a, a );
		SPEEK( 0xd41a, b );
	} while ( a != b );
	mouseData[ 1 ] = b;
	do {
		SPEEK( 0xdc01, a );
		SPEEK( 0xdc01, b );
	} while ( a != b );
	mouseData[ 2 ] = ( a >> 2 ) & 3;*/

	SPEEK( 0xd419, mouseData[ 0 ] );
	SPEEK( 0xd41a, mouseData[ 1 ] );
	// SPEEK( 0xdc01, a );
	// mouseData[ 2 ] = ( a >> 2 ) & 3;

	SPOKE( 0xdc02, tmp02 );
	SPOKE( 0xdc02, tmp02 );

	SPOKE( 0xdc00, 0xff );
	SPOKE( 0xdc00, 0xff );

	SPEEK( 0xdc00, a );
	a = ~a;
	mouseData[ 2 ] = ( (a&1) << 1 ) | ( (a&16) >> 4 );

	SPOKE( 0xdc02, 0 );
	SPOKE( 0xdc00, 0 );

	// color RAM is not double buffered => update it in both previous screens
	memcpy( prevScreenBuffer + prevScreenOfs, koalaData, 10000 );
	memcpy( prevScreenBuffer + 9000, koalaData+9000, 1000 );

	fc ++;

	EnableIRQs();

	write32 (ARM_IC_FIQ_CONTROL, nFIQSave);	
}

int hijackDOOM( bool alreadyInDMA )
{
	checkForRPiZero();

	DisableFIQs();
	DisableIRQs();
	
	register u32 g2;
	u8 x;

	if ( !alreadyInDMA )
		waitAndHijackMenu( g2 );

	checkForC128();
	checkForNTSC();

	justBooted = 0;

	BUS_RESYNC
	SPOKE( 0xD418, 0 );

	SIDKickVersion[ 0 ] = 0;

	// initialize CIA2
	SPOKE( 0xdd02, 0x3f );
	SPOKE( 0xdd0d, 0x7f );
//	SPOKE( 0xdd03, 0x06 );
	SPOKE( 0xdd03, 0xff );
	SPOKE( 0xdd01, 0x06 );

	PEEK( 0xdd00, x );
	x |= 4;
	SPOKE( 0xdd00, x );

	const u8 vic[] = { 
		0, 0, 0, 0, 0, 0, 0, 0, 
		0, 0, 0, 0, 0, 0, 0, 0, 
		0, 0x1B-0x10, 0, 0, 0, 0, 8, 0, 
		0x14*0+8, 0, 0, 0, 0, 0, 0, 0,
		0*14, 6*0, 1, 2, 3, 4, 0, 1, 
		2, 3, 4, 5, 6, 7
	};

	for ( int j = 0; j < 46; j++ )
		SPOKE( 0xd000 + j, vic[ j ] );

	// init SID
	BUS_RESYNC
	for ( int i = 0; i < 32; i++ )
		SPOKE( 0xd400 + i, 0 );

	supportDAC = hasSIDKick = 0;

	int j = 0; 
	while ( j++ < 16 && SIDType == 0 )
	{
		POKE( 0xd41f, 0xff );
		for ( int i = 0; i < 16 + 16; i++ )
		{
			POKE( 0xd41e, 224 + i );
			PEEK( 0xd41d, *(u8*)&SIDKickVersion[ i ] );
			BUS_RESYNC
			BUS_RESYNC
			BUS_RESYNC
			BUS_RESYNC
		}

		if ( SIDKickVersion[ 0 ] == 0x53 &&
			 SIDKickVersion[ 1 ] == 0x49 &&
			 SIDKickVersion[ 2 ] == 0x44 &&
			 SIDKickVersion[ 3 ] == 0x4b &&
			 SIDKickVersion[ 4 ] == 0x09 &&
			 SIDKickVersion[ 5 ] == 0x03 &&
			 SIDKickVersion[ 6 ] == 0x0b )
			 {
				// found SIDKick!
				SIDKickVersion[ 16 ] = 0; 
				hasSIDKick = 1;

				// let's see if it's version 0.21 (supporting direct DAC)
				const unsigned char VERSION_STR_ext[10] = { 0x53, 0x49, 0x44, 0x4b, 0x09, 0x03, 0x0b, 0x00, 0, 21 };

				supportDAC = 1;
				// check for signature
				for ( int i = 0; i < 8; i++ )
					if ( SIDKickVersion[ i + 20 ] != VERSION_STR_ext[ i ] )
						supportDAC = 0;
				
				if ( supportDAC )
				{
					int version = SIDKickVersion[ 20 + 8 ] * 100 + SIDKickVersion[ 20 + 9 ];
					if ( version < 21 )
						supportDAC = 0;
				}

				if ( supportDAC && SIDKickVersion[ 20 + 10 ] == 0 )
					supportDAC = 0;

			 } else
			  SIDKickVersion[ 0 ] = 0;

		bool badline = false;

		do {
			u8 y;
			PEEK( 0xd012, y );
			u16 curRasterLine = y;
			do
			{
				PEEK( 0xd012, y );
			} while ( y == curRasterLine );

			badline = ( curRasterLine & 7 ) == 3;
		} while ( badline );

		u8 a1 = detectSID();
		u8 a2 = detectSID();
		u8 a3 = detectSID();

		if ( a1 == a2 && a2 == a3 )
			SIDType = a1; else		// detection succesful: 6581 or 8580
			SIDType = 0;			// no success => maybe SwinSID
	}

	#ifdef PLAY_MUSIC
	BUS_RESYNC
	if ( SIDType == 0 )
	{
		SPOKE( 0xd405, 0 );
		SPOKE( 0xd406, 0xff );
		SPOKE( 0xd40d, 0xff );
		SPOKE( 0xd414, 0xff );
		SPOKE( 0xd404, 0x49 );
		SPOKE( 0xd40b, 0x49 );
		SPOKE( 0xd412, 0x49 );
		SPOKE( 0xd40c, 0 );
		SPOKE( 0xd413, 0 );
		SPOKE( 0xd415, 0 );
		SPOKE( 0xd416, 0x10 );
		SPOKE( 0xd417, 0xf7 );
	} else
	{
		// Mahoney's technique
		SPOKE( 0xd405, 0x0f );
		SPOKE( 0xd40c, 0x0f );
		SPOKE( 0xd413, 0x0f );
		SPOKE( 0xd406, 0xff );
		SPOKE( 0xd40d, 0xff );
		SPOKE( 0xd414, 0xff );
		SPOKE( 0xd404, 0x49 );
		SPOKE( 0xd40b, 0x49 );
		SPOKE( 0xd412, 0x49 );
		SPOKE( 0xd415, 0xff );
		SPOKE( 0xd416, 0xff );
		SPOKE( 0xd417, 0x03 );

// if SIDKick: enter SID-only mode
//SPOKE( 0xd41f, 0xfd );
// SIDKick DAC-mode
		if ( supportDAC )
			SPOKE( 0xd41f, 0xfc );

	}

	if ( SIDType == 0 )
	{
		for ( int i = 0; i < 256; i++ )
			SwinSIDLUT[ i ] = i >> 4;
		mahoneyLUT = SwinSIDLUT;
	} else
		mahoneyLUT = ( SIDType == (6581 & 255) ) ? lookup6581 : lookup8580;

	if ( supportDAC )
	{
		for ( int i = 0; i < 256; i++ )
			SwinSIDLUT[ i ] = i;
		mahoneyLUT = SwinSIDLUT;
	}
	#endif

#ifdef USE_DIGIMAX
	SIDType = 0;
	for ( int i = 0; i < 256; i++ )
		SwinSIDLUT[ i ] = i;
	mahoneyLUT = SwinSIDLUT;
#endif

	SPOKE( 0xdc03, 0 );		// port b ddr (input)
	SPOKE( 0xdc02, 0xff );	// port a ddr (output)

	// multicolor bitmap
	SPOKE( 0xd020, 0 );
	SPOKE( 0xd021, 0 );

	SPOKE( 0xd011, 0x3b );
	SPOKE( 0xd018, 0x18 );
	SPOKE( 0xd016, 0x18 );
	SPOKE( 0xdd00, 0x03 + 4 );


	EnableIRQs();
	EnableFIQs();
	return 0;
}


struct WAVHEADER
{
	u8  riff[ 4 ];
	u32 filesize;
	u8  wave[ 4 ];
	u8  fmtChunkMarker[ 4 ];
	u32 fmtLength;
	u32 fmtType;
	u32 nChannels;
	u32 sampleRate;
	u32 byteRate;
	u32 blockAlign;
	u32 bpp;
	u8  dataChunkHeader[ 4 ];
	u32 dataSize;
};

static struct WAVHEADER header;

static u8 buffer4[ 4 ];
static u8 buffer2[ 2 ];

void convertWAV2RAW_inplace( u8 *_data )
{
	u8 *data = _data;
	u8 *rawOut = data;

	#define FREAD( dst, s ) { memcpy( (u8*)dst, data, s ); data += s; }

	FREAD( header.riff, sizeof( header.riff ) );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.filesize = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	FREAD( header.wave, sizeof( header.wave ) );

	FREAD( header.fmtChunkMarker, sizeof( header.fmtChunkMarker ) );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.fmtLength = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	FREAD( buffer2, sizeof( buffer2 ) );
	header.fmtType = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( buffer2, sizeof( buffer2 ) );
	header.nChannels = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.sampleRate = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	// ... = header.sampleRate;

	FREAD( buffer4, sizeof( buffer4 ) );
	header.byteRate = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	FREAD( buffer2, sizeof( buffer2 ) );

	header.blockAlign = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( buffer2, sizeof( buffer2 ) );
	header.bpp = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( header.dataChunkHeader, sizeof( header.dataChunkHeader ) );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.dataSize = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	long num_samples = ( 8 * header.dataSize ) / ( header.nChannels * header.bpp );

	long size_of_each_sample = ( header.nChannels * header.bpp ) / 8;

	// duration in secs: (float)header.filesize / header.byteRate;

#ifdef PLAY_MUSIC
	nWAVSamples = 0;
#endif

	if ( header.fmtType == 1 ) // PCM
	{
		char data_buffer[ size_of_each_sample ];

		long bytes_in_each_channel = ( size_of_each_sample / header.nChannels );

		if ( ( bytes_in_each_channel  * header.nChannels ) == size_of_each_sample ) // size if correct?
		{
			for ( u32 i = 1; i <= num_samples; i++ )
			{
				FREAD( data_buffer, sizeof( data_buffer ) );

				unsigned int  xnChannels = 0;
				int data_in_channel = 0;
				int offset = 0; // move the offset for every iteration in the loop below

				for ( xnChannels = 0; xnChannels < header.nChannels; xnChannels++ )
				{
					if ( bytes_in_each_channel == 4 )
					{
						data_in_channel = ( data_buffer[ offset ] & 0x00ff ) | ( ( data_buffer[ offset + 1 ] & 0x00ff ) << 8 ) | ( ( data_buffer[ offset + 2 ] & 0x00ff ) << 16 ) | ( data_buffer[ offset + 3 ] << 24 );
						data_in_channel += 2147483648;
						data_in_channel >>= 24;
					} else
					if ( bytes_in_each_channel == 2 )
					{
						data_in_channel = ( data_buffer[ offset ] & 0x00ff ) | ( data_buffer[ offset + 1 ] << 8 );
						data_in_channel += 32768;
						data_in_channel >>= 8;
					} else
					if ( bytes_in_each_channel == 1 )
					{
						data_in_channel = data_buffer[ offset ] & 0x00ff; // 8 bit unsigned
					}

					if ( xnChannels == 0 )
					{
						*rawOut = (u8)data_in_channel;
						rawOut ++;
						#ifdef PLAY_MUSIC
						nWAVSamples ++;
						#endif
					}

					// if stereo => mix with channel #0
					if ( xnChannels == 1 )
					{
						u16 t = *( rawOut - 1 );
						t += (u16)data_in_channel;
						*( rawOut - 1 ) = (u8)( t >> 1 );
					}

					offset += bytes_in_each_channel;
				}
			}
		}
	}
}
