#!/bin/bash
# The Xenia smoke test (tools/xenia_smoke.sh) on a Linux machine you reach
# over ssh, from the computer that built the XEX (a Mac, say):
#
#   tools/xenia_smoke_remote.sh --host linux-box
#
# It copies default.xex and xenia_smoke.sh to a new folder on the host,
# runs the test there, brings the logs back, and removes the folder. The
# host needs Xenia Canary (the Linux build), Xvfb and Vulkan (Mesa's
# llvmpipe will do), python3, and a data root script that links your maps
# (the maps are linked there, never copied, and never sent).
#
# --host      the ssh host (default $XENIA_SMOKE_HOST)
# --xenia     xenia_canary on the host (default $XENIA_SMOKE_XENIA, else
#             ~/emulators/xenia-canary/xenia_canary)
# --maps-root the data root script on the host (default
#             $XENIA_SMOKE_MAPS_ROOT, else ~/halo-maps/new-data-root.sh)
# --xex       the XEX here (default build/xbox360/default.xex)
# --logs      where the logs come back to here (default
#             build/xbox360/xenia-smoke; replaced on each run)
# --keep      leave the folder on the host
# Anything else goes to xenia_smoke.sh (--map, --seconds, --timeout).
#
# Paths on the host may start with ~/, which the host expands.
set -euo pipefail

cd "$(dirname "$0")/.."
host=${XENIA_SMOKE_HOST:-}
xenia=${XENIA_SMOKE_XENIA:-'~/emulators/xenia-canary/xenia_canary'}
maps_root=${XENIA_SMOKE_MAPS_ROOT:-'~/halo-maps/new-data-root.sh'}
xex=build/xbox360/default.xex
logs=build/xbox360/xenia-smoke
keep=0
pass=()

while [ $# -gt 0 ]; do
	case "$1" in
		--host) host=$2; shift 2 ;;
		--xenia) xenia=$2; shift 2 ;;
		--maps-root) maps_root=$2; shift 2 ;;
		--xex) xex=$2; shift 2 ;;
		--logs) logs=$2; shift 2 ;;
		--keep) keep=1; shift ;;
		--map|--seconds|--timeout) pass+=("$1" "$2"); shift 2 ;;
		*) echo "$0: unknown argument $1" >&2; exit 2 ;;
	esac
done
if [ -z "$host" ]; then
	echo "usage: $0 --host HOST [--xenia PATH] [--maps-root SCRIPT] [--xex FILE] [--logs DIR] [--keep] [--map NAME] [--seconds N] [--timeout N]" >&2
	exit 2
fi
if [ ! -f "$xex" ]; then
	echo "$0: no $xex (ninja xbox360 first)" >&2
	exit 2
fi

# a path for the host's shell: ~/ left for it to expand, the rest quoted
remote_path() {
	case "$1" in
		'~/'*) printf '"$HOME"/%q' "${1#\~/}" ;;
		*) printf '%q' "$1" ;;
	esac
}

ssh_host() {
	ssh -o BatchMode=yes "$host" "$@"
}

work=$(ssh_host 'mktemp -d "$HOME/xenia-smoke.XXXXXX"')
case "$work" in
	*/xenia-smoke.*) ;;
	*) echo "$0: no folder made on $host" >&2; exit 1 ;;
esac
cleanup() {
	if [ "$keep" = 0 ]; then
		ssh_host "rm -rf $(printf '%q' "$work")" || true
	else
		echo "left on $host: $work"
	fi
}
trap cleanup EXIT

echo "copying the build to $host"
scp -q -o BatchMode=yes "$xex" tools/xenia_smoke.sh "$host:$work/"

args="--xenia $(remote_path "$xenia") --maps-root $(remote_path "$maps_root")"
args+=" --xex $(printf '%q' "$work/default.xex") --out $(printf '%q' "$work/out")"
for a in "${pass[@]+"${pass[@]}"}"; do
	args+=" $(printf '%q' "$a")"
done
status=0
ssh_host "bash $(printf '%q' "$work/xenia_smoke.sh") $args" || status=$?

# the logs: each phase's folder (the test's game and data folders are gone
# by now, and Xenia's own cache stays behind)
rm -rf "$logs"
mkdir -p "$logs"
ssh_host "cd $(printf '%q' "$work/out") && tar -cf - --exclude=game --exclude=data --exclude=xenia ." |
	tar -xf - -C "$logs" || echo "$0: could not bring the logs back" >&2
echo "logs: $logs"
exit $status
