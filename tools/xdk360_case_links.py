#!/usr/bin/env python3
"""The 360 SDK's file names as the code spells them, for a build on Linux.
Linux is case sensitive and the SDK's names aren't consistent (StdDef.h,
XTL.h, sys/Types.h): symlinks beside the originals, which aren't touched, each
name in lower case and each spelling an #include in the SDK or the given
source trees uses, in the two include folders the build reads
(include/xbox and TechPreview/Jul12Compiler/include/xbox). Run once after
extracting the SDK, and again after a submodule update; the build workflow
runs it on the xbox-sdk runner. As warthog-xbox's tools/xdk_case_links.py.

    python3 tools/xdk360_case_links.py [--xdk DIR] port chupathingyce/source chupathingyce/port/console chupathingyce/port/linux/game

DIR is the extracted SDK (lib/xbox/xboxkrnl.lib in it): --xdk, else XDK_DIR,
else ~/xdk360/XDK or ~/xdk360-extract/sdk/XDK as the build looks.
"""
import argparse
import os
import re
import sys
from pathlib import Path

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)


def find_xdk(given):
    candidates = [given, os.environ.get("XDK_DIR"), Path.home() / "xdk360" / "XDK",
                  Path.home() / "xdk360-extract" / "sdk" / "XDK", Path("/opt/xdk360/XDK")]
    for c in candidates:
        if c and (Path(c).expanduser() / "lib" / "xbox" / "xboxkrnl.lib").is_file():
            return Path(c).expanduser()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--xdk", type=Path)
    parser.add_argument("roots", nargs="*", type=Path)
    args = parser.parse_args()
    xdk = find_xdk(args.xdk)
    if not xdk:
        print("no extracted Xbox 360 SDK found (--xdk DIR, or XDK_DIR)", file=sys.stderr)
        return 1
    include_dirs = [xdk / "include" / "xbox", xdk / "TechPreview" / "Jul12Compiler" / "include" / "xbox"]
    include_dirs = [d for d in include_dirs if d.is_dir()]
    made = 0

    def link(folder, name, target):
        nonlocal made
        path = folder / name
        if not path.exists() and not path.is_symlink():
            path.symlink_to(target)
            made += 1

    # every name in lower case, in the include folders and their subfolders
    for top in include_dirs:
        for folder in [top] + [d for d in top.rglob("*") if d.is_dir() and not d.is_symlink()]:
            for f in list(folder.iterdir()):
                if not f.is_symlink() and f.name != f.name.lower():
                    link(folder, f.name.lower(), f.name)

    # every spelling the SDK's own headers and the sources use
    for top in include_dirs:
        real = {f.name.lower(): f.name for f in top.iterdir() if not f.is_symlink()}
        sources = [p for p in top.rglob("*") if p.is_file() and not p.is_symlink()]
        for root in args.roots:
            sources += [p for p in root.rglob("*") if p.suffix.lower() in (".c", ".cpp", ".h", ".hpp", ".inl") and p.is_file()]
        for source in sources:
            try:
                text = source.read_bytes()
            except OSError:
                continue
            for m in INCLUDE.finditer(text):
                name = m.group(1).decode("latin1").replace("\\", "/")
                if "/" in name:
                    continue
                if name.lower() in real and name != real[name.lower()]:
                    link(top, name, real[name.lower()])
    print(f"{made} links")
    return 0


if __name__ == "__main__":
    sys.exit(main())
