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
        - this file implements the necessary callbacks for Doom Generic and includes most notably
          * color reduction and creation of a multicolor C64 image
          * sound/music rendering and mixing 
          * optional MIDI music rendering

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
#include "../rad_doom_defs.h"

#ifdef RENDER_SOUND
#include "z_zone.h"
#include "i_sound.h"
#include "w_wad.h"
#include "sounds.h"
#endif
#include "doomkeys.h"
#include "doomgeneric.h"
#include "m_misc.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <arm_neon.h>

extern void blitScreenDOOM( uint8_t *koalaData, uint32_t *kbEvents, uint8_t *nEvents, uint8_t *mouseData );
extern void prepareC64();
extern void disableInterrupts();
extern void enableInterrupts();
extern void radPOKE( uint16_t a, uint8_t v );

#ifdef USE_MIDI
extern void writeMIDI( uint16_t a, uint8_t v );

uint16_t midiAddr = MIDI_ADDRESS;
uint16_t midiRingBuf[ MIDI_BUF_SIZE ];
uint16_t midiCur = 0, midiLast = 0;
void musicRenderMIDI( int nSamples );

#define MIDI_CLR_BUFFER() { midiCur = midiLast = 0; }
#define MIDI_CMD( x ) { midiRingBuf[ midiLast++ ] = (x); midiLast &= 8191; }
#endif

#define max(a,b) ({          \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b; })

#define min(a,b) ({          \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b; })

#ifdef RENDER_SOUND
#define SAMPLE_RATE 22050

#define MUS_IMPLEMENTATION
#include "mus.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

typedef struct {
	uint8_t *pCur, *pEnd;
	int sfxid, handle, volL, volR;
} SOUND_CHANNEL;

static tsf      *pTSF;
static void     *pMusicRAWData;
static uint32_t musicLength, musicVolume, musicReset, musicPendingSamples;
static mus_t    *pMUS;
static float    soundMixingBuffer[ SOUND_BUF_SIZE ];

static uint8_t  soundNamePrefix;
static uint16_t soundCurHandle;
static uint32_t soundSrcRate;
static uint32_t soundSamplePos;
static int      soundLengths[ 128 ];
static SOUND_CHANNEL soundChannel[ NCHANNELS ];

void musicRender( int nSamples );
void soundRender( int nSamples );

#endif

#define KEY_QUEUE_SIZE 16

static uint16_t s_KeyQueue[ KEY_QUEUE_SIZE ];
static uint32_t s_KeyQueueWriteIndex = 0;
static uint32_t s_KeyQueueReadIndex = 0;

uint8_t *charset;

static int first = 1;

const float invGamma = 0.96111111111f;
const int   exposure = 350;

static uint8_t ditherMode = 2;
static uint8_t selectedPreset = 0;
static uint8_t alternatePattern = 1;
static int displayStatus = 0;
static int displayPreset = 0;
static int displayHelp = 0;
static int flickerMode = 0;
static int16_t brightnessScale = 20;

void setDisplayPreset( int p )
{
	switch ( p )
	{
		default: case 0:			// nothing fancy, just ordered dithering
			flickerMode = 0;
			ditherMode = 2;
			alternatePattern = 0;
			break;
		case 1:						// CRT, ordered dithering, moderate flicker
			flickerMode = 80;
			ditherMode = 2;
			alternatePattern = 0;
			break;
		case 2:						// TFT, ordered dithering, alternated pattern
			flickerMode = 256;
			ditherMode = 2;
			alternatePattern = 1;
			break;
		case 3:						// TFT, ordered dithering, NO alternated pattern
			flickerMode = 256;
			ditherMode = 2;
			alternatePattern = 0;
			break;
	};
}

static int mouseControlActive = 0;

#define VK_F1  		133
#define VK_F3  		134
#define VK_F5  		135
#define VK_F7  		136
#define VK_F8  		140
#define VK_ESC		 95
#define VK_DELETE	 20
#define VK_RETURN	 13
#define VK_SHIFT_L    1
#define VK_SHIFT_R    6
#define VK_LEFT		157
#define VK_RIGHT	 29
#define VK_UP		145
#define VK_DOWN		 17
#define VK_HOME		 19
#define VK_COMMODORE  4
#define VK_SPACE	 32

static uint8_t convertToDoomKey(uint8_t key)
{
	if ( mouseControlActive )
	{
		switch (key)
		{
		case VK_RETURN:
			key = KEY_ENTER;
			break;
		case VK_ESC:
			key = KEY_ESCAPE;
			break;
		case '@':
		case VK_UP:
			key = KEY_UPARROW;
			break;
		case ';':
		case VK_DOWN:
			key = KEY_DOWNARROW;
			break;
		case VK_SPACE:
			key = KEY_USE;
			break;
		case 'z': case 'Z':
			key = KEY_STRAFE_L;
			break;
		case 'x': case 'X':
			key = KEY_STRAFE_R;
			break;
		case VK_SHIFT_L: case VK_SHIFT_R: 
			key = KEY_RSHIFT;
			break;
		case '=': 
			key = 0; 
			break;
		case VK_F7:
			reboot();
			break;
		default:
			key = tolower(key);
			break;
		}

	} else
	{
		switch (key)
		{
		case VK_RETURN:
			key = KEY_ENTER;
			break;
		case VK_ESC:
			key = KEY_ESCAPE;
			break;
		case 'j': case 'J':
		//case 'f': case 'F':
		case VK_LEFT:
			key = KEY_LEFTARROW;
			break;
		case 'l': case 'L':
		//case 'h': case 'H':
		case VK_RIGHT:
			key = KEY_RIGHTARROW;
			break;
		case 'i': case 'I':
		//case 't': case 'T':
		case '@':
		case VK_UP:
			key = KEY_UPARROW;
			break;
		case 'k': case 'K':
		//case 'g': case 'G':
		case ';':
		case VK_DOWN:
			key = KEY_DOWNARROW;
			break;
		case VK_COMMODORE:
			key = KEY_FIRE;
			break;
		case VK_SPACE:
			key = KEY_USE;
			break;
		case 'z': case 'Z':
			key = KEY_STRAFE_L;
			break;
		case 'x': case 'X':
			key = KEY_STRAFE_R;
			break;
		case VK_SHIFT_L: case VK_SHIFT_R: 
			key = KEY_RSHIFT;
			break;
		case '=': 
			key = 0; 
			break;
		case VK_F7:
			reboot();
			break;
		default:
			key = tolower(key);
			break;
		}
	}

	return key;
}

static int delta[ 2 ] = { 0, 0 };
int mouseDoomData[ 4 ] = { 0, 0, 0, 0 };
int mouseMinVal[ 2 ] = { 255, 255 };
int mouseMaxVal[ 2 ] = { 0, 0 };
int mouseLastVal[ 2 ];
uint8_t mouseFirstPos = 1;

static void addKeyToQueue( int pressed, uint8_t keyCode )
{
	uint8_t key = convertToDoomKey( keyCode );

	uint16_t keyData = ( pressed << 8 ) | key;

	if ( pressed )
		switch ( keyCode )
		{
		case VK_F1:
			displayHelp = 1 - displayHelp; return;
		case VK_F3:
			mouseControlActive = 1 - mouseControlActive; 			displayStatus = 100; displayPreset = 0;
			mouseMinVal[ 0 ] = mouseMinVal[ 1 ] = 255;
			mouseMaxVal[ 0 ] = mouseMaxVal[ 1 ] = 0;
			return;
		case VK_F5:
			displayStatus = 100; 
			return;
		case 'A': case 'a':
			ditherMode = max( 0, (int)ditherMode - 1 );				displayStatus = 100; displayPreset = 0;
			return;
		case 'Q': case 'q':
			ditherMode = min( 4, (int)ditherMode + 1 );				displayStatus = 100; displayPreset = 0;
			return;
		case 'S': case 's':
			flickerMode = max( 0, (int)flickerMode - 8 );			displayStatus = 100; displayPreset = 0;
			return;
		case 'W': case 'w':
			flickerMode = min( 256, (int)flickerMode + 8 );			displayStatus = 100; displayPreset = 0;
			return;
		case 'D': case 'd':
			brightnessScale = max( 0, (int)brightnessScale - 1 );	displayStatus = 100; displayPreset = 0;
			return;
		case 'E': case 'e':
			brightnessScale = min( 256, (int)brightnessScale + 1 );	displayStatus = 100; displayPreset = 0;
			return;
		case 'R': case 'r':
			selectedPreset = ( selectedPreset + 1 ) % 4;			displayStatus = 100; displayPreset = 1; 
			setDisplayPreset( selectedPreset );
			return;
		case 'F': case 'f':
			alternatePattern = 1 - alternatePattern;				displayStatus = 100; displayPreset = 0;
			return;

/*		case 'F': case 'f':
			testVal = max( 0, (int)testVal - 1 );	displayStatus = 100;
			return;
		case 'R': case 'r':
			testVal = min( 256, (int)testVal + 1 );	displayStatus = 100;
			return;*/
		default: break;
		}

	s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
	s_KeyQueueWriteIndex++;
	s_KeyQueueWriteIndex %= KEY_QUEUE_SIZE;
}

uint8_t bluenoise256[ 256 * 256 ];

void DG_Init()
{
	#ifdef RENDER_SOUND
	FILE *f = fopen( "SD:RADDOOM/soundfont.sf2", "rb" );
	fseek( f, 0, SEEK_END );
	uint32_t s = ftell( f );
	fseek( f, 0, SEEK_SET );
	uint8_t *tmp;
	tmp = malloc( s );
	int read = fread( tmp, 1, s, f );
	fclose( f );

	pTSF = tsf_load_memory( tmp, s );
	tsf_set_output( pTSF, TSF_MONO, SAMPLE_RATE, 0 );
	free( tmp );
	#endif

	FILE *g = fopen( "SD:RADDOOM/bluenoise256.raw", "rb" );
	fread( bluenoise256, 1, 256 * 256, g );
	fclose( g );

	extern uint8_t font_bin[ 4096 ];
	charset = font_bin;

	memset( s_KeyQueue, 0, KEY_QUEUE_SIZE * sizeof( uint16_t ) );
}

//
// ___  _ ___ _  _ ____ ____ _ _  _ ____ 
// |  \ |  |  |__| |___ |__/ | |\ | | __ 
// |__/ |  |  |  | |___ |  \ | | \| |__] 
//
// 

#define RGB_QUANTIZE_BITS	4
#define RGB_LEVELS			( 1 << RGB_QUANTIZE_BITS )

uint8_t mapRGB2C64[ RGB_LEVELS * RGB_LEVELS * RGB_LEVELS ];

int c64PalettePepto[ 16 ][ 3 ] = {
		{0x00, 0x00, 0x00}, {0xFF, 0xFF, 0xFF}, {0x68, 0x37, 0x2B}, {0x70, 0xA4, 0xB2},
		{0x6F, 0x3D, 0x86}, {0x58, 0x8D, 0x43}, {0x35, 0x28, 0x79}, {0xB8, 0xC7, 0x6F},
		{0x6F, 0x4F, 0x25}, {0x43, 0x39, 0x00}, {0x9A, 0x67, 0x59}, {0x44, 0x44, 0x44},
		{0x6C, 0x6C, 0x6C}, {0x9A, 0xD2, 0x84}, {0x6C, 0x5E, 0xB5}, {0x95, 0x95, 0x95},
};

const uint8_t ditherMatrix4x4[ 4 * 4 ] = {
	0, 12, 3, 15,
	8, 4, 11, 7,
	2, 14, 1, 13,
	10, 6, 9, 5 };

const uint8_t ditherMatrix4x4_line[ 4 * 4 ] = {
	 0,  4,  2,  6,
	 8, 12, 10, 14,
	 3,  7,  1,  5,
	11, 15,  9, 13 };

static const uint8_t ditherMatrix8x8[ 8 * 8 ] = {
	 0, 48, 12, 60,  3, 51, 15, 63, 
	32, 16, 44, 28, 35, 19, 47, 31, 
	 8, 56,  4, 52, 11, 59,  7, 55, 
	40, 24, 36, 20, 43, 27, 39, 23, 
	 2, 50, 14, 62,  1, 49, 13, 61, 
	34, 18, 46, 30, 33, 17, 45, 29, 
	10, 58,  6, 54,  9, 57,  5, 53, 
	42, 26, 38, 22, 41, 25, 37, 21 };

static const uint8_t ditherMatrix8x8_line[ 8 * 8 ] = {
	 0, 16,  4, 20,  2, 18,  6, 22, 
	32, 48, 36, 52, 34, 50, 38, 54, 
	 8, 24, 12, 28, 10, 28, 14, 30, 
	40, 56, 44, 60, 42, 58, 46, 62, 
	 3, 19,  7, 23,  1, 17,  5, 21, 
	35, 51, 39, 55, 33, 49, 37, 53, 
	11, 27, 15, 31,  9, 25, 13, 29, 
	43, 59, 47, 63, 41, 57, 45, 61 };

// rank how close color j is to color i (idx = i * 16 + j)
const uint8_t mapC64Closest[ 16 * 16 ] = {
    0x09, 0x06, 0x0B, 0x02, 0x04, 0x08, 0x0C, 0x0E, 0x0A, 0x05, 0x0F, 0x03, 0x07, 0x0D, 0x01, 0xFF,
    0x0D, 0x07, 0x03, 0x0F, 0x05, 0x0A, 0x0C, 0x0E, 0x08, 0x04, 0x0B, 0x02, 0x09, 0x06, 0x00, 0xFF,
    0x08, 0x09, 0x0B, 0x04, 0x0C, 0x0A, 0x06, 0x0E, 0x05, 0x00, 0x0F, 0x03, 0x07, 0x0D, 0x01, 0xFF,
    0x0F, 0x05, 0x0C, 0x0D, 0x07, 0x0A, 0x0E, 0x04, 0x08, 0x0B, 0x01, 0x02, 0x09, 0x06, 0x00, 0xFF,
    0x0E, 0x06, 0x0B, 0x0C, 0x02, 0x08, 0x09, 0x0A, 0x0F, 0x05, 0x03, 0x00, 0x07, 0x0D, 0x01, 0xFF,
    0x0C, 0x0F, 0x0A, 0x03, 0x08, 0x07, 0x0D, 0x0B, 0x02, 0x0E, 0x09, 0x04, 0x06, 0x01, 0x00, 0xFF,
    0x04, 0x0B, 0x09, 0x02, 0x0E, 0x08, 0x00, 0x0C, 0x0A, 0x05, 0x0F, 0x03, 0x07, 0x0D, 0x01, 0xFF,
    0x0D, 0x0F, 0x03, 0x05, 0x01, 0x0A, 0x0C, 0x08, 0x0E, 0x04, 0x02, 0x0B, 0x09, 0x06, 0x00, 0xFF,
    0x02, 0x0B, 0x09, 0x0A, 0x0C, 0x04, 0x05, 0x06, 0x0E, 0x0F, 0x03, 0x00, 0x07, 0x0D, 0x01, 0xFF,
    0x0B, 0x02, 0x08, 0x06, 0x04, 0x0C, 0x00, 0x0A, 0x05, 0x0E, 0x0F, 0x03, 0x07, 0x0D, 0x01, 0xFF,
    0x0C, 0x08, 0x0F, 0x05, 0x02, 0x04, 0x0B, 0x03, 0x0E, 0x09, 0x07, 0x06, 0x0D, 0x00, 0x01, 0xFF,
    0x09, 0x02, 0x08, 0x04, 0x06, 0x0C, 0x0A, 0x0E, 0x05, 0x00, 0x0F, 0x03, 0x07, 0x0D, 0x01, 0xFF,
    0x0A, 0x08, 0x05, 0x0F, 0x0B, 0x04, 0x0E, 0x02, 0x03, 0x09, 0x06, 0x07, 0x0D, 0x00, 0x01, 0xFF,
    0x07, 0x03, 0x0F, 0x05, 0x01, 0x0A, 0x0C, 0x08, 0x0E, 0x04, 0x0B, 0x02, 0x09, 0x06, 0x00, 0xFF,
    0x04, 0x0C, 0x06, 0x0A, 0x0B, 0x0F, 0x03, 0x08, 0x02, 0x05, 0x09, 0x07, 0x0D, 0x00, 0x01, 0xFF,
    0x03, 0x05, 0x0C, 0x0A, 0x07, 0x0D, 0x0E, 0x08, 0x04, 0x0B, 0x02, 0x09, 0x01, 0x06, 0x00, 0xFF
};

static uint8_t koalaData[ 10000 + 1 ];

int colorDistance( const int r, const int g, const int b, const int r_, const int g_, const int b_ )
{
	return ( r - r_ ) * ( r - r_ ) + ( g - g_ ) * ( g - g_ ) + ( b - b_ ) * ( b - b_ );
}

void precomputeColorQuantization()
{
	memset( koalaData, 0, 10000 + 1 );

	// rank how close color j is to color i
	#if 0
	for ( int i = 0; i < 16; i++ )
	{
		int r, g, b;
		r = c64PalettePepto[ i ][ 0 ];
		g = c64PalettePepto[ i ][ 1 ];
		b = c64PalettePepto[ i ][ 2 ];

		int err[ 16 ], idx[ 16 ];

		for ( int j = 0; j < 16; j++ )
		{
			err[ j ] = 1 << 30;
			idx[ j ] = -1;
		}

		for ( int j = 0; j < 16; j++ )
		if ( j != i )
		{
			int e = colorDistance( r, g, b, c64PalettePepto[ j ][ 0 ], c64PalettePepto[ j ][ 1 ], c64PalettePepto[ j ][ 2 ] );

			for ( int k = 0; k < 16; k++ )
			if ( e < err[ k ] )
			{
				for ( int l = 15; l >= k + 1; l -- )
				{
					idx[ l ] = idx[ l - 1 ];
					err[ l ] = err[ l - 1 ];
				}
				idx[ k ] = j;
				err[ k ] = e;
				goto colorIsRanked;
			}
		colorIsRanked:;
		}

		for ( int j = 0; j < 16; j++ )
			mapC64Closest[ i * 16 + j ] = idx[ j ];
	}
	#endif

	// map quantized RGB-values to C64-colors
	for ( int r = 0; r < RGB_LEVELS; r++ )
		for ( int g = 0; g < RGB_LEVELS; g++ )
			for ( int b = 0; b < RGB_LEVELS; b++ )
			{
				int ofs = ( ( ( r << RGB_QUANTIZE_BITS ) + g ) << RGB_QUANTIZE_BITS ) + b;

				int rr = ( ( r << ( 8 - RGB_QUANTIZE_BITS ) ) * exposure ) >> 8;
				int gg = ( ( g << ( 8 - RGB_QUANTIZE_BITS ) ) * exposure ) >> 8;
				int bb = ( ( b << ( 8 - RGB_QUANTIZE_BITS ) ) * exposure ) >> 8;

				rr = powf( (float)rr / 255.0f, invGamma ) * 255.0f;
				gg = powf( (float)gg / 255.0f, invGamma ) * 255.0f;
				bb = powf( (float)bb / 255.0f, invGamma ) * 255.0f;

				int idx = 0;
				int minError = 1 << 30;
				for ( int i = 0; i < 16; i++ )
				{
					int e = colorDistance( c64PalettePepto[ i ][ 0 ], c64PalettePepto[ i ][ 1 ], c64PalettePepto[ i ][ 2 ], rr, gg, bb );

					if ( e < minError )
					{
						idx = i;
						minError = e;
					}
				}
				mapRGB2C64[ ofs ] = idx;
			}
}


const int16x4_t quantMax = { RGB_LEVELS-1, RGB_LEVELS-1, RGB_LEVELS-1, RGB_LEVELS-1 }, quantMin = { 0, 0, 0, 0 };

int bgColor = 0;


void doImageConversion()
{
	//const uint8_t *dm;
	//uint8_t dmask, dshift, dofs, dmul = 4 * 2, dmax;

	static int fc = 0;
	fc ++;

	static int xofs = 0, yofs = 0;

	if ( ditherMode == 4 )	
	{
		if ( ( fc & 1 ) )
		{
			xofs += 137;
			yofs += 119;
		}
	} else
		xofs = yofs = 0;

	static uint8_t dynDM[ 4096 ];
	static uint8_t *dm = dynDM;
	static uint8_t lastDitherMode = 255;

	static uint8_t dshift, dmaskX, dmaskY;
	static int dmul, dofs;

	if ( ditherMode < 3 && ditherMode != lastDitherMode ) // ordered dithering
	{
		int M = 1 + ditherMode, L = M + 1;
		int sizeX = 1 << M, sizeY = 1 << L;

		uint8_t *p = dynDM;
		// matrix generation code from: https://bisqwit.iki.fi/story/howto/dither/jy/
		for ( int y = 0; y < sizeY; y++ )
		{
			for ( int x = 0; x < sizeX; x++ )
			{
				int v = 0, ofs = 0, maskX = M, maskY = L;
				if ( M == 0 || ( M > L && L != 0 ) )
				{
					int xc = x ^ ( ( y << M ) >> L ), yc = y;
					for ( int bit = 0; bit < M + L; )
					{
						v |= ( ( yc >> --maskY ) & 1 ) << bit++;
						for ( ofs += M; ofs >= L; ofs -= L )
							v |= ( ( xc >> --maskX ) & 1 ) << bit++;
					}
				} else
				{
					int xc = x, yc = y ^ ( ( x << L ) >> M );
					for ( int bit = 0; bit < M + L; )
					{
						v |= ( ( xc >> --maskX ) & 1 ) << bit++;
						for ( ofs += L; ofs >= M; ofs -= M )
							v |= ( ( yc >> --maskY ) & 1 ) << bit++;
					}
				}
				*( p++ ) = v;
			}
		}

		dm = dynDM;
		dmul = 1024 >> (int)( M + L );
		dofs = 512 / dmul - 1; 
		dmaskX = sizeX - 1;
		dmaskY = sizeY - 1;
		dshift = M;
		dmul *= 2;
	}

	if ( ditherMode >= 3 && ditherMode < 5  && ditherMode != lastDitherMode ) // blue noise
	{
		dm = bluenoise256;			
		dmaskX = dmaskY = 255; 
		dshift = 8; 
		dofs = 128; 
		dmul = 4 * 2;
	}

	lastDitherMode = ditherMode;

	static int colorOccurence[ 16 ];

	static int firstBGGuess = 1;

	if ( firstBGGuess /*|| ( testVal & 2 ) == 0*/ )
	{
		firstBGGuess = 0;
		bgColor = 0;
		memset( colorOccurence, 0, sizeof( int ) * 16 );
	} else
	//if ( fc & 1 )
	{
		int freq = colorOccurence[ 0 ];
		bgColor = 0;
		for ( int i = 1; i < 15; i++ )
			if ( colorOccurence[ i ] > freq )
			{
				freq = colorOccurence[ i ];
				bgColor = i;
			}
		memset( colorOccurence, 0, sizeof( int ) * 16 );
	}

	for ( int y = 0; y < DOOMGENERIC_RESY; y += 8 )
	{
		for ( int x = 0; x < DOOMGENERIC_RESX; x += 8 )
		{
			uint8_t histo[ 16 ];
			memset( histo, 0, 16 );
			
			uint8_t blockColors[ 4 * 8 ];

			// here we map 8x8 RGB-pixels to 4x8 C64-color-pixels (without restrictions)
			for ( int c = 0; c < 8; c++ )
			{
				for ( int a = 0; a < 8; a += 2 )
				{
					uint8x8_t  rgba_2    = vld1_u8( &DG_ScreenBuffer[ ( x + a ) + ( y + c ) * DOOMGENERIC_RESX ] );
					uint16x8_t rgba_2_16 = vmovl_u8( rgba_2 );

					int c64color;
					if ( rgba_2[ 3 ] ) // most-significant byte of first pixel: if set => stores c64 palette index
					{
						c64color = rgba_2[ 3 ];
					} else
					{
						int16x4_t t1 = vget_low_s16 ( *(int16x8_t*)&rgba_2_16 );
						int16x4_t t2 = vget_high_s16( *(int16x8_t*)&rgba_2_16 );

						t1 = vqadd_s16( t1, t2 );

						const int32x4_t lumaWeight = { 114, 587, 299, 0 };
						int32x4_t wc = vmulq_s32( vmovl_s16( t1 ), lumaWeight );
						int lum = ( (int)wc[ 0 ] + (int)wc[ 1 ] + (int)wc[ 2 ] ) >> 11;

						t1 = vmul_s16( t1, vld1_dup_s16( &brightnessScale ) );

						int i = (a/2 + x/2) + 0;

						int curdofs, curdmul, ydisp;

						if ( lum < flickerMode )
						{
							curdofs = dofs / 1;
							curdmul = dmul / 2;
							ydisp = fc & 1;

							if ( alternatePattern )
								ydisp ^= i & 1;
						} else
						{
							curdofs = dofs / 2;
							curdmul = dmul / 1;
							if ( ditherMode >= 3 ) curdmul >>= 1; // bluenoise 
							ydisp = 0;
						}
							
						int cc = ( c + y ) * 2 + ydisp;
						i += xofs; cc += yofs;				
						int16_t ditherValue = ( dm[ ( i & dmaskX ) + ( ( cc & dmaskY ) << dshift ) ] - curdofs ) * curdmul;

						t1 = vqadd_s16( t1, vld1_dup_s16( &ditherValue ) );
						t1 >>= 5 + ( 8 - RGB_QUANTIZE_BITS ); 
						t1 = vmin_s16( t1, quantMax );
						t1 = vmax_s16( t1, quantMin );
						
						register uint16_t r, g, b;
						r = *(uint16_t *)&t1[ 2 ];
						g = *(uint16_t *)&t1[ 1 ];
						b = *(uint16_t *)&t1[ 0 ];

						int ofs = ( ( ( r << RGB_QUANTIZE_BITS ) + g ) << RGB_QUANTIZE_BITS ) + b;
						c64color = mapRGB2C64[ ofs ];
					}

					histo[ c64color ] ++;
					blockColors[ ( a >> 1 ) + c * 4 ] = c64color;
				}
			}

			// count in how many blocks we had color x
			for ( int i = 0; i < 16; i++ )
				if ( histo[ i ] )
					colorOccurence[ i ] ++;

			// now apply restrictions of the multicolor mode

			// if background color is fixed:
			//const int bgColor = 0;
			histo[ bgColor ] = 32;

			int nColors = 0;
			uint8_t map[ 16 ];
			for ( int i = 0; i < 16; i++ )
			{
				if ( histo[ i ] ) nColors ++;	// count each color occurence
				map[ i ] = i;					// initial mapping 1:1
			}

			while ( nColors > 4 )
			{
				// find and remove least frequent color
				int minV = 256, minIdx = 255;
				for ( int i = 0; i < 16; i++ )
					if ( histo[ i ] && histo[ i ] < minV )
					{
						minV = histo[ i ];
						minIdx = i;
					}
				nColors --;

				// now remap color 'minIdx' to best fit... (and add its occurence to the target color)
				uint8_t tmpCount = histo[ minIdx ];
				histo[ minIdx ] = 0;

				const uint8_t *mapColors = &mapC64Closest[ minIdx * 16 ];
				int i = 0;
				while ( i < 15 && histo[ mapColors[ i ] ] == 0 ) 
					i ++;
				map[ minIdx ] = mapColors[ i ];

				for ( int j = 0; j < 16; j++ )
					if ( histo[ j ] == 0 && map[ j ] == minIdx )
						map[ j ] = mapColors[ i ];

				histo[ mapColors[ i ] ] += tmpCount;
			};

			// determined colors for screen and color RAM
			int w = 0;

			uint8_t screenRAM = 0, colorRAM = 0;
			
			// optional (for double-buffering): choose the same color for color RAM  ($d800) is possible
			uint8_t prevColorRAM = koalaData[ 9000 + ( y / 8 ) * 40 + ( x / 8 ) ];

			// if we can keep the color RAM nibble, do that
			if ( prevColorRAM != bgColor && histo[ prevColorRAM ] )
			{
				w = 1;
				histo[ prevColorRAM ] = 0;
				colorRAM = prevColorRAM;
			}

			for ( int i = 0; i < 16; i++ )
			if ( i != bgColor && histo[ i ] )
			{
				if ( w == 2 )
				{
					screenRAM |= i << 4;
					w ++;
				} else
				if ( w == 1 )
				{
					screenRAM |= i;
					w ++;
				} else
				if ( w == 0 )
				{
					colorRAM = i;
					w ++;
				}
			}

			koalaData[ 8000 + ( y / 8 ) * 40 + ( x / 8 ) ] = screenRAM;
			koalaData[ 9000 + ( y / 8 ) * 40 + ( x / 8 ) ] = colorRAM;

			for ( int c = 0; c < 8; c++ )
			{
				register uint8_t kBitmapData = 0;
				for ( int a = 0; a < 4; a ++ )
				{
					uint8_t c64color = map[ blockColors[ a + c * 4 ] ];

					uint8_t pixelValue = 0;
					if ( c64color == colorRAM ) pixelValue = 0b11; else
					if ( c64color == (screenRAM & 15) ) pixelValue = 0b10; else
					if ( c64color == (screenRAM >> 4) ) pixelValue = 0b01; 

					// debug out: show chosen background color in top-right corner
					//if ( x > 300 && y < 20 )
					//	pixelValue = 0b00;

					kBitmapData <<= 2;
					kBitmapData |= pixelValue;
				}

				koalaData[ y * 40 + x + c ] = kBitmapData;
			}
		}
	}

	koalaData[ 10000 ] = bgColor;
}


//
// ____ _  _ ___  _ ____ 
// |__| |  | |  \ | |  | 
// |  | |__| |__/ | |__| 
//                      
//

static uint8_t firstSoundMix = 1;
//static uint64_t soundFirstTick = 0;
extern uint64_t soundFirstuSecTick;
static uint64_t framesSoFar = 0;

uint8_t soundRingBuffer[ SOUND_RINGBUF_SIZE ];

extern uint64_t GetuSec();
extern uint64_t GetTickCount();

#ifdef RENDER_SOUND

#ifdef USE_MIDI
#define NMIDI_CHANNELS 16
static int midiChannelMap[ NMIDI_CHANNELS ];
#endif

//static int minSampleV = 0, maxSampleV = 256;

extern uint64_t getCurrentSamplePos();

static void audioRender() 
{
	int nSamples;

	const int msPerFrame = 100; // short buffer

	if ( firstSoundMix )
	{
		firstSoundMix = 0;

		#ifdef USE_MIDI
		// init MIDI
		midiCur = midiLast = 0;

		disableInterrupts();

		radPOKE( midiAddr, 0x03 );
//		radPOKE( midiAddr, 0x12 );
		radPOKE( midiAddr, 0x15 );

		for ( int channel = 0; channel < NMIDI_CHANNELS; ++channel )
			midiChannelMap[ channel ] = -1;

		enableInterrupts();
		#endif

		framesSoFar = 0;
		nSamples = SAMPLE_RATE * msPerFrame / 1000;
		soundFirstuSecTick = GetuSec();
		memset( soundRingBuffer, 127, SOUND_RINGBUF_SIZE );

		extern void startSIDSamplePlayer();
		startSIDSamplePlayer();
	} else
	{
		uint64_t totalFrames = getCurrentSamplePos();
		totalFrames += SAMPLE_RATE * msPerFrame / 1000;
		nSamples = totalFrames - framesSoFar; 
	}

	memset( soundMixingBuffer, 0, sizeof( float ) * SOUND_BUF_SIZE );

	if ( nSamples > 0 ) 
	{
		#ifdef USE_MIDI
		musicRenderMIDI( nSamples );
		#else
		musicRender( nSamples );
		#endif
		soundRender( nSamples );
	}

	static int rv = 123456789;
	for ( int i = 0; i < nSamples; i++ )
	{
		//rv = ( 1103515245 * rv + 12345 ) & ( (uint32_t)(1<<31) - 1 );
		//float r = (float)( (rv>>16) & 255 ) / 255.0f / 64.0f;
		const float r = 0.0f;
		int f = (int)( (soundMixingBuffer[ i ] + r) * 1.0f * 127.0f + 128.0f );

		if ( f < 0 ) f = 0;
		if ( f > 255 ) f = 255;

		extern const uint8_t *mahoneyLUT;
		#ifdef USE_DIGIMAX
		soundRingBuffer[ ( framesSoFar ++ ) & (SOUND_RINGBUF_SIZE-1) ] = (uint8_t)f;
		#else
		soundRingBuffer[ ( framesSoFar ++ ) & (SOUND_RINGBUF_SIZE-1) ] = mahoneyLUT[ (uint8_t)f ];
		#endif
	}
}
#endif

int introPrepare()
{
	precomputeColorQuantization();
	return 0;
}

int introShowFrame()
{
	doImageConversion();


	uint32_t  kbEvents[ 16 ];
	uint8_t nEvents = 0;
	uint8_t mouseData[ 4 ];
	blitScreenDOOM( koalaData,  kbEvents, &nEvents, mouseData );

	if ( nEvents )
		return 1;

	return 0;
}

int (*functionAddress[]) (void) = {
   introPrepare,
   introShowFrame,
};


void printC64( const char *t, int x_, int y, uint32_t color )
{
	int len = strlen( t );
	for ( int i = 0; i < len; i++ )
	{
		uint8_t c = t[ i ];
		int x = x_ + i * 8;

		if ( c == '@' )
			c = 0; else
		if ( c == '_' )
			c = 100; else
		if ( ( c >= 'a' ) && ( c <= 'z' ) )
			c = c + 1 - 'a';

		if ( c != 32 && c != ( 32 + 128 ) )
		for ( int b = 0; b < 8; b++ )
		{
			uint8_t v = charset[ 2048 + c * 8 + b ];

			for ( int p = 0; p < 7; p++ )
			{
				if ( v & 128 )
				{
					DG_ScreenBuffer[ (x+p)*2 + (y+b) * 320 ] = color;
					DG_ScreenBuffer[ (x+p)*2+1 + (y+b) * 320 ] = color;
				}
				v <<= 1;
			}
		}
	}
}

uint64_t endLastFrame = -1;
extern uint64_t GetuSec();

void handleMouseUpdate( uint8_t *mouseData )
{
	mouseMinVal[ 0 ] = min( mouseMinVal[ 0 ], mouseData[ 0 ] );
	mouseMinVal[ 1 ] = min( mouseMinVal[ 1 ], mouseData[ 1 ] );
	mouseMaxVal[ 0 ] = max( mouseMaxVal[ 0 ], mouseData[ 0 ] );
	mouseMaxVal[ 1 ] = max( mouseMaxVal[ 1 ], mouseData[ 1 ] );
	
	if ( mouseFirstPos ) 
	{
		mouseFirstPos = 0;
		mouseLastVal[ 0 ] = mouseData[ 0 ];
		mouseLastVal[ 1 ] = mouseData[ 1 ];
		mouseDoomData[ 0 ] = mouseDoomData[ 1 ] = mouseDoomData[ 2 ] = 0;
	}

//	if ( mouseDoomData[ 3 ] == 0 )
//		mouseDoomData[ 0 ] = mouseDoomData[ 1 ] = 0;

	for ( int i = 0; i < 2; i++ )
	{
		int span  = ( mouseMaxVal[ i ] - mouseMinVal[ i ] ) / 2;

		int lastDelta = delta[ i ];	
		delta[ i ] = mouseData[ i ] - mouseLastVal[ i ];

		//if ( delta[ i ] > 0 &&  delta[ i ] < span ) {} else		// positive movement w/o wraparound
		//if ( delta[ i ] < 0 && -delta[ i ] < span ) {} else		// negative movement w/o wraparound
		if ( delta[ i ] > 0 && ( delta[ i ] >= span || lastDelta >= span ) ) 			// negative movement with wraparound
		{
			delta[ i ] = -( mouseLastVal[ i ] - mouseMinVal[ i ] + mouseMaxVal[ i ] - mouseData[ i ] );
		} else
		if ( delta[ i ] < 0 && ( -delta[ i ] >= span || -lastDelta >= span ) ) 			// positive movement with wraparound
		{
			delta[ i ] = mouseData[ i ] - mouseMinVal[ i ] + mouseMaxVal[ i ] - mouseLastVal[ i ];
		} 
		mouseDoomData[ i ] += delta[ i ];
		mouseLastVal[ i ] = mouseData[ i ];
	}
	mouseDoomData[ 2 ] = mouseData[ 2 ];
	mouseDoomData[ 3 ] = 1; // event generated
}

void DG_DrawFrame()
{
	if ( endLastFrame != -1 )
	{
		uint64_t time = GetuSec();

		if ( time - endLastFrame < 5 * 1000 )
			return; else
			endLastFrame = time;
	} else
		endLastFrame = GetuSec();

	if ( first )
	{
		first = 0;

		// set up C64 system...
		prepareC64();
		precomputeColorQuantization();

		firstSoundMix = 1;
		//soundFirstTick = 0;
		framesSoFar = 0;

		#ifdef USE_MIDI
		midiCur = midiLast = 0;
		#endif		

		 s_KeyQueueWriteIndex = 0;
		 s_KeyQueueReadIndex = 0;
	}

	// print static text 
	int ofs[9][2]={ {-1,-1}, {-1,0},{-1,1}, {0,-1}, {0,1}, {1,-1},{1,0},{1,1},{0,0}};
	if ( displayHelp )
	{
		uint32_t *p = DG_ScreenBuffer;
		for ( int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++, p++ )
		{
			(*p) = ( (*p) & 0xfefefe ) >> 1;
		}
		for ( int p = 0; p < 9; p++ )
		{
			uint32_t color = 0xffffff;
			uint32_t color2 = 0x5f5f5f;
			int x = 4, spacing = 10;
			int y = 10;

			if ( p != 8 ) color = color2 = 0;
			x += ofs[ p ][ 0 ];
			y += ofs[ p ][ 1 ];

			printC64( "weapons:     move:  ", x, y, color2 ); y += spacing;
			if ( !mouseControlActive )
			{
				printC64( "12345...       I    ", x, y, color ); y += spacing;
				printC64( "              JKL   ", x, y, color ); y += spacing; y += spacing; y += spacing;
				printC64( " C=   SHIFT    ZX   ", x, y, color ); y += spacing;
			} else
			{
				printC64( "12345...       @    ", x, y, color ); y += spacing;
				printC64( "               ;    ", x, y, color ); y += spacing; y += spacing; y += spacing;
				printC64( "btn#1 SHIFT  btn#2  ", x, y, color ); y += spacing;
			}
			printC64( "shoot  run   strafe ", x, y, color2 ); y += spacing; y += spacing;
			printC64( "      SPACE         ", x, y, color ); y += spacing;
			printC64( "       use          ", x, y, color2 ); y += spacing; y += spacing;
			printC64( "dither ....... Q,A  ", x, y, color2 ); y += spacing;
			printC64( "colormix ..... W,S  ", x, y, color2 ); y += spacing;
			printC64( "brightness ... E,D  ", x, y, color2 ); y += spacing;
			printC64( "shuffle ...... F    ", x, y, color2 ); y += spacing;
			printC64( "presets ...... R    ", x, y, color2 ); y += spacing;
			printC64( "show config .. F5   ", x, y, color2 ); y += spacing;

			if ( mouseControlActive )
				{ printC64( "keyboard ..... F3      ", x, y, color2 ); y += spacing; } else
				{ printC64( "mouse ........ F3      ", x, y, color2 ); y += spacing; }
		}
	} else
	if ( displayStatus )
	{
		const char *dmString[5] = { "ordered-2", "ordered-4", "ordered-8", "blue noise", "blue dyn" };
		const char *prString[5] = { "standard", "CRT", "TFT/sh", "mixing" };

		char s[ 30 ];
		char s2[ 30 ], s3[30], s4[ 30 ];
		sprintf( s,  "flicker: %d", flickerMode );
		sprintf( s2, "luma:    %d", brightnessScale );
		sprintf( s3, "shuffle: %s", alternatePattern ? "yes":"no" );
		sprintf( s4, "preset:  %s", prString[ selectedPreset ] );

		extern uint8_t supportDAC, SIDType, hasSIDKick;
		char s5[ 30 ], s6[ 30 ];
		#ifdef USE_DIGIMAX
			sprintf( s5, "Digimax" ); 
		#else
		if ( supportDAC )
			sprintf( s5, "SIDKick (DAC)" ); else
		if ( hasSIDKick && ( SIDType == (6581 & 255) ) )
			sprintf( s5, "SIDKick (6581)" ); else
		if ( hasSIDKick && ( SIDType == (8580 & 255) ) )
			sprintf( s5, "SIDKick (8580)" ); else
		if ( hasSIDKick && ( SIDType == (6581 & 255) ) )
			sprintf( s5, "MOS 6581" ); else
		if ( hasSIDKick && ( SIDType == (8580 & 255) ) )
			sprintf( s5, "MOS 8580" ); else
			sprintf( s5, "SID unknown (4-Bit)" ); 
		#endif

		#ifndef USE_MIDI
		sprintf( s6, "%s", s5 );
		#else
		sprintf( s6, "%s+MIDI", s5 );
		#endif

		for ( int p = 0; p < 9; p++ )
		{
			uint32_t color = (p==8)?0xffffff:0;
			
			printC64( "dither:", 4+ofs[p][0], 4+ofs[p][1], color );
			printC64( dmString[ ditherMode ], 76+ofs[p][0], 4+ofs[p][1], color );
			printC64( s, 4+ofs[p][0], 12+ofs[p][1], color );
			printC64( s2, 4+ofs[p][0], 20+ofs[p][1], color );
			printC64( s3, 4+ofs[p][0], 28+ofs[p][1], color );

			if ( displayPreset )
				printC64( s4, 4+ofs[p][0], 36+ofs[p][1], color );

			if ( mouseControlActive )
				printC64( "mouse in port #2", 4 + ofs[p][0], 56 + ofs[p][1], color ); else
				printC64( "keyboard control", 4 + ofs[p][0], 56 + ofs[p][1], color ); 

			int yp = 72;
			printC64( s6, 4+ ofs[p][0], yp + ofs[p][1], color );
		}



		if ( displayStatus ) displayStatus --;
	}

	/*
	{
		uint32_t color = 0xffffff;
		int p = 4;
		char s[ 30 ];
		sprintf( s,  "  dx=%3d    dy=%3d", delta[0], delta[1] ); printC64( s, 4, p, color ); p+= 8;
		sprintf( s,  "minx=%3d  miny=%3d", mouseMinVal[0], mouseMinVal[1] ); printC64( s, 4, p, color ); p+= 8;
		sprintf( s,  "maxx=%3d  maxy=%3d", mouseMaxVal[0], mouseMaxVal[1] ); printC64( s, 4, p, color ); p+= 8;
	}
	*/

	//
	// per-frame image conversion
	//
	doImageConversion();

	uint32_t  kbEvents[ 16 ];
	uint8_t nEvents = 0;

	uint8_t mouseData[ 4 ];
	blitScreenDOOM( koalaData,  kbEvents, &nEvents, mouseData );


	handleMouseUpdate( mouseData );

	// todo: activate mouse by pressing button

	for ( int i = 0; i < nEvents; i++ )
	{
		int k = kbEvents[ i ];
		if ( k > 255 )
			addKeyToQueue( 1, k & 255 ); else	// key down
			addKeyToQueue( 0, k & 255 ); 		// key up
	}

	#ifdef RENDER_SOUND
	audioRender();
	#endif
}

void DG_SleepMs(uint32_t ms)
{
	extern uint64_t GetuSec();
	uint64_t start = GetuSec();
	uint64_t passed;

	do {
		// we could do something useful here
		passed = GetuSec() - start;
	} while ( passed < ms * 1000 );
}

extern uint64_t GetTickCount();

uint32_t DG_GetTicksMs()
{
	return GetTickCount();
}

int DG_MouseData( int *mb, int *rx, int *ry )
{
	if ( mouseDoomData[ 3 ] && mouseControlActive )
	{
		mouseDoomData[ 3 ] = 0;

		if ( abs( mouseDoomData[ 0 ] ) < 4 ) *rx = 0; else { *rx = mouseDoomData[ 0 ] * 6; mouseDoomData[ 0 ] = 0; }
		if ( abs( mouseDoomData[ 1 ] ) < 4 ) *ry = 0; else { *ry = mouseDoomData[ 1 ] * 1; mouseDoomData[ 1 ] = 0; }
		*mb = mouseDoomData[ 2 ];
		return 1;
	}
	return 0;
}

int DG_GetKey(int* pressed, uint8_t* doomKey)
{
	if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex)
	{
		//key queue is empty
		return 0;
	} else
	{
		uint16_t keyData = s_KeyQueue[s_KeyQueueReadIndex];
		s_KeyQueueReadIndex++;
		s_KeyQueueReadIndex %= KEY_QUEUE_SIZE;

		*pressed = keyData >> 8;
		*doomKey = keyData & 0xFF;
		return 1;
	}
}

void DG_SetWindowTitle(const char * title)
{
}

#ifdef RENDER_SOUND
//
// the sound and music support (musicRender-function) has been adapted from Doom-Sokol by Andre Weissflog (https://github.com/floooh/doom-sokol)
// the original code can be found in this file: https://github.com/floooh/doom-sokol/blob/master/src/doomgeneric_sokol.c
// it makes use of:
// - TinySoundFont by Bernhard Schelling: https://github.com/schellingb/TinySoundFont
// - MUS handling by Mattias Gustavsson: https://github.com/mattiasgustavsson/doom-crt/blob/main/libs_win32/mus.h (slightly modified to support arbritrary sample rates)
//

static void *soundLoadWADFX( const char *sfxname, int *len ) 
{
	char name[ 20 ];
	snprintf( name, sizeof( name ), "ds%s", sfxname );

	int sfxlump;
	if ( W_CheckNumForName( name ) == -1 ) 
		sfxlump = W_GetNumForName( "dspistol" ); else 
		sfxlump = W_GetNumForName( name );

	uint8_t *sfx = W_CacheLumpNum( sfxlump, PU_STATIC );
	*len = W_LumpLength( sfxlump ) - 8;
	return sfx + 8;
}

static int soundAddFX( int sfxid, int slot, int volume, int separation ) 
{
	soundChannel[ slot ].sfxid = sfxid;
	soundCurHandle ++;
	if ( soundCurHandle == 0 ) soundCurHandle = 1;
	soundChannel[ slot ].handle = soundCurHandle;
	soundChannel[ slot ].pCur = S_sfx[ sfxid ].driver_data;
	soundChannel[ slot ].pEnd = soundChannel[ slot ].pCur + soundLengths[ sfxid ];

	separation += 1;
	int left_sep = separation + 1;
	int volL = volume - ( ( volume * left_sep * left_sep ) >> 16 );
	int right_sep = separation - 256;
	int volR = volume - ( ( volume * right_sep * right_sep ) >> 16 );

	soundChannel[ slot ].volL = volL;
	soundChannel[ slot ].volR = volR;

	return soundChannel[ slot ].handle;
}

void soundRender( int nSamples ) 
{
	float cur_left_sample = 0.0f;

	for ( int i = 0; i < nSamples; i++ ) 
	{
		if ( soundSamplePos >= SAMPLE_RATE ) 
		{
			soundSamplePos -= SAMPLE_RATE;
			int dl = 0;
			int dr = 0;
			for ( int slot = 0; slot < NCHANNELS; slot++ ) 
			{
				SOUND_CHANNEL *chn = &soundChannel[ slot ];
				if ( chn->pCur ) 
				{
					int sample = ( (int)( *chn->pCur++ ) ) - 128;
					dl += sample * chn->volL;
					dr += sample * chn->volR;
					// sound effect done?
					if ( chn->pCur >= chn->pEnd ) 
						*chn = (SOUND_CHANNEL){ 0 };
				}
			}
			cur_left_sample = ( (float)(dl + dr) ) * 0.5f / 16383.0f;
		}
		soundSamplePos += soundSrcRate;
		soundMixingBuffer[ i ] += cur_left_sample;
	}
}

static boolean soundInit( boolean use_sfx_prefix ) 
{
	soundNamePrefix = use_sfx_prefix;
	soundSamplePos = SAMPLE_RATE;
	soundSrcRate = 11025;    // sound effect are in 11025Hz
	return true;
}

static int soundGetLump( sfxinfo_t *sfx ) 
{
	char namebuf[ 20 ];
	if ( soundNamePrefix ) 
		M_snprintf( namebuf, sizeof( namebuf ), "dp%s", sfx->name ); else 
		strncpy( namebuf, sfx->name, sizeof( namebuf ) );
	return W_GetNumForName( namebuf );
}

static void soundUpdateParameters( int handle, int vol, int sep ) {}

static int soundStartFX( sfxinfo_t *sfxinfo, int channel, int vol, int sep ) 
{
	return soundAddFX( sfxinfo - S_sfx, channel, vol, sep );
}

static void soundStopFX( int handle ) 
{
	for ( int i = 0; i < NCHANNELS; i++ ) 
		if ( soundChannel[ i ].handle == handle ) 
			soundChannel[ i ] = (SOUND_CHANNEL){ 0 };
}

static boolean soundIsPlaying( int handle ) 
{
	for ( int i = 0; i < NCHANNELS; i++ ) 
		if ( soundChannel[ i ].handle == handle ) 
			return true;
	return false;
}

static void soundCacheFX( sfxinfo_t *sounds, int num_sounds ) {
	for ( int i = 0; i < num_sounds; i++ ) {
		if ( 0 == sounds[ i ].link ) 
		{
			// load data from WAD file
			sounds[ i ].driver_data = soundLoadWADFX( sounds[ i ].name, &soundLengths[ i ] );
		} else 
		{
			// previously loaded already?
			const int snd_index = sounds[ i ].link - sounds;
			sounds[ i ].driver_data = sounds[ i ].link->driver_data;
			soundLengths[ i ] = soundLengths[ snd_index ];
		}
	}
}

static void soundNotRequired() {}

static snddevice_t soundDeviceRAD[] = { SNDDEVICE_SB };

sound_module_t soundModuleRAD = {
	.sound_devices     = soundDeviceRAD,
	.num_sound_devices = arrlen( soundDeviceRAD ),
	.Init              = soundInit,
	.Shutdown          = soundNotRequired,
	.GetSfxLumpNum     = soundGetLump,
	.Update            = soundNotRequired,
	.UpdateSoundParams = soundUpdateParameters,
	.StartSound        = soundStartFX,
	.StopSound         = soundStopFX,
	.SoundIsPlaying    = soundIsPlaying,
	.CacheSounds       = soundCacheFX,
};

#ifdef USE_MIDI

static const uint8_t mapMIDIController[] =
{
    0x00, 0x20, 0x01, 0x07, 0x0A, 0x0B, 0x5B, 0x5D,
    0x40, 0x43, 0x78, 0x7B, 0x7E, 0x7F, 0x79
};

#define MIDI_PERCUSSION_CHAN 9
#define MUS_PERCUSSION_CHAN  15

static int midiGetFreeChannel(void)
{
    int ch = -1;

	for ( int i = 0; i < NMIDI_CHANNELS; i++ )
		if ( midiChannelMap[ i ] > ch )
			ch = midiChannelMap[ i ];

	ch ++;

	if ( ch == MIDI_PERCUSSION_CHAN ) ch  ++;

    return ch;
}

static int midiGetChannel(int musCh)
{
	if ( musCh == MUS_PERCUSSION_CHAN )
		return MIDI_PERCUSSION_CHAN;

	if ( midiChannelMap[ musCh ] == -1 )
	{
		midiChannelMap[ musCh ] = midiGetFreeChannel();
		MIDI_CMD( 0xb0 + midiChannelMap[ musCh ] );
		MIDI_CMD( 0x7b );
		MIDI_CMD( 0x00 );
	}

	return midiChannelMap[ musCh ];
}


void musicRenderMIDI( int nSamples )  
{
	// we are not generating 'samples', nSamples is just used for timing (analogous to the TSF output)

	if ( !pMUS ) return;

	if ( musicReset ) 
	{
		musicReset = false;
		MIDI_CLR_BUFFER();	

		midiCur = midiLast = 0;

		MIDI_CMD( 0xff ); 

		for ( int i = 0; i < 16; i++ )
		{
			midiChannelMap[ i ] = -1;
			// all sounds off
			MIDI_CMD( 0xb0 + i ); 
			MIDI_CMD( 0x78 ); 
			MIDI_CMD( 0x00 ); 
		}

	}

	int nSamplesNeeded = nSamples;
	int nSamplesRemaining = 0;

	if ( musicPendingSamples > 0 ) 
	{
		int count = musicPendingSamples;
		if ( count > nSamplesNeeded ) {
			nSamplesRemaining = count - nSamplesNeeded;
			count = nSamplesNeeded;
		}
		nSamplesNeeded -= count;
	}
	if ( nSamplesRemaining > 0 ) 
	{
		musicPendingSamples = nSamplesRemaining;
		enableInterrupts();
		return;
	}

	while ( nSamplesNeeded ) 
	{
		int tmp;
		mus_event_t ev;
		mus_next_event( pMUS, &ev );
		int midiChannel = midiGetChannel( ev.channel );

		switch ( ev.cmd ) 
		{
		default: break;
		case MUS_CMD_PLAY_NOTE:
			MIDI_CMD( 0x90 + midiChannel ); 
			MIDI_CMD( ev.data.play_note.note ); 
			MIDI_CMD( ev.data.play_note.volume ); 
			break;
		case MUS_CMD_RELEASE_NOTE:
			MIDI_CMD( 0x80 + midiChannel ); 
			MIDI_CMD( ev.data.play_note.note ); 
			MIDI_CMD( 0 ); 
			break;
		case MUS_CMD_PITCH_BEND: 
			tmp = ( ev.data.pitch_bend.bend_amount - 128 ) * 64 + 8192;
			MIDI_CMD( 0xe0 + midiChannel ); 
			MIDI_CMD( tmp ); 
			break;
		case MUS_CMD_SYSTEM_EVENT:
			switch ( ev.data.system_event.event ) 
			{
			case MUS_SYSTEM_EVENT_ALL_SOUNDS_OFF:
				MIDI_CMD( 0xb0 + midiChannel ); 
				MIDI_CMD( 0x78 ); 
				MIDI_CMD( 0x00 ); 
				break;
			case MUS_SYSTEM_EVENT_ALL_NOTES_OFF:
				MIDI_CMD( 0xb0 + midiChannel ); 
				MIDI_CMD( 0x7b ); 
				MIDI_CMD( 0x00 ); 
				break;
			case MUS_SYSTEM_EVENT_MONO:
			case MUS_SYSTEM_EVENT_POLY:
				break;
			case MUS_SYSTEM_EVENT_RESET_ALL_CONTROLLERS:
				//tsf_channel_midi_control( pTSF, ev.channel, 121, 0 );
				break;
			}
			break;
		case MUS_CMD_CONTROLLER: 
			tmp = ev.data.controller.value;
			if ( ev.data.controller.controller == 0 )
			{
				MIDI_CMD( 0xc0 + midiChannel ); 
				MIDI_CMD( tmp ); 
			} else
			if ( ev.data.controller.controller <= 9 )
			{
				MIDI_CMD( 0xb0 + midiChannel ); 
				MIDI_CMD( mapMIDIController[ev.data.controller.controller] ); 
				MIDI_CMD( tmp ); 
			}
		 break;
		case MUS_CMD_END_OF_MEASURE:
			break;
		case MUS_CMD_FINISH:
			mus_restart( pMUS );
			break;
		case MUS_CMD_RENDER_SAMPLES: 
			tmp = ev.data.render_samples.samples_count;
			if ( tmp > nSamplesNeeded ) 
			{
				nSamplesRemaining = tmp - nSamplesNeeded;
				tmp = nSamplesNeeded;
			}
			nSamplesNeeded -= tmp;
			break;
		}
	}
	musicPendingSamples = nSamplesRemaining;
}

#endif

void musicRender( int nSamples ) 
{
	if ( !pMUS ) return;

	if ( musicReset ) 
	{
		tsf_reset( pTSF );
		tsf_set_volume( pTSF, (float)musicVolume * 2.0f / 127.0f );
		musicReset = false;
	}

	tsf_set_volume( pTSF, (float)musicVolume * 2.0f / 127.0f );

	float *output = soundMixingBuffer;

	int nSamplesNeeded = nSamples;
	int nSamplesRemaining = 0;

	if ( musicPendingSamples > 0 ) 
	{
		int count = musicPendingSamples;
		if ( count > nSamplesNeeded ) {
			nSamplesRemaining = count - nSamplesNeeded;
			count = nSamplesNeeded;
		}
		tsf_render_float( pTSF, output, count, 0 );
		nSamplesNeeded -= count;
		output += count;
	}
	if ( nSamplesRemaining > 0 ) 
	{
		musicPendingSamples = nSamplesRemaining;
		return;
	}

	while ( nSamplesNeeded ) 
	{
		int tmp;
		mus_event_t ev = { 0 };
		mus_next_event( pMUS, &ev );
		switch ( ev.cmd ) 
		{
		case MUS_CMD_PLAY_NOTE:
			tsf_channel_note_on( pTSF, ev.channel, ev.data.play_note.note, ev.data.play_note.volume / 127.0f );
			break;
		case MUS_CMD_RELEASE_NOTE:
			tsf_channel_note_off( pTSF, ev.channel, ev.data.release_note.note );
			break;
		case MUS_CMD_PITCH_BEND: 
			tmp = ( ev.data.pitch_bend.bend_amount - 128 ) * 64 + 8192;
			tsf_channel_set_pitchwheel( pTSF, ev.channel, tmp );
			break;
		case MUS_CMD_SYSTEM_EVENT:
			switch ( ev.data.system_event.event ) 
			{
			case MUS_SYSTEM_EVENT_ALL_SOUNDS_OFF:
				tsf_channel_sounds_off_all( pTSF, ev.channel );
				break;
			case MUS_SYSTEM_EVENT_ALL_NOTES_OFF:
				tsf_channel_note_off_all( pTSF, ev.channel );
				break;
			case MUS_SYSTEM_EVENT_MONO:
			case MUS_SYSTEM_EVENT_POLY:
				break;
			case MUS_SYSTEM_EVENT_RESET_ALL_CONTROLLERS:
				tsf_channel_midi_control( pTSF, ev.channel, 121, 0 );
				break;
			}
			break;
		case MUS_CMD_CONTROLLER: 
			tmp = ev.data.controller.value;
			switch ( ev.data.controller.controller ) 
			{
			case MUS_CONTROLLER_CHANGE_INSTRUMENT:
				if ( ev.channel == 15 ) 
					tsf_channel_set_presetnumber( pTSF, 15, 0, 1 ); else 
					tsf_channel_set_presetnumber( pTSF, ev.channel, tmp, 0 );
				break;
			case MUS_CONTROLLER_BANK_SELECT:
				tsf_channel_set_bank( pTSF, ev.channel, tmp );
				break;
			case MUS_CONTROLLER_VOLUME:
				tsf_channel_midi_control( pTSF, ev.channel, 7, tmp );
				break;
			case MUS_CONTROLLER_PAN:
				tsf_channel_midi_control( pTSF, ev.channel, 10, tmp );
				break;
			case MUS_CONTROLLER_EXPRESSION:
				tsf_channel_midi_control( pTSF, ev.channel, 11, tmp );
				break;
			case MUS_CONTROLLER_MODULATION:
			case MUS_CONTROLLER_REVERB_DEPTH:
			case MUS_CONTROLLER_CHORUS_DEPTH:
			case MUS_CONTROLLER_SUSTAIN_PEDAL:
			case MUS_CONTROLLER_SOFT_PEDAL:
				break;
			}
		 break;
		case MUS_CMD_END_OF_MEASURE:
			break;
		case MUS_CMD_FINISH:
			mus_restart( pMUS );
			break;
		case MUS_CMD_RENDER_SAMPLES: 
			tmp = ev.data.render_samples.samples_count;
			if ( tmp > nSamplesNeeded ) 
			{
				nSamplesRemaining = tmp - nSamplesNeeded;
				tmp = nSamplesNeeded;
			}
			tsf_render_float( pTSF, output, tmp, 0 );
			nSamplesNeeded -= tmp;
			output += tmp;
			break;
		}
	}
	musicPendingSamples = nSamplesRemaining;
}

static boolean musicInit() 
{
	musicReset = true;
	musicVolume = 127;
	return true;
}

static void musicQuit() 
{
	if ( pMUS ) mus_destroy( pMUS );
	pMUS = 0;
}

static void musicSetVolume( int v ) 
{
	musicVolume = v;
}

static void *musicRegisterSong( void *data, int len ) 
{
	pMusicRAWData = data;
	musicLength = len;
	return 0;
}

static void musicUnregisterSong( void *handle ) 
{
	pMusicRAWData = 0;
	musicLength = 0;
}

static void musicPlaySong( void *handle, boolean looping ) 
{
	musicQuit();
	pMUS = mus_create( pMusicRAWData, musicLength, 0 );
	mus_set_rate( pMUS, SAMPLE_RATE );
	#ifdef USE_MIDI
	midiCur = midiLast = 0;
	#endif
	musicPendingSamples = 0;
	musicReset = true;
}

static void musicStopSong() 
{
	mus_destroy( pMUS );
	pMUS = 0;
	musicPendingSamples = 0;
	musicReset = true;
}

static boolean musicIsPlaying() 
{
	return false;
}

static void musicNotImplemented() {}

static snddevice_t musicDeviceRAD[] = { SNDDEVICE_AWE32 };

music_module_t musicModuleRAD = 
{
	.sound_devices     = musicDeviceRAD,
	.num_sound_devices = arrlen( musicDeviceRAD ),
	.Init              = musicInit,
	.Shutdown          = musicQuit,
	.SetMusicVolume    = musicSetVolume,
	.PauseMusic        = musicNotImplemented,
	.ResumeMusic       = musicNotImplemented,
	.RegisterSong      = musicRegisterSong,
	.UnRegisterSong    = musicUnregisterSong,
	.PlaySong          = musicPlaySong,
	.StopSong          = musicStopSong,
	.MusicIsPlaying    = musicIsPlaying,
	.Poll              = musicNotImplemented,
};


#endif
