<p align="center"><img src="docs/icon-160.png" width="120" alt=""></p>

<h1 align="center">Warthog 360</h1>

<p align="center"><b>Chupathingy on the Xbox 360: homebrew for RGH and JTAG consoles, and development kits.</b></p>

<p align="center">
<a href="https://github.com/ChupathingyCE/chupathingyce">The game (Chupathingy)</a> ·
<a href="https://github.com/ChupathingyCE/warthog-xbox">Warthog (original Xbox)</a> ·
<a href="https://stats.chupathingy.org">Games online now</a> ·
<a href="https://discord.gg/chupathingy">Discord</a>
</p>

Warthog 360 is the Halo: Combat Evolved decompilation built for the Xbox
360, on the same game code as
[Chupathingy](https://github.com/ChupathingyCE/chupathingyce) (the Windows,
Mac, Linux and Android builds) and
[Warthog](https://github.com/ChupathingyCE/warthog-xbox) (the original
Xbox). It runs as homebrew on an RGH or JTAG console, or on a development
kit.

This repository holds the Xbox 360 port only. The game is the
`chupathingyce` submodule.

You need your own copy of Halo. No SDK files or game files are included.
There is a test build, shared on the [Discord](https://discord.gg/chupathingy)
(#xbox-360); you can also build it yourself.

## Status

**Test build.** Chupathingy plays on an RGH Xbox 360: Blood Gulch and the
multiplayer maps load, draw and play, and the console lists and joins servers
online through ONLINE GAMES. It is early and has had little time on hardware.

- **Multiplayer:** the focus. The campaign is not a goal yet.
- **Networking:** joins PC and server games. The game's messages go on the
  wire in the PC's byte order, and a start-up self-test checks it.
- **Memory:** sized for the 360's 512 MB, not the original Xbox's 64 MB.
- **No high-res HUD:** the 360 draws the maps' own HUD and fonts. The
  desktop extras stay out until the baseline holds, then come back one at a
  time.
- **How it draws:** the game's Direct3D 8 goes through the desktop
  renderer's OpenGL, which a GL layer of the port's own puts on the
  console's Direct3D 9, with GLSL turned into HLSL on the console.

Known issues: lag; a film-grain-like noise in the picture over time; HUD and
reticle vertices popping.

[port/xbox360/README.md](port/xbox360/README.md) has the details, the known
issues and the developer tools.

## What you need

- **A console that runs your own code.** An RGH or JTAG Xbox 360, started
  from a launcher such as Aurora, or a development kit.
- **Your own Halo maps.** The original Xbox's North American (NTSC)
  release, 01.10.12.2276, as every Chupathingy build and server uses.
  Warthog never provides maps, disc images or other game data, and never
  links to them. Don't share yours.
- **To build it** (below): a Mac or Linux computer, and your own Xbox 360
  SDK.

## Install

With Aurora (or another launcher for RGH and JTAG consoles):

1. Get `default.xex`: the test build from the Discord, or build it yourself
   (see "Building it yourself").
2. Copy the `maps` folder from your own disc to your computer.
3. Lay the game out: `python3 tools/xbox360_package.py --maps /path/to/maps --out ~/Downloads/Warthog-360`.
   This puts `default.xex` and your maps in one folder.
4. Copy that folder to the console, for example to `Hdd1:\Games\Warthog360\`.
5. Add that folder to Aurora's content paths, or start `default.xex` from your launcher's file browser.

The maps are your own NTSC maps (01.10.12.2276) in the `maps` folder beside
`default.xex`; at least `ui.map` is needed.

To report a problem, tell us in #xbox-360 on the Discord and attach the logs.

The game writes its log to `game:\debug.txt`, and the port's own to
`game:\port.txt` and the debug channel. Attach them to bug reports.
Settings are in `game:\config.toml`, written at the first start.
Compiled shaders are kept in `game:\shaders`, so each one is compiled
once, not at every start. Deleting the folder is safe: they are compiled
again.

## Building it yourself

Clone with the game:

```sh
git clone --recursive https://github.com/ChupathingyCE/warthog-360.git
cd warthog-360
```

(In a clone you already have: `git submodule update --init`.)

You need:
- Python 3 and [ninja](https://ninja-build.org/).
- [OXDK360](https://github.com/MrMilenko/OXDK360) in `~/OXDK360` or
  `~/OXDK/xbox360` (`--oxdk360 DIR`), with its `cxex` built, and its patched
  LLVM in `~/llvm-xenon` (`--llvm-xenon DIR`).
- Your own extracted Xbox 360 SDK, the folder holding
  `lib/xbox/xboxkrnl.lib` (`--xdk360 DIR`, or `XDK_DIR`).

Build:

```sh
python3 chupathingyce/configure.py --console-port port/xbox360 [--xdk360 /path/to/XDK]
ninja xbox360                       # build/xbox360/default.xex
```

Without the toolchain or the SDK, `ninja xbox360` says what is missing.
`--console-debug-tools` adds the developer tools (the frame profiler, the
GL layer's timers, the bring-up logs and test switches); a normal build has
none. [docs/building.md](docs/building.md) has every step.

Changes to the game itself go to
[ChupathingyCE/chupathingyce](https://github.com/ChupathingyCE/chupathingyce)
first; this repository then moves its submodule to them. Nothing from
Microsoft's SDKs is in this repository, and none of it may be committed.

## License

Warthog 360 is free software: you can redistribute it and modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License or (at your option) any
later version (`GPL-3.0-or-later`), with the console SDK permission below.
It comes with no warranty. The full text is in [LICENSE](LICENSE).

- **From the GPL change onward.** Warthog follows Chupathingy, which is GPL
  from release 0.8.0. The source at every commit before the change was
  released under [CC0 1.0](LICENSES/CC0-1.0.txt) and stays that way.
- **What Warthog builds on.** The Halo CE decompilation
  ([punpckhdq/halo](https://github.com/punpckhdq/halo) and
  [bnunu/halo-1](https://github.com/bnunu/halo-1)) and OpenCE are released
  under CC0 1.0, which lets anyone build on them under any terms. Their
  repositories stay CC0.
- **The game** (the `chupathingyce` submodule) is under its own
  repository's license, with its third-party code, fonts and art under
  theirs.
- **Halo itself is not covered.** The license covers the work of Warthog's
  and Chupathingy's authors. It gives no rights in Halo, its maps or
  anything else Microsoft owns, and no SDK is included.

### Console SDK permission

You may build Warthog yourself with a console maker's SDK (see "Building it
yourself"), and share what you build:

> **Additional permission under GNU GPL version 3 section 7**
>
> If you modify this Program, or any covered work, by linking or combining
> it with the libraries of a Microsoft Xbox or Xbox 360 software development
> kit (such as the Xbox Development Kit and the Xbox 360 SDK, including
> their `xnet.lib` and Direct3D libraries), or a modified version of those
> libraries, containing parts covered by the terms of those kits' licenses,
> the licensors of this Program grant you additional permission to convey
> the resulting work. Corresponding Source for a non-source form of such a
> combination shall include the source code of the covered work, and need
> not include the source code of those libraries.

The permission only lifts the GPL's own objection. It grants no rights in
Microsoft's libraries, whose own terms still apply, and you may remove it
from your copies (section 7). Warthog's own published builds are made only
with free, legally available tools and libraries; none contains code from
Microsoft's SDKs.

## Support

Chupathingy is free and always will be. Support through
[Ko-fi](https://ko-fi.com/milenko) goes to running the site and the servers. If
there's a specific developer or port you'd like to support, ask them: they may
not be part of our community.

## Credits

- The decompilation: [punpckhdq/halo](https://github.com/punpckhdq/halo) and
  [bnunu/halo-1](https://github.com/bnunu/halo-1), of the Xbox build 2342.
- The game: [Chupathingy](https://github.com/ChupathingyCE/chupathingyce),
  and OpenCE and its contributors, up to October 9, 2026.
- Warthog 360: [Milenko](https://github.com/MrMilenko) and contributors.
  The toolchain: [OXDK360](https://github.com/MrMilenko/OXDK360).
- Libraries: zlib, KCP, Monocypher, Expat, tomlc17 and the other libraries
  the game uses, with their licenses in the game's `port/third_party`.

The port's history up to this repository is in
[ChupathingyCE/warthog-xbox#28](https://github.com/ChupathingyCE/warthog-xbox/pull/28)
(branch `xbox360-next`).

## Links

- The game: [ChupathingyCE/chupathingyce](https://github.com/ChupathingyCE/chupathingyce)
- The original Xbox: [ChupathingyCE/warthog-xbox](https://github.com/ChupathingyCE/warthog-xbox)
- Games online now: [stats.chupathingy.org](https://stats.chupathingy.org)
- Discord: [discord.gg/chupathingy](https://discord.gg/chupathingy)
  (#warthog-support for help, #warthog-dev for development)

Halo is a trademark of Microsoft. Warthog is a fan project, not made or
endorsed by Microsoft, Bungie or 343 Industries, and includes none of the
game's content. The code is released under the GNU GPL, version 3 or later
(see [License](#license)).
