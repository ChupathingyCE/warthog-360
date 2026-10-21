# Building Warthog 360

Warthog 360 builds on macOS or Linux with OXDK360's patched clang and your
own Xbox 360 SDK. Windows has no build path yet.

## 1. The repository and the game

```sh
git clone --recursive https://github.com/ChupathingyCE/warthog-360.git
cd warthog-360
```

The game is the `chupathingyce` submodule. In a clone you already have,
`git submodule update --init` fetches it; after a pull that moved it, run
it again.

## 2. The toolchain

- Python 3 and [ninja](https://ninja-build.org/) (`brew install ninja`).
- [OXDK360](https://github.com/MrMilenko/OXDK360), in `~/OXDK360` or
  `~/OXDK/xbox360` (or `--oxdk360 DIR`, `OXDK360_DIR`), with
  `tools/cxex` built as its README says.
- OXDK360's patched LLVM (the Xenon calling convention), built in
  `~/llvm-xenon` (or `--llvm-xenon DIR`, `LLVM_XENON_DIR`).
- LLVM's `llvm-objcopy` (`brew install llvm` on macOS), if the patched
  LLVM has none.

## 3. Your own Xbox 360 SDK

Extract it so that `lib/xbox/xboxkrnl.lib` is under one folder, and give
that folder with `--xdk360 DIR` (or `XDK_DIR`). The build also looks in
`~/xdk360/XDK`, `~/xdk360-extract/sdk/XDK` and `/opt/xdk360/XDK`. Nothing
from the SDK is in this repository, and none of it may be committed.

## 4. Configure and build

```sh
python3 chupathingyce/configure.py --console-port port/xbox360 [--xdk360 DIR]
ninja xbox360
```

The output is `build/xbox360/default.xex`. `ninja` reruns `configure.py`
with the same options when the build's files change. Without the
toolchain or the SDK, `ninja xbox360` stops and says what is missing.

| Option | What it does |
| --- | --- |
| `--console-debug-tools` | the developer tools: the probe triangle, the frame profiler, the GL layer's timers, the allocation counting, the bring-up logs and the test switches of `game:\gl.txt` |
| `--console-players {16,24,32,64}` | the players the network tables hold; 16, the Xbox's, by default |
| `--console-no-debug-asserts` | the debug decomp's asserts compiled out, as retail compiled them out |
| `--release` | a failed assertion is logged and the game goes on |

`ninja xbox360-game-objects` and `ninja xbox360-platform-objects` compile
one half each, without linking.

## 5. Package

```sh
python3 tools/xbox360_package.py --maps /path/to/maps --out ~/Downloads/Warthog-360
```

puts `default.xex` and your maps in one folder for the console. See
[port/xbox360/README.md](../port/xbox360/README.md) for running it and its
logs.

## Updating the game

A change to the shared code goes to
[ChupathingyCE/chupathingyce](https://github.com/ChupathingyCE/chupathingyce)
first. Once it is merged, move the submodule to it here:

```sh
git -C chupathingyce fetch origin
git -C chupathingyce checkout origin/main
git add chupathingyce
```

and build the 360 before opening the pull request.
