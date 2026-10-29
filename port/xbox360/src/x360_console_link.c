/*
X360_CONSOLE_LINK.C

Link Profile on the Xbox 360 (the game's port/console/link/console_link.h):
the hooks it asks of the console. Its requests go to the console list's
host over plain HTTP (x360_online_request, src/x360_online.c), on a
short-lived thread of their own. The device key and the linked profile are
kept beside the game: game:\delta_device.key and game:\delta_link.txt.
Delta Peer's PROFILE carries console_profile_id() (HALO_CONSOLE_PROFILE_ID).
*/

#include <xtl.h>
#include <string.h>

#include "x360_port.h"
#include "console_link.h"

int x360_online_request(const char *path, const char *body, int *status, char *reply, int reply_size);

/* (the requests' frames and Monocypher's) */
#define LINK_STACK (64 * 1024)

static CRITICAL_SECTION link_lock;
static int link_started;

struct link_thread
{
	void (*function)(void *context);
	void *context;
};

static struct link_thread link_thread_start;

static DWORD WINAPI link_thread(LPVOID parameter)
{
	struct link_thread *start = parameter;

	start->function(start->context);
	return 0;
}

static int link_start_thread(void (*function)(void *context), void *context)
{
	HANDLE thread;

	/* (console_link.c runs one at a time, so one start's place is enough) */
	link_thread_start.function = function;
	link_thread_start.context = context;
	thread = CreateThread(NULL, LINK_STACK, link_thread, &link_thread_start, 0, NULL);
	if (!thread)
		return 0;
	CloseHandle(thread);
	return 1;
}

static void link_enter(void)
{
	EnterCriticalSection(&link_lock);
}

static void link_leave(void)
{
	LeaveCriticalSection(&link_lock);
}

static void link_random(void *buffer, int size)
{
	XNetRandom((BYTE *)buffer, (UINT)size);
}

static unsigned long link_milliseconds(void)
{
	return GetTickCount();
}

static void link_log(const char *text)
{
	x360_log("%s", text);
}

static const struct console_link_port x360_link_port =
{
	"xbox360",
	"Warthog 360",
	"game:\\delta_device.key",
	"game:\\delta_link.txt",
	x360_online_request,
	/* (no TLS on the console yet) */
	NULL,
	link_start_thread,
	link_enter,
	link_leave,
	link_random,
	link_milliseconds,
	link_log,
};

/* the main thread's, before the panel or Delta Peer asks */
void x360_console_link_start(void)
{
	if (link_started)
		return;
	link_started = 1;
	InitializeCriticalSection(&link_lock);
	console_link_initialize(&x360_link_port);
}

int console_profile_id(unsigned char *id)
{
	x360_console_link_start();
	return console_link_player_id(id);
}
