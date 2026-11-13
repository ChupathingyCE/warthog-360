#!/usr/bin/env python3
"""Lay out the Xbox 360 build for console or emulator.

    python3 tools/xbox360_package.py --maps assets/maps --out ~/Downloads/Warthog-360

writes OUT/default.xex (build/xbox360/default.xex, from `ninja xbox360`),
OUT/maps with the maps folder's *.map and loading.tga, and the shader cache
that ships with the build (port/xbox360/shaders, made by
tools/xbox360_shader_cache.sh): OUT/shaders with the compiled shaders and
OUT/shader_cache.bin, the game's list of them. Copy OUT to the Xbox 360 hard
drive (e.g. Hdd1:\\Games\\Warthog\\) or launch directly in Xenia.
"""

import argparse
import os
import shutil
import sys
from pathlib import Path


def place(source: Path, destination: Path) -> None:
    if destination.exists():
        destination.unlink()
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xex", type=Path, default=Path("build/xbox360/default.xex"))
    parser.add_argument("--maps", type=Path, default=Path("assets/maps"), help="the disc's maps folder")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--no-maps", action="store_true",
                        help="no maps folder: the game only (the builds CI publishes)")
    parser.add_argument("--shaders", type=Path, default=Path("port/xbox360/shaders"),
                        help="the shader cache that ships with the build (none if the folder is missing)")
    args = parser.parse_args()

    maps = args.maps.expanduser()
    out = args.out.expanduser()
    if not args.xex.is_file():
        print(f"no {args.xex}: run ninja xbox360 first", file=sys.stderr)
        return 1
    shaders = sorted(p for p in args.shaders.glob("*") if p.suffix in (".vsh", ".psh") and p.is_file())
    shader_list = args.shaders / "shader_cache.bin"
    if shaders:
        (out / "shaders").mkdir(parents=True, exist_ok=True)
    files = []
    if not args.no_maps:
        files = sorted(maps.glob("*.map")) + [p for p in [maps / "loading.tga"] if p.is_file()]
        if not any(p.name == "ui.map" for p in files):
            print(f"no ui.map in {maps}", file=sys.stderr)
            return 1
        (out / "maps").mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.xex, out / "default.xex")
    for path in files:
        place(path, out / "maps" / path.name)
    for path in shaders:
        place(path, out / "shaders" / path.name)
    if shader_list.is_file():
        place(shader_list, out / "shader_cache.bin")
    print(f"{out}: default.xex, {'no maps' if args.no_maps else f'{len(files)} map files'}, "
          f"{len(shaders)} cached shaders{' and shader_cache.bin' if shader_list.is_file() else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
