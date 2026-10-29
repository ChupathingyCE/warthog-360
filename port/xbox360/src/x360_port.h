/*
X360_PORT.H

What the console's own units (port/xbox360/src/x360_*.c, compiled against
the console's SDK) give each other and the rest of the build. Only plain C
types cross this line: the game and the desktop platform layer see the
original Xbox's SDK declarations, whose types of the same names differ.
*/

#ifndef __X360_PORT_H
#define __X360_PORT_H

#include <stddef.h>
#include "../include/x360_alloc_hook.h"

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

/* ---------- barriers (the SDK's __sync and __lwsync are __emit, an
intrinsic of Microsoft's compiler alone) */

#define X360_SYNC() __asm__ __volatile__("sync" ::: "memory")
#define X360_LWSYNC() __asm__ __volatile__("lwsync" ::: "memory")

/* ---------- the log: the debug channel (XBDM) and game:\debug.txt */

void console_debug_print(const char *text);
void x360_log(const char *format, ...);

/* ---------- compiled shaders on the drive (x360_shader_cache.c) */

/* the cache's name for a shader: its HLSL text and its profile */
unsigned long long x360_shader_cache_key(const char *text, int vertex);
/* the compiled shader kept under `key` (malloc'd, `size` bytes), or NULL */
void *x360_shader_cache_load(unsigned long long key, int vertex, unsigned long *size);
/* 1 if the compiled shader was written whole */
int x360_shader_cache_store(unsigned long long key, int vertex, const void *code, unsigned long size);

/* ---------- the Xbox window (x360_memory.c) */
void x360_window_flush(void);
/* a log line of the host's free memory and the Xbox heap's use, 'when' naming the moment */
void x360_memory_report(const char *when);
/* a report when the game opens a map file (the stages of a map load) */
void x360_memory_file(const char *path);

/* physical memory by kind, counted for the report and the failure dump */
enum { X360_PHYS_TEXTURE, X360_PHYS_BUFFER, X360_PHYS_TARGET, X360_PHYS_OTHER, X360_PHYS_KINDS };
/* XPhysicalAlloc / XPhysicalFree, counted under 'kind' */
void *x360_phys_alloc(int kind, size_t size, unsigned long flags);
void x360_phys_free(void *memory);
/* the host's free physical bytes now (a Direct3D resource's size is the drop across its creation) */
unsigned long x360_phys_avail(void);
#ifdef HALO_CONSOLE_DEBUG_TOOLS
/* a Direct3D resource of 'kind' now holds 'bytes' more (or, negative, fewer) */
void x360_phys_account(int kind, long bytes);
#else
/* (counted only with the developer tools: no cost, no memory-status call
per texture, otherwise) */
#define x360_phys_account(kind, bytes) ((void)(kind), (void)(bytes))
#define x360_phys_avail() 0UL
#endif
/* something refused to allocate: the first one dumps the sites */
void x360_phys_failure(const char *what, unsigned long size);

/* whether [address, address + size) is inside the window */
int x360_window_contains(const void *address, size_t size);
/* makes the range usable (the window is committed whole: always 1) */
int x360_window_commit(void *address, size_t size);

/* ---------- errors */

int x360_errno_from_error(unsigned long error);

/* ---------- paths

A host path of the desktop platform layer (forward slashes, relative to
the current folder) as the console takes it: backslashes, and relative
paths below game:\ (the title's folder, its only mounted drive). */
void x360_path(const char *path, char *buffer, size_t size);

/* ---------- controllers (x360_input_pad.c), for xbox360_input.c */

struct x360_pad
{
	unsigned short buttons; /* the original Xbox's XINPUT_GAMEPAD_* digital bits */
	unsigned char analog[8]; /* A, B, X, Y, black, white, left and right trigger */
	short thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};

/* fills pad; 0 if no controller is in that port (0 to 3) */
int x360_pad_read(int port, struct x360_pad *pad);
void x360_pad_rumble(int port, unsigned short left, unsigned short right);

/* ---------- the network (x360_net.c) */

/* starts XNet (insecure: the internet and the PCs) and Winsock, once;
nonzero if they are up */
int x360_network_start(void);

/* ---------- the game list (x360_online.c, x360_game_list.h) */

/* the network self-test and the first game list, on a thread of their own */
void x360_online_start(void);

/* ---------- HTTPS (x360_https.c: the game's port/console/https)

Requests block, and need CONSOLE_HTTPS_STACK_SIZE of the calling thread's
stack: they run on the game list's thread (x360_online.c), whose stack is
sized for them. */

/* the client set up (once the network is up; any thread, once) */
void x360_https_start(void);
/* one plain HTTP GET of host's path on port 80, the whole response into
response (x360_online.c, as the plain list asks): 1 if one came. The HTTPS
client's plain_get, for the signed pin document */
int x360_online_plain_get(const char *host, const char *path, char *response, unsigned long size,
	unsigned long *received, unsigned long timeout_ms);
/* the signed legacy table fetched again, when it is due (at the first call,
then every 4 hours, or 30 minutes after a failure); blocking, on a thread
with CONSOLE_HTTPS_STACK_SIZE + DELTA_REFRESH_STACK_SIZE to spare */
void x360_https_table_refresh(void);

/* ---------- the screen (x360_video.c) */

/* the Direct3D 9 device (D3DDevice *), made on the first call; NULL if it
cannot be */
void *x360_video_device(void);
/* HLSL to microcode on the console: an ID3DXBuffer, or NULL (logged) */
void *x360_video_compile(const char *source, const char *profile, unsigned long *size);
const void *x360_video_shader_code(void *shader);
void x360_video_shader_free(void *shader);
/* the device, the compiler, a triangle and a present, logged */
void x360_video_probe(void);

/* ---------- OpenGL on the console (x360_gl.c) */

/* an entry point by its name, NULL if there is none */
void *x360_gl_function(const char *name);
/* the frame drawn: presented */
void x360_gl_swap(void);

/* ---------- leaving */

void console_exit_to_launcher(void);
/* logs why and the return addresses of the calls that led here */
void console_log_backtrace(const char *why);

#endif
