/*
X360_MEMORY.C

The Xbox window (source/cseries/xbox_address.h, HALO_WINDOW_TRANSLATED): one block
of the console's memory holding every Xbox address the game uses. The
console's executable is where the Xbox's memory was (0x82000000), so the
block goes wherever the console has room, and Xbox address X is at host
address xbox_address_space_base + X.

The block is committed whole: the console has no pages backed on first
touch, and its 512 MB leave room for it (port/xbox360/README.md).
*/

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#define X360_NO_ALLOC_HOOK
#include "x360_port.h"

/* the host address of Xbox address 0, modulo 2^32 (xbox_address.h) */
unsigned int xbox_address_space_base;

static unsigned char *window_begin;
static size_t window_size;

void *platform_xbox_window_reserve(unsigned int size)
{
	MEMORYSTATUS status;
	void *block;

	if (window_begin)
		return window_begin;
	GlobalMemoryStatus(&status);
	x360_log("memory: %u MB of %u MB free before the Xbox window",
		(unsigned int)(status.dwAvailPhys >> 20), (unsigned int)(status.dwTotalPhys >> 20));
	/* 64 KB pages: fewer TLB misses than 4 KB, and no need for the 16 MB
	pages' alignment */
	block = XPhysicalAlloc(size, MAXULONG_PTR, 0, PAGE_READWRITE | MEM_LARGE_PAGES);
	if (!block)
	{
		x360_log("memory: XPhysicalAlloc of %u MB for the Xbox window failed", size >> 20);
		return NULL;
	}
	memset(block, 0, size);
	window_begin = block;
	window_size = size;
	GlobalMemoryStatus(&status);
	x360_log("memory: Xbox window at host 0x%08x, %u MB; %u MB free after", (unsigned int)(UINT_PTR)block,
		size >> 20, (unsigned int)(status.dwAvailPhys >> 20));
	return block;
}

int x360_window_contains(const void *address, size_t size)
{
	const unsigned char *pointer = address;

	return window_begin && pointer >= window_begin && size <= window_size &&
		(size_t)(pointer - window_begin) <= window_size - size;
}

int x360_window_commit(void *address, size_t size)
{
	return x360_window_contains(address, size);
}

/* every cache line of the window written back to memory, where the GPU reads it */
void x360_window_flush(void)
{
	unsigned char *line;

	if (!window_begin)
		return;
	for (line = window_begin; line < window_begin + window_size; line += 128)
		__asm__ volatile ("dcbst 0, %0" : : "r"(line) : "memory");
	__asm__ volatile ("sync" : : : "memory");
}

/* ---------- the host's memory, counted

Developer tools (HALO_CONSOLE_DEBUG_TOOLS, configure.py
--console-debug-tools): every malloc, calloc, realloc and free of the game and the platform layer (x360_alloc_hook.h), and the GL layer's
physical memory (x360_phys_*), are counted by call site / by kind in fixed
tables (no allocation of their own). The report prints the totals; the first
refusal prints the sites holding the most, as return addresses to look up in
the link's symbols (nm -n of the title's ELF).

In a normal build none of this exists: the allocator is the C runtime's, the
physical calls go straight to the kernel's, and the report is a few lines of
the console's own counts. */

extern size_t xbox_heap_used(void);

#ifdef HALO_CONSOLE_DEBUG_TOOLS

unsigned long x360_phys_avail(void)
{
	MEMORYSTATUS status;

	GlobalMemoryStatus(&status);
	return (unsigned long)status.dwAvailPhys;
}

#define SITE_MAX 1024
#define BLOCK_MAX (1 << 17)
#define BLOCK_EMPTY 0u
#define BLOCK_DELETED 1u
#define TAG_PHYS 0x10000u
#define MARK_STEP (16u << 20)

struct site
{
	unsigned int address;
	unsigned int blocks, bytes, calls;
};

struct block
{
	unsigned int pointer, size, tag;
};

static struct site sites[SITE_MAX];
static struct block blocks[BLOCK_MAX];
static unsigned int blocks_used;       /* entries not empty (live or deleted) */
static unsigned int heap_bytes, heap_blocks, untracked;
static unsigned int phys_bytes[X360_PHYS_KINDS];
static unsigned int phys_blocks[X360_PHYS_KINDS];
static unsigned int heap_mark = MARK_STEP, phys_mark = MARK_STEP;
static unsigned int heap_peak;
static volatile long lock;
static volatile int reporting, failed;
static unsigned int call_count;

static void lock_take(void)
{
	while (InterlockedCompareExchange(&lock, 1, 0) != 0)
		;
}

static void lock_drop(void)
{
	X360_LWSYNC();
	lock = 0;
}

static unsigned int pointer_hash(unsigned int pointer)
{
	return ((pointer >> 4) * 2654435761u) >> (32 - 17);
}

static struct site *site_find(unsigned int address)
{
	unsigned int index = ((address >> 2) * 2654435761u) >> 22, tries;

	for (tries = 0; tries < SITE_MAX; tries++, index = (index + 1) % SITE_MAX)
	{
		if (sites[index].address == address)
			return &sites[index];
		if (!sites[index].address)
		{
			sites[index].address = address;
			return &sites[index];
		}
	}
	return &sites[SITE_MAX - 1];
}

/* (lock held) */
static struct block *block_find(unsigned int pointer)
{
	unsigned int index = pointer_hash(pointer), tries;

	for (tries = 0; tries < BLOCK_MAX; tries++, index = (index + 1) & (BLOCK_MAX - 1))
	{
		if (blocks[index].pointer == pointer)
			return &blocks[index];
		if (blocks[index].pointer == BLOCK_EMPTY)
			break;
	}
	return NULL;
}

/* (lock held) forgets a block, taking its bytes back off its owner */
static void block_remove(unsigned int pointer)
{
	struct block *block = block_find(pointer);

	if (!block)
		return;
	if (block->tag & TAG_PHYS)
	{
		unsigned int kind = block->tag & 0xFFFF;

		phys_bytes[kind] -= block->size;
		phys_blocks[kind]--;
	}
	else
	{
		struct site *site = &sites[block->tag];

		site->bytes -= block->size;
		site->blocks--;
		heap_bytes -= block->size;
		heap_blocks--;
	}
	block->pointer = BLOCK_DELETED;
}

/* (lock held) */
static void block_add(unsigned int pointer, unsigned int size, unsigned int site_address, int kind)
{
	unsigned int index = pointer_hash(pointer), tries;
	struct block *slot = NULL;

	block_remove(pointer);
	for (tries = 0; tries < BLOCK_MAX; tries++, index = (index + 1) & (BLOCK_MAX - 1))
	{
		if (blocks[index].pointer == BLOCK_EMPTY)
		{
			if (!slot)
			{
				slot = &blocks[index];
				blocks_used++;
			}
			break;
		}
		if (blocks[index].pointer == BLOCK_DELETED && !slot)
			slot = &blocks[index];
	}
	if (!slot || blocks_used > BLOCK_MAX - BLOCK_MAX / 8)
	{
		untracked++;
		return;
	}
	slot->pointer = pointer;
	slot->size = size;
	if (kind >= 0)
	{
		slot->tag = TAG_PHYS | (unsigned int)kind;
		phys_bytes[kind] += size;
		phys_blocks[kind]++;
	}
	else
	{
		struct site *site = site_find(site_address);

		slot->tag = (unsigned int)(site - sites);
		site->bytes += size;
		site->blocks++;
		site->calls++;
		heap_bytes += size;
		heap_blocks++;
		if (heap_bytes > heap_peak)
			heap_peak = heap_bytes;
	}
}

static unsigned int phys_total(void)
{
	unsigned int total = 0;
	int kind;

	for (kind = 0; kind < X360_PHYS_KINDS; kind++)
		total += phys_bytes[kind];
	return total;
}

static const char *heap_check(void)
{
	/* (the C runtime's own walk: the kernel's RtlValidateHeap is not an
	import the console's title can have) */
	switch (_heapchk())
	{
		case _HEAPOK:
		case _HEAPEMPTY:
			return "ok";
		default:
			return "CORRUPT";
	}
}

static void dump_sites(void)
{
	struct site *top[15];
	int count = 0, index, place;

	for (index = 0; index < SITE_MAX; index++)
	{
		struct site *site = &sites[index];

		if (!site->address || !site->bytes)
			continue;
		for (place = count; place > 0 && top[place - 1]->bytes < site->bytes; place--)
			if (place < 15)
				top[place] = top[place - 1];
		if (place < 15)
		{
			top[place] = site;
			if (count < 15)
				count++;
		}
	}
	for (index = 0; index < count; index++)
		x360_log("memtrack: heap site 0x%08x: %u KB in %u blocks (%u calls)", top[index]->address,
			top[index]->bytes >> 10, top[index]->blocks, top[index]->calls);
}

void x360_memory_report(const char *when)
{
	MEMORYSTATUS status;

	if (reporting)
		return;
	reporting = 1;
	GlobalMemoryStatus(&status);
	x360_log("memory: %s: %u MB of %u MB free, Xbox heap %u KB used, window %u MB; host heap %u KB in %u blocks "
		"(peak %u KB, %u untracked), physical %u KB (texture %u, buffer %u, target %u, other %u KB); heap check %s",
		when, (unsigned int)(status.dwAvailPhys >> 20), (unsigned int)(status.dwTotalPhys >> 20),
		(unsigned int)(xbox_heap_used() >> 10), (unsigned int)(window_size >> 20), heap_bytes >> 10, heap_blocks,
		heap_peak >> 10, untracked, phys_total() >> 10, phys_bytes[0] >> 10, phys_bytes[1] >> 10,
		phys_bytes[2] >> 10, phys_bytes[3] >> 10, heap_check());
	reporting = 0;
}

void x360_phys_failure(const char *what, unsigned long size)
{
	if (reporting)
		return;
	if (failed)
	{
		if (failed++ < 8)
			x360_log("memtrack: another refusal: %s of %lu bytes", what, size);
		return;
	}
	failed = 1;
	x360_log("memtrack: FIRST REFUSAL: %s of %lu bytes", what, size);
	x360_memory_report("refusal");
	reporting = 1;
	dump_sites();
	reporting = 0;
}

/* a mark crossed: a line saying where the memory is going */
static void marks_check(void)
{
	char text[24];

	if (heap_bytes >= heap_mark)
	{
		heap_mark = (heap_bytes & ~(MARK_STEP - 1)) + MARK_STEP;
		_snprintf(text, sizeof(text), "heap %u MB", heap_bytes >> 20);
		x360_memory_report(text);
	}
	else if (phys_total() >= phys_mark)
	{
		phys_mark = (phys_total() & ~(MARK_STEP - 1)) + MARK_STEP;
		_snprintf(text, sizeof(text), "physical %u MB", phys_total() >> 20);
		x360_memory_report(text);
	}
}

static void low_memory_check(void)
{
	if (!failed && (++call_count & 255) == 0 && x360_phys_avail() < (12u << 20))
		x360_phys_failure("low memory (under 12 MB free)", 0);
}

static void __attribute__((noinline)) heap_add(void *memory, size_t size, void *caller)
{
	lock_take();
	block_add((unsigned int)(UINT_PTR)memory, (unsigned int)size, (unsigned int)(UINT_PTR)caller, -1);
	lock_drop();
	marks_check();
}

void *x360_track_malloc(size_t size)
{
	void *caller = __builtin_return_address(0);
	void *memory = malloc(size);

	if (memory)
		heap_add(memory, size, caller);
	else
		x360_phys_failure("malloc", size);
	low_memory_check();
	return memory;
}

void *x360_track_calloc(size_t count, size_t size)
{
	void *caller = __builtin_return_address(0);
	void *memory = calloc(count, size);

	if (memory)
		heap_add(memory, count * size, caller);
	else
		x360_phys_failure("calloc", count * size);
	low_memory_check();
	return memory;
}

void *x360_track_realloc(void *old, size_t size)
{
	void *caller = __builtin_return_address(0);
	void *memory = realloc(old, size);

	if (memory)
	{
		if (old && old != memory)
		{
			lock_take();
			block_remove((unsigned int)(UINT_PTR)old);
			lock_drop();
		}
		heap_add(memory, size, caller);
	}
	else if (size)
		x360_phys_failure("realloc", size);
	return memory;
}

void x360_track_free(void *memory)
{
	if (memory)
	{
		lock_take();
		block_remove((unsigned int)(UINT_PTR)memory);
		lock_drop();
	}
	free(memory);
}

void *x360_phys_alloc(int kind, size_t size, unsigned long flags)
{
	void *memory = XPhysicalAlloc(size, MAXULONG_PTR, 0, flags);

	if (!memory)
	{
		x360_phys_failure("XPhysicalAlloc", size);
		return NULL;
	}
	lock_take();
	block_add((unsigned int)(UINT_PTR)memory, (unsigned int)size, 0, kind);
	lock_drop();
	marks_check();
	return memory;
}

void x360_phys_free(void *memory)
{
	if (!memory)
		return;
	lock_take();
	block_remove((unsigned int)(UINT_PTR)memory);
	lock_drop();
	XPhysicalFree(memory);
}

void x360_phys_account(int kind, long bytes)
{
	lock_take();
	if (bytes < 0 && phys_bytes[kind] < (unsigned int)-bytes)
		phys_bytes[kind] = 0;
	else
		phys_bytes[kind] += bytes;
	if (bytes > 0)
		phys_blocks[kind]++;
	else if (phys_blocks[kind])
		phys_blocks[kind]--;
	lock_drop();
	if (bytes > 0)
		marks_check();
}

#else /* HALO_CONSOLE_DEBUG_TOOLS */

void x360_memory_report(const char *when)
{
	MEMORYSTATUS status;

	GlobalMemoryStatus(&status);
	x360_log("memory: %s: %u MB of %u MB free, Xbox heap %u KB used, window %u MB", when,
		(unsigned int)(status.dwAvailPhys >> 20), (unsigned int)(status.dwTotalPhys >> 20),
		(unsigned int)(xbox_heap_used() >> 10), (unsigned int)(window_size >> 20));
}

/* the first refusals only, as a line each */
void x360_phys_failure(const char *what, unsigned long size)
{
	static int failures;

	if (failures++ == 0)
	{
		x360_log("memory: FIRST REFUSAL: %s of %lu bytes", what, size);
		x360_memory_report("refusal");
	}
	else if (failures <= 8)
	{
		x360_log("memory: another refusal: %s of %lu bytes", what, size);
	}
}

void *x360_phys_alloc(int kind, size_t size, unsigned long flags)
{
	void *memory = XPhysicalAlloc(size, MAXULONG_PTR, 0, flags);

	(void)kind;
	if (!memory)
		x360_phys_failure("XPhysicalAlloc", size);
	return memory;
}

void x360_phys_free(void *memory)
{
	if (memory)
		XPhysicalFree(memory);
}

#endif /* HALO_CONSOLE_DEBUG_TOOLS */

/* the stages of a map load: its files opening */
void x360_memory_file(const char *path)
{
	static int reports;
	size_t length = strlen(path);
	const char *name;

	if (length < 5 || _stricmp(path + length - 4, ".map") != 0 || reports >= 40)
		return;
	reports++;
	name = strrchr(path, '/');
	if (!name)
		name = strrchr(path, '\\');
	{
		char text[64];

		_snprintf(text, sizeof(text), "open %s", name ? name + 1 : path);
		text[sizeof(text) - 1] = '\0';
		x360_memory_report(text);
	}
}
