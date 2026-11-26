/*
IDENTITY_LEFT_OUT.C

The certificate checks the game's networks list (port/linux/src/
networks.c) asks of Delta's device identity, which the consoles leave out
(HALO_CONSOLE_NO_DELTA_IDENTITY: port/linux/src/identity.c is empty here).
The 360's link keeps every call, so they answer as a build that cannot
check a certificate: none is proven, and no network's server is shown as
one it signed. A root key is not refused for its form alone, so the list
still loads.
*/

#include "halo_port_features.h"
#include "identity.h"

#include <string.h>

#ifdef HALO_CONSOLE_NO_DELTA_IDENTITY

int identity_parse_issuer(const unsigned char *data, int size, struct identity_issuer *issuer)
{
	(void)data;
	(void)size;
	if (issuer)
		memset(issuer, 0, sizeof(*issuer));
	return 0;
}

int identity_check_issuer(const unsigned char *data, int size, const unsigned char (*roots)[IDENTITY_KEY_SIZE],
	int root_count, unsigned long now, struct identity_issuer *issuer)
{
	(void)roots;
	(void)root_count;
	(void)now;
	identity_parse_issuer(data, size, issuer);
	return IDENTITY_MALFORMED;
}

int identity_issuer_signed_by(const unsigned char *data, int size, const unsigned char *root)
{
	(void)data;
	(void)size;
	(void)root;
	return 0;
}

int identity_key_weak(const unsigned char *public_key)
{
	(void)public_key;
	return 0;
}

#endif
