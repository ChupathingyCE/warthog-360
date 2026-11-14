/*
X360_MCC_PREVIEW.C
The MCC Workshop packages' preview pictures (port/linux/src/posix_mcc_preview.c
reads them from the host's disk with Win32 calls the console's kernel lacks,
GetFinalPathNameByHandle among them). The console has no Workshop packages
yet, so nothing is read: the caller (mcc_preview_image.c) shows no picture.
*/
#include "posix.h"

int posix_mcc_preview_read(const char *root, const char *relative, void *buffer, posix_ulong capacity)
{
	(void)root;
	(void)relative;
	(void)buffer;
	(void)capacity;
	return 0;
}
