/*
X360_ONLINE.C

The console game list (x360_game_list.h) and, before its first fetch, a
network self-test at start-up, both on a thread of their own so nothing
waits on them:

1. XNet and Winsock up (x360_network_start: Butter and Jelly's order).
2. DNS: warthog.milenko.org looked up.
3. UDP: a datagram to this console's own port (127.0.0.1, which x360_net.c
   sends to the console's address), read back.
4. HTTP: GET /v1/console/games?coop=1&packs=1 from warthog.milenko.org, plain
   HTTP/1.0 as the original Xbox asks for it, parsed; the list is the
   Multiplayer menu's ONLINE GAMES (game/x360_browser_screen.c).

Each step is one line in game:\port.txt and on the debug channel ("net
test: ..."). Never an address: only its kind, as x360_net.c logs them.
The sockets are x360_net.c's (posix.h), so the list and the self-test go
through what the game's own networking uses.
*/

#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <string.h>

#include "posix.h"
#include "x360_port.h"
#include "x360_game_list.h"

/* (the game's network_distributed.c) the messages turned to the PC's byte order and
back, against the PC's bytes; the count that differ */
long network_wire_self_test(char *report, long report_size);

#define FETCH_TIMEOUT 15000
#define ATTEMPTS 3
#define RETRY_WAIT 10000
#define ECHO_WAIT 2000

enum
{
	_list_idle,
	_list_fetching,
	_list_done,
	_list_failed
};

/* the fetch's thread writes fetched, then the state (an interlocked store,
so the list is whole before it); the main thread takes it into shown once
the state is done, and only then may a new fetch start */
static volatile LONG list_state = _list_idle;
static int list_taken = 1;
static struct game_list fetched;
static int list_error;
/* the main thread's: the last list that came, kept while a refresh fails */
static struct game_list shown;
static int shown_lists;
static int shown_error;
static int shown_state = X360_GAME_LIST_NONE;
/* the self-test runs once, before the first fetch */
static int self_tested;

static void set_state(LONG state)
{
	X360_LWSYNC();
	InterlockedExchange(&list_state, state);
}

/* ---------- waits */

/* until the socket can be read (or written) or the deadline: 1 ready */
static int wait_ready(int handle, int write, DWORD deadline)
{
	int handles[1], count = 1;
	long left = (long)(deadline - GetTickCount());

	if (left <= 0)
		return 0;
	handles[0] = handle;
	if (posix_socket_select(write ? NULL : handles, write ? NULL : &count, write ? handles : NULL,
		write ? &count : NULL, NULL, NULL, left / 1000, (left % 1000) * 1000, 0) <= 0)
	{
		return 0;
	}
	return count > 0;
}

/* ---------- the HTTP request */

/* whether the response's first size bytes hold its headers' end, and the
response's whole size if they give a Content-Length (else 0) */
static int header_end(const char *data, unsigned long size, unsigned long *total)
{
	struct game_list_response response;
	unsigned long index;

	for (index = 1; index < size; index++)
	{
		if (data[index] == '\n' && (data[index - 1] == '\n' || (index >= 2 && data[index - 1] == '\r' &&
			data[index - 2] == '\n')))
		{
			int result = game_list_parse_response(data, size, &response);

			*total = 0;
			if ((result == GAME_LIST_OK || result == GAME_LIST_ERROR_BODY_TRUNCATED) && response.content_length >= 0)
				*total = index + 1 + (unsigned long)response.content_length;
			return 1;
		}
	}
	return 0;
}

/* GET path (or POST body, JSON, when it is not NULL) from address:port
(network order), with Host: host, into buffer (the whole response, never
past size), within timeout_ms */
static int http_request(unsigned long address, unsigned short port, const char *host, const char *path,
	const char *body, char *buffer, unsigned long size, unsigned long *received, unsigned long timeout_ms)
{
	char request[1024];
	struct sockaddr_in target;
	DWORD deadline = GetTickCount() + timeout_ms;
	unsigned long sent = 0, total = 0;
	int headers_done = 0, length, handle, result = GAME_LIST_OK;

	*received = 0;
	if (body)
		length = _snprintf(request, sizeof(request) - 1,
			"POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Warthog360/1\r\nAccept: text/plain\r\n"
			"Accept-Encoding: identity\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
			"Connection: close\r\n\r\n%s", path, host, (int)strlen(body), body);
	else
		length = _snprintf(request, sizeof(request) - 1,
			"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Warthog360/1\r\nAccept: text/plain\r\n"
			"Accept-Encoding: identity\r\nConnection: close\r\n\r\n", path, host);
	if (length <= 0 || length >= (int)sizeof(request) - 1)
		return GAME_LIST_ERROR_SEND;
	handle = posix_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (handle < 0)
		return GAME_LIST_ERROR_CONNECT;
	memset(&target, 0, sizeof(target));
	target.sin_family = AF_INET;
	target.sin_port = htons(port);
	target.sin_addr.s_addr = address;
	/* (not blocking, so the deadline bounds the connect too) */
	posix_socket_set_nonblocking(handle, 1);
	if (posix_socket_connect(handle, &target, sizeof(target)) != 0)
	{
		int code = posix_socket_last_error();

		if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS)
		{
			posix_socket_close(handle);
			return GAME_LIST_ERROR_CONNECT;
		}
		if (!wait_ready(handle, 1, deadline))
		{
			posix_socket_close(handle);
			return GAME_LIST_ERROR_CONNECT;
		}
	}
	while (sent < (unsigned long)length)
	{
		int count;

		if (!wait_ready(handle, 1, deadline))
		{
			result = GAME_LIST_ERROR_TIMEOUT;
			break;
		}
		count = posix_socket_send(handle, request + sent, length - (int)sent, 0);
		if (count <= 0)
		{
			if (count < 0 && posix_socket_last_error() == WSAEWOULDBLOCK)
				continue;
			result = GAME_LIST_ERROR_SEND;
			break;
		}
		sent += (unsigned long)count;
	}
	/* to the server's close, or the Content-Length's end, never past size */
	while (result == GAME_LIST_OK)
	{
		int count;

		if (*received >= size)
		{
			result = GAME_LIST_ERROR_TOO_LARGE;
			break;
		}
		if (!wait_ready(handle, 0, deadline))
		{
			result = GAME_LIST_ERROR_TIMEOUT;
			break;
		}
		count = posix_socket_recv(handle, buffer + *received, (int)(size - *received), 0);
		if (count < 0)
		{
			if (posix_socket_last_error() == WSAEWOULDBLOCK)
				continue;
			result = GAME_LIST_ERROR_RECEIVE;
			break;
		}
		if (count == 0)
			break;
		*received += (unsigned long)count;
		if (!headers_done)
			headers_done = header_end(buffer, *received, &total);
		if (headers_done && total && *received >= total)
			break;
	}
	posix_socket_close(handle);
	return result;
}

/* ---------- the self-test's UDP step */

/* a datagram to this console's own port, read back: 1 if it came back
whole, its round trip in *milliseconds */
static int udp_echo(DWORD *milliseconds, const char **failure)
{
	static const char probe[] = "warthog360 echo";
	struct sockaddr_in local, to, from;
	int length = sizeof(local), from_length = sizeof(from), handle, count;
	char reply[64];
	DWORD start;

	*failure = "no socket";
	handle = posix_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (handle < 0)
		return 0;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	*failure = "could not bind";
	if (posix_socket_bind(handle, &local, sizeof(local)) != 0 ||
		posix_socket_getsockname(handle, &local, &length) != 0 || !local.sin_port)
	{
		posix_socket_close(handle);
		return 0;
	}
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = local.sin_port;
	to.sin_addr.s_addr = htonl(0x7F000001UL);
	start = GetTickCount();
	*failure = "send failed";
	if (posix_socket_sendto(handle, probe, sizeof(probe), 0, &to, sizeof(to)) != (int)sizeof(probe))
	{
		posix_socket_close(handle);
		return 0;
	}
	*failure = "nothing came back";
	if (!wait_ready(handle, 0, start + ECHO_WAIT))
	{
		posix_socket_close(handle);
		return 0;
	}
	count = posix_socket_recvfrom(handle, reply, sizeof(reply), 0, &from, &from_length);
	*milliseconds = GetTickCount() - start;
	posix_socket_close(handle);
	*failure = "a different datagram came back";
	return count == (int)sizeof(probe) && !memcmp(reply, probe, sizeof(probe));
}

/* ---------- the list */

static int fetch_once(char *buffer, int test)
{
	struct game_list_response response;
	unsigned long address, received = 0;
	DWORD start = GetTickCount();
	int result;

	address = posix_resolve_ipv4(GAME_LIST_HOST);
	if (test)
	{
		if (address)
			x360_log("net test: DNS %s resolved (%lu ms)", GAME_LIST_HOST, (unsigned long)(GetTickCount() - start));
		else
			x360_log("net test: DNS %s FAILED (%lu ms)", GAME_LIST_HOST, (unsigned long)(GetTickCount() - start));
	}
	if (!address)
		return GAME_LIST_ERROR_RESOLVE;
	start = GetTickCount();
	result = http_request(address, GAME_LIST_PORT, GAME_LIST_HOST, GAME_LIST_PATH, NULL, buffer,
		GAME_LIST_RESPONSE_SIZE, &received, FETCH_TIMEOUT);
	if (result == GAME_LIST_OK)
		result = game_list_parse_response(buffer, received, &response);
	if (result == GAME_LIST_OK && response.status != 200)
		result = GAME_LIST_ERROR_HTTP;
	if (result == GAME_LIST_OK)
		result = game_list_parse(response.body, response.body_size, &fetched);
	if (test)
	{
		if (result == GAME_LIST_OK)
		{
			int index, warthog = 0;

			for (index = 0; index < fetched.count; index++)
				warthog += strstr(fetched.games[index].name, "Warthog") != NULL;
			x360_log("net test: HTTP GET %s: status 200, %lu bytes, %d games (%d named Warthog) (%lu ms)",
				GAME_LIST_PATH, received, fetched.count, warthog, (unsigned long)(GetTickCount() - start));
		}
		else
		{
			x360_log("net test: HTTP GET %s FAILED: %s, %lu bytes received (%lu ms)", GAME_LIST_PATH,
				game_list_error_string(result), received, (unsigned long)(GetTickCount() - start));
		}
	}
	return result;
}

static void self_test(void)
{
	DWORD milliseconds = 0;
	const char *failure;
	char report[256];
	long failed;

	x360_log("net test: start");
	failed = network_wire_self_test(report, (long)sizeof(report));
	x360_log("net test: byte order %s: %s", failed ? "FAILED" : "ok", report);
	if (!x360_network_start())
	{
		x360_log("net test: the network did not start; no DNS, UDP or HTTP test");
		return;
	}
	if (udp_echo(&milliseconds, &failure))
		x360_log("net test: UDP to this console's own port came back (%lu ms)", (unsigned long)milliseconds);
	else
		x360_log("net test: UDP to this console's own port FAILED: %s", failure);
}

static DWORD WINAPI online_thread(LPVOID parameter)
{
	/* (the stack's: 10 KB the thread has no other use for) */
	char buffer[GAME_LIST_RESPONSE_SIZE];
	int attempt, result = GAME_LIST_ERROR_CONNECT, test = !self_tested;

	(void)parameter;
	if (test)
	{
		self_tested = 1;
		self_test();
	}
	if (!x360_network_start())
	{
		list_error = GAME_LIST_ERROR_CONNECT;
		set_state(_list_failed);
		return 0;
	}
	for (attempt = 1; attempt <= ATTEMPTS; attempt++)
	{
		result = fetch_once(buffer, test && attempt == 1);
		if (result == GAME_LIST_OK || result == GAME_LIST_ERROR_FORMAT)
			break;
		x360_log("game list: try %d of %d: %s", attempt, ATTEMPTS, game_list_error_string(result));
		if (attempt < ATTEMPTS)
			Sleep(RETRY_WAIT);
	}
	list_error = result;
	if (result == GAME_LIST_OK)
		x360_log("game list: %d games (format %d)%s", fetched.count, fetched.format,
			fetched.skipped ? ", some lines left out" : "");
	if (test)
		x360_log("net test: done");
	set_state(result == GAME_LIST_OK ? _list_done : _list_failed);
	return 0;
}

/* one request to the list's host (Link Profile's: src/x360_console_link.c),
on the caller's thread: 1 with the answer's status and body (NUL
terminated, cut to reply_size - 1), else 0 */
int x360_online_request(const char *path, const char *body, int *status, char *reply, int reply_size)
{
	static char buffer[2048];
	struct game_list_response response;
	unsigned long address, received = 0, size;

	if (reply_size <= 0 || !x360_network_start())
		return 0;
	address = posix_resolve_ipv4(GAME_LIST_HOST);
	if (!address || http_request(address, GAME_LIST_PORT, GAME_LIST_HOST, path, body, buffer, sizeof(buffer),
		&received, FETCH_TIMEOUT) != GAME_LIST_OK || game_list_parse_response(buffer, received, &response) != GAME_LIST_OK)
	{
		return 0;
	}
	*status = response.status;
	size = response.body_size < (unsigned long)reply_size - 1 ? response.body_size : (unsigned long)reply_size - 1;
	memcpy(reply, response.body, size);
	reply[size] = 0;
	return 1;
}

/* the start-up's self-test and first list (xbox360_platform.c) */
void x360_online_start(void)
{
	x360_game_list_request();
}

void x360_game_list_request(void)
{
	HANDLE thread;

	/* (not while a fetch runs, or before its list is taken) */
	if (list_state == _list_fetching || !list_taken)
		return;
	list_taken = 0;
	shown_state = X360_GAME_LIST_FETCHING;
	set_state(_list_fetching);
	/* (64 KB: the response's buffer and the fetch's frames) */
	thread = CreateThread(NULL, 64 * 1024, online_thread, NULL, 0, NULL);
	if (!thread)
	{
		list_error = GAME_LIST_ERROR_CONNECT;
		set_state(_list_failed);
		return;
	}
	CloseHandle(thread);
}

/* a list that came taken (the main thread's) */
static void take(void)
{
	LONG state = list_state;

	if (list_taken || (state != _list_done && state != _list_failed))
		return;
	X360_LWSYNC();
	list_taken = 1;
	if (state == _list_failed)
	{
		shown_error = list_error;
		shown_state = X360_GAME_LIST_FAILED;
		x360_log("game list: could not get the list from %s (%s)", GAME_LIST_HOST,
			game_list_error_string(list_error));
		return;
	}
	memcpy(&shown, &fetched, sizeof(shown));
	shown_error = GAME_LIST_OK;
	shown_state = X360_GAME_LIST_READY;
	shown_lists++;
}

const struct game_list *x360_game_list_get(int *lists)
{
	take();
	if (lists)
		*lists = shown_lists;
	return shown_lists ? &shown : NULL;
}

int x360_game_list_state(void)
{
	take();
	return shown_state;
}

const char *x360_game_list_error(void)
{
	return game_list_error_string(shown_error);
}
