/*
D3D8_X360.C

The game's native renderer on the console (the game's console_renderer.h,
display.renderer = "native"): the Xbox's Direct3D 8 device, as the game's
port/linux/src/d3d8_console.inl hands it over draw by draw, drawn with the
console's Direct3D 9 directly, without the OpenGL layer (x360_gl.c, which
stays as the fallback, display.renderer = "gl").

The game's side keeps the Direct3D 8 meaning (which textures, which shader,
which state); this file is the graphics layer under it, and the only one
that calls Direct3D 9. Another layer (the GPU's own registers, say) could
take its place under the same calls.

How the Xbox's model meets the console's:
- Textures are made once from the texels the game side decodes, ARGB words
  in the processor's order (D3DFMT_A8R8G8B8), or DXT blocks as the maps have
  them, little-endian (D3DFMT_LE_DXT*): linear, as the GL layer makes them,
  or in the GPU's tiled layout (XGTileTextureLevel) with
  console.native_tiled_textures, a test. Nothing keeps a copy of them in the
  processor's memory.
- Render targets have EDRAM of their own, handed out once, and a texture
  each that a resolve copies their pixels into when a draw samples them.
- Shaders come as the game's GLSL, are turned into HLSL (x360_glsl.c),
  given fixed registers for every uniform (so a constant set stays set
  whatever shader draws next), and compiled once: the compiled code is kept
  on the drive (x360_shader_cache.c), so a later start reads it.
- State is set only when it differs from what the device has.
- Vertex and index data the game keeps (the map's) are read where they are
  in the Xbox window; the rest is copied into a ring of write-combined
  memory with aligned word stores, two frames of it, fenced.
- Occlusion queries are read a frame or more later, never waited for.
*/

#include <xtl.h>
#include <xgraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x360_port.h"
#include "x360_glsl.h"
#include "x360_frame_counters.h"
#include "x360_gpu_reads.h"
#include "../../../chupathingyce/port/linux/include/console_renderer.h"

/* (d3dx9mesh.h's: the drive's shader cache hands its bytes over in one) */
HRESULT WINAPI D3DXCreateBuffer(DWORD NumBytes, LPD3DXBUFFER *ppBuffer);

#define RING_SIZE (16 * 1024 * 1024)
/* EDRAM: 10 MB in tiles of 5,120 bytes; the screen's back buffer is in the
first 720 (x360_video.c's 1280x720, without depth) */
#define EDRAM_TILES 2048
#define SAMPLERS 4
#define QUERY_POOL 2048
#define QUERY_PENDING 4
#define QUERY_SLOTS 4096
#define BINDING_BUCKETS 512
#define DECLARATION_BUCKETS 256
#define RETIRED_MAXIMUM 512

/* ---------- objects */

struct console_texture
{
	D3DBaseTexture *d3d;
	int kind, format;
	unsigned long width, height, depth, levels;
	D3DFORMAT d3d_format;
	/* a render target's: its pixels drawn since the last resolve */
	struct console_target *target;
};

struct console_target
{
	D3DSurface *surface;
	struct console_texture texture;
	unsigned long width, height;
	int depth;
	BOOL unresolved;
};

struct console_shader
{
	int vertex;
	void *code;              /* ID3DXBuffer */
	D3DVertexShader *vertex_shader;
	D3DPixelShader *pixel_shader;
	/* the input registers the compiled vertex shader reads */
	unsigned long inputs;
};

/* a vertex shader bound to a layout (its fetches' formats and strides are
written into its code: x360_gl.c's vertex_shader_bound) */
struct binding
{
	struct binding *next;
	struct console_shader *shader;
	D3DVertexDeclaration *declaration;
	DWORD strides[CONSOLE_VERTEX_STREAMS + 1];
	D3DVertexShader *bound;
};

struct declaration
{
	struct declaration *next;
	unsigned long hash;
	int count;
	D3DVERTEXELEMENT9 elements[CONSOLE_VERTEX_REGISTERS + 1];
	D3DVertexDeclaration *d3d;
};

/* the uniforms' fixed registers: vertex and pixel, -1 where a stage has
none, and their vectors */
static const struct
{
	const char *name;
	int vertex, pixel, count;
} uniforms[NUMBER_OF_CONSOLE_UNIFORMS] =
{
	{ "c", 0, -1, 192 },
	{ "viewport_scale", 192, 47, 1 },
	{ "viewport_offset", 193, 48, 1 },
	{ "point_size", 194, -1, 1 },
	{ "screen_offset", 195, 46, 1 },
	{ "ps_c0", -1, 0, 8 },
	{ "ps_c1", -1, 8, 8 },
	{ "ps_final_c0", -1, 16, 1 },
	{ "ps_final_c1", -1, 17, 1 },
	{ "fog_color", -1, 18, 1 },
	{ "fog_parameters", -1, 19, 1 },
	{ "alpha_reference", -1, 20, 1 },
	{ "bump_matrix", -1, 21, 4 },
	{ "bump_luminance", -1, 25, 4 },
	{ "texture_scale", -1, 29, 4 },
	{ "texture_lod_bias", -1, 33, 1 },
	{ "model_lights", -1, 34, 12 },
};

static struct
{
	D3DDevice *device;
	BOOL ready;
	DWORD edram_next;
	BOOL screen_tiles_taken;
	/* the ring: this frame's half, and the fences of both */
	unsigned char *ring;
	unsigned long ring_offset;
	int half;
	DWORD fences[2];
	/* what the device has */
	struct console_target *color, *depth;
	BOOL screen_bound;
	BOOL targets_dirty;
	struct console_state state;
	BOOL state_known;
	struct console_sampler samplers[SAMPLERS];
	unsigned long sampler_levels[SAMPLERS];
	BOOL samplers_known[SAMPLERS];
	D3DVertexShader *vertex_shader;
	D3DPixelShader *pixel_shader;
	D3DVertexDeclaration *declaration;
	/* the last draw's layout, its declaration and binding */
	const void *last_layout;
	struct console_shader *last_shader;
	DWORD last_strides[CONSOLE_VERTEX_STREAMS + 1];
	unsigned long last_element_count;
	D3DVertexDeclaration *last_declaration;
	D3DVertexShader *last_bound;
	struct binding *bindings[BINDING_BUCKETS];
	struct declaration *declarations[DECLARATION_BUCKETS];
	/* stream and index headers: the device copies them into its commands
	at the draw, so one set serves */
	D3DVertexBuffer streams[CONSOLE_VERTEX_STREAMS + 1];
	D3DIndexBuffer indices;
	/* the present's quad */
	D3DVertexShader *blit_vertex;
	D3DPixelShader *blit_pixel;
	D3DVertexDeclaration *blit_declaration;
	/* resources released two frames on */
	struct
	{
		D3DResource *resources[RETIRED_MAXIMUM];
		int count;
	} retired[2];
	/* occlusion queries: free ones, and each slot's waiting for results */
	D3DQuery *free_queries[QUERY_POOL];
	int free_count, made_queries;
	D3DQuery *active_query;
	struct
	{
		D3DQuery *queries[QUERY_PENDING];
		unsigned char count;
	} slots[QUERY_SLOTS];
	unsigned long frame;
	unsigned long shaders_compiled, shaders_read, shader_failures;
} x;

/* ---------- the log, and the time in this layer */

#define DEVICE_TIME_BEGIN() unsigned long long device_ticks_start = x360_ticks()
#define DEVICE_TIME_END() (x360_frame_counters.device_ticks += x360_ticks() - device_ticks_start)

static void log_once(const char *what)
{
	static const char *seen[32];
	static int count;
	int index;

	for (index = 0; index < count; index++)
	{
		if (seen[index] == what)
			return;
	}
	if (count < (int)(sizeof(seen) / sizeof(seen[0])))
		seen[count++] = what;
	x360_log("native: %s", what);
}

/* ---------- resources the GPU may still read: released two frames on */

static void retire(D3DResource *resource)
{
	int half = x.half;

	if (!resource)
		return;
	if (x.retired[half].count == RETIRED_MAXIMUM)
	{
		int index;

		IDirect3DDevice9_BlockUntilIdle(x.device);
		for (index = 0; index < x.retired[half].count; index++)
			IDirect3DResource9_Release(x.retired[half].resources[index]);
		x.retired[half].count = 0;
	}
	x.retired[half].resources[x.retired[half].count++] = resource;
}

/* ---------- the ring: this frame's copies for the GPU */

/* words copied with aligned 32-bit stores: the ring is write-combined,
where the C library's copy is many times slower (and a misaligned wide
store faults) */
static void copy_words(unsigned long *destination, const void *source, unsigned long size)
{
	unsigned long words = size / 4, index;

	if (!((UINT_PTR)source & 3))
	{
		const unsigned long *from = source;

		for (index = 0; index + 4 <= words; index += 4)
		{
			unsigned long a = from[index], b = from[index + 1], c = from[index + 2], d = from[index + 3];

			destination[index] = a;
			destination[index + 1] = b;
			destination[index + 2] = c;
			destination[index + 3] = d;
		}
		for (; index < words; index++)
			destination[index] = from[index];
	}
	else
	{
		const unsigned char *from = source;

		for (index = 0; index < words; index++, from += 4)
			destination[index] = ((unsigned long)from[0] << 24) | ((unsigned long)from[1] << 16) |
				((unsigned long)from[2] << 8) | from[3];
	}
}

void *console_frame_copy(const void *data, unsigned long size)
{
	unsigned long half = RING_SIZE / 2, start = (unsigned long)x.half * half;
	unsigned long aligned = (x.ring_offset + 31) & ~31UL;
	unsigned char *memory;

	size = (size + 3) & ~3UL;
	if (aligned < start)
		aligned = start;
	if (!x.ring || size > half)
		return NULL;
	if (aligned + size > start + half)
	{
		/* (the frame's half is full: wait for the GPU to finish what is in
		it, and start it again) */
		log_once("a frame's copies passed the ring's half; waiting for the GPU");
		IDirect3DDevice9_BlockUntilIdle(x.device);
		aligned = start;
	}
	x.ring_offset = aligned + size;
	memory = x.ring + aligned;
	if (data)
	{
		copy_words((unsigned long *)memory, data, size);
		X360_COUNT(ring_bytes, size);
	}
	return memory;
}

void console_memory_written(const void *data, unsigned long size)
{
	unsigned long line = (unsigned long)(UINT_PTR)data & ~127UL;
	unsigned long end = (unsigned long)(UINT_PTR)data + size;

	for (; line < end; line += 128)
		__asm__ volatile ("dcbst 0, %0" : : "r"(line) : "memory");
	__asm__ volatile ("sync" : : : "memory");
}

/* ---------- start-up */

static const char blit_vertex_source[] =
	"struct output { float4 position : POSITION; float2 uv : TEXCOORD0; };\n"
	"output main(float4 position : POSITION, float2 uv : TEXCOORD0)\n"
	"{ output result; result.position = position; result.uv = uv; return result; }\n";
static const char blit_pixel_source[] =
	"sampler2D source : register(s0);\n"
	"float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(tex2D(source, uv).rgb, 1.0); }\n";

int console_renderer_initialize(void)
{
	void *code;

	if (x.ready)
		return 1;
	x.device = x360_video_device();
	if (!x.device)
		return 0;
	x.ring = x360_phys_alloc(X360_PHYS_OTHER, RING_SIZE, PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!x.ring)
	{
		x360_log("native: cannot allocate the ring");
		return 0;
	}
	/* (after the screen's back buffer: 1280x720, 32 bits, no depth) */
	x.edram_next = XGSurfaceSize(1280, 720, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
	code = x360_video_compile(blit_vertex_source, "vs_3_0", NULL);
	if (code)
	{
		x.blit_vertex = D3DDevice_CreateVertexShader(x360_video_shader_code(code));
		x360_video_shader_free(code);
	}
	code = x360_video_compile(blit_pixel_source, "ps_3_0", NULL);
	if (code)
	{
		x.blit_pixel = D3DDevice_CreatePixelShader(x360_video_shader_code(code));
		x360_video_shader_free(code);
	}
	{
		static const D3DVERTEXELEMENT9 elements[] =
		{
			{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};

		x.blit_declaration = D3DDevice_CreateVertexDeclaration(elements);
	}
	if (!x.blit_vertex || !x.blit_pixel || !x.blit_declaration)
	{
		x360_log("native: cannot make the present's shaders");
		return 0;
	}
	x.ready = TRUE;
	x.targets_dirty = TRUE;
	x360_log("native: ready (EDRAM from tile %lu, a ring of %u MB)", (unsigned long)x.edram_next, RING_SIZE >> 20);
	return 1;
}

/* ---------- textures */

/* (the game's settings: port_config.c) */
int config_boolean(const char *name);

/* console.native_tiled_textures: textures in the GPU's tiled layout
(XGTileTextureLevel), which it samples faster, or linear ones as the GL
layer makes them. Tiled is off by default for now: in Xenia the tiled
textures' colors differ from the linear ones' (Blood Gulch's ground and
cliffs), which matches the GL layer; a test on hardware decides. */
static BOOL textures_tiled(void)
{
	static int tiled = -1;

	if (tiled < 0)
		tiled = config_boolean("console.native_tiled_textures") ? 1 : 0;
	return tiled;
}

static D3DFORMAT texture_format(int format)
{
	BOOL tiled = textures_tiled();

	switch (format)
	{
	case _console_format_dxt1: return (D3DFORMAT)MAKELEFMT(tiled ? D3DFMT_DXT1 : D3DFMT_LIN_DXT1);
	case _console_format_dxt3: return (D3DFORMAT)MAKELEFMT(tiled ? D3DFMT_DXT2 : D3DFMT_LIN_DXT2);
	case _console_format_dxt5: return (D3DFORMAT)MAKELEFMT(tiled ? D3DFMT_DXT4 : D3DFMT_LIN_DXT4);
	default: return tiled ? D3DFMT_A8R8G8B8 : D3DFMT_LIN_A8R8G8B8;
	}
}

/* a level's rows into a locked linear texture, at its pitch */
static void rows_copy(unsigned char *destination, unsigned long pitch, const void *source, unsigned long row_bytes,
	unsigned long rows)
{
	unsigned long row;

	for (row = 0; row < rows; row++)
		memcpy(destination + row * pitch, (const unsigned char *)source + row * row_bytes, row_bytes);
}

struct console_texture *console_texture_create(int kind, int format, unsigned long width, unsigned long height,
	unsigned long depth, unsigned long levels)
{
	struct console_texture *texture = calloc(1, sizeof(*texture));
	HRESULT result;
	DEVICE_TIME_BEGIN();

	if (!texture)
		return NULL;
	texture->kind = kind;
	texture->format = format;
	texture->width = width;
	texture->height = height;
	texture->depth = depth ? depth : 1;
	texture->levels = levels ? levels : 1;
	texture->d3d_format = texture_format(format);
	if (kind == _console_texture_volume)
		result = IDirect3DDevice9_CreateVolumeTexture(x.device, width, height, texture->depth, texture->levels, 0,
			texture->d3d_format, 0, (D3DVolumeTexture **)&texture->d3d, NULL);
	else if (kind == _console_texture_cube)
		result = IDirect3DDevice9_CreateCubeTexture(x.device, width, texture->levels, 0, texture->d3d_format, 0,
			(D3DCubeTexture **)&texture->d3d, NULL);
	else
		result = IDirect3DDevice9_CreateTexture(x.device, width, height, texture->levels, 0, texture->d3d_format, 0,
			(D3DTexture **)&texture->d3d, NULL);
	if (FAILED(result) || !texture->d3d)
	{
		x360_log("native: cannot make a %lux%lux%lu texture (0x%08lx)", width, height, texture->depth,
			(unsigned long)result);
		x360_phys_failure("texture", width * height * texture->depth * 4);
		free(texture);
		DEVICE_TIME_END();
		return NULL;
	}
	X360_COUNT(texture_uploads, 1);
	DEVICE_TIME_END();
	return texture;
}

static unsigned long level_size(unsigned long size, unsigned long level)
{
	size >>= level;
	return size ? size : 1;
}

void console_texture_level(struct console_texture *texture, unsigned long face, unsigned long level, const void *data)
{
	BOOL compressed = texture->format != _console_format_argb8;
	unsigned long width = level_size(texture->width, level), height = level_size(texture->height, level);
	unsigned long depth = texture->kind == _console_texture_volume ? level_size(texture->depth, level) : 1;
	unsigned long block_bytes = texture->format == _console_format_dxt1 ? 8 : 16;
	unsigned long row_pitch = compressed ? (width + 3) / 4 * block_bytes : width * 4;
	DWORD gpu_format = XGGetGpuFormat(texture->d3d_format);
	DEVICE_TIME_BEGIN();

	if (!texture->d3d || !data || level >= texture->levels)
		return;
	if (texture->kind == _console_texture_volume)
	{
		D3DLOCKED_BOX box;

		if (SUCCEEDED(IDirect3DVolumeTexture9_LockBox((D3DVolumeTexture *)texture->d3d, (UINT)level, &box, NULL, 0)))
		{
			unsigned long rows = compressed ? (height + 3) / 4 : height, slice;

			if (textures_tiled())
				XGTileVolumeTextureLevel(texture->width, texture->height, texture->depth, (UINT)level, gpu_format, 0,
					box.pBits, NULL, data, row_pitch, row_pitch * rows, NULL);
			else
				for (slice = 0; slice < depth; slice++)
					rows_copy((unsigned char *)box.pBits + slice * box.SlicePitch, box.RowPitch,
						(const unsigned char *)data + slice * row_pitch * rows, row_pitch, rows);
			IDirect3DVolumeTexture9_UnlockBox((D3DVolumeTexture *)texture->d3d, (UINT)level);
		}
	}
	else
	{
		D3DLOCKED_RECT locked;
		HRESULT result = texture->kind == _console_texture_cube ?
			IDirect3DCubeTexture9_LockRect((D3DCubeTexture *)texture->d3d, (D3DCUBEMAP_FACES)face, (UINT)level,
				&locked, NULL, 0) :
			IDirect3DTexture9_LockRect((D3DTexture *)texture->d3d, (UINT)level, &locked, NULL, 0);

		if (SUCCEEDED(result))
		{
			if (textures_tiled())
				XGTileTextureLevel(texture->width, texture->height, (UINT)level, gpu_format, 0, locked.pBits, NULL, data,
					row_pitch, NULL);
			else
				rows_copy(locked.pBits, locked.Pitch, data, row_pitch, compressed ? (height + 3) / 4 : height);
			if (texture->kind == _console_texture_cube)
				IDirect3DCubeTexture9_UnlockRect((D3DCubeTexture *)texture->d3d, (D3DCUBEMAP_FACES)face, (UINT)level);
			else
				IDirect3DTexture9_UnlockRect((D3DTexture *)texture->d3d, (UINT)level);
		}
	}
	X360_COUNT(texture_bytes, row_pitch * (compressed ? (height + 3) / 4 : height) * depth);
	DEVICE_TIME_END();
}

void console_texture_delete(struct console_texture *texture)
{
	int index;

	if (!texture)
		return;
	/* (not left set where a later draw would read it) */
	for (index = 0; index < SAMPLERS; index++)
	{
		if (x.samplers[index].texture == texture)
			x.samplers_known[index] = FALSE;
	}
	retire((D3DResource *)texture->d3d);
	free(texture);
}

/* ---------- render targets */

struct console_target *console_target_create(unsigned long width, unsigned long height, int depth)
{
	struct console_target *target = calloc(1, sizeof(*target));
	D3DSURFACE_PARAMETERS parameters;
	D3DFORMAT format = depth ? D3DFMT_D24S8 : D3DFMT_A8R8G8B8;
	DWORD tiles = XGSurfaceSize(width, height, format, D3DMULTISAMPLE_NONE);
	DWORD screen_tiles = XGSurfaceSize(1280, 720, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE);
	DWORD base = x.edram_next;
	HRESULT result;

	if (!target)
		return NULL;
	if (x.edram_next + tiles > EDRAM_TILES)
	{
		/* (the screen's back buffer is drawn only as a frame is presented,
		after the targets are done with; the next frame clears them: one
		target past the end shares its tiles) */
		if (x.screen_tiles_taken || tiles > screen_tiles)
		{
			x360_log("native: EDRAM is full: a %lux%lu target cannot be made", width, height);
			free(target);
			return NULL;
		}
		x.screen_tiles_taken = TRUE;
		base = 0;
	}
	memset(&parameters, 0, sizeof(parameters));
	parameters.Base = base;
	parameters.HierarchicalZBase = 0xFFFFFFFF;
	if (depth)
		result = IDirect3DDevice9_CreateDepthStencilSurface(x.device, width, height, format, D3DMULTISAMPLE_NONE, 0,
			FALSE, &target->surface, &parameters);
	else
		result = IDirect3DDevice9_CreateRenderTarget(x.device, width, height, format, D3DMULTISAMPLE_NONE, 0, FALSE,
			&target->surface, &parameters);
	if (FAILED(result) || !target->surface)
	{
		x360_log("native: cannot make a %lux%lu target in EDRAM (0x%08lx)", width, height, (unsigned long)result);
		free(target);
		return NULL;
	}
	if (base == x.edram_next)
		x.edram_next += tiles;
	target->width = width;
	target->height = height;
	target->depth = depth;
	if (!depth)
	{
		/* (its pixels: a texture the resolve writes, tiled) */
		target->texture.kind = _console_texture_2d;
		target->texture.width = width;
		target->texture.height = height;
		target->texture.depth = 1;
		target->texture.levels = 1;
		target->texture.d3d_format = D3DFMT_A8R8G8B8;
		target->texture.target = target;
		if (FAILED(IDirect3DDevice9_CreateTexture(x.device, width, height, 1, 0, D3DFMT_A8R8G8B8, 0,
			(D3DTexture **)&target->texture.d3d, NULL)))
		{
			target->texture.d3d = NULL;
			x360_phys_failure("target texture", width * height * 4);
		}
	}
	x360_log("native: target %lux%lu %s at EDRAM tile %lu", width, height, depth ? "depth" : "color",
		(unsigned long)base);
	return target;
}

static void targets_bind(void)
{
	if (!x.targets_dirty)
		return;
	x.targets_dirty = FALSE;
	/* (a target with no color keeps the last one bound: the console cannot
	take none) */
	if (x.color)
		IDirect3DDevice9_SetRenderTarget(x.device, 0, x.color->surface);
	IDirect3DDevice9_SetDepthStencilSurface(x.device, x.depth ? x.depth->surface : NULL);
	x.screen_bound = FALSE;
}

void console_targets_set(struct console_target *color, struct console_target *depth)
{
	if (color == x.color && depth == x.depth && !x.screen_bound)
		return;
	if (color)
		x.color = color;
	x.depth = depth;
	x.targets_dirty = TRUE;
}

/* EDRAM's pixels of a color target into its texture, when it has newer */
static void target_resolve(struct console_target *target)
{
	D3DRECT rectangle;
	D3DPOINT point;

	if (!target->unresolved || !target->texture.d3d)
		return;
	target->unresolved = FALSE;
	IDirect3DDevice9_SetRenderTarget(x.device, 0, target->surface);
	IDirect3DDevice9_SetDepthStencilSurface(x.device, NULL);
	rectangle.x1 = 0;
	rectangle.y1 = 0;
	rectangle.x2 = (LONG)target->width;
	rectangle.y2 = (LONG)target->height;
	point.x = 0;
	point.y = 0;
	IDirect3DDevice9_Resolve(x.device, D3DRESOLVE_RENDERTARGET0, &rectangle, target->texture.d3d, &point, 0, 0,
		NULL, 1.0f, 0, NULL);
	X360_COUNT(resolves, 1);
	/* (the draws' targets bound again before the next draw) */
	x.targets_dirty = TRUE;
}

struct console_texture *console_target_texture(struct console_target *target)
{
	if (!target || target->depth)
		return NULL;
	target_resolve(target);
	return &target->texture;
}

/* ---------- shaders */

/* the HLSL's uniforms given their fixed registers (the table above), and
the samplers tex0 to tex3 theirs */
static char *hlsl_registers(const char *hlsl, int vertex)
{
	size_t length = strlen(hlsl), capacity = length + 4096, used = 0;
	char *out = malloc(capacity);
	const char *line = hlsl;

	if (!out)
		return NULL;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t line_length = end ? (size_t)(end - line) : strlen(line);
		char name[64] = "";
		char annotation[48] = "";

		if (!strncmp(line, "uniform ", 8) && line_length < 200 && line[line_length - 1] == ';')
		{
			const char *space = memchr(line + 8, ' ', line_length - 8);

			if (space)
			{
				size_t name_length = (size_t)(line + line_length - 1 - (space + 1));
				const char *bracket = memchr(space + 1, '[', name_length);
				int index;

				if (bracket)
					name_length = (size_t)(bracket - (space + 1));
				if (name_length < sizeof(name))
				{
					memcpy(name, space + 1, name_length);
					name[name_length] = '\0';
				}
				if (!strncmp(line + 8, "sampler", 7) && !strncmp(name, "tex", 3) && name[3] >= '0' && name[3] <= '3')
					_snprintf(annotation, sizeof(annotation) - 1, " : register(s%c)", name[3]);
				for (index = 0; !annotation[0] && index < NUMBER_OF_CONSOLE_UNIFORMS; index++)
				{
					int reg = vertex ? uniforms[index].vertex : uniforms[index].pixel;

					if (!strcmp(name, uniforms[index].name) && reg >= 0)
						_snprintf(annotation, sizeof(annotation) - 1, " : register(c%d)", reg);
				}
				if (!annotation[0])
				{
					static int logged;

					if (logged++ < 8)
						x360_log("native: a %s shader's uniform %s has no register of its own", vertex ? "vertex" : "pixel",
							name);
				}
			}
		}
		if (used + line_length + sizeof(annotation) + 2 > capacity)
		{
			char *grown;

			capacity = (used + line_length + sizeof(annotation) + 2) * 2;
			grown = realloc(out, capacity);
			if (!grown)
			{
				free(out);
				return NULL;
			}
			out = grown;
		}
		if (annotation[0])
		{
			memcpy(out + used, line, line_length - 1);
			used += line_length - 1;
			memcpy(out + used, annotation, strlen(annotation));
			used += strlen(annotation);
			out[used++] = ';';
		}
		else
		{
			memcpy(out + used, line, line_length);
			used += line_length;
		}
		out[used++] = '\n';
		line += line_length + (end ? 1 : 0);
	}
	out[used] = '\0';
	return out;
}

struct console_shader *console_shader_create(int vertex, const char *glsl)
{
	struct console_shader *shader;
	char *translated, *hlsl;
	LPD3DXBUFFER code = NULL, errors = NULL;
	unsigned long long key;
	unsigned long size = 0;
	void *cached;
	DEVICE_TIME_BEGIN();

	translated = x360_glsl_to_hlsl(glsl, vertex);
	if (!translated)
		return NULL;
	hlsl = hlsl_registers(translated, vertex);
	free(translated);
	if (!hlsl)
		return NULL;
	/* (compiled on an earlier run: x360_shader_cache.c) */
	key = x360_shader_cache_key(hlsl, vertex);
	cached = x360_shader_cache_load(key, vertex, &size);
	if (cached && SUCCEEDED(D3DXCreateBuffer((DWORD)size, &code)) && code)
	{
		memcpy(code->lpVtbl->GetBufferPointer(code), cached, size);
		x.shaders_read++;
	}
	free(cached);
	if (!code)
	{
		HRESULT result = D3DXCompileShader(hlsl, (UINT)strlen(hlsl), NULL, NULL, "main", vertex ? "vs_3_0" : "ps_3_0",
			0, &code, &errors, NULL);

		if (FAILED(result) || !code)
		{
			x.shader_failures++;
			x360_log("native: a %s shader did not compile: %s", vertex ? "vertex" : "pixel",
				errors ? (const char *)errors->lpVtbl->GetBufferPointer(errors) : "(no message)");
			if (x.shader_failures <= 8)
			{
				char path[64];
				FILE *file;

				CreateDirectoryA("game:\\glsl", NULL);
				_snprintf(path, sizeof(path) - 1, "game:\\glsl\\native%02lu.hlsl", x.shader_failures);
				path[sizeof(path) - 1] = '\0';
				if ((file = fopen(path, "wb")) != NULL)
				{
					fputs(hlsl, file);
					fclose(file);
				}
			}
			if (errors)
				errors->lpVtbl->Release(errors);
			free(hlsl);
			DEVICE_TIME_END();
			return NULL;
		}
		x.shaders_compiled++;
		x360_shader_cache_store(key, vertex, code->lpVtbl->GetBufferPointer(code),
			(unsigned long)code->lpVtbl->GetBufferSize(code));
	}
	if (errors)
		errors->lpVtbl->Release(errors);
	free(hlsl);
	X360_COUNT(shaders_made, 1);
	shader = calloc(1, sizeof(*shader));
	if (!shader)
	{
		code->lpVtbl->Release(code);
		return NULL;
	}
	shader->vertex = vertex;
	shader->code = code;
	if (vertex)
	{
		D3DXSEMANTIC semantics[MAXD3DDECLLENGTH];
		UINT count = 0, index;

		if (SUCCEEDED(D3DXGetShaderInputSemantics((const DWORD *)code->lpVtbl->GetBufferPointer(code), semantics,
			&count)))
		{
			for (index = 0; index < count; index++)
			{
				if (semantics[index].Usage == D3DDECLUSAGE_TEXCOORD && semantics[index].UsageIndex < CONSOLE_VERTEX_REGISTERS)
					shader->inputs |= 1UL << semantics[index].UsageIndex;
			}
		}
		shader->vertex_shader = D3DDevice_CreateVertexShader(code->lpVtbl->GetBufferPointer(code));
	}
	else
	{
		shader->pixel_shader = D3DDevice_CreatePixelShader(code->lpVtbl->GetBufferPointer(code));
	}
	if ((x.shaders_compiled + x.shaders_read) % 50 == 1)
		x360_log("native: shaders so far: %lu compiled, %lu read from game:\\shaders, %lu failed", x.shaders_compiled,
			x.shaders_read, x.shader_failures);
	DEVICE_TIME_END();
	if (!shader->vertex_shader && !shader->pixel_shader)
	{
		free(shader);
		return NULL;
	}
	return shader;
}

/* ---------- uniforms */

void console_uniform_set(int uniform, unsigned long first, const float *vectors, unsigned long count)
{
	if (uniform < 0 || uniform >= NUMBER_OF_CONSOLE_UNIFORMS || first + count > (unsigned long)uniforms[uniform].count)
		return;
	if (uniforms[uniform].vertex >= 0)
		IDirect3DDevice9_SetVertexShaderConstantF(x.device, (UINT)(uniforms[uniform].vertex + first), vectors,
			(DWORD)count);
	if (uniforms[uniform].pixel >= 0)
		IDirect3DDevice9_SetPixelShaderConstantF(x.device, (UINT)(uniforms[uniform].pixel + first), vectors,
			(DWORD)count);
	X360_COUNT(vertex_constants, uniforms[uniform].vertex >= 0 ? count : 0);
	X360_COUNT(pixel_constants, uniforms[uniform].pixel >= 0 ? count : 0);
}

/* ---------- state */

static const D3DBLEND blend_factors[] =
{
	D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLEND_SRCCOLOR, D3DBLEND_INVSRCCOLOR, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA, D3DBLEND_DESTCOLOR, D3DBLEND_INVDESTCOLOR, D3DBLEND_SRCALPHASAT,
	D3DBLEND_BLENDFACTOR, D3DBLEND_INVBLENDFACTOR, D3DBLEND_CONSTANTALPHA, D3DBLEND_INVCONSTANTALPHA,
};
static const D3DBLENDOP blend_operations[] =
{
	D3DBLENDOP_ADD, D3DBLENDOP_SUBTRACT, D3DBLENDOP_REVSUBTRACT, D3DBLENDOP_MIN, D3DBLENDOP_MAX,
};

#define SET_IF(field, state_name, value) \
	do { if (!known || old->field != now->field) { IDirect3DDevice9_SetRenderState(x.device, state_name, value); \
		X360_COUNT(render_states, 1); } } while (0)

static void state_apply(const struct console_state *now)
{
	const struct console_state *old = &x.state;
	BOOL known = x.state_known;

	if (!known)
	{
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_HALFPIXELOFFSET, TRUE);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_HIZENABLE, D3DHIZ_DISABLE);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		/* (the game's scissor is the viewport: d3d8_gl.c's apply_raster_state) */
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_SCISSORTESTENABLE, TRUE);
	}
	SET_IF(blend, D3DRS_ALPHABLENDENABLE, now->blend);
	SET_IF(blend_source, D3DRS_SRCBLEND, blend_factors[now->blend_source % 15]);
	SET_IF(blend_destination, D3DRS_DESTBLEND, blend_factors[now->blend_destination % 15]);
	SET_IF(blend_operation, D3DRS_BLENDOP, blend_operations[now->blend_operation % 5]);
	SET_IF(blend_color, D3DRS_BLENDFACTOR, now->blend_color);
	SET_IF(depth_test, D3DRS_ZENABLE, now->depth_test);
	SET_IF(depth_function, D3DRS_ZFUNC, (D3DCMPFUNC)(now->depth_function & 7));
	SET_IF(depth_write, D3DRS_ZWRITEENABLE, now->depth_write);
	SET_IF(stencil, D3DRS_STENCILENABLE, now->stencil);
	SET_IF(stencil_function, D3DRS_STENCILFUNC, (D3DCMPFUNC)(now->stencil_function & 7));
	SET_IF(stencil_reference, D3DRS_STENCILREF, now->stencil_reference);
	SET_IF(stencil_mask, D3DRS_STENCILMASK, now->stencil_mask);
	SET_IF(stencil_write_mask, D3DRS_STENCILWRITEMASK, now->stencil_write_mask);
	SET_IF(stencil_fail, D3DRS_STENCILFAIL, (D3DSTENCILOP)(now->stencil_fail & 7));
	SET_IF(stencil_depth_fail, D3DRS_STENCILZFAIL, (D3DSTENCILOP)(now->stencil_depth_fail & 7));
	SET_IF(stencil_pass, D3DRS_STENCILPASS, (D3DSTENCILOP)(now->stencil_pass & 7));
	SET_IF(cull, D3DRS_CULLMODE, now->cull == _console_cull_cw ? D3DCULL_CW :
		now->cull == _console_cull_ccw ? D3DCULL_CCW : D3DCULL_NONE);
	SET_IF(color_write, D3DRS_COLORWRITEENABLE, now->color_write);
	SET_IF(wireframe, D3DRS_FILLMODE, now->wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
	if (!known || old->depth_bias != now->depth_bias)
	{
		float bias = now->depth_bias / 16777216.0f;

		IDirect3DDevice9_SetRenderState(x.device, D3DRS_DEPTHBIAS, *(DWORD *)&bias);
		X360_COUNT(render_states, 1);
	}
	if (!known || old->slope_bias != now->slope_bias)
	{
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD *)&now->slope_bias);
		X360_COUNT(render_states, 1);
	}
	if (!known || memcmp(old->viewport, now->viewport, sizeof(now->viewport)) || old->depth_near != now->depth_near ||
		old->depth_far != now->depth_far)
	{
		D3DVIEWPORT9 viewport;
		RECT scissor;

		viewport.X = now->viewport[0];
		viewport.Y = now->viewport[1];
		viewport.Width = now->viewport[2];
		viewport.Height = now->viewport[3];
		viewport.MinZ = now->depth_near;
		viewport.MaxZ = now->depth_far;
		IDirect3DDevice9_SetViewport(x.device, &viewport);
		scissor.left = (LONG)now->viewport[0];
		scissor.top = (LONG)now->viewport[1];
		scissor.right = (LONG)(now->viewport[0] + now->viewport[2]);
		scissor.bottom = (LONG)(now->viewport[1] + now->viewport[3]);
		IDirect3DDevice9_SetScissorRect(x.device, &scissor);
	}
	x.state = *now;
	x.state_known = TRUE;
}

/* ---------- samplers */

static D3DTEXTUREADDRESS address_mode(unsigned char mode)
{
	switch (mode)
	{
	case _console_address_mirror: return D3DTADDRESS_MIRROR;
	case _console_address_clamp: return D3DTADDRESS_CLAMP;
	case _console_address_border: return D3DTADDRESS_BORDER;
	default: return D3DTADDRESS_WRAP;
	}
}

static void sampler_apply(DWORD index, const struct console_sampler *now)
{
	const struct console_sampler *old = &x.samplers[index];
	BOOL known = x.samplers_known[index];
	struct console_texture *texture = now->texture;
	unsigned long levels = texture ? texture->levels : 1;

	if (!known || old->texture != texture)
	{
		IDirect3DDevice9_SetTexture(x.device, index, texture ? texture->d3d : NULL);
		X360_COUNT(textures, 1);
	}
	if (!known || memcmp(old->address, now->address, sizeof(now->address)))
	{
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_ADDRESSU, address_mode(now->address[0]));
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_ADDRESSV, address_mode(now->address[1]));
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_ADDRESSW, address_mode(now->address[2]));
		X360_COUNT(sampler_states, 3);
	}
	if (!known || old->magnify != now->magnify || old->minify != now->minify || old->mip != now->mip ||
		old->anisotropy != now->anisotropy || x.sampler_levels[index] != levels || old->top_level != now->top_level)
	{
		unsigned long top = now->top_level < levels ? now->top_level : levels - 1;

		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_MAGFILTER, now->magnify ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_MINFILTER, now->minify == 2 ? D3DTEXF_ANISOTROPIC :
			now->minify ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_MIPFILTER, now->mip == 0 || levels <= 1 ? D3DTEXF_NONE :
			now->mip == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_MAXANISOTROPY, now->anisotropy ? now->anisotropy : 1);
		/* (the console's MAXMIPLEVEL is the most detailed level used, the
		Xbox's; MINMIPLEVEL the least, the texture's last) */
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_MAXMIPLEVEL, top);
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_MINMIPLEVEL, levels - 1);
		X360_COUNT(sampler_states, 6);
		x.sampler_levels[index] = levels;
	}
	if (!known || old->lod_bias != now->lod_bias)
	{
		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_MIPMAPLODBIAS, *(DWORD *)&now->lod_bias);
		X360_COUNT(sampler_states, 1);
	}
	if (!known || old->border != now->border)
	{
		/* (the console's border is one of four colors, black or white, clear
		or opaque: the nearest; x360_gl.c's) */
		DWORD color = (DWORD)now->border;
		DWORD alpha = (color >> 24) >= 0x80 ? 0xFF000000 : 0;
		DWORD white = ((color >> 16 & 0xFF) + (color >> 8 & 0xFF) + (color & 0xFF)) >= 3 * 0x80 ? 0x00FFFFFF : 0;

		IDirect3DDevice9_SetSamplerState(x.device, index, D3DSAMP_BORDERCOLOR, alpha ? alpha | white : 0);
		X360_COUNT(sampler_states, 1);
	}
	x.samplers[index] = *now;
	x.samplers_known[index] = TRUE;
}

/* ---------- vertex layouts */

static DWORD vertex_type(int format)
{
	DWORD type;

	switch (format)
	{
	case _console_vertex_float1: type = D3DDECLTYPE_FLOAT1; break;
	case _console_vertex_float2: type = D3DDECLTYPE_FLOAT2; break;
	case _console_vertex_float3: type = D3DDECLTYPE_FLOAT3; break;
	case _console_vertex_color: type = D3DDECLTYPE_D3DCOLOR; break;
	case _console_vertex_short2: type = D3DDECLTYPE_SHORT2; break;
	case _console_vertex_short4: type = D3DDECLTYPE_SHORT4; break;
	case _console_vertex_short2n: type = D3DDECLTYPE_SHORT2N; break;
	case _console_vertex_short4n: type = D3DDECLTYPE_SHORT4N; break;
	case _console_vertex_ubyte4n: type = D3DDECLTYPE_UBYTE4N; break;
	/* (the translators' shaders take it unpacked: x360_glsl.c) */
	case _console_vertex_normpacked3: type = D3DDECLTYPE_HEND3N; break;
	default: type = D3DDECLTYPE_FLOAT4; break;
	}
	/* (the stream's fetch constant has the byte order that counts; this one
	is not used by the fetch: x360_gl.c's vertex_type) */
	return (type & ~D3DDECLTYPE_ENDIAN_MASK) | (GPUENDIAN_NONE << D3DDECLTYPE_ENDIAN_SHIFT);
}

static D3DVertexDeclaration *declaration_get(const D3DVERTEXELEMENT9 *elements, int count)
{
	unsigned long hash = 2166136261UL;
	const unsigned char *bytes = (const unsigned char *)elements;
	size_t index, size = sizeof(D3DVERTEXELEMENT9) * (size_t)count;
	struct declaration *entry;
	struct declaration **bucket;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 16777619UL;
	bucket = &x.declarations[hash % DECLARATION_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && entry->count == count && !memcmp(entry->elements, elements, size))
			return entry->d3d;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->hash = hash;
	entry->count = count;
	memcpy(entry->elements, elements, size);
	{
		static const D3DVERTEXELEMENT9 end = D3DDECL_END();
		D3DVERTEXELEMENT9 terminated[CONSOLE_VERTEX_REGISTERS + 2];

		memcpy(terminated, elements, size);
		terminated[count] = end;
		entry->d3d = D3DDevice_CreateVertexDeclaration(terminated);
	}
	entry->next = *bucket;
	*bucket = entry;
	return entry->d3d;
}

static D3DVertexShader *binding_get(struct console_shader *shader, D3DVertexDeclaration *declaration,
	const DWORD *strides)
{
	unsigned long hash = ((unsigned long)(UINT_PTR)shader * 2654435761UL) ^ (unsigned long)(UINT_PTR)declaration;
	struct binding **bucket;
	struct binding *entry;
	int index;

	for (index = 0; index <= CONSOLE_VERTEX_STREAMS; index++)
		hash = hash * 31 + strides[index];
	bucket = &x.bindings[hash % BINDING_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->shader == shader && entry->declaration == declaration &&
			!memcmp(entry->strides, strides, sizeof(entry->strides)))
			return entry->bound;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->shader = shader;
	entry->declaration = declaration;
	memcpy(entry->strides, strides, sizeof(entry->strides));
	entry->bound = D3DDevice_CreateVertexShader(((LPD3DXBUFFER)shader->code)->lpVtbl->GetBufferPointer(
		(LPD3DXBUFFER)shader->code));
	if (entry->bound)
		D3DVertexShader_Bind(entry->bound, 0, declaration, entry->strides, NULL);
	entry->next = *bucket;
	*bucket = entry;
	return entry->bound;
}

/* ---------- draws */

static D3DPRIMITIVETYPE primitive_type(int primitive)
{
	switch (primitive)
	{
	case _console_primitive_points: return D3DPT_POINTLIST;
	case _console_primitive_lines: return D3DPT_LINELIST;
	case _console_primitive_line_strip: return D3DPT_LINESTRIP;
	case _console_primitive_triangle_fan: return D3DPT_TRIANGLEFAN;
	case _console_primitive_triangle_strip: return D3DPT_TRIANGLESTRIP;
	case _console_primitive_quads: return D3DPT_QUADLIST;
	default: return D3DPT_TRIANGLELIST;
	}
}

/* the stream's fetch constant read little-endian (the map's vertices, as
loaded); the SDK's bitfields are laid out from the low bit by Microsoft's
compiler, from the high bit by this one, so the dword is written whole */
static void fetch_little_endian(D3DVertexBuffer *stream)
{
	stream->Format.dword[1] = (stream->Format.dword[1] & ~3UL) | GPUENDIAN_NONE;
}

static void reads_mark(const void *data, unsigned long size);

static void stream_set(UINT index, const void *data, unsigned long size, unsigned long stride, BOOL little_endian)
{
	D3DVertexBuffer *header = &x.streams[index];

	reads_mark(data, size);
	XGSetVertexBufferHeader(size ? size : 4, 0, 0, 0, header);
	XGOffsetResourceAddress((D3DResource *)header, (void *)data);
	if (little_endian)
		fetch_little_endian(header);
	IDirect3DDevice9_SetStreamSource(x.device, index, header, 0, (UINT)stride);
	X360_COUNT(streams, 1);
}

void console_draw(const struct console_draw *draw)
{
	struct console_shader *vertex = draw->vertex_shader;
	D3DVERTEXELEMENT9 elements[CONSOLE_VERTEX_REGISTERS + 1];
	DWORD strides[CONSOLE_VERTEX_STREAMS + 1];
	unsigned long provided = 0, constant_registers, index, constant_count = 0;
	int element_count = 0;
	UINT constant_stream = (UINT)draw->stream_count;
	D3DPRIMITIVETYPE type = primitive_type(draw->primitive);
	DEVICE_TIME_BEGIN();

	if (!x.ready || !vertex || !vertex->vertex_shader || !draw->pixel_shader || !draw->pixel_shader->pixel_shader)
		return;
	/* the samplers first: a sampled target is resolved, which binds it */
	for (index = 0; index < draw->sampler_count && index < SAMPLERS; index++)
	{
		if (draw->samplers[index].texture && draw->samplers[index].texture->target)
			target_resolve(draw->samplers[index].texture->target);
	}
	targets_bind();
	for (index = 0; index < draw->sampler_count && index < SAMPLERS; index++)
		sampler_apply((DWORD)index, &draw->samplers[index]);
	state_apply(draw->state);

	/* the layout: the elements of the registers the shader reads, and a
	stream of their own for the ones no stream gives */
	memset(strides, 0, sizeof(strides));
	for (index = 0; index < draw->element_count; index++)
	{
		const struct console_element *element = &draw->elements[index];
		D3DVERTEXELEMENT9 *out;

		if (!((vertex->inputs >> element->reg) & 1) || ((provided >> element->reg) & 1))
			continue;
		provided |= 1UL << element->reg;
		out = &elements[element_count++];
		out->Stream = element->stream;
		out->Offset = element->offset;
		out->Type = vertex_type(element->format);
		out->Method = D3DDECLMETHOD_DEFAULT;
		out->Usage = D3DDECLUSAGE_TEXCOORD;
		out->UsageIndex = element->reg;
		strides[element->stream] = draw->streams[element->stream].stride;
	}
	constant_registers = vertex->inputs & ~provided;
	for (index = 0; index < CONSOLE_VERTEX_REGISTERS; index++)
	{
		D3DVERTEXELEMENT9 *out;

		if (!((constant_registers >> index) & 1))
			continue;
		out = &elements[element_count++];
		out->Stream = (WORD)constant_stream;
		out->Offset = (WORD)(constant_count * 16);
		out->Type = vertex_type(_console_vertex_float4);
		out->Method = D3DDECLMETHOD_DEFAULT;
		out->Usage = D3DDECLUSAGE_TEXCOORD;
		out->UsageIndex = (BYTE)index;
		constant_count++;
	}
	if (constant_count)
		strides[constant_stream] = constant_count * 16;

	/* the declaration and the shader bound to it: the last draw's, when it
	had this layout */
	if (draw->layout_key && draw->layout_key == x.last_layout && vertex == x.last_shader &&
		(unsigned long)element_count == x.last_element_count && !memcmp(strides, x.last_strides, sizeof(strides)))
	{
		/* (the same) */
	}
	else
	{
		x.last_declaration = declaration_get(elements, element_count);
		x.last_bound = x.last_declaration ? binding_get(vertex, x.last_declaration, strides) : NULL;
		x.last_layout = draw->layout_key;
		x.last_shader = vertex;
		x.last_element_count = (unsigned long)element_count;
		memcpy(x.last_strides, strides, sizeof(strides));
	}
	if (!x.last_declaration || !x.last_bound)
	{
		DEVICE_TIME_END();
		return;
	}
	if (x.declaration != x.last_declaration)
	{
		IDirect3DDevice9_SetVertexDeclaration(x.device, x.last_declaration);
		X360_COUNT(declarations, 1);
		x.declaration = x.last_declaration;
	}
	if (x.vertex_shader != x.last_bound)
	{
		IDirect3DDevice9_SetVertexShader(x.device, x.last_bound);
		X360_COUNT(shaders, 1);
		x.vertex_shader = x.last_bound;
	}
	if (x.pixel_shader != draw->pixel_shader->pixel_shader)
	{
		IDirect3DDevice9_SetPixelShader(x.device, draw->pixel_shader->pixel_shader);
		X360_COUNT(shaders, 1);
		x.pixel_shader = draw->pixel_shader->pixel_shader;
	}

	/* the streams, and the constant registers' (every vertex its own copy:
	x360_gl.c found one read by all not to work) */
	for (index = 0; index < draw->stream_count; index++)
	{
		if (strides[index] && draw->streams[index].data)
			stream_set((UINT)index, draw->streams[index].data, draw->streams[index].size, draw->streams[index].stride,
				draw->streams[index].little_endian);
	}
	if (constant_count)
	{
		unsigned long vertex_bytes = constant_count * 16, vertices = draw->vertex_count ? draw->vertex_count : 1;
		/* (only the vertices drawn: the stream starts that many before its
		copies, which the draw never reads; x360_gl.c's) */
		unsigned long skipped = draw->vertex_first < vertices ? draw->vertex_first : 0;
		float vector[CONSOLE_VERTEX_REGISTERS][4];
		unsigned long *copy;
		unsigned long reg, made = 0;

		for (reg = 0; reg < CONSOLE_VERTEX_REGISTERS; reg++)
		{
			if ((constant_registers >> reg) & 1)
				memcpy(vector[made++], draw->constant_values[reg], 16);
		}
		copy = console_frame_copy(NULL, vertex_bytes * (vertices - skipped));
		if (!copy || (UINT_PTR)copy < skipped * vertex_bytes + 0x100000)
		{
			DEVICE_TIME_END();
			return;
		}
		for (index = 0; index < vertices - skipped; index++)
			copy_words(copy + index * (vertex_bytes / 4), vector, vertex_bytes);
		X360_COUNT(ring_bytes, vertex_bytes * (vertices - skipped));
		stream_set(constant_stream, (unsigned char *)copy - skipped * vertex_bytes, vertex_bytes * vertices,
			vertex_bytes, FALSE);
	}

	if (draw->indices)
	{
		XGSetIndexBufferHeader(draw->index_count * 2, 0, D3DFMT_INDEX16, 0, 0, &x.indices);
		XGOffsetResourceAddress((D3DResource *)&x.indices, (void *)draw->indices);
		reads_mark(draw->indices, draw->index_count * 2);
		IDirect3DDevice9_SetIndices(x.device, &x.indices);
		X360_COUNT(index_buffers, 1);
		if (!draw->indices_resident)
			X360_COUNT(index_bytes, draw->index_count * 2);
		IDirect3DDevice9_DrawIndexedVertices(x.device, type, 0, 0, (UINT)draw->index_count);
	}
	else
	{
		IDirect3DDevice9_DrawVertices(x.device, type, 0, (UINT)draw->vertex_count);
	}
	x360_count_draw();
	if (x.color)
		x.color->unresolved = TRUE;
	DEVICE_TIME_END();
}

void console_clear(unsigned long flags, const unsigned long *rectangles, unsigned long rectangle_count,
	unsigned long color, float depth, unsigned long stencil)
{
	DWORD clear = 0;
	D3DVECTOR4 value;
	unsigned long index;
	DEVICE_TIME_BEGIN();

	if (!x.ready)
		return;
	targets_bind();
	if ((flags & 1) && x.color)
		clear |= D3DCLEAR_TARGET;
	if ((flags & 2) && x.depth)
		clear |= D3DCLEAR_ZBUFFER;
	if ((flags & 4) && x.depth)
		clear |= D3DCLEAR_STENCIL;
	if (!clear)
		return;
	value.x = (float)((color >> 16) & 0xff) / 255.0f;
	value.y = (float)((color >> 8) & 0xff) / 255.0f;
	value.z = (float)(color & 0xff) / 255.0f;
	value.w = (float)((color >> 24) & 0xff) / 255.0f;
	/* (the console clears whole tiles of the rectangle; the scissor
	is off for it) */
	IDirect3DDevice9_SetRenderState(x.device, D3DRS_SCISSORTESTENABLE, FALSE);
	for (index = 0; index < rectangle_count; index++)
	{
		const unsigned long *rect = rectangles + index * 4;
		D3DRECT rectangle;

		rectangle.x1 = (LONG)rect[0];
		rectangle.y1 = (LONG)rect[1];
		rectangle.x2 = (LONG)(rect[0] + rect[2]);
		rectangle.y2 = (LONG)(rect[1] + rect[3]);
		IDirect3DDevice9_ClearF(x.device, clear, &rectangle, &value, depth, (DWORD)stencil);
		X360_COUNT(clears, 1);
	}
	IDirect3DDevice9_SetRenderState(x.device, D3DRS_SCISSORTESTENABLE, TRUE);
	if ((clear & D3DCLEAR_TARGET) && x.color)
		x.color->unresolved = TRUE;
	DEVICE_TIME_END();
}

/* ---------- occlusion queries */

static D3DQuery *query_take(void)
{
	D3DQuery *query = NULL;

	if (x.free_count)
		return x.free_queries[--x.free_count];
	if (x.made_queries < QUERY_POOL && SUCCEEDED(IDirect3DDevice9_CreateQuery(x.device, D3DQUERYTYPE_OCCLUSION, &query)))
	{
		x.made_queries++;
		return query;
	}
	return NULL;
}

static void query_give_back(D3DQuery *query)
{
	if (query && x.free_count < QUERY_POOL)
		x.free_queries[x.free_count++] = query;
}

void console_query_begin(void)
{
	if (!x.ready || x.active_query)
		return;
	targets_bind();
	x.active_query = query_take();
	if (x.active_query)
	{
		IDirect3DQuery9_Issue(x.active_query, D3DISSUE_BEGIN);
		X360_COUNT(queries, 1);
	}
}

void console_query_end(unsigned long slot)
{
	D3DQuery *query = x.active_query;

	x.active_query = NULL;
	if (!query)
		return;
	IDirect3DQuery9_Issue(query, D3DISSUE_END);
	slot %= QUERY_SLOTS;
	if (x.slots[slot].count == QUERY_PENDING)
	{
		/* (the oldest, still without its result, goes: released, and the
		pool may make another) */
		log_once("a visibility test slot has more tests waiting than it keeps; the oldest is dropped");
		IDirect3DResource9_Release((D3DResource *)x.slots[slot].queries[0]);
		x.made_queries--;
		memmove(&x.slots[slot].queries[0], &x.slots[slot].queries[1],
			(QUERY_PENDING - 1) * sizeof(x.slots[slot].queries[0]));
		x.slots[slot].count--;
	}
	x.slots[slot].queries[x.slots[slot].count++] = query;
}

int console_query_result(unsigned long slot, unsigned long *samples)
{
	int index, newest = -1;
	DWORD result = 0, newest_result = 0;

	slot %= QUERY_SLOTS;
	/* (the newest test that has its count; never waited for) */
	for (index = 0; index < x.slots[slot].count; index++)
	{
		if (IDirect3DQuery9_GetData(x.slots[slot].queries[index], &result, sizeof(result), 0) == S_OK)
		{
			newest = index;
			newest_result = result;
		}
	}
	if (newest < 0)
	{
		if (x.slots[slot].count)
			X360_COUNT(query_waits, 1);
		return 0;
	}
	for (index = 0; index <= newest; index++)
		query_give_back(x.slots[slot].queries[index]);
	memmove(&x.slots[slot].queries[0], &x.slots[slot].queries[newest + 1],
		(size_t)(x.slots[slot].count - newest - 1) * sizeof(x.slots[slot].queries[0]));
	x.slots[slot].count = (unsigned char)(x.slots[slot].count - newest - 1);
	*samples = newest_result;
	return 1;
}

/* ---------- the game's memory read in place (x360_gpu_reads.h)

The renderer draws the game's vertices and indices where they are, so the
game's locks ask whether a frame that read them is still in flight
(xbox360_gpu_hooks.c, as for the GL layer): this frame always, the last
until its fence has passed. The frame being built is x.frame + 1 (x.frame
counts the presented ones). */

extern unsigned int xbox_address_space_base;

static unsigned long window_offset(const void *data)
{
	return (unsigned long)((unsigned int)(UINT_PTR)data - xbox_address_space_base) - 0x80000000UL;
}

static unsigned short frame_being_built(void)
{
	unsigned short frame = (unsigned short)(x.frame + 1);

	return frame ? frame : 1;
}

static void reads_mark(const void *data, unsigned long size)
{
	x360_gpu_reads_mark(window_offset(data), size, frame_being_built());
}

BOOL x360_native_active(void)
{
	return x.device != NULL;
}

unsigned long x360_native_memory_read_frame(const void *data, unsigned long size)
{
	return x360_gpu_reads_newest(window_offset(data), size, frame_being_built());
}

BOOL x360_native_frame_done(unsigned long frame)
{
	unsigned short current = frame_being_built();

	if (!frame || !x.device)
		return TRUE;
	if ((unsigned short)frame == current)
		return FALSE;
	if ((unsigned short)frame == (unsigned short)(current - 1))
	{
		DWORD fence = x.fences[x.half ^ 1];

		return !fence || !D3DDevice_IsFencePending(fence);
	}
	return TRUE;
}

void x360_native_frame_wait(unsigned long frame)
{
	if (x360_native_frame_done(frame))
		return;
	if ((unsigned short)frame == frame_being_built())
	{
		DWORD fence = IDirect3DDevice9_InsertFence(x.device);

		if (fence)
			D3DDevice_BlockOnFence(fence);
	}
	else
		D3DDevice_BlockOnFence(x.fences[x.half ^ 1]);
	X360_COUNT(lock_waits, 1);
}

/* ---------- the frame */

static void frame_next(void)
{
	int index;

	x.fences[x.half] = IDirect3DDevice9_InsertFence(x.device);
	x.half ^= 1;
	if (x.fences[x.half])
		D3DDevice_BlockOnFence(x.fences[x.half]);
	for (index = 0; index < x.retired[x.half].count; index++)
		IDirect3DResource9_Release(x.retired[x.half].resources[index]);
	x.retired[x.half].count = 0;
	x.ring_offset = (unsigned long)x.half * (RING_SIZE / 2);
}

/* (the game's settings: port_config.c) */
long config_integer(const char *name);

/* console.picture_frame = N (off by default): the picture of frames N and
3N to game:\\picture<n>.raw (untiled ARGB words), for comparing the two
renderers (x360_gl.c writes the same) */
static void picture_dump(struct console_target *picture)
{
	static long frame = -1;
	D3DLOCKED_RECT locked;
	unsigned long size;
	unsigned char *linear;
	char path[64];
	FILE *file;

	if (frame < 0)
		frame = config_integer("console.picture_frame");
	if (frame <= 0 || !picture || !picture->texture.d3d ||
		(x.frame != (unsigned long)frame && x.frame != (unsigned long)frame * 3))
		return;
	size = picture->width * picture->height * 4;
	linear = malloc(size);
	if (!linear)
		return;
	IDirect3DDevice9_BlockUntilIdle(x.device);
	IDirect3DTexture9_LockRect((D3DTexture *)picture->texture.d3d, 0, &locked, NULL, D3DLOCK_READONLY);
	XGUntileSurface(linear, picture->width * 4, NULL, locked.pBits, picture->width, picture->height, NULL, 4);
	IDirect3DTexture9_UnlockRect((D3DTexture *)picture->texture.d3d, 0);
	_snprintf(path, sizeof(path) - 1, "game:\\picture%lu.raw", x.frame);
	path[sizeof(path) - 1] = '\0';
	if ((file = fopen(path, "wb")) != NULL)
	{
		fwrite(linear, 1, size, file);
		fclose(file);
		x360_log("native: the picture (%lux%lu) written to %s", picture->width, picture->height, path);
	}
	free(linear);
}

void console_present(struct console_target *picture)
{
	unsigned long long ticks = x360_ticks();
	D3DSurface *back_buffer = NULL;
	float *quad;
	int index;

	if (!x.ready)
		return;
	if (picture && !picture->depth)
		target_resolve(picture);
	if (picture && !picture->depth)
		picture_dump(picture);
	IDirect3DDevice9_GetBackBuffer(x.device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer);
	IDirect3DDevice9_SetRenderTarget(x.device, 0, back_buffer);
	IDirect3DDevice9_SetDepthStencilSurface(x.device, NULL);
	if (back_buffer)
		IDirect3DResource9_Release((D3DResource *)back_buffer);
	x.screen_bound = TRUE;
	x.targets_dirty = TRUE;
	quad = console_frame_copy(NULL, 4 * 6 * sizeof(float));
	if (picture && picture->texture.d3d && quad)
	{
		static const float corners[4][6] =
		{
			{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
			{ 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
			{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f },
			{ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
		};
		D3DVIEWPORT9 viewport = { 0, 0, 1280, 720, 0.0f, 1.0f };

		copy_words((unsigned long *)quad, corners, sizeof(corners));
		IDirect3DDevice9_SetViewport(x.device, &viewport);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_SCISSORTESTENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_ALPHABLENDENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_ZENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_STENCILENABLE, FALSE);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_CULLMODE, D3DCULL_NONE);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_COLORWRITEENABLE, 0xf);
		IDirect3DDevice9_SetRenderState(x.device, D3DRS_FILLMODE, D3DFILL_SOLID);
		IDirect3DDevice9_SetVertexDeclaration(x.device, x.blit_declaration);
		IDirect3DDevice9_SetVertexShader(x.device, x.blit_vertex);
		IDirect3DDevice9_SetPixelShader(x.device, x.blit_pixel);
		IDirect3DDevice9_SetTexture(x.device, 0, picture->texture.d3d);
		IDirect3DDevice9_SetSamplerState(x.device, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(x.device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice9_SetSamplerState(x.device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		IDirect3DDevice9_SetSamplerState(x.device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(x.device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(x.device, 0, D3DSAMP_MAXMIPLEVEL, 0);
		IDirect3DDevice9_SetSamplerState(x.device, 0, D3DSAMP_MINMIPLEVEL, 0);
		/* (floats written by this processor: its byte order, the default) */
		XGSetVertexBufferHeader(sizeof(corners), 0, 0, 0, &x.streams[0]);
		XGOffsetResourceAddress((D3DResource *)&x.streams[0], quad);
		IDirect3DDevice9_SetStreamSource(x.device, 0, &x.streams[0], 0, 6 * sizeof(float));
		IDirect3DDevice9_DrawVertices(x.device, D3DPT_QUADLIST, 0, 4);
	}
	D3DDevice_Present(x.device);
	/* (what the present's quad set is the device's: everything is set
	again for the next draw) */
	x.state_known = FALSE;
	for (index = 0; index < SAMPLERS; index++)
		x.samplers_known[index] = FALSE;
	x.declaration = NULL;
	x.vertex_shader = NULL;
	x.pixel_shader = NULL;
	x.last_layout = NULL;
	frame_next();
	x.frame++;
	x360_frame_counters.present_ticks += x360_ticks() - ticks;
	x360_frame_counters_present("native");
	if (x.frame == 1 || x.frame % 1800 == 0)
		x360_memory_report("native frame");
}
