/*
X360_FRAME_COUNTERS.H

A frame's counts and times, in every build, and one log line about them
every 10 seconds (x360_frame_counters.c): how many draws a frame makes,
what the renderer sets on the device for them, how many bytes it copies,
and where the processor's time goes (the game, the renderer, the present).
A tester's log then says where a slow frame's time went on real hardware.

The counts are additions to a structure; the times read the time base
register (mftb), a single instruction. Units that call the device count
their calls through the X360_COUNT_* macros.
*/

#ifndef __X360_FRAME_COUNTERS_H
#define __X360_FRAME_COUNTERS_H

struct x360_frame_counters
{
	/* the device calls made */
	unsigned long draws, clears, resolves;
	/* draws made in the render's effects part (the particles, contrails and
	the like), with console.perf_log on (the game says which part runs) */
	unsigned long effect_draws;
	/* occlusion queries begun, and results waited for (the GPU not done) */
	unsigned long queries, query_waits;
	/* textures made and given texels */
	unsigned long texture_uploads;
	unsigned long render_states, sampler_states, textures;
	unsigned long vertex_constants, pixel_constants; /* vectors */
	unsigned long shaders, declarations, streams, index_buffers;
	/* bytes copied for the GPU: vertices and indices into a ring, texels
	into textures */
	unsigned long ring_bytes, index_bytes, texture_bytes;
	/* shaders compiled or read from the drive in the period */
	unsigned long shaders_made;
	/* the game's locks that asked whether the GPU still reads the memory
	(xbox360_gpu_hooks.c): all of them, those on memory some draw read in
	place, those busy (given other storage), those that waited for the GPU */
	unsigned long lock_asked, lock_marked, lock_busy, lock_waits;
	/* time base ticks: in the renderer (the game's Direct3D calls, from
	d3d8_gl.c: console_render_time), in the device layer below it (x360_gl.c
	or d3d8_x360.c), and in the present */
	unsigned long long renderer_ticks, device_ticks, present_ticks;
	/* frames, and the ticks between the first one's start and the last one's */
	unsigned long frames;
	unsigned long long frame_ticks;
};

extern struct x360_frame_counters x360_frame_counters;

static __inline unsigned long long x360_ticks(void)
{
	unsigned long upper, lower, again;

	/* (the 32-bit reads of the 64-bit register: the upper half read again
	in case the lower carried into it between them) */
	do
	{
		__asm__ __volatile__("mftbu %0" : "=r"(upper));
		__asm__ __volatile__("mftb %0" : "=r"(lower));
		__asm__ __volatile__("mftbu %0" : "=r"(again));
	} while (upper != again);
	return ((unsigned long long)upper << 32) | lower;
}

/* (counted calls: x360_gl.c and d3d8_x360.c) */
#define X360_COUNT(field, count) (x360_frame_counters.field += (unsigned long)(count))

/* a draw made: counted, and in the effects part too when it runs */
void x360_count_draw(void);

/* a frame presented: counts it, and logs the period's line once it has run
10 seconds; renderer names the device layer in the line */
void x360_frame_counters_present(const char *renderer);

#endif
