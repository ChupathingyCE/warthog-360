/*
X360_GPU_READS.H
Which of the game's memory the GPU reads in place, and in which frame.

The GL layer draws the Xbox window's vertices where they are (the mirror's
segments are aliases of the window). The game rewrites its dynamic vertex
buffers in that memory every frame, so a draw still in flight on the GPU
could read the next frame's vertices, or half of them: stretched triangles,
wedges and warped HUD shapes on the console (Xenia's GPU keeps up, so it
never shows them). This table remembers, per 1 KB of the window, the frame
whose draw last read it in place; a lock of that memory asks whether the
GPU is done with that frame (halo_resource_busy, xbox360_gpu_hooks.c) and
takes other storage or waits. No SDK here: the host tests build it.
*/
#ifndef __X360_GPU_READS_H
#define __X360_GPU_READS_H

/* the window: the 256 MB the console reserves (x360_memory.c) from Xbox
address 0x80000000, in 1 KB grains (512 KB of table) */
#define X360_GPU_READS_GRAIN 1024UL
#define X360_GPU_READS_SIZE 0x10000000UL
#define X360_GPU_READS_ENTRIES (X360_GPU_READS_SIZE / X360_GPU_READS_GRAIN)

/* the frames are kept in 16 bits: 0 means never read; a frame number
whose low 16 bits are 0 is skipped by the caller */

/* [offset, offset + size) of the window read in place by a draw of frame;
outside the window nothing is kept */
void x360_gpu_reads_mark(unsigned long offset, unsigned long size, unsigned short frame);

/* the newest frame that read any of [offset, offset + size) in place, as
seen from current (the frame being built); 0 if none did */
unsigned short x360_gpu_reads_newest(unsigned long offset, unsigned long size, unsigned short current);

/* forgets everything (a test's, or a new map's) */
void x360_gpu_reads_clear(void);

#endif
