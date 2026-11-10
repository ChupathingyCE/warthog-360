#!/bin/sh
# The Xbox 360 port's checks on the host, under AddressSanitizer and
# UndefinedBehaviorSanitizer where the compiler has them:
#
#   port/xbox360/tests/run.sh    which hosts the console joins (a Delta host
#                                by its wire and range, whatever OpenCE
#                                number it announces), and that the build
#                                and the game take the Delta path, and
#                                that every render target the game draws
#                                into gets EDRAM (edram_test.c)
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
# the console's own units (the list's request among them) get the build's
# players, as the game units do
grep -q '^        players_flag,$' port/xbox360/build.py || {
	echo "$0: build.py does not give the console's units HALO_CONSOLE_PLAYERS" >&2
	exit 1
}
# a build of 32 players (configure.py --console-players 32, the tester
# build) asks the console list for the games of up to 32
path=$(printf '#include "x360_game_list.h"\nGAME_LIST_PATH\n' |
	$CC -E -P -DHALO_CONSOLE_PLAYERS=32 -Iport/xbox360/include - | tail -1)
[ "$path" = '"/v1/console/games?coop=1&packs=1&players=" "32"' ] || {
	echo "$0: a 32-player build asks the list for $path" >&2
	exit 1
}
# the EDRAM allocator (x360_gl.c's struct texture and its EDRAM section,
# with Direct3D stood in for): every target the game draws into gets EDRAM,
# at 1280x720 and with the sharper mirrors' screen-sized secondary target
SRC=port/xbox360/src/x360_gl.c
{
	grep '^#define EDRAM_TILES ' "$SRC"
	awk '/^struct texture$/{on=1} on{print} on&&/^};/{exit}' "$SRC"
	awk '/^\/\* ---------- EDRAM: a framebuffer attachment.s surface \*\//{on=1} on{print}
		on&&/^static BOOL texture_surface\(/{body=1} body&&/^}/{exit}' "$SRC"
} > "$OUT/edram_under_test.inc"
grep -q '^static BOOL texture_surface(' "$OUT/edram_under_test.inc" || {
	echo "$0: texture_surface is not in $SRC's EDRAM section" >&2
	exit 1
}
$CC -std=c99 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter $SAN -I"$OUT" \
	port/xbox360/tests/edram_test.c -o "$OUT/edram_test"
for screen in 1280x720 1280x720-sharp-mirrors; do
	"$OUT/edram_test" $screen
done
# (1920x1080 does not fit: its 3D view and depth alone take 3,264 of the
# 2,048 tiles, so the console draws at 1280x720; shown, not required)
"$OUT/edram_test" 1920x1080 || echo "$0: 1920x1080 does not fit EDRAM (the console draws at 1280x720)"
echo "$0: ok"
