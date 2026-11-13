#!/bin/bash
# The shader cache that ships with the Xbox 360 build (port/xbox360/shaders),
# made by running the build through every map in Xenia on a Linux box
# (tools/xenia_smoke_remote.sh, one run a map) and keeping what the
# console compiled: the GL layer's compiled shaders (game:\shaders, one
# file each, x360_shader_cache.c) and the game's own list of the shaders
# it drew with (shader_cache.bin, which the game compiles at start-up).
# With both in the game's folder, a shader the console has seen before is
# a file read at start-up instead of a compile in the middle of a draw.
#
#   tools/xbox360_shader_cache.sh --host linux-box --xenia PATH --maps-root SCRIPT
#
# --host, --xenia, --maps-root as xenia_smoke_remote.sh (or $XENIA_SMOKE_HOST,
#             $XENIA_SMOKE_XENIA, $XENIA_SMOKE_MAPS_ROOT)
# --xex       the XEX here (default build/xbox360/default.xex)
# --out       where the cache goes (default port/xbox360/shaders): the
#             compiled shaders and shader_cache.bin, replaced whole
# --logs      where each map's run logs go (default build/xbox360/shader-cache-runs)
# --maps      the maps to run, space-separated (default: every campaign and
#             multiplayer map); a campaign map runs --campaign-seconds (default
#             240: its opening cutscene moves the camera around), a
#             multiplayer map --mp-seconds (default 60) after it has loaded
# --resume    keep what --out holds and run only the maps not yet in
#             --logs with a passed map phase
#
# The runs are chained: each boot gets the shader_cache.bin the last one
# left, so the game compiles the list so far at start-up and the last
# run's shaders folder holds every shader seen in every map. The cache is
# keyed by the compiled HLSL text and x360_shader_cache.c's CACHE_VERSION;
# a file the build's shaders no longer match is simply never read, so a
# cache made for one build is safe with another, only less complete.
#
# A run is Xenia on a software GPU: a map takes a few minutes. All of them
# take an hour or two. Run it in the background and check --logs.
set -euo pipefail

cd "$(dirname "$0")/.."
host=${XENIA_SMOKE_HOST:-}
xenia=${XENIA_SMOKE_XENIA:-}
maps_root=${XENIA_SMOKE_MAPS_ROOT:-}
xex=build/xbox360/default.xex
out=port/xbox360/shaders
logs=build/xbox360/shader-cache-runs
campaign_seconds=240
mp_seconds=60
resume=0
campaign="a10 a30 a50 b30 b40 c10 c20 c40 d20 d40"
multiplayer="beavercreek bloodgulch boardingaction carousel chillout damnation hangemhigh longest prisoner putput ratrace sidewinder wizard"
maps="$campaign $multiplayer"

while [ $# -gt 0 ]; do
	case "$1" in
		--host) host=$2; shift 2 ;;
		--xenia) xenia=$2; shift 2 ;;
		--maps-root) maps_root=$2; shift 2 ;;
		--xex) xex=$2; shift 2 ;;
		--out) out=$2; shift 2 ;;
		--logs) logs=$2; shift 2 ;;
		--maps) maps=$2; shift 2 ;;
		--campaign-seconds) campaign_seconds=$2; shift 2 ;;
		--mp-seconds) mp_seconds=$2; shift 2 ;;
		--resume) resume=1; shift ;;
		*) echo "$0: unknown argument $1" >&2; exit 2 ;;
	esac
done
if [ -z "$host" ] || [ -z "$xenia" ] || [ -z "$maps_root" ]; then
	echo "usage: $0 --host HOST --xenia PATH --maps-root SCRIPT [--xex FILE] [--out DIR] [--logs DIR] [--maps 'a10 bloodgulch'] [--resume]" >&2
	exit 2
fi
if [ ! -f "$xex" ]; then
	echo "$0: no $xex (ninja xbox360 first)" >&2
	exit 2
fi

mkdir -p "$logs"
if [ "$resume" = 0 ]; then
	rm -rf "$out"
fi
mkdir -p "$out"
list="$logs/shader_cache.bin"
if [ "$resume" = 0 ] || [ ! -f "$list" ]; then
	rm -f "$list"
fi

# the map's path in the cache files, and how long it runs
map_path() {
	case " $campaign " in
		*" $1 "*) echo "levels\\$1\\$1" ;;
		*) echo "levels\\test\\$1\\$1" ;;
	esac
}
map_seconds() {
	case " $campaign " in
		*" $1 "*) echo "$campaign_seconds" ;;
		*) echo "$mp_seconds" ;;
	esac
}

failed=
for map in $maps; do
	run="$logs/$map"
	if [ "$resume" = 1 ] && grep -qs '^map: PASS' "$run/map/result.txt"; then
		echo "$map: done already"
		continue
	fi
	put=()
	if [ -f "$list" ]; then
		put=(--put "$list")
	fi
	echo "$map: running ($(map_seconds "$map") s in the map)"
	status=0
	tools/xenia_smoke_remote.sh --host "$host" --xenia "$xenia" --maps-root "$maps_root" --xex "$xex" \
		--logs "$run" --map "$(map_path "$map")" --seconds "$(map_seconds "$map")" --shots-every 0 \
		"${put[@]+"${put[@]}"}" > "$run.out" 2>&1 || status=$?
	# (the shaders of both phases; a file is named by its key, so the same
	# name is the same shader)
	for phase in menu map; do
		if [ -d "$run/$phase/shaders" ]; then
			cp "$run/$phase/shaders"/*.vsh "$run/$phase/shaders"/*.psh "$out/" 2> /dev/null || true
		fi
		if [ -f "$run/$phase/shader_cache.bin" ]; then
			cp "$run/$phase/shader_cache.bin" "$list"
		fi
	done
	if [ "$status" != 0 ]; then
		echo "$map: FAILED ($(tail -1 "$run/summary.txt" 2> /dev/null || echo "no summary"))"
		failed="$failed $map"
	else
		echo "$map: $(tail -1 "$run/summary.txt")"
	fi
	echo "  cache: $(ls "$out" | grep -c '\.vsh$') vertex, $(ls "$out" | grep -c '\.psh$') pixel shaders"
done
if [ -f "$list" ]; then
	cp "$list" "$out/shader_cache.bin"
fi
echo "shader cache in $out: $(ls "$out" | grep -c '\.vsh$') vertex and $(ls "$out" | grep -c '\.psh$') pixel shaders$(
	[ -f "$out/shader_cache.bin" ] && echo ", shader_cache.bin $(wc -c < "$out/shader_cache.bin") bytes")"
if [ -n "$failed" ]; then
	echo "maps that did not pass:$failed (their shaders so far are in the cache)"
	exit 1
fi
