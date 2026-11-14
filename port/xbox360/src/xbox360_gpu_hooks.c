/*
XBOX360_GPU_HOOKS.C
The game's GPU resource hooks (HALO_CONSOLE_GPU_RESOURCE_HOOKS,
halo_console_features.h) for the Xbox 360: a resource is busy while a frame
whose draws read its memory in place is not through on the GPU, and a lock
of a busy one waits, or takes other storage (d3d8_resources.c's rename
ring). The GL layer keeps the table (x360_gpu_reads.h, x360_gl.c); the
native renderer marks nothing yet, so under it nothing is busy, as before.

A resource's Lock field carries the frame that last read it, the way the
Switch's renderer stamps it: the rename ring probes with a Lock alone.
*/
#include "platform.h"
#include "xgpu.h"

#include "x360_frame_counters.h"

/* the GL layer's (x360_gl.c) and the native renderer's (d3d8_x360.c): one
of them is up */
BOOL x360_gl_active(void);
unsigned long x360_gl_memory_read_frame(const void *data, unsigned long size);
BOOL x360_gl_frame_done(unsigned long frame);
void x360_gl_frame_wait(unsigned long frame);
BOOL x360_native_active(void);
unsigned long x360_native_memory_read_frame(const void *data, unsigned long size);
BOOL x360_native_frame_done(unsigned long frame);
void x360_native_frame_wait(unsigned long frame);

static unsigned long memory_read_frame(const void *data, unsigned long size)
{
	if (x360_gl_active())
		return x360_gl_memory_read_frame(data, size);
	if (x360_native_active())
		return x360_native_memory_read_frame(data, size);
	return 0;
}

static BOOL frame_done(unsigned long frame)
{
	if (x360_gl_active())
		return x360_gl_frame_done(frame);
	if (x360_native_active())
		return x360_native_frame_done(frame);
	return TRUE;
}

static void frame_wait(unsigned long frame)
{
	if (x360_gl_active())
		x360_gl_frame_wait(frame);
	else if (x360_native_active())
		x360_native_frame_wait(frame);
}

BOOL halo_resource_busy(D3DResource *resource)
{
	unsigned long frame;

	if (!resource)
		return FALSE;
	if (resource->Data)
	{
		void *data = PLATFORM_PHYSICAL_TO_VIRTUAL(resource->Data);
		unsigned long size = platform_contiguous_block_size(data);

		frame = size ? memory_read_frame(data, size) : 0;
		resource->Lock = frame;
		X360_COUNT(lock_asked, 1);
		if (frame)
			X360_COUNT(lock_marked, 1);
	}
	else
		frame = resource->Lock;
	if (!frame || frame_done(frame))
		return FALSE;
	X360_COUNT(lock_busy, 1);
	return TRUE;
}

void halo_resource_wait(D3DResource *resource)
{
	if (halo_resource_busy(resource))
		frame_wait(resource->Lock);
}
