/* The table of the window's memory the GPU reads in place
(port/xbox360/src/x360_gpu_reads.c): a draw marks what it reads with its
frame, a lock asks for the newest frame that read a range, the frames wrap
in 16 bits, and nothing outside the window is kept. */
#include <stdio.h>
#include <stdlib.h>

#include "../src/x360_gpu_reads.h"

static int failures;

#define CHECK(condition) \
	do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

int main(void)
{
	/* nothing read: nothing is busy */
	CHECK(x360_gpu_reads_newest(0, 4096, 10) == 0);
	CHECK(x360_gpu_reads_newest(X360_GPU_READS_SIZE - 1024, 1024, 10) == 0);

	/* a draw of frame 5 reads 100 bytes at 3000: the grains 2 and 3 */
	x360_gpu_reads_mark(3000, 100, 5);
	CHECK(x360_gpu_reads_newest(3000, 100, 5) == 5);
	CHECK(x360_gpu_reads_newest(2048, 1, 5) == 5);
	CHECK(x360_gpu_reads_newest(3072, 1, 5) == 5);
	CHECK(x360_gpu_reads_newest(4096, 1, 5) == 0);
	CHECK(x360_gpu_reads_newest(0, 2048, 5) == 0);
	/* a lock of memory ending in the grain sees it too */
	CHECK(x360_gpu_reads_newest(0, 2049, 5) == 5);

	/* a later frame's read of part of it: the newest wins for the range */
	x360_gpu_reads_mark(3072, 16, 7);
	CHECK(x360_gpu_reads_newest(3000, 100, 7) == 7);
	CHECK(x360_gpu_reads_newest(2048, 1024, 7) == 5);

	/* the frames wrap: 0xffff then 1 (0 is skipped), 1 is the newer */
	x360_gpu_reads_mark(10240, 1024, 0xffff);
	x360_gpu_reads_mark(11264, 1024, 1);
	CHECK(x360_gpu_reads_newest(10240, 2048, 1) == 1);
	CHECK(x360_gpu_reads_newest(10240, 2048, 0xffff) == 0xffff);

	/* the window's end: a range past it is cut, one outside is nothing */
	x360_gpu_reads_mark(X360_GPU_READS_SIZE - 512, 4096, 9);
	CHECK(x360_gpu_reads_newest(X360_GPU_READS_SIZE - 1024, 1024, 9) == 9);
	x360_gpu_reads_mark(X360_GPU_READS_SIZE, 4096, 9);
	CHECK(x360_gpu_reads_newest(X360_GPU_READS_SIZE, 4096, 9) == 0);
	x360_gpu_reads_mark(0xffffffffUL, 16, 9);
	CHECK(x360_gpu_reads_newest(0xffffffffUL, 16, 9) == 0);

	/* a size of 0 is nothing */
	x360_gpu_reads_mark(20480, 0, 3);
	CHECK(x360_gpu_reads_newest(20480, 0, 3) == 0);
	CHECK(x360_gpu_reads_newest(20480, 1, 3) == 0);

	/* cleared */
	x360_gpu_reads_clear();
	CHECK(x360_gpu_reads_newest(3000, 100, 7) == 0);

	if (failures)
	{
		printf("gpu_reads_test: %d failures\n", failures);
		return 1;
	}
	printf("gpu_reads_test: ok\n");
	return 0;
}
