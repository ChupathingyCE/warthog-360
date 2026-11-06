#!/bin/bash
# A smoke test of an Xbox 360 build in Xenia Canary (Linux): boot to the
# main menu, then boot again straight into a map, run it for a while, and
# keep the logs.
#
#   tools/xenia_smoke.sh --xenia ~/emulators/xenia-canary/xenia_canary --xex build/xbox360/default.xex --out smoke
#
# --xenia     xenia_canary from https://github.com/xenia-canary/xenia-canary-releases
#             (the Linux build). Its settings, content and cache go under
#             --out, never to the Xenia you play with.
# --xex       the build's default.xex
# --out       where the logs go (one folder a phase), and the scratch data
# --maps-root the script that makes a data root of links to your own maps
#             (default ~/halo-maps/new-data-root.sh); the maps are linked,
#             never copied, and never written
# --map       the map the second boot loads (default
#             levels\test\bloodgulch\bloodgulch)
# --seconds   how long the map runs once it is loaded (default 60)
# --timeout   the longest a phase may take to reach its mark (default 1800)
# --display   an X display to run on; without one, the test starts its own
#             Xvfb (Xenia needs a window, and Vulkan: a GPU's, or Mesa's
#             llvmpipe on the CPU, which is slow but enough)
#
# The first boot passes when the game starts the main menu's music; the
# second when the map has loaded and the game has run it for --seconds
# without a halt. Either fails on a halt (the game's EXCEPTION halt, or
# the port's "x360: halted"), on Xenia
# leaving, or on the time running out.
#
# The second boot loads its map with the game's own d:\init.txt in the
# test's game folder. The XEX runs as it was built: ninja xbox360 gives it
# the DOS header Microsoft's linker writes (OXDK's cxex), which Xenia
# checks, and the test stops early on one without it.
# Xenia writes in the game's folder only with --allow_game_relative_writes,
# which the test gives it (the game keeps its settings and logs there).
#
# A pass here is a pass in Xenia, not on an Xbox 360.
set -euo pipefail

xenia=
xex=
out=
maps_root="$HOME/halo-maps/new-data-root.sh"
map='levels\test\bloodgulch\bloodgulch'
seconds=60
timeout=1800
display=${DISPLAY:-}

while [ $# -gt 0 ]; do
	case "$1" in
		--xenia) xenia=$2; shift 2 ;;
		--xex) xex=$2; shift 2 ;;
		--out) out=$2; shift 2 ;;
		--maps-root) maps_root=$2; shift 2 ;;
		--map) map=$2; shift 2 ;;
		--seconds) seconds=$2; shift 2 ;;
		--timeout) timeout=$2; shift 2 ;;
		--display) display=$2; shift 2 ;;
		*) echo "$0: unknown argument $1" >&2; exit 2 ;;
	esac
done
if [ -z "$xenia" ] || [ -z "$xex" ] || [ -z "$out" ]; then
	echo "usage: $0 --xenia PATH --xex FILE --out DIR [--maps-root SCRIPT] [--map NAME] [--seconds N] [--timeout N] [--display :N]" >&2
	exit 2
fi
if [ ! -x "$xenia" ]; then
	echo "$0: no Xenia at $xenia" >&2
	exit 2
fi
if [ ! -f "$xex" ]; then
	echo "$0: no $xex" >&2
	exit 2
fi
if [ ! -x "$maps_root" ]; then
	echo "$0: no data root script at $maps_root" >&2
	exit 2
fi
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
if [ -e "$out/game" ] && [ ! -f "$out/.xenia-smoke" ]; then
	echo "$0: $out has a game folder that is not this test's (no .xenia-smoke)" >&2
	exit 2
fi
touch "$out/.xenia-smoke"

xvfb_pid=
xenia_pid=
# (Xenia keeps its guest memory and code in /dev/shm, and leaves them there
# when it is killed: hundreds of MB a run, until /dev/shm is full and the
# next Xenia stops while it starts. The ones that were not there before the
# test are removed.)
shm_before=$(ls /dev/shm 2> /dev/null | grep '^xenia_' || true)
stop_xenia() {
	local file
	if [ -n "$xenia_pid" ] && kill -0 "$xenia_pid" 2> /dev/null; then
		kill "$xenia_pid" 2> /dev/null || true
		for _ in 1 2 3 4 5 6 7 8 9 10; do
			kill -0 "$xenia_pid" 2> /dev/null || break
			sleep 1
		done
		kill -9 "$xenia_pid" 2> /dev/null || true
		wait "$xenia_pid" 2> /dev/null || true
	fi
	xenia_pid=
	for file in /dev/shm/xenia_*; do
		if [ -f "$file" ] && ! printf '%s\n' "$shm_before" | grep -q -x -F -- "${file##*/}"; then
			rm -f "$file"
		fi
	done
}
finish() {
	stop_xenia
	if [ -n "$xvfb_pid" ]; then
		kill "$xvfb_pid" 2> /dev/null || true
	fi
}
trap finish EXIT
if [ -z "$display" ]; then
	command -v Xvfb > /dev/null || { echo "$0: no DISPLAY and no Xvfb" >&2; exit 2; }
	number=190
	while [ -e "/tmp/.X$number-lock" ]; do number=$((number + 1)); done
	display=":$number"
	Xvfb "$display" -screen 0 1280x720x24 -nolisten tcp > /dev/null 2>&1 &
	xvfb_pid=$!
	sleep 2
fi

# Xenia's Linux build opens a file asked for with GENERIC_READ |
# GENERIC_WRITE as O_WRONLY (FileHandle::OpenExisting in
# base/filesystem_posix.cc, at least to 02d2cb5), so every read of a file
# the game opened to read and write fails, and Xenia reports it as the end
# of the file. The game's cache files are opened that way, as on the Xbox,
# so no map loaded ("the map 'ui' is in no cache file"). A small library the
# test preloads into Xenia opens such files under the test's folder for
# reading too, as Windows would. It needs a C compiler on the machine; the
# test says so when there is none.
shim="$out/shim/xenia_rdwr.so"
mkdir -p "$out/shim"
cat > "$out/shim/xenia_rdwr.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* open(): a file under XENIA_RDWR_ROOT opened write-only is opened for
reading too */
static int adjust(const char *path, int flags)
{
	const char *root = getenv("XENIA_RDWR_ROOT");

	if (root && path && (flags & O_ACCMODE) == O_WRONLY && !strncmp(path, root, strlen(root)))
		flags = (flags & ~O_ACCMODE) | O_RDWR;
	return flags;
}

int open(const char *path, int flags, ...)
{
	static int (*real_open)(const char *, int, ...);
	mode_t mode = 0;

	if (flags & (O_CREAT | O_TMPFILE))
	{
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
	}
	if (!real_open)
		real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
	return real_open(path, adjust(path, flags), mode);
}

int open64(const char *path, int flags, ...)
{
	static int (*real_open64)(const char *, int, ...);
	mode_t mode = 0;

	if (flags & (O_CREAT | O_TMPFILE))
	{
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
	}
	if (!real_open64)
		real_open64 = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open64");
	return real_open64(path, adjust(path, flags), mode);
}
EOF
if ! cc -shared -fPIC -O2 -o "$shim" "$out/shim/xenia_rdwr.c" -ldl 2> "$out/shim/cc.log"; then
	echo "$0: cannot build the preload library (a C compiler is needed: $out/shim/cc.log); no map will load" >&2
	shim=
fi

# the test's game folder: a copy of the build, and the maps as links, each
# file on its own
game="$out/game"
rm -rf "$game" "$out/xenia"
mkdir -p "$game/maps" "$out/xenia/storage" "$out/xenia/content" "$out/xenia/cache"
python3 -I - "$xex" <<'EOF'
import struct, sys
data = open(sys.argv[1], "rb").read()
if data[:4] != b"XEX2":
    sys.exit("not an XEX2 file")
image = struct.unpack(">I", data[8:12])[0]
if data[image:image + 4] != b"MZ\x90\x00":
    sys.exit("its DOS header is not the one Xenia loads (built with an OXDK older than its DOS stub?)")
EOF
cp "$xex" "$game/default.xex"
data="$out/data"
"$maps_root" "$data" > /dev/null
linked=0
for file in "$data"/maps/*.map; do
	if [ -f "$file" ]; then
		ln -s "$(cd "$(dirname "$file")" && pwd -P)/$(basename "$file")" "$game/maps/$(basename "$file")"
		linked=$((linked + 1))
	fi
done
if [ ! -f "$game/maps/ui.map" ]; then
	echo "$0: no ui.map in $data/maps" >&2
	exit 2
fi
echo "maps: $linked linked from $data/maps"

# the mark in the logs, after the line that must come first when there is one
marked_in() {
	local after=$1 mark=$2
	shift 2
	if [ -z "$after" ]; then
		grep -q -h -F -- "$mark" "$@"
	else
		awk -v after="$after" -v mark="$mark" \
			'index($0, after) { seen = 1 } seen && index($0, mark) { found = 1 } END { exit !found }' "$@"
	fi
}

# a phase: boot, wait for its mark in the game's log, then hold, and keep the
# logs (name, init.txt's text or empty, the line that must come first or
# empty, the mark, the seconds to hold after it)
phase() {
	local name=$1 init=$2 after=$3 mark=$4 hold=$5
	local logs="$out/$name" started now result=fail reason="" marked=0
	# (the game's lines go to game:\logs\debug.txt, the port's to
	# game:\port.txt and game:\debug.txt: the marks are looked for in all)
	local logs_found
	mkdir -p "$logs"
	rm -rf "$game/logs" "$game/z"
	rm -f "$game/port.txt" "$game/debug.txt" "$game/init.txt"
	if [ -n "$init" ]; then
		printf '%s\r\n' "$init" > "$game/init.txt"
	fi
	echo "$name: starting Xenia"
	DISPLAY=$display XENIA_RDWR_ROOT="$out/" LD_PRELOAD="$shim" "$xenia" --gpu=vulkan --apu=nop --allow_game_relative_writes=true \
		--storage_root="$out/xenia/storage" --content_root="$out/xenia/content" --cache_root="$out/xenia/cache" \
		--log_file="$logs/xenia.log" "$game/default.xex" > "$logs/xenia.out" 2>&1 &
	xenia_pid=$!
	started=$(date +%s)
	while :; do
		sleep 5
		now=$(date +%s)
		if ! kill -0 "$xenia_pid" 2> /dev/null; then
			reason="Xenia left"
			break
		fi
		logs_found=$(ls "$game/logs/debug.txt" "$game/port.txt" "$game/debug.txt" 2> /dev/null || true)
		if [ -n "$logs_found" ] && grep -q -h -e "EXCEPTION halt" -e "x360: halted" $logs_found; then
			reason="the game halted: $(grep -h -m1 -e "EXCEPTION halt" -e "x360: halted" $logs_found | head -1 | cut -c1-300)"
			break
		fi
		if [ "$marked" = 0 ]; then
			if [ -n "$logs_found" ] && marked_in "$after" "$mark" $logs_found; then
				marked=$now
				echo "$name: reached \"$mark\" after $((now - started)) s; running $hold s more"
			elif [ $((now - started)) -ge "$timeout" ]; then
				reason="no \"$mark\" in $timeout s"
				break
			fi
		elif [ $((now - marked)) -ge "$hold" ]; then
			result=pass
			break
		fi
	done
	stop_xenia
	cp "$game/logs/debug.txt" "$logs/" 2> /dev/null || true
	cp "$game/port.txt" "$game/config.toml" "$logs/" 2> /dev/null || true
	cp "$game/debug.txt" "$logs/port-debug.txt" 2> /dev/null || true
	if [ "$result" = pass ]; then
		echo "$name: PASS ($(( $(date +%s) - started )) s)" | tee "$logs/result.txt"
	else
		echo "$name: FAIL: $reason" | tee "$logs/result.txt"
	fi
	[ "$result" = pass ]
}

status=0
phase menu "" "" "starting main menu music" 10 || status=1
# (the map: the game's init.txt; it is in once its scripts are read, which
# follows its tags and its structure bsp)
phase map "map_name $map" "starting precaching of map '${map##*\\}'" "scenario scripts:" "$seconds" || status=1
rm -rf "$data" "$game" "$out/shim"
if [ "$status" = 0 ]; then
	echo "xenia smoke: PASS (Xenia only; not an Xbox 360)"
else
	echo "xenia smoke: FAIL (logs in $out)"
fi
exit $status
