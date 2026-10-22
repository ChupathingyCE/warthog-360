# Xbox 360

`ninja xbox360` builds the game for the Xbox 360: `build/xbox360/default.xex`. It is the game's Windows build's arrangement (the game's port/windows/README.md) on the console's PowerPC: the game's sources and the desktop platform layer (the game's `port/linux/src`, in the `chupathingyce` submodule), with the console's own units in `src`. `build.py` is the build, which the game's `configure.py` loads (`--console-port port/xbox360`).

## Status

Blood Gulch loads, draws and plays (driven in a Ghost on hardware, still a little laggy). Networking is new and untested on hardware (below). The multiplayer baseline (a game started and played on the console) is in progress. The campaign is not a goal yet. There is no high-res HUD on the 360: it is built without the high-res HUD's pictures, and the maps' own HUD is drawn. The build is a console build (`HALO_CONSOLE`): the desktop extras stay out until the baseline holds, then come back one at a time. It is sized for the 360's 512 MB, not the original Xbox's 64 MB.

Known issues, under investigation:

- A progressive, film-grain-like noise in the picture over time.
- HUD and reticle vertices popping.
- Lag.

How it is put together:

- The game and `port/linux/src` see the original Xbox's SDK declarations (`port/include/xdk`), as on every other port. `src/x360_*.c` see the console's own SDK. Functions both define are renamed for the game's side (`include/halo_xbox360_api_names.h`).
- Pointers are 32 bits, but the Xbox's addresses are the console's executable's, so the game runs with the 64-bit builds' pointer mode over a relocatable window (`HALO_64BIT` and `HALO_WINDOW_TRANSLATED`, the game's `source/cseries/xbox_address.h`): Xbox addresses 0x78000000 to 0x88000000 (the heap, then 128 MB of contiguous memory) are one 256 MB block wherever the console has it, and an Xbox address is one add away from its pointer (`src/x360_memory.c`).
- `src/x360_posix.c`, `x360_files.c`, `x360_net.c` and `x360_system.c` give the platform layer the POSIX, file, socket and SDL calls it makes, on the console's kernel, XNet and C runtime. Paths are made the console's: backslashes, below `game:\`.
- Thread-local variables are emulated (`-femulated-tls`, `src/x360_tls.c`): the console's TLS is Microsoft's compiler's.
- `src/xbox360_platform.c` and `src/xbox360_input.c` stand in for `sdl_platform.c` and `xinput_sdl.c`, as the dedicated server's units do; the controllers are the console's (`src/x360_input_pad.c`).
- The shared code's hooks are the game's console capabilities, turned on from `HALO_XBOX360_CONSOLE` (the game's `port/linux/include/halo_console_features.h`): the debug channel (`console_debug_print`), halts back to the launcher (`console_exit_to_launcher`), the GL layer (`console_gl_buffer_alias`), the Xbox's texture cache and draw limits, no write watch, and the bring-up logs with the developer tools. The maps are byte-swapped as they load by the game's `port/console/game` (`HALO_BIG_ENDIAN`).

## Requirements

- [OXDK360](https://github.com/MrMilenko/OXDK360) in `~/OXDK360` or `~/OXDK/xbox360` (`--oxdk360 DIR`), with `tools/cxex` built.
- Its patched clang (the Xenon calling convention) in `~/llvm-xenon` (`--llvm-xenon DIR`).
- Your own extracted Xbox 360 SDK, the folder holding `lib/xbox/xboxkrnl.lib` (`--xdk360 DIR` or `XDK_DIR`). Without it there is no target, and `ninja xbox360` says what is missing. Nothing from the SDK is in this repository, and none of it may be committed.

## Build

From the repository's root:

```sh
python3 chupathingyce/configure.py --console-port port/xbox360 --xdk360 ~/xdk360-extract/sdk/XDK
ninja xbox360
```

`--console-players` (16 by default) and `--console-no-debug-asserts` are the game's options for every console (the game's docs/building.md, "Console ports").

For the developer tools (below), configure with `--console-debug-tools`.

Every build stamps its first log line with the version, commit and build time, so a test can tell which build ran (a failed upload leaves the old XEX in place).

## Run

Put `default.xex` in a folder of its own on the console (`Hdd1:\Games\Warthog360\`), with the Xbox disc's `maps` folder beside it (the NTSC maps, 01.10.12.2276: at least `ui.map`). The log is on the debug channel (XBDM) and in `game:\port.txt` (the console's layer) and `game:\debug.txt` (the game's). Settings are `game:\config.toml`, written at the first start.

## Developer tools

A normal build has none of the following. Configured with `--console-debug-tools` (`HALO_CONSOLE_DEBUG_TOOLS`), the build adds them; the code is kept, off by default:

- Harness files read from the build's folder: `game:\gl.txt` (one switch per word: `fake_pad`, `log_draws`, `no_draws`, `clear_red`, `index_le`, `log_frame=N`, `dump_at=N` and others, listed in `src/x360_gl.c`) which a normal build ignores. (`d:\camera.txt` is the game's own saved camera, read only when a script calls `director_load_camera`, as on the Xbox.) `fake_pad` keeps controller 1 connected at rest for automated runs; a real controller is detected without it. `init.txt` is the game's own start-up file and is read in every build.
- The probe triangle at start-up, the main loop's pulse and the frame profiler in the log.
- The allocation counting (every `malloc` and `free` counted by call site, and the GPU layer's physical memory by kind), with a report at the first refusal. A normal build uses the C runtime's allocator directly, with no cost per call.

## Tag swap tables

The tables that byte-swap the tags (the game's `port/console/game/tag_byte_swap_tables.c`) are generated from the game's structures as this build compiles them. After a change to a tag structure, regenerate them from a configured build and send the result to the game:

```sh
python3 chupathingyce/tools/tag_swap_generate.py --build . --maps /path/to/maps
sh chupathingyce/port/console/tests/run_tag_byte_swap_test.sh /path/to/maps/bloodgulch.map
```

## Networking

Untested on hardware. The network comes up as the owner's Butter and Jelly (a Jellyfin client that runs networking and verified HTTPS on an RGH 360) brings it up, in `src/x360_net.c`:

- XNet starts with `XNET_STARTUP_BYPASS_SECURITY`, restarted on those terms if it was already up, then Winsock, then a wait of up to 8 seconds for the console's address.
- Every socket gets `setsockopt` `SO_UNENCRYPTED` (0x5801): without it the console encrypts what it sends and drops replies while reporting success. The startup flag alone is not enough.
- `socket()` always names `IPPROTO_TCP` or `IPPROTO_UDP`: protocol 0 gives a handle that never connects.
- DNS is `XNetDnsLookup`, polled until it is done; a connecting socket that `select` leaves out of the writable set is asked with `getpeername`.
- `XNetStartupParams` and `XNADDR` come from the console's own SDK headers, so their sizes are the linked `xnet.lib`'s. The game's own XNet calls never reach the SDK: they are the platform layer's (the game's `port/linux/src/xnet.c`, renamed in `include/halo_xbox360_api_names.h`).

At start-up a self-test runs on a thread of its own (`src/x360_online.c`) and logs `net test:` lines to `port.txt` and the debug channel: XNet and the link, a UDP datagram to the console's own port read back, `warthog.milenko.org` looked up, and `GET /v1/console/games?packs=1` over plain HTTP. That list is the Multiplayer menu's ONLINE GAMES (`game/x360_browser_screen.c`, `HALO_CONSOLE_ONLINE_GAMES`): the original Xbox's screen and list parser (ChupathingyCE/warthog-xbox), joining a game through its invite and internet play's tunnel, as on the Xbox. X refreshes, B goes back.

Byte order: the network is big-endian, which the 360 already is. The game's transport addresses and ports, message headers and encoded messages, and the platform layer's `htonl`/`htons`, `inet_addr` and internet play's helpers swap only on little-endian machines (the game's `NETWORK_SWAP2`/`NETWORK_SWAP4`, `HALO_BIG_ENDIAN`). Still to do before a whole game with the PCs: the port's distributed netcode (the game's `port/linux/game/network_*.c`) sends its messages as the machine's own structures, so a 360 reads a PC host's game-time, positions and counts in the wrong byte order. Those messages need a wire layer of their own.

Logs never show an address, only its kind (private, public).

## Notes for the port

From the owner's earlier OXDK360 ports, and what phase 0 is built around:

- Paths: backslashes only, and device names longer than a letter (`game:`). A forward slash fails silently on writes, and `path[1] == ':'` checks miss the device.
- `game:` is writable when launched from Aurora; `Hdd:` and `Hdd1:` may not be mounted.
- Exit with `XLaunchNewImage("", 0)`. Tearing Direct3D or sound down mid-present can hang the console.
- OXDK360's clang returns 64-bit values in a register pair where the SDK uses one register: an SDK call returning a 64-bit value (file positions, `_time64`) comes back wrong. Build with `-D_USE_32BIT_TIME_T=1`.
- Variadic doubles format as garbage: `%f` through the SDK's `printf` family is broken. The game's debug and HUD text use it widely.
- `_byteswap_ushort`, `_ulong` and `_uint64` are declared by the SDK and defined nowhere: the port provides them as functions (not macros).
- No C++ exceptions, no VMX128 (XNA math is the scalar fallback), no `vsnprintf` (`_vsnprintf`).
- Direct3D 9 on Xenos has no fixed function and no `UpdateTexture`; textures are tiled (linear uploads use the `D3DFMT_LIN_*` formats). Shaders are compiled offline with the SDK's `fxc` under Wine (`/Zpr`, `row_major`).
- Big-endian: every binary loader byte-swaps after its reads, and unaligned `*(int *)` reads fault; use memcpy readers. Pointer and integer type mismatches are real bugs here: don't silence them.

## Compile census

The game units the original Xbox build compiles (478: `source/`, `port/linux/game`'s shared netcode, `port/xbox/game`, `d3d_intimacy.cpp`), compiled for Xenon with the patched clang, no link, against the Xbox build's own declarations (`port/include/xdk` and the original Xbox SDK's C runtime headers, `_WIN32` defined):

| Configuration | Compiles |
|---|---|
| as the Xbox build (`HALO_XBOX_CONSOLE`) | 102 of 478: `fast_ftol`'s x86 `__asm` in `cseries.h` stops 366 |
| the same, `fast_ftol`'s asm gated on `_M_IX86` | 461 |
| the native builds' code paths (no `HALO_XBOX_CONSOLE`) | **465** |

What fails without `HALO_XBOX_CONSOLE`: 10 libtiff units (K&R prototypes for an unknown platform, as on the Wii U; tools only), `real_math.c` (x87 `CW_DEFAULT`), `shell_xbox.c` (SEH `__try`), `transport_endpoint_set_winsock.c` (the prefix must define `FD_SETSIZE` first). With `HALO_XBOX_CONSOLE`, add x86 `__asm` in `profile.c`, `stack_walk_windows.c`, `decals.c`, `hud_draw.c` (`naked`) and `bink_playback.c`.

Compiling is the easy part: byte order and alignment show up at run time, not here. Warnings worth a look on this target: 165 implicit declarations (`snprintf`, `vsnprintf`, `debug_malloc`), and a handful of pointer mismatches, one a real big-endian bug (`ui_widget_event_handler_functions.c` passes a `word *` as `long *`).
