/*
X360_ALLOC_HOOK.H

The C runtime's allocator calls, counted per call site (port/xbox360/src/
x360_memory.c: x360_memory_report names the sites holding the most). Included
by the prefix header and x360_port.h after the C runtime's own headers.
*/

#ifndef __X360_ALLOC_HOOK_H
#define __X360_ALLOC_HOOK_H

#include <stddef.h>

void *x360_track_malloc(size_t size);
void *x360_track_calloc(size_t count, size_t size);
void *x360_track_realloc(void *memory, size_t size);
void x360_track_free(void *memory);

/* (only in a build with the developer tools: otherwise the allocator is the
C runtime's own, with no cost per call) */
#if defined(HALO_CONSOLE_DEBUG_TOOLS) && !defined(X360_NO_ALLOC_HOOK)
#define malloc x360_track_malloc
#define calloc x360_track_calloc
#define realloc x360_track_realloc
#define free x360_track_free
#endif

#endif /* __X360_ALLOC_HOOK_H */
