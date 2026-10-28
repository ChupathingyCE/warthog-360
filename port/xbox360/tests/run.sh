#!/bin/sh
# The Xbox 360 port's checks on the host, under AddressSanitizer and
# UndefinedBehaviorSanitizer where the compiler has them:
#
#   port/xbox360/tests/run.sh    which hosts the console joins (a Delta host
#                                by its wire and range, whatever OpenCE
#                                number it announces), and that the build
#                                and the game take the Delta path
#
# CC picks the compiler (default: cc). The binaries go to build/xbox360-tests.
# Run from this repository, with the game in chupathingyce/ (git submodule
# update --init; GAME names another checkout of it).
set -e
cd "$(dirname "$0")/../../.."
G=${GAME:-chupathingyce}
if [ ! -f "$G/port/linux/src/delta_compat.c" ]; then
	echo "$0: the game is not in $G (git submodule update --init, or GAME=DIR)" >&2
	exit 2
fi
CC=${CC:-cc}
OUT=build/xbox360-tests
mkdir -p "$OUT"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'int main(void) { return 0; }' > "$OUT/probe.c"
$CC $SAN "$OUT/probe.c" -o "$OUT/probe" 2>/dev/null || SAN=
# the console's defines (include/halo_xbox360_prefix.h, build.py)
CONSOLE="-DHALO_XBOX360_CONSOLE -DHALO_CONSOLE -DHALO_BIG_ENDIAN -DHALO_CONSOLE_PLAYERS=16 -DHALO_CONSOLE_DELTA"
# which hosts the console joins (delta_compat.c as the console builds it)
$CC -std=c89 -pedantic -Wall -Wextra -Werror $SAN $CONSOLE \
	-I$G/port/linux/src -idirafter $G/port/linux/include port/xbox360/tests/delta_join_test.c \
	$G/port/linux/src/delta_compat.c -o "$OUT/delta_join_test"
"$OUT/delta_join_test"
# the build defines HALO_CONSOLE_DELTA, and the game's join check takes the
# Delta wire path on a console
grep -q '"-DHALO_CONSOLE_DELTA"' port/xbox360/build.py || {
	echo "$0: build.py does not build Delta Peer's client (HALO_CONSOLE_DELTA)" >&2
	exit 1
}
grep -q '^#if defined(HALO_GAME_BROWSER) || defined(HALO_CONSOLE_DELTA) || defined(HALO_CONSOLE_ONLINE_GAMES)$' \
	$G/source/networking/network_client_manager.c || {
	echo "$0: the game's join check does not take the Delta wire path on the console" >&2
	exit 1
}
echo "$0: ok"
