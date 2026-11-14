/*
EDRAM_TEST.C

x360_gl.c's EDRAM allocator (texture_surface, and the sharing around it) on
the host. run.sh takes struct texture and the EDRAM section of x360_gl.c
(build/xbox360-tests/edram_under_test.inc), and this file stands in for
Direct3D and XGraphics, with XGSurfaceSize's arithmetic: tiles of 80x16
pixels at 32 bits, 2,048 of them (10 MB).

Each case makes the targets the game draws into, at a screen size, in the
order the game first binds them, and checks:
- every target gets a surface (its own tiles, or tiles it shares);
- a color target and the depth drawn with it never share tiles;
- two color targets share tiles only through the allocator's own sharing
  (edram_targets, which resolves one before the other draws), or the one
  screen-sized target that may take the back buffer's.

Before warthog-360#48, at 1280x720 the screen effect target and the scope's
zoom targets got no surface and drew into the 3D view (camouflage upside
down, stripes in the scope: warthog-360#47).

    edram_test <case>      1280x720 | 1280x720-sharp-mirrors | 1920x1080
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the Direct3D and XGraphics the section uses */

typedef int BOOL;
typedef unsigned long DWORD;
typedef long HRESULT;
typedef unsigned int GLenum;
typedef int GLint;
typedef float GLfloat;
typedef enum { D3DFMT_A8R8G8B8 = 1, D3DFMT_D24S8 = 2 } D3DFORMAT;
#define TRUE 1
#define FALSE 0
#define FAILED(result) ((result) < 0)
#define D3DMULTISAMPLE_NONE 0
#define X360_PHYS_TEXTURE 1
#define X360_PHYS_TARGET 2

typedef struct D3DSurface
{
	DWORD base, tiles;
	BOOL depth;
} D3DSurface;
typedef struct D3DBaseTexture { int unused; } D3DBaseTexture;
typedef D3DBaseTexture D3DTexture;
typedef D3DBaseTexture D3DResource;
typedef struct
{
	DWORD Base;
	DWORD HierarchicalZBase;
	DWORD ColorExpBias;
} D3DSURFACE_PARAMETERS;

static int log_lines;

static void x360_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("  x360_gl: ");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
	log_lines++;
}

static DWORD XGSurfaceSize(DWORD width, DWORD height, D3DFORMAT format, int multisample)
{
	(void)format;
	(void)multisample;
	return ((width + 79) / 80) * ((height + 15) / 16);
}

static HRESULT surface_make(DWORD width, DWORD height, D3DFORMAT format, D3DSurface **surface,
	const D3DSURFACE_PARAMETERS *parameters)
{
	DWORD tiles = XGSurfaceSize(width, height, format, 0);

	/* (Direct3D refuses a surface that runs past the end of EDRAM) */
	if (parameters->Base + tiles > 2048)
		return -1;
	*surface = calloc(1, sizeof(**surface));
	(*surface)->base = parameters->Base;
	(*surface)->tiles = tiles;
	(*surface)->depth = format == D3DFMT_D24S8;
	return 0;
}

#define IDirect3DDevice9_CreateRenderTarget(device, width, height, format, multisample, quality, lockable, surface, \
	parameters) ((void)(device), surface_make(width, height, format, surface, parameters))
#define IDirect3DDevice9_CreateDepthStencilSurface(device, width, height, format, multisample, quality, discard, \
	surface, parameters) ((void)(device), surface_make(width, height, format, surface, parameters))
#define IDirect3DDevice9_CreateTexture(device, width, height, levels, usage, format, pool, texture, shared) \
	((*(texture) = calloc(1, sizeof(D3DBaseTexture))) ? 0 : -1)
#define IDirect3DResource9_Release(resource) free(resource)
#define IDirect3DDevice9_SetRenderTarget(device, index, surface) ((void)(surface))
#define IDirect3DDevice9_SetDepthStencilSurface(device, surface) ((void)(surface))
#define x360_phys_account(kind, bytes) ((void)(kind), (void)(bytes))
#define x360_phys_avail() 0UL
#define x360_phys_failure(what, size) x360_log("phys failure: %s", what)

static struct
{
	void *device;
	DWORD edram_next;
	BOOL bound_screen;
	D3DSurface *bound_color, *bound_depth;
} gl;

/* (what the section calls from the rest of x360_gl.c: below) */
struct texture;
static void texture_count_phys(struct texture *texture, unsigned long before);
static void texture_resolve(struct texture *texture);
static void texture_memory_uncount(struct texture *texture);
static void blit_quad(struct texture *source, float u0, float v0, float u1, float v1, float x0, float y0, float x1,
	float y1, unsigned long width, unsigned long height, BOOL linear, BOOL to_screen);

#include "edram_under_test.inc"

/* (what the section declares and the rest of x360_gl.c defines) */
static void texture_count_phys(struct texture *texture, unsigned long before)
{
	(void)texture;
	(void)before;
}

static void texture_resolve(struct texture *texture) { (void)texture; }

static void texture_memory_uncount(struct texture *texture) { (void)texture; }

static void blit_quad(struct texture *source, float u0, float v0, float u1, float v1, float x0, float y0, float x1,
	float y1, unsigned long width, unsigned long height, BOOL linear, BOOL to_screen)
{
	(void)source; (void)u0; (void)v0; (void)u1; (void)v1; (void)x0; (void)y0; (void)x1; (void)y1;
	(void)width; (void)height; (void)linear; (void)to_screen;
}

/* ---------- the game's targets */

#define TARGETS 16

struct target
{
	const char *name;
	unsigned long width, height;
	BOOL depth;
	/* the depth target drawn with this color target (an index, or -1) */
	int drawn_with;
	struct texture texture;
};

static struct target targets[TARGETS];
static int target_count;
static int failures;

static int add(const char *name, unsigned long width, unsigned long height, BOOL depth, int drawn_with)
{
	struct target *target = &targets[target_count];

	target->name = name;
	target->width = width;
	target->height = height;
	target->depth = depth;
	target->drawn_with = drawn_with;
	memset(&target->texture, 0, sizeof(target->texture));
	target->texture.width = width;
	target->texture.height = height;
	target->texture.depth_format = depth;
	return target_count++;
}

static void fail(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("FAIL: ");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
	failures++;
}

static BOOL overlap(const D3DSurface *a, const D3DSurface *b)
{
	return a->base < b->base + b->tiles && b->base < a->base + a->tiles;
}

static BOOL shared_by_the_allocator(const struct texture *a, const struct texture *b)
{
#ifdef EDRAM_TARGETS
	/* (warthog-360#48: both tracked, so binding one resolves the other) */
	int index;
	BOOL a_tracked = FALSE, b_tracked = FALSE;

	for (index = 0; index < EDRAM_TARGETS; index++)
	{
		a_tracked |= edram_targets[index] == a;
		b_tracked |= edram_targets[index] == b;
	}
	return a_tracked && b_tracked;
#else
	(void)a;
	(void)b;
	return FALSE;
#endif
}

/* the targets of a frame with a mirror or camouflage, then the scope's zoom:
the 3D view first, as the game binds them */
static void game_targets(unsigned long width, unsigned long height, BOOL sharp_mirrors)
{
	int view_depth;
	unsigned long divisor = (height + 180) / 360;
	int index;

	add("3D view", width, height, FALSE, 1);
	view_depth = add("3D view depth", width, height, TRUE, -1);
	/* (the secondary target, drawn with the 3D view's depth: the Xbox's
	320x240, or the 3D view's pixels with the sharper mirrors change) */
	if (sharp_mirrors)
		add("secondary (mirrors, camouflage)", width, height, FALSE, view_depth);
	else
		add("secondary (mirrors, camouflage)", 320, 240, FALSE, -1);
	add("water", 128, 128, FALSE, -1);
	add("shadow primary", 128, 128, FALSE, -1);
	add("shadow secondary", 128, 128, FALSE, -1);
	add("sun glow primary", 64, 64, FALSE, -1);
	add("sun glow secondary", 64, 64, FALSE, -1);
	/* (the scope's zoom: three targets a 360th of the view's lines each,
	rasterizer_xbox_screen_effect.c's fast warp) */
	for (index = 0; index < 3; index++)
		add("scope zoom", width / divisor, height / divisor, FALSE, -1);
}

static void check_targets(void)
{
	int i, j;

	for (i = 0; i < target_count; i++)
	{
		struct target *target = &targets[i];

		if (!texture_surface(&target->texture) || !target->texture.surface)
		{
			fail("%s (%lux%lu%s) got no EDRAM: its draws go into whatever target was bound last", target->name,
				target->width, target->height, target->depth ? " depth" : "");
			continue;
		}
		printf("  %-32s %4lux%-4lu tiles %4lu-%4lu\n", target->name, target->width, target->height,
			target->texture.surface->base, target->texture.surface->base + target->texture.surface->tiles - 1);
	}
	for (i = 0; i < target_count; i++)
	{
		struct target *a = &targets[i];

		if (!a->texture.surface)
			continue;
		if (a->drawn_with >= 0 && targets[a->drawn_with].texture.surface &&
			overlap(a->texture.surface, targets[a->drawn_with].texture.surface))
			fail("%s and %s are drawn together but share EDRAM tiles", a->name, targets[a->drawn_with].name);
		for (j = i + 1; j < target_count; j++)
		{
			struct target *b = &targets[j];

			if (a->depth || b->depth || !b->texture.surface || !overlap(a->texture.surface, b->texture.surface))
				continue;
			/* (the back buffer's tiles: one target may take them, as it is
			only drawn when the frame is presented) */
			if (a->texture.surface->base == 0 || b->texture.surface->base == 0)
				continue;
			if (!shared_by_the_allocator(&a->texture, &b->texture))
				fail("%s and %s share EDRAM tiles that nothing hands between them", a->name, b->name);
		}
	}
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";
	unsigned long width, height;
	BOOL sharp_mirrors = FALSE;

	if (!strcmp(name, "1280x720"))
		width = 1280, height = 720;
	else if (!strcmp(name, "1280x720-sharp-mirrors"))
		width = 1280, height = 720, sharp_mirrors = TRUE;
	else if (!strcmp(name, "1920x1080"))
		width = 1920, height = 1080;
	else
	{
		fprintf(stderr, "usage: edram_test 1280x720 | 1280x720-sharp-mirrors | 1920x1080\n");
		return 2;
	}
	/* (as gl_initialize: the screen's back buffer, 1280x720, first) */
	gl.edram_next = XGSurfaceSize(1280, 720, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
	printf("edram_test %s: back buffer tiles 0-%lu of %d\n", name, gl.edram_next - 1, EDRAM_TILES);
	game_targets(width, height, sharp_mirrors);
	check_targets();
	if (failures)
	{
		printf("edram_test %s: %d failures\n", name, failures);
		return 1;
	}
	printf("edram_test %s: ok\n", name);
	return 0;
}
