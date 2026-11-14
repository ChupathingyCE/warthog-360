/*
X360_FRAME_COUNTERS.C

The frame's counts (x360_frame_counters.h), logged every 10 seconds in
every build, and the renderer's time from the game's side (d3d8_gl.c calls
console_render_time around its Direct3D entry points).
*/

#include <xtl.h>
#include <string.h>

#include "x360_port.h"
#include "x360_frame_counters.h"

struct x360_frame_counters x360_frame_counters;

#define REPORT_SECONDS 10

/* the game's (port/console/game/console_perf_log.c): the render's part
running now, NONE (-1) while console.perf_log is off */
int halo_perf_render_part_now(void);
/* (halo_perf_log.h's _halo_perf_render_effects) */
#define PERF_RENDER_EFFECTS 5

static double ticks_per_ms;
static unsigned long long period_start, last_present;
static int render_depth, logged_clock;
static unsigned long long render_entered;

static void frequency_read(void)
{
	LARGE_INTEGER frequency;

	if (ticks_per_ms > 0.0)
		return;
	/* (the console's performance counter is the time base) */
	if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart)
		ticks_per_ms = (double)frequency.QuadPart / 1000.0;
	else
		ticks_per_ms = 49875.0;
}

/* the game entering (1) or leaving (0) one of the renderer's entry points;
nested calls count once */
void console_render_time(int entering)
{
	if (entering)
	{
		if (render_depth++ == 0)
			render_entered = x360_ticks();
	}
	else if (render_depth > 0 && --render_depth == 0)
		x360_frame_counters.renderer_ticks += x360_ticks() - render_entered;
}

void x360_count_draw(void)
{
	x360_frame_counters.draws++;
	if (halo_perf_render_part_now() == PERF_RENDER_EFFECTS)
		x360_frame_counters.effect_draws++;
}

void x360_frame_counters_present(const char *renderer)
{
	struct x360_frame_counters *c = &x360_frame_counters;
	unsigned long long now = x360_ticks();
	double frames, frame_ms, renderer_ms, device_ms, present_ms, front_ms;
	MEMORYSTATUS memory;

	frequency_read();
	if (!period_start)
	{
		/* (the first frame starts the first period) */
		period_start = last_present = now;
		memset(c, 0, sizeof(*c));
		return;
	}
	c->frames++;
	c->frame_ticks += now - last_present;
	if (c->frames == 300 && !logged_clock++)
		x360_log("frame: the time base, %.0f ticks a millisecond; %llu ticks in 300 frames", ticks_per_ms,
			now - period_start);
	last_present = now;
	if ((double)(now - period_start) < REPORT_SECONDS * 1000.0 * ticks_per_ms || !c->frames)
		return;
	frames = (double)c->frames;
	frame_ms = (double)c->frame_ticks / ticks_per_ms / frames;
	renderer_ms = (double)c->renderer_ticks / ticks_per_ms / frames;
	device_ms = (double)c->device_ticks / ticks_per_ms / frames;
	present_ms = (double)c->present_ticks / ticks_per_ms / frames;
	/* (the renderer's own share: the game's calls less the layer below and
	the present) */
	front_ms = renderer_ms - device_ms - present_ms;
	if (front_ms < 0.0)
		front_ms = 0.0;
	GlobalMemoryStatus(&memory);
	x360_log("frame (%s): %.1f fps, %.2f ms a frame: game %.2f, renderer %.2f (translation %.2f, device %.2f), "
		"present %.2f; %lu MB free", renderer, frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0, frame_ms,
		frame_ms - renderer_ms, renderer_ms, front_ms, device_ms, present_ms,
		(unsigned long)(memory.dwAvailPhys >> 20));
	x360_log("frame (%s): a frame: %lu draws (%lu in effects), %lu clears, %lu resolves, %lu occlusion queries "
		"(%lu waits), %lu render states, %lu sampler states, %lu textures, %lu+%lu constants, %lu shaders, "
		"%lu declarations, %lu streams, %lu index buffers; copied %lu KB vertices, %lu KB indices, %lu KB texels; "
		"in %lu frames: %lu texture uploads, %lu shaders made; %lu locks asked (%lu on memory drawn in place, %lu busy, %lu waited)",
		renderer, (unsigned long)(c->draws / frames), (unsigned long)(c->effect_draws / frames),
		(unsigned long)(c->clears / frames), (unsigned long)(c->resolves / frames),
		(unsigned long)(c->queries / frames), (unsigned long)(c->query_waits / frames),
		(unsigned long)(c->render_states / frames),
		(unsigned long)(c->sampler_states / frames), (unsigned long)(c->textures / frames),
		(unsigned long)(c->vertex_constants / frames), (unsigned long)(c->pixel_constants / frames),
		(unsigned long)(c->shaders / frames), (unsigned long)(c->declarations / frames),
		(unsigned long)(c->streams / frames), (unsigned long)(c->index_buffers / frames),
		(unsigned long)(c->ring_bytes / frames / 1024), (unsigned long)(c->index_bytes / frames / 1024),
		(unsigned long)(c->texture_bytes / frames / 1024), c->frames, c->texture_uploads, c->shaders_made,
		c->lock_asked, c->lock_marked, c->lock_busy, c->lock_waits);
	memset(c, 0, sizeof(*c));
	period_start = now;
}
