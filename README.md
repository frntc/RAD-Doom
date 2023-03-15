<table  align="right">
<tr><td>
<img  align="right"  width="280"  <img  align="right"  width="280"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/RAD-Doom.jpg">
</td></tr>
<tr><td  align="center">
<font  size="-1">image courtesy of <a  href="https://www.youtube.com/c/emulaThor">emulaThor</a></font>
</td></tr>
</table)

Welcome to this tech-demo for the **RAD Expansion Unit**. The "RAD" is a Raspberry Pi-powered cartridge/expansion for the C64 and C128 and, for example, can be used to emulate memory expansions (please have a look at its [main page](https://github.com/frntc/RAD)).

In this tech-demo the **RAD replaces the MOS6510/8500/8502-CPU of your C64/C128** and natively runs DOOM on the ARM CPU. The purpose of the demo is to experiment with real-time graphics (on-the-fly color reduction and conversion from RGB-frame buffers), sound streaming to the SID, and using C64 peripherals such as keyboard, mouse and MIDI-interfaces. Note that it is currently set up for **PAL machines only**.

In short, here is what you can experience:

- play Doom on your C64/C128 (based on [Doom Generic](https://github.com/ozkl/doomgeneric))

- stable 50 frames per second rendering

- 22050Hz sample playback using the SID (MOS6581/8580/SIDKick) or Digimax

- optional: special dithering modes for color mixing and exploiting interlacing peculiarities of TFT displays

- optional: playing game-music via MIDI (Datel/Sequential interfaces) and Digimax sound output


[emulaThor's youtube channel](https://www.youtube.com/@emulaThor) has videos showing gameplay and sound options:


<a  href="https://www.youtube.com/watch?v=tG2TMkBB6JU">
<img align="right" width="128"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/thumb_gameplay.jpg">
</a>
<a  href="https://www.youtube.com/watch?v=bXb6k6sJZ9k">
<img align="right" width="128"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/thumb_sound.jpg">
</a>
  

## Quickstart/Setup

Obviously you need a RAD Expansion Unit (see [project page](https://github.com/frntc/RAD) on how to build and setup one) and a Raspberry Pi 3A+/3B+/Zero 2. To play Doom, first download the release archive of this project and copy it to a FAT-formatted SD-card.

Second, you need to download the "doom1.wad" of the shareware version of Doom (e.g. from [doomwiki.org](https://doomwiki.org/wiki/DOOM1.WAD)) and copy it into the "RADDOOM" folder on the SD-card.

You also need a *sound font* in *.sf2*-format for the built-in MIDI synthesizer which plays via the SID or Digimax (the sound synthesis uses [TinySoundFont](https://github.com/schellingb/TinySoundFont)). This file also has to be copied to the "RADDOOM"-folder and named *"soundfont.sf2"*. One freely available sound font (but there are many others) is the [GeneralUser GS](http://schristiancollins.com/generaluser.php), which is also included in the [mt32pi](https://github.com/dwhinham/mt32-pi) release. mt32pi is a MIDI-synthesizer based on a Raspberry Pi which can be used with MIDI interfaces for the C64/C128. Note that the size of the sound font-file dominates the loading time.

Speaking of MIDI interfaces: external MIDI interfaces will requires a cartridge expander. The [SIDKick](https://github.com/frntc/SIDKick) provides a MIDI interface while keeping the expansion port free. The Digimax output expects the device connected to the user port.

To **configure the sound output**, check the *config.txt* on the SD-card. In case the RAD needs some timing adjustments, please follow the instructions on the RAD main page and adjust timings first with the standard RAD software.

Pressing *F1* while running Doom shows a help screen, *F3* toggles between mouse (port #2) and keyboard controls.
  

## Technical Details

I will add some more technical details here on request. In short, the framework which enables running Doom (and maybe other projects in the future) takes a 320x200 RGB8 framebuffer, a floating point audio stream and MIDI commands as input and converts and sends both to the VIC2 and SID/MIDI/Digimax.

### Color Reduction & Dithering

The frame buffer is displayed using plain multicolor mode. First it is downsampled to 160x200, dithering is applied (more on this below), and the resulting colors are mapped to the VIC2 color palette. Next, the 4x8 block-based color restrictions are enforced by iteratively mapping the least-frequent color in a block to the most similar one available in this block. While doing so, the relative frequencies are updated, and more colors removed if necessary. The choice of the background color is based on a simple (yet seemingly good) heuristic: the palette entry which is present across the most blocks in frame N is used as background color in frame N+1.

As you may have seen in other demos and image formats, colors can be mixed by showing two alternating images. Two alternating light stimuli are perceived as steady when the flicker frequency is above the *critical flicker fusion frequency* (CFFF) or *flicker fusion threshold*. Unfortunately the 50Hz of the VIC2 output is not a high frequency in terms of flicker fusion, but some colors (in particular those of low or similar luminance) can be mixed with acceptable flickering. Since graphics output is running at stable 50Hz, this color mixing can optionally be exploited.

This color mixing works pretty decent with CRT displays. The behavior of TFT displays (and likewise USB video capture devices) with the VIC2 output is also interesting and I experienced different effects across displays:

- if you are unlucky, your TFT will simply display every other image only (25Hz)

- the two half-frames (two VIC2 images) are mixed somewhat similar to what CRTs output, i.e. color mixing works (yet there seems to be some weighting of images over time, similar to exponential weighted averaging)

- the two half-frames are displayed interlaced. This is nice, because by controlling the stable 50Hz output we can actually exploit this fact and dither colors for display on a virtual 160x400 resolution!

For most displays I could test, I observed interlaced display combined with a slight temporal effect. To exploit this, the dithering stage uses rectangular, partly modified Bayer dithering matrices for adaptive mixing/interlacing which alternate every frame. Additionally there is also a Bluenoise dithering option (see [this webpage for information how to compute these dither matrices](http://momentsingraphics.de/BlueNoise.html)). When playing Doom there are a couple of parameters that allow you to control dithering and color mixing so that you can adjust and experiment with these settings. The following images illustrate some dithering possibilities reproduced for display here:

<table>
<tr>
<td>  <img  width="318"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/reference.jpg">  </td>
<td>reference RGB-image
</tr>
<tr>
<td>  <img  width="318"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/multicolor.jpg">  </td>
<td>standard multicolor mode (160x200, 4 colors per 4x8 block)
</tr>
<tr>
<td>  <img  width="318"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/ideal_flickerfusion.jpg">  </td>
<td>idealistic reproduction of mixing two multicolor images <br> (in particular bright image regions would flicker)
</tr>
<tr>
<td>  <img  width="318"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/practical_flickerfusion.jpg">  </td>
<td>idealistic reproduction of mixing only is low-luminance regions (little flickering)
</tr>
<tr>
<td>  <img  width="318"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/interlace_flicker.jpg">  </td>
<td>reproduction of interlaced display with dithering for 160x400 resolution
<br> (on some "modern" displays this flickers for dynamic content)
</tr>
<tr>
<td>  <img  width="318"  src="https://raw.githubusercontent.com/frntc/RAD-Doom/main/Images/interlace_flickerfree.jpg">  </td>
<td>reproduction of interlaced display (160x400 resolution) with modified dithering to suppress flickering
</tr>
</table>
  

## Audio Mixing and Streaming

Upon startup the framework detects whether there is a MOS 6581, a MOS 8580, or a SIDKick (Digimax and MIDI is configured manually, see above). In case none of these is present it assumes there is a SwinSID (or another inferior replacement) and the sound is output as plain 4 Bit samples. Real SIDs, good replacements and Digimax use Mahoney's technique/8 Bit samples, the latest SIDKick firmware provides a better quality, pure 8-Bit DAC output mode.

The transfer of the audio stream is handled using an interrupt running on the RPi, except during DMA transfers (e.g., copying the frame buffer data) where writes to the SID/Digimax are manually timed. The MIDI output is sent at 50Hz together with the image.
  

## Disclaimer

Be careful not to damage your RPi or 8-bit computer, or anything attached to it. I am not responsible if you or your hardware gets damaged. In principle, you can attach the RPi and other cartridges at the same time, as long as they do not conflict (e.g. in IO ranges or driving signals). I recommend to NOT do this. Again, I'm not taking any responsibility -- if you don't know what you're doing, better don't... use everything at your own risk.
  

## Where did you get your RAD? How to get one?

You've built it yourself? Cool, this project is for tinkerers!

If you have questions about assembling one, don't hesitate to ask!

If you can't build one yourself, there are official sellers of Sidekick64/RAD/SIDKick: www.restore-store.de (all three projects), [www.retro-updates.com](http://www.retro-updates.com) (Sidekick64).

*It is also perfectly fine and appreciated* if someone sells spare PCBs (populated or not) of a PCB-order or manufactures a small batch and offers them on a forum, but I expect the price tag to be lower than that of the aforementioned official sellers.

If you bought a Sidekick64/RAD/SIDKick for the same price or even more, this clearly is a violation of the license and you should ask the seller why they are not respecting open source/CC developers' licenses.   

## License

The *source code* is licensed under *GPLv3*.
The *RAD-PCB* is work licensed under a *Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License*.


## Misc

There are many people to thank, please have a look at the RAD main page. Here special thanks fly to emulaThor ([youtube](https://www.youtube.com/@emulaThor), [github](https://github.com/hpingel)) for patient and extensive testing and providing continuous feedback! The default soundtrack in the intro is [*dazzler* by Jester](https://modarchive.org/module.php?65221), the intro uses the [audiowide font](https://www.1001freefonts.com/audiowide.font), and in-game status messages use [Retrofan's system font](https://compidiaries.wordpress.com/).


### Trademarks

Doom is a trademark of ID SOFTWARE LLC. 

Raspberry Pi is a trademark of the Raspberry Pi Foundation.

  