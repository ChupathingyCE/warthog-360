/*
X360_NET.C

The socket half of port/linux/src/posix.h on the console's Winsock (XNet):
what posix_net.c gives the desktop builds and win32_net.c the Windows
build. The game's Winsock and XNet are the desktop platform layer's
(port/linux/src/xnet.c), which calls these.

The network is brought up as the owner's Butter and Jelly does it on an
RGH console (its src/platform/xenon/platform_xenon.cpp, StartNetwork, and
third_party/xdkcompat/xenon_winsock.c; GPL-2.0-or-later, by the same
author), which talks to LAN and internet servers from the same consoles:

- XNet starts insecure (XNET_STARTUP_BYPASS_SECURITY), restarted on those
  terms if something started it first; then Winsock; then a wait of up to
  8 seconds for the title's address (connecting before it has one hangs
  instead of failing).
- Every socket is marked unencrypted (SO_UNENCRYPTED, 0x5801, undocumented):
  without it the console encrypts what it sends and drops the replies while
  every call reports success. The startup flag alone is not enough.
- socket() always names its protocol (IPPROTO_TCP or IPPROTO_UDP): with 0
  the console hands back a socket that never connects.
- A host is looked up with XNetDnsLookup, polled until it is no longer in
  progress.
- A non-blocking connect that select does not report writable is asked with
  getpeername.

The result is plain UDP and TCP with the PCs, the game list and the
internet, which a secure XNet refuses. Never an address in the log (only
its kind).
*/

#include <xtl.h>
#include <winsockx.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "posix.h"
#include "x360_port.h"

static volatile LONG network_state; /* 0 not started, 1 starting, 2 up, 3 failed */

/* the console's per-socket "send this in the clear" option (Butter and
Jelly's BJ_SO_UNENCRYPTED; not in the SDK's headers) */
#define X360_SO_UNENCRYPTED 0x5801
/* the longest wait for the title's address after Winsock starts */
#define ADDRESS_WAIT_MS 8000
#define ADDRESS_POLL_MS 250
/* the longest wait for a host's lookup */
#define DNS_WAIT_MS 5000

static const char *address_kind(unsigned long address)
{
	if ((address >> 24) == 10 || (address >> 20) == 0xac1 || (address >> 16) == 0xc0a8)
		return "private";
	if ((address >> 16) == 0xa9fe)
		return "link-local";
	return "public";
}

int x360_network_start(void)
{
	XNetStartupParams parameters;
	WSADATA winsock;
	XNADDR address;
	DWORD status = XNET_GET_XNADDR_PENDING;
	DWORD link;
	int waited, result;

	if (network_state == 2)
		return 1;
	if (InterlockedCompareExchange(&network_state, 1, 0) != 0)
	{
		while (network_state == 1)
			Sleep(10);
		return network_state == 2;
	}
	memset(&parameters, 0, sizeof(parameters));
	parameters.cfgSizeOfStruct = sizeof(parameters);
	parameters.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
	result = XNetStartup(&parameters);
	if (result != 0)
	{
		/* (something started XNet first, maybe without the bypass: LAN and
		internet packets would be dropped while every call succeeds; started
		again on these terms) */
		x360_log("network: XNetStartup returned %d; restarting it", result);
		XNetCleanup();
		result = XNetStartup(&parameters);
	}
	x360_log("network: XNetStartup -> %d, security bypassed (parameters %u bytes, the SDK's)", result,
		(unsigned)sizeof(parameters));
	if (result != 0)
	{
		network_state = 3;
		return 0;
	}
	result = WSAStartup(MAKEWORD(2, 2), &winsock);
	if (result != 0)
	{
		x360_log("network: WSAStartup failed (%d)", result);
		XNetCleanup();
		network_state = 3;
		return 0;
	}
	link = XNetGetEthernetLinkStatus();
	x360_log("network: link%s%s%s", (link & XNET_ETHERNET_LINK_ACTIVE) ? " active" : " DOWN",
		(link & XNET_ETHERNET_LINK_WIRELESS) ? " wireless" : "", (link & XNET_ETHERNET_LINK_100MBPS) ? " 100Mbps" : "");
	/* (the title's address: PENDING while the interface comes up) */
	for (waited = 0; waited < ADDRESS_WAIT_MS; waited += ADDRESS_POLL_MS)
	{
		memset(&address, 0, sizeof(address));
		status = XNetGetTitleXnAddr(&address);
		if (status != XNET_GET_XNADDR_PENDING)
			break;
		Sleep(ADDRESS_POLL_MS);
	}
	if (status == XNET_GET_XNADDR_PENDING)
		x360_log("network: no address after %d ms; the interface never came up", ADDRESS_WAIT_MS);
	else if ((status & XNET_GET_XNADDR_NONE) || !address.ina.s_addr)
		x360_log("network: no address after %d ms (status 0x%08lx)", waited, (unsigned long)status);
	else
		x360_log("network: a %s address after %d ms (status 0x%08lx%s%s%s)",
			address_kind((unsigned long)ntohl(address.ina.s_addr)), waited, (unsigned long)status,
			(status & XNET_GET_XNADDR_DHCP) ? " dhcp" : "", (status & XNET_GET_XNADDR_GATEWAY) ? " gateway" : "",
			(status & XNET_GET_XNADDR_DNS) ? " dns" : " no-dns");
	network_state = 2;
	return 1;
}

/* ---------- the console's own address for 127.0.0.1

The game reaches itself at 127.0.0.1 (a host's own client joins its game
there: split screen and hosting). XNet has no loopback, so traffic to
127.0.0.1 goes to the console's own address, and traffic from that address
is reported as from 127.0.0.1, as xnet.c does for network.address on the
desktop. */

#define LOOPBACK_ADDRESS 0x7F000001UL

static unsigned long own_address(void)
{
	XNADDR address;

	memset(&address, 0, sizeof(address));
	if (XNetGetTitleXnAddr(&address) & XNET_GET_XNADDR_NONE)
		return 0;
	return address.ina.s_addr;
}

/* a destination of 127.0.0.1: the console's own address in storage */
static const void *outgoing(const void *address, int address_length, struct sockaddr_in *storage)
{
	const struct sockaddr_in *in = address;
	unsigned long own;

	if (!in || address_length < (int)sizeof(*in) || in->sin_family != AF_INET ||
		in->sin_addr.s_addr != htonl(LOOPBACK_ADDRESS) || !(own = own_address()))
	{
		return address;
	}
	memcpy(storage, in, sizeof(*storage));
	storage->sin_addr.s_addr = own;
	return storage;
}

/* a source of the console's own address: 127.0.0.1 */
static void incoming(void *address, const int *address_length)
{
	struct sockaddr_in *in = address;
	unsigned long own;

	if (in && address_length && *address_length >= (int)sizeof(*in) && in->sin_family == AF_INET &&
		(own = own_address()) != 0 && in->sin_addr.s_addr == own)
	{
		in->sin_addr.s_addr = htonl(LOOPBACK_ADDRESS);
	}
}

int posix_socket_last_error(void)
{
	return WSAGetLastError();
}

int posix_socket(int family, int type, int protocol)
{
	SOCKET handle;

	if (!x360_network_start())
	{
		WSASetLastError(WSAENETDOWN);
		return -1;
	}
	/* (the protocol always named: with 0 the console's socket never
	connects) */
	if (protocol == 0)
	{
		if (type == SOCK_STREAM)
			protocol = IPPROTO_TCP;
		else if (type == SOCK_DGRAM)
			protocol = IPPROTO_UDP;
	}
	handle = socket(family, type, protocol);
	if (handle == INVALID_SOCKET)
		return -1;
	{
		/* (in the clear: XNET_STARTUP_BYPASS_SECURITY alone still encrypts
		what a socket sends) */
		static int told;
		BOOL plain = TRUE;

		if (setsockopt(handle, SOL_SOCKET, X360_SO_UNENCRYPTED, (const char *)&plain, sizeof(plain)) != 0 && !told)
		{
			told = 1;
			x360_log("network: a socket could not be marked unencrypted (%d)", WSAGetLastError());
		}
	}
	if (type == SOCK_DGRAM)
	{
		/* (a datagram socket may broadcast: system link's searches) */
		BOOL on = TRUE;

		setsockopt(handle, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
	}
	return (int)handle;
}

/* (below: the sockets bound for 127.0.0.1) */
static int is_loopback_socket(int handle);
static void loopback_socket_set(int handle, int bound);

int posix_socket_close(int handle)
{
	if (is_loopback_socket(handle))
		loopback_socket_set(handle, 0);
	return closesocket((SOCKET)handle) == 0 ? 0 : -1;
}

/* ---------- sockets bound to 127.0.0.1

XNet binds nothing to 127.0.0.1 alone, and what is sent there goes to the
console's own address (outgoing, above), which a socket bound to 127.0.0.1
would never see. Internet play binds its stand-ins there (port/linux/src/
p2p.c: the host's datagrams for the game come from them, its connections go
to them), so a bind to 127.0.0.1 takes every address instead, and the
socket still says it is bound to 127.0.0.1 (getsockname): xnet.c and p2p.c
see the binding they asked for, and p2p.c takes only the game's own
traffic from them. */

#define MAXIMUM_LOOPBACK_SOCKETS 64

static volatile LONG loopback_sockets[MAXIMUM_LOOPBACK_SOCKETS];

static int is_loopback_socket(int handle)
{
	int index;

	for (index = 0; index < MAXIMUM_LOOPBACK_SOCKETS; index++)
	{
		if (handle && loopback_sockets[index] == (LONG)handle)
			return 1;
	}
	return 0;
}

static void loopback_socket_set(int handle, int bound)
{
	int index;

	for (index = 0; index < MAXIMUM_LOOPBACK_SOCKETS; index++)
	{
		if (bound ? InterlockedCompareExchange(&loopback_sockets[index], (LONG)handle, 0) == 0 :
			InterlockedCompareExchange(&loopback_sockets[index], 0, (LONG)handle) == (LONG)handle)
		{
			return;
		}
	}
	if (bound)
	{
		static int told;

		if (!told)
			x360_log("network: more than %d sockets bound to 127.0.0.1; the rest say every address",
				MAXIMUM_LOOPBACK_SOCKETS);
		told = 1;
	}
}

/* a failed socket call (not one that would block): the first few logged,
then one in ten seconds, with how many since */
static void socket_failed(const char *call, int error)
{
	static LONG logged, since;
	static DWORD logged_time;
	DWORD now = GetTickCount();

	if (error == WSAEWOULDBLOCK)
		return;
	InterlockedIncrement(&since);
	if (logged < 8 || now - logged_time >= 10000)
	{
		x360_log("network: %s failed (WSA error %d%s; %ld failure(s) since the last told)", call, error,
			error == WSAEINPROGRESS ? ", another thread was using the socket" : "", (long)since);
		logged++;
		logged_time = now;
		since = 0;
	}
	WSASetLastError(error);
}

int posix_socket_bind(int handle, const void *address, int address_length)
{
	const struct sockaddr_in *in = address;

	if (in && address_length >= (int)sizeof(*in) && in->sin_family == AF_INET &&
		in->sin_addr.s_addr == htonl(LOOPBACK_ADDRESS))
	{
		static int told;
		struct sockaddr_in any;

		memcpy(&any, in, sizeof(any));
		any.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind((SOCKET)handle, (const struct sockaddr *)&any, sizeof(any)) != 0)
		{
			socket_failed("a bind meant for 127.0.0.1", WSAGetLastError());
			return -1;
		}
		if (!told)
			x360_log("network: XNet binds nothing to 127.0.0.1 alone; internet play's stand-ins take every address");
		told = 1;
		loopback_socket_set(handle, 1);
		return 0;
	}
	if (bind((SOCKET)handle, address, address_length) != 0)
	{
		socket_failed("bind", WSAGetLastError());
		return -1;
	}
	return 0;
}

int posix_socket_connect(int handle, const void *address, int address_length)
{
	struct sockaddr_in storage;

	address = outgoing(address, address_length, &storage);
	return connect((SOCKET)handle, address, address_length) == 0 ? 0 : -1;
}

int posix_socket_listen(int handle, int backlog)
{
	return listen((SOCKET)handle, backlog) == 0 ? 0 : -1;
}

int posix_socket_accept(int handle, void *address, int *address_length)
{
	SOCKET accepted = accept((SOCKET)handle, address, address_length);

	if (accepted == INVALID_SOCKET)
		return -1;
	incoming(address, address_length);
	return (int)accepted;
}

int posix_socket_send(int handle, const void *buffer, int length, int flags)
{
	int result = send((SOCKET)handle, buffer, length, flags);

	return result == SOCKET_ERROR ? -1 : result;
}

int posix_socket_sendto(int handle, const void *buffer, int length, int flags, const void *address,
	int address_length)
{
	struct sockaddr_in storage;
	int result;

	address = outgoing(address, address_length, &storage);
	result = sendto((SOCKET)handle, buffer, length, flags, address, address_length);
	if (result == SOCKET_ERROR)
	{
		/* (XNet refuses a socket another thread is using, its select
		included: what p2p.c's P2P_SEND_FROM_THREAD keeps from happening) */
		socket_failed("sendto", WSAGetLastError());
		return -1;
	}
	return result;
}

int posix_socket_recv(int handle, void *buffer, int length, int flags)
{
	int result = recv((SOCKET)handle, buffer, length, flags);

	return result == SOCKET_ERROR ? -1 : result;
}

int posix_socket_recvfrom(int handle, void *buffer, int length, int flags, void *address, int *address_length)
{
	int result = recvfrom((SOCKET)handle, buffer, length, flags, address, address_length);

	if (result == SOCKET_ERROR)
		return -1;
	incoming(address, address_length);
	return result;
}

int posix_socket_shutdown(int handle, int how)
{
	return shutdown((SOCKET)handle, how) == 0 ? 0 : -1;
}

int posix_socket_set_nonblocking(int handle, int nonblocking)
{
	u_long value = nonblocking ? 1 : 0;

	return ioctlsocket((SOCKET)handle, FIONBIO, &value) == 0 ? 0 : -1;
}

int posix_socket_bytes_available(int handle, posix_ulong *count)
{
	u_long value = 0;

	if (ioctlsocket((SOCKET)handle, FIONREAD, &value) != 0)
		return -1;
	*count = value;
	return 0;
}

int posix_socket_set_nodelay(int handle)
{
	BOOL on = TRUE;

	return setsockopt((SOCKET)handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on)) == 0 ? 0 : -1;
}

int posix_socket_setsockopt(int handle, int level, int name, const void *value, int length)
{
	return setsockopt((SOCKET)handle, level, name, value, length) == 0 ? 0 : -1;
}

int posix_socket_getsockopt(int handle, int level, int name, void *value, int *length)
{
	return getsockopt((SOCKET)handle, level, name, value, length) == 0 ? 0 : -1;
}

int posix_socket_getsockname(int handle, void *address, int *address_length)
{
	if (getsockname((SOCKET)handle, address, address_length) != 0)
		return -1;
	/* (one bound for 127.0.0.1 is bound there, as far as the game knows) */
	if (address && address_length && *address_length >= (int)sizeof(struct sockaddr_in) &&
		((struct sockaddr_in *)address)->sin_family == AF_INET && is_loopback_socket(handle))
	{
		((struct sockaddr_in *)address)->sin_addr.s_addr = htonl(LOOPBACK_ADDRESS);
	}
	return 0;
}

int posix_socket_getpeername(int handle, void *address, int *address_length)
{
	if (getpeername((SOCKET)handle, address, address_length) != 0)
		return -1;
	incoming(address, address_length);
	return 0;
}

static void fill_set(fd_set *set, const int *handles, int count)
{
	int index;

	FD_ZERO(set);
	for (index = 0; index < count && index < FD_SETSIZE; index++)
		FD_SET((SOCKET)handles[index], set);
}

static void keep_ready(fd_set *set, int *handles, int *count)
{
	int index, kept = 0;

	for (index = 0; index < *count; index++)
	{
		if (FD_ISSET((SOCKET)handles[index], set))
			handles[kept++] = handles[index];
	}
	*count = kept;
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count,
	posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	fd_set read_set, write_set, error_set, asked_write;
	struct timeval timeout;
	int result;
	unsigned index;

	fill_set(&read_set, read, read ? *read_count : 0);
	fill_set(&write_set, write, write ? *write_count : 0);
	fill_set(&error_set, error, error ? *error_count : 0);
	timeout.tv_sec = timeout_seconds;
	timeout.tv_usec = timeout_microseconds;
	if (!read_set.fd_count && !write_set.fd_count && !error_set.fd_count)
	{
		/* (Winsock fails a select of nothing: a wait) */
		if (!infinite)
			Sleep((DWORD)(timeout_seconds * 1000 + timeout_microseconds / 1000));
		return 0;
	}
	asked_write = write_set;
	result = select(0, read ? &read_set : NULL, write ? &write_set : NULL, error ? &error_set : NULL,
		infinite ? NULL : &timeout);
	if (result == SOCKET_ERROR)
		return -1;
	/* (a connected socket answers getpeername, one still connecting does
	not: the console's select can leave a connected socket out of the
	writable set) */
	for (index = 0; write && index < asked_write.fd_count; index++)
	{
		SOCKET handle = asked_write.fd_array[index];
		struct sockaddr_in peer;
		int length = sizeof(peer);

		if (FD_ISSET(handle, &write_set) || (error && FD_ISSET(handle, &error_set)))
			continue;
		if (getpeername(handle, (struct sockaddr *)&peer, &length) != 0)
			continue;
		FD_SET(handle, &write_set);
		result++;
	}
	if (read)
		keep_ready(&read_set, read, read_count);
	if (write)
		keep_ready(&write_set, write, write_count);
	if (error)
		keep_ready(&error_set, error, error_count);
	return result;
}

/* ---------- internet play's tunnel, to the game (p2p.c, P2P_TRACE_DATAGRAMS)

What became of the datagrams the host sends the game through the tunnel:
the handover from the tunnel to the game's own sockets, as the original
Xbox and the Wii U log it. Each outcome its first four times, then every 10
seconds; the packets the tunnel opened, by type, every 10 seconds for the
first two minutes, then every minute. Ports only, never an address. */

void p2p_trace_datagram(unsigned short source_port, unsigned short port, int size, int result)
{
	static const char *const outcomes[] =
	{
		"no stand-in for it (one could not be made)",
		"not a port of the game's",
		"not sent to the game",
		"sent to the game",
	};
	static struct
	{
		long count;
		long logged;
		DWORD time;
	} traced[4];
	int outcome = result + 2;
	DWORD now = GetTickCount();

	if (outcome < 0 || outcome > 3)
		return;
	traced[outcome].count++;
	if (traced[outcome].logged < 4 || now - traced[outcome].time >= 10000)
	{
		x360_log("tunnel: %ld datagram(s) from the host's port %u for the game's port %u (%d bytes): %s",
			traced[outcome].count, (unsigned)ntohs(source_port), (unsigned)ntohs(port), size, outcomes[outcome]);
		traced[outcome].count = 0;
		traced[outcome].logged++;
		traced[outcome].time = now;
	}
}

void p2p_trace_packet(int type, int size)
{
	static long counts[8];
	static long bytes, total;
	static DWORD time, first;
	DWORD now = GetTickCount();

	if (!total++)
	{
		first = time = now;
		x360_log("tunnel: the first packet from a peer opened (type %d, %d bytes)", type, size);
	}
	counts[type >= 0 && type < 8 ? type : 7]++;
	bytes += size;
	if (now - time >= (now - first < 120000 ? 10000UL : 60000UL))
	{
		x360_log("tunnel: in the last %lu s, from peers: types 0-7 %ld %ld %ld %ld %ld %ld %ld %ld (%ld bytes)",
			(unsigned long)((now - time) / 1000), counts[0], counts[1], counts[2], counts[3], counts[4],
			counts[5], counts[6], counts[7], bytes);
		memset(counts, 0, sizeof(counts));
		bytes = 0;
		time = now;
	}
}

posix_ulong posix_local_ipv4_address(void)
{
	XNADDR address;

	if (!x360_network_start())
		return 0;
	memset(&address, 0, sizeof(address));
	if (XNetGetTitleXnAddr(&address) & (XNET_GET_XNADDR_NONE))
		return 0;
	return address.ina.s_addr;
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	unsigned long address = inet_addr(host);
	XNDNS *lookup = NULL;
	DWORD deadline;
	posix_ulong result = 0;

	if (address != INADDR_NONE)
		return address;
	if (!x360_network_start())
		return 0;
	/* (no event: XNet answers in the background, polled until it is done) */
	if (XNetDnsLookup(host, NULL, &lookup) != 0 || !lookup)
		return 0;
	deadline = GetTickCount() + DNS_WAIT_MS;
	while (lookup->iStatus == WSAEINPROGRESS && (long)(deadline - GetTickCount()) > 0)
		Sleep(20);
	if (lookup->iStatus == 0 && lookup->cina > 0)
		result = lookup->aina[0].s_addr;
	XNetDnsRelease(lookup);
	return result;
}

/* no UPnP on the console (yet) */
int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	(void)external_address;
	(void)external_port;
	if (error_size > 0)
	{
		_snprintf(error, error_size - 1, "no UPnP on the Xbox 360");
		error[error_size - 1] = '\0';
	}
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}
