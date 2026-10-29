/*
X360_SHADER_CACHE.C

The GL layer's compiled shaders kept on the console's drive (game:\shaders),
so that D3DXCompileShader compiles each one once per install, not once a run
(x360_gl.c shader_compile). The game's shader_cache.bin lists the shaders it
has drawn with, and makes them all at start-up (xgpu_shader_cache_warm); with
this cache, that is a file read each instead of a compile on the console. The
Wii U keeps CafeGLSL's shaders on its SD card the same way
(warthog-wiiu port/wiiu/src/wiiu_shader_cache.c).

A file is named by a hash of the HLSL text and the profile it was compiled
for, and holds the compiled shader as D3DXCompileShader gave it (its
microcode with the constant table in it). A file that does not read back
whole, or whose bytes do not match its check, is ignored and compiled again.
Deleting the folder starts the cache again.
*/

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x360_port.h"

#define CACHE_FOLDER "game:\\shaders"
#define CACHE_MAGIC 0x58534331UL /* 'XSC1' */
/* (a compiled shader is a few KB; anything far larger is not one) */
#define CACHE_MAXIMUM_BYTES (1024UL * 1024)

struct cache_header
{
	unsigned long magic;
	unsigned long size;
	unsigned long check;
};

static unsigned long bytes_check(const unsigned char *bytes, unsigned long size)
{
	unsigned long hash = 2166136261UL, index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 16777619UL;
	return hash;
}

static void cache_path(char *path, size_t size, unsigned long long key, int vertex)
{
	_snprintf(path, size - 1, CACHE_FOLDER "\\%08lx%08lx.%s", (unsigned long)(key >> 32), (unsigned long)key,
		vertex ? "vsh" : "psh");
	path[size - 1] = '\0';
}

unsigned long long x360_shader_cache_key(const char *text, int vertex)
{
	unsigned long long hash = 14695981039346656037ULL;
	const unsigned char *at = (const unsigned char *)text;

	/* (the profile first: vs_3_0 or ps_3_0) */
	hash = (hash ^ (unsigned char)(vertex ? 'v' : 'p')) * 1099511628211ULL;
	for (; at && *at; at++)
		hash = (hash ^ *at) * 1099511628211ULL;
	return hash;
}

void *x360_shader_cache_load(unsigned long long key, int vertex, unsigned long *size)
{
	char path[64];
	struct cache_header header;
	unsigned char *bytes;
	FILE *file;

	*size = 0;
	cache_path(path, sizeof(path), key, vertex);
	file = fopen(path, "rb");
	if (!file)
		return NULL;
	if (fread(&header, sizeof(header), 1, file) != 1 || header.magic != CACHE_MAGIC || !header.size ||
		header.size > CACHE_MAXIMUM_BYTES)
	{
		fclose(file);
		return NULL;
	}
	bytes = malloc(header.size);
	if (!bytes || fread(bytes, 1, header.size, file) != header.size || bytes_check(bytes, header.size) != header.check)
	{
		free(bytes);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*size = header.size;
	return bytes;
}

int x360_shader_cache_store(unsigned long long key, int vertex, const void *code, unsigned long size)
{
	static int folder_made;
	struct cache_header header;
	char path[64];
	FILE *file;
	int ok;

	if (!code || !size || size > CACHE_MAXIMUM_BYTES)
		return 0;
	if (!folder_made)
	{
		CreateDirectoryA(CACHE_FOLDER, NULL);
		folder_made = 1;
	}
	cache_path(path, sizeof(path), key, vertex);
	file = fopen(path, "wb");
	if (!file)
		return 0;
	header.magic = CACHE_MAGIC;
	header.size = size;
	header.check = bytes_check(code, size);
	ok = fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(code, 1, size, file) == size;
	if (fclose(file) != 0)
		ok = 0;
	if (!ok)
		remove(path);
	return ok;
}
