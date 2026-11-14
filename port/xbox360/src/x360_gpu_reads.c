/*
X360_GPU_READS.C
The table of the window's memory the GPU reads in place (x360_gpu_reads.h).
*/
#include <stdlib.h>
#include <string.h>

#include "x360_gpu_reads.h"

static unsigned short *reads;

static int reads_range(unsigned long offset, unsigned long size, unsigned long *first, unsigned long *last)
{
	if (!size || offset >= X360_GPU_READS_SIZE)
		return 0;
	*first = offset / X360_GPU_READS_GRAIN;
	if (size > X360_GPU_READS_SIZE - offset)
		size = X360_GPU_READS_SIZE - offset;
	*last = (offset + size - 1) / X360_GPU_READS_GRAIN;
	return 1;
}

void x360_gpu_reads_mark(unsigned long offset, unsigned long size, unsigned short frame)
{
	unsigned long first, last, index;

	if (!reads)
	{
		reads = calloc(X360_GPU_READS_ENTRIES, sizeof(*reads));
		if (!reads)
			return;
	}
	if (!reads_range(offset, size, &first, &last))
		return;
	for (index = first; index <= last; index++)
		reads[index] = frame;
}

unsigned short x360_gpu_reads_newest(unsigned long offset, unsigned long size, unsigned short current)
{
	unsigned long first, last, index;
	unsigned short newest = 0, newest_age = 0xffff;

	if (!reads || !reads_range(offset, size, &first, &last))
		return 0;
	for (index = first; index <= last; index++)
	{
		unsigned short frame = reads[index], age;

		if (!frame)
			continue;
		/* (the frames wrap in 16 bits: the newest is the one least behind
		the current frame) */
		age = (unsigned short)(current - frame);
		if (age < newest_age)
		{
			newest_age = age;
			newest = frame;
		}
	}
	return newest;
}

void x360_gpu_reads_clear(void)
{
	if (reads)
		memset(reads, 0, X360_GPU_READS_ENTRIES * sizeof(*reads));
}
