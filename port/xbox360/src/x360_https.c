/*
X360_HTTPS.C

HTTPS on the Xbox 360 (x360_port.h): the game's shared client
(chupathingyce/port/console/https, Mbed TLS) over x360_net.c's sockets (the
posix.h layer the plain game list uses), the hosts looked up as the plain
list looks them up (posix_resolve_ipv4), random numbers from XNetRandom,
and a critical section so that one request runs at a time. The game list
(x360_online.c) and the signed legacy table (the game's delta.c) call
through here, on the list's thread.

The log names the host and what went wrong, never an address.
*/

#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <string.h>

#include "posix.h"
#include "x360_port.h"
#include "console_https.h"

/* the game's (port/linux/src/delta.c, HALO_CONSOLE_HTTPS) */
int delta_legacy_refresh(void);

/* (port/linux/src/port_config.h) */
void config_folder(char *path, size_t size);
/* the newest pin document's file (console_https.h's pins_path) */
static char pins_path[300];

/* the signed table again this often, and this long after a failure, as the
desktop builds (delta.c's DELTA_REFRESH_INTERVAL and DELTA_RETRY_INTERVAL) */
#define TABLE_REFRESH (4ul * 60 * 60 * 1000)
#define TABLE_RETRY (30ul * 60 * 1000)

static CRITICAL_SECTION https_lock;
static volatile LONG started;
static volatile LONG table_busy;
static DWORD table_next;
static int table_asked;

/* until the socket can be read (or written), within timeout_ms: 1 ready */
static int wait_ready(int handle, int write, unsigned long timeout_ms)
{
	int handles[1], count = 1;

	handles[0] = handle;
	if (posix_socket_select(write ? NULL : handles, write ? NULL : &count, write ? handles : NULL,
		write ? &count : NULL, NULL, NULL, (long)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000, 0) <= 0)
	{
		return 0;
	}
	return count > 0;
}

static int https_connect(const char *host, unsigned short port, unsigned long timeout_ms,
	console_https_socket *out)
{
	struct sockaddr_in target;
	unsigned long address;
	int handle;

	address = posix_resolve_ipv4(host);
	if (!address)
	{
		x360_log("https: %s did not resolve", host);
		return 0;
	}
	handle = posix_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (handle < 0)
		return 0;
	memset(&target, 0, sizeof(target));
	target.sin_family = AF_INET;
	target.sin_port = htons(port);
	target.sin_addr.s_addr = address;
	/* (not blocking, so the time given bounds the connect too, as the plain
	list's) */
	posix_socket_set_nonblocking(handle, 1);
	if (posix_socket_connect(handle, &target, sizeof(target)) != 0)
	{
		int code = posix_socket_last_error();

		if ((code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) || !wait_ready(handle, 1, timeout_ms))
		{
			x360_log("https: could not connect to %s (%d)", host, code);
			posix_socket_close(handle);
			return 0;
		}
	}
	*out = (console_https_socket)(LONG_PTR)handle;
	return 1;
}

static int https_send(console_https_socket socket, const unsigned char *data, int size, unsigned long timeout_ms)
{
	int handle = (int)(LONG_PTR)socket, count;

	if (!wait_ready(handle, 1, timeout_ms))
		return CONSOLE_HTTPS_IO_TIMEOUT;
	count = posix_socket_send(handle, data, size, 0);
	if (count < 0 && posix_socket_last_error() == WSAEWOULDBLOCK)
		return CONSOLE_HTTPS_IO_TIMEOUT;
	return count > 0 ? count : -1;
}

static int https_receive(console_https_socket socket, unsigned char *buffer, int size, unsigned long timeout_ms)
{
	int handle = (int)(LONG_PTR)socket, count;

	if (!wait_ready(handle, 0, timeout_ms))
		return CONSOLE_HTTPS_IO_TIMEOUT;
	count = posix_socket_recv(handle, buffer, size, 0);
	if (count < 0)
	{
		int code = posix_socket_last_error();

		/* (a reset is the server's close, as on the original Xbox) */
		if (code == WSAECONNRESET)
			return 0;
		return code == WSAEWOULDBLOCK ? CONSOLE_HTTPS_IO_TIMEOUT : -1;
	}
	return count;
}

static void https_close(console_https_socket socket)
{
	posix_socket_close((int)(LONG_PTR)socket);
}

/* XNet's random numbers (the console's own cryptographic source) */
static int https_random(unsigned char *buffer, int size)
{
	return XNetRandom(buffer, (UINT)size) == 0;
}

static unsigned long https_milliseconds(void)
{
	return GetTickCount();
}

static void https_lock_enter(void)
{
	EnterCriticalSection(&https_lock);
}

static void https_lock_leave(void)
{
	LeaveCriticalSection(&https_lock);
}

static void https_log(const char *text)
{
	x360_log("%s", text);
}

static struct console_https_port x360_https_port =
{
	https_connect, https_send, https_receive, https_close, https_random, https_milliseconds,
	https_lock_enter, https_lock_leave, https_log, "Warthog360/1",
	/* (the pin document: plain HTTP from the list's host, x360_online.c;
	where it is kept is set at the start) */
	x360_online_plain_get, NULL,
};

void x360_https_start(void)
{
	/* (the first caller sets it up: the game list's thread, once the
	network is up) */
	if (InterlockedCompareExchange(&started, 1, 0) != 0)
		return;
	InitializeCriticalSection(&https_lock);
	/* (the newest pin document, beside config.toml) */
	config_folder(pins_path, sizeof(pins_path) - 32);
	strcat(pins_path, "console_pins.signed");
	x360_https_port.pins_path = pins_path;
	console_https_initialize(&x360_https_port);
}

void x360_https_table_refresh(void)
{
	DWORD now = GetTickCount();

	if (!console_https_available() || (table_asked && (LONG)(now - table_next) < 0))
		return;
	/* (one thread at a time: another list thread may be here already) */
	if (InterlockedCompareExchange(&table_busy, 1, 0) != 0)
		return;
	table_asked = 1;
	table_next = GetTickCount() + (delta_legacy_refresh() ? TABLE_REFRESH : TABLE_RETRY);
	X360_LWSYNC();
	InterlockedExchange(&table_busy, 0);
}
