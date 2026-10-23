"""Ninja rules for the Xbox 360 build (``ninja xbox360``): the port's build.py,
which the game's configure.py loads (--console-port port/xbox360; the game's
tools/console_build.py). From this repository's root, with the game in
chupathingyce/ (a submodule):

    python3 chupathingyce/configure.py --console-port port/xbox360
    ninja xbox360

Target: PowerPC Xenon 32-bit big-endian, compiled by OXDK360's patched clang
(the Xenon calling convention) against your own Xbox 360 SDK, linked and
wrapped by OXDK360's oxdklink into build/xbox360/default.xex.

It is the game's Windows build's arrangement (its tools/windows_build.py) on
the console: the game's sources, the desktop platform layer (the game's
port/linux/src) less what the console does its own way, and this port's
src. The game and port/linux/src see the Xbox SDK declarations of the game's
port/include/xdk; port/xbox360/src/x360_*.c see the console's SDK. The game
has the 64-bit builds' pointer mode over a window of Xbox addresses placed
wherever the console has room (HALO_64BIT, HALO_WINDOW_TRANSLATED: the
game's source/cseries/xbox_address.h).

The port's options (configure.py --help):

    --oxdk360 DIR       OXDK360's checkout (default OXDK360_DIR, ~/OXDK360 or
                        ~/OXDK/xbox360)
    --llvm-xenon DIR    its patched LLVM (default LLVM_XENON_DIR or ~/llvm-xenon)
    --xdk360 DIR        your own extracted Xbox 360 SDK, the folder holding
                        lib/xbox/xboxkrnl.lib (default XDK_DIR, or OXDK360's
                        search)

The flags are OXDK360's C path (oxdk360.mk). Nothing from the SDK belongs in
this repository.
"""

import os
import shlex
from pathlib import Path
from typing import Any, List, Optional

from tools.console_build import ConsolePort, console_defines, relative
from tools.embed_assets import hud_asset_inputs
from tools.linux_build import (EXPAT_DIR, KCP_DIR, MONOCYPHER_DIR, MUSL_MATH_DIR, QRCODEGEN_DIR, TOML_DIR,
                               XDK_INCLUDE, ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES)
from tools.ninja_syntax import Writer
from tools.server_build import SDL_INCLUDE, fetch_sdl_headers
from tools.version import version

# this repository's tools (the build's stamp), beside the port
TOOLS_DIR = relative(Path(__file__).resolve().parents[2] / "tools")

# the game's directories the build takes from (under the game's root)
LINUX_DIR = Path("port/linux")
WINDOWS_DIR = Path("port/windows")
CONSOLE_DIR = Path("port/console")

TARGET_TRIPLE = "powerpc-unknown-none-elf"

# oxdk360.mk's C flags: the SDK's headers are MSVC 2005's (_MSC_VER 1400; its
# _MSC_FULL_VER picks the vadefs.h va_start clang implements), _PPC_ and
# _M_PPCBE select their PowerPC blocks, wchar_t is 16 bits, and the Xenon
# calling convention is the patched clang's xenon-abi feature
BASE_FLAGS = [
    "-target", TARGET_TRIPLE,
    "-fms-extensions",
    "-fms-compatibility",
    "-fms-compatibility-version=14",
    "-fshort-wchar",
    "-fno-autolink",
    "-Wno-invalid-token-paste",
    "-D_WIN32",
    "-D_M_PPCBE",
    "-D_M_PPC",
    "-D_PPC_",
    "-D_XBOX",
    "-D_XENON",
    "-DXBOX",
    "-D_MSC_VER=1400",
    "-D_MSC_FULL_VER=140050727",
    "-D_SIZE_T_DEFINED",
    "-D_W64=",
    # SAL 2 annotations clang would read as K&R prototypes
    "'-D_Releases_lock_(x)='",
    # the SDK's 64-bit returns come back wrong from this compiler (the
    # register pair gap): the C runtime's 32-bit time_t
    "-D_USE_32BIT_TIME_T=1",
    # the C runtime headers' __inline functions are MSVC's pick-any COMDATs,
    # which an ELF target emits in every unit: each unit's own copy instead
    "'-D__inline=static __inline__'",
    "-Xclang", "-target-feature", "-Xclang", "+xenon-abi",
    # thread-local variables through x360_tls.c: the console's TLS is
    # Microsoft's compiler's, and ELF TLS reads through r2, which it leaves 0
    "-femulated-tls",
    # the Xenon core's square root instruction (the generic PowerPC has
    # none: every sqrtf was a call), and no errno from the math functions,
    # so sqrtf and fabsf are single instructions as on the other ports
    "-Xclang", "-target-feature", "-Xclang", "+fsqrt",
    "-fno-math-errno",
    "-O2",
]

# the console's own units, the third-party libraries and the SDK's helpers
# keep oxdk360.mk's freestanding build (x360_system.c defines C99 math the
# runtime lacks, which the compiler's builtins would turn into calls to
# themselves); the game and the platform layer have the builtins (memcpy of
# a small structure inlined, sqrtf an instruction), less the roundings whose
# float forms the runtime lacks (the compiler would make floorf of a float's
# floor) and stpcpy
FREESTANDING_FLAGS = ["-ffreestanding", "-fno-builtin"]
HOSTED_FLAGS = [f"-fno-builtin-{name}" for name in ("floor", "ceil", "fmod", "trunc", "round", "rint",
                                                     "nearbyint", "stpcpy")]

# the game's and the desktop platform layer's ABI, as the other ports give
# it (the game's tools/linux_build.py): tentative definitions shared between
# units, no optimizations that assume the absence of MSVC-tolerated UB, the
# same floating point results on every port (no fused multiply-adds, which
# PowerPC would otherwise emit everywhere), and small structures and unions
# returned in registers as Win32 does (hs_runtime.c calls union-returning
# converters through pointers typed as returning long)
GAME_ABI_FLAGS = [
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-ffp-contract=off",
    "-msvr4-struct-return",
    # char is signed, as MSVC has it (PowerPC's is unsigned: a char holding
    # NONE would read 255)
    "-fsigned-char",
    "-DHALO_64BIT",
    "-DHALO_WINDOW_TRANSLATED",
]

GAME_FLAGS = [
    "-std=gnu89",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

# the menus' map list and the Xbox map packs (maps_<pack>: Halo CE+), as the
# original Xbox and the desktop builds have them (the game's
# port/linux/game/ui_map_list.c, map_packs.c); the console list's packs=1
MAP_LIST_DEFINES = ["-DHALO_MAP_LIST"]

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-DHALO_LINUX_PLATFORM_LAYER",
    # internet play's tunnel (p2p.c) logs what becomes of the host's
    # datagrams for the game (the log is src/x360_net.c's), and sends the
    # game's datagrams for peers from its own thread: XNet lets one thread
    # at a time use a socket (another's call fails with WSAEINPROGRESS) and
    # its select holds the tunnel's socket, so the game's system link search
    # sent onto the tunnel from the game's thread never left (as on the
    # original Xbox: warthog-xbox's port/xbox/p2p/platform.h)
    "-DP2P_TRACE_DATAGRAMS",
    "-DP2P_SEND_FROM_THREAD",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-duplicate-decl-specifier",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-ignored-pragmas",
    "-Wno-microsoft-enum-forward-reference",
    "-Wno-language-extension-token",
    "-Werror=incompatible-pointer-types",
    "-Werror=int-conversion",
    "-Werror=implicit-function-declaration",
]

# port/xbox360/src/x360_*.c talk to the console only: its SDK, warnings on
# (on this big-endian ABI a pointer or integer mismatch is a real bug)
CONSOLE_FLAGS = [
    "-std=gnu11",
    "-Wall",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-unused-function",
    "-Werror=incompatible-pointer-types",
    "-Werror=int-conversion",
    "-Werror=implicit-function-declaration",
    "-Werror=format",
]

# the desktop platform layer's units the console does its own way
# (port/xbox360/src), or has no use for
REPLACED_PLATFORM_SOURCES = {
    # the window, the input and the sound device (xbox360_platform.c,
    # xbox360_input.c; dsound_sdl.c's mixer stays, over x360_audio.c)
    "sdl_platform.c",
    "xinput_sdl.c",
    "touch_input.c",
    "touch_menu.c",
    # the C library's (the console's libcMT is MSVC's: windows_crt.c)
    "msvc_crt.c",
    "msvc_wide.c",
    # the host's files, sockets and processes (x360_files.c, x360_net.c,
    # x360_system.c)
    "posix_files.c",
    "posix_net.c",
    "posix_update.c",
    "posix_upnp.c",
    "posix_browser.c",
    "posix_ui_font.c",
    # page protection and the fault handler (x360_memory.c)
    "memory_watch.c",
    # the desktop crash reports: the console's faults go to its debug channel
    "posix_crash.c",
    # the self-updater: a console is updated with its files
    "updater.c",
    "update_signature.c",
}

# what the XEX links: XAM and the kernel come in as imports through xapilib
# and xboxkrnl
XDK_LIBS = "xapilib,d3d9,d3dx9,xgraphics,xaudio2,x3daudio,xmcore,xmahal,xboxkrnl,xnet,oldnames,libcMT"
XEX_BASE = "0x82000000"
# the game's main thread: the desktop builds give it 8 MB
XEX_STACK = "0x800000"


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return shlex.quote(text)


def add_arguments(parser: Any) -> None:
    """the port's own configure.py options"""
    parser.add_argument(
        "--oxdk360",
        metavar="DIR",
        type=Path,
        help="OXDK360 checkout for `ninja xbox360` (default: OXDK360_DIR, ~/OXDK360, or ~/OXDK/xbox360)",
    )
    parser.add_argument(
        "--llvm-xenon",
        metavar="DIR",
        type=Path,
        help="patched LLVM directory for `ninja xbox360` (default: LLVM_XENON_DIR or ~/llvm-xenon)",
    )
    parser.add_argument(
        "--xdk360",
        metavar="DIR",
        type=Path,
        help="your extracted Xbox 360 SDK for `ninja xbox360`, the folder holding lib/xbox/xboxkrnl.lib "
        "(default: XDK_DIR, or OXDK360's search)",
    )


def _oxdk360_dir(sln: Any) -> Optional[Path]:
    candidates = [
        getattr(sln.args, "oxdk360", None),
        os.environ.get("OXDK360_DIR"),
        Path.home() / "OXDK360",
        Path.home() / "OXDK" / "xbox360",
    ]
    for c in candidates:
        if c:
            p = Path(c).expanduser().resolve()
            if (p / "tools" / "oxdklink" / "oxdklink.py").is_file():
                return p
    return None


def _llvm_xenon_bin(sln: Any) -> Optional[Path]:
    candidates = [
        getattr(sln.args, "llvm_xenon", None),
        os.environ.get("LLVM_XENON_DIR"),
        Path.home() / "llvm-xenon",
    ]
    for c in candidates:
        if c:
            p = Path(c).expanduser().resolve()
            if (p / "build" / "bin" / "clang").is_file():
                return p / "build" / "bin"
            if (p / "bin" / "clang").is_file():
                return p / "bin"
    return None


def _x360_sdk_dir(sln: Any) -> Optional[Path]:
    candidates = [
        getattr(sln.args, "xdk360", None),
        os.environ.get("XDK_DIR"),
        os.environ.get("OXDK_XBOX360_XDK"),
        # OXDK360's own search (tools/common/toolchain.py)
        Path.home() / "xdk360" / "XDK",
        Path.home() / "xdk360-extract" / "sdk" / "XDK",
        Path("/opt/xdk360/XDK"),
    ]
    for c in candidates:
        if c:
            p = Path(c).expanduser().resolve()
            if (p / "lib" / "xbox" / "xboxkrnl.lib").is_file():
                return p
    return None


def _llvm_tool(llvm_bin: Path, name: str) -> Path:
    for directory in (llvm_bin, Path("/opt/homebrew/opt/llvm/bin"), Path("/usr/local/opt/llvm/bin")):
        if (directory / name).is_file():
            return directory / name
    return Path(name)


def configure_inputs(port: ConsolePort) -> List[Path]:
    """Files whose change must re-run configure.py."""
    game = port.game
    return [
        port.path("src"),
        port.path("include"),
        game / CONSOLE_DIR / "include",
        game / CONSOLE_DIR / "game",
        game / LINUX_DIR / "src",
        game / LINUX_DIR / "game",
    ]


def _missing(n: Writer, missing: List[str]) -> None:
    """`ninja xbox360` says what is missing rather than not existing"""
    text = "; ".join(missing)
    n.comment("Xbox 360 build (ninja xbox360): not available")
    n.rule(
        name="xbox360_missing",
        command=f"echo {shlex.quote('ninja xbox360 needs: ' + text + ' (port/xbox360/README.md)')} && exit 1",
        description="XBOX360 not configured",
    )
    n.build(outputs="xbox360", rule="xbox360_missing")
    n.newline()


def generate(n: Writer, sln: Any, port: ConsolePort) -> None:
    game = port.game
    PORT_DIR = port.root
    oxdk360 = _oxdk360_dir(sln)
    llvm_bin = _llvm_xenon_bin(sln)
    xdk = _x360_sdk_dir(sln)

    missing = []
    if not oxdk360:
        missing.append("OXDK360 (--oxdk360 or OXDK360_DIR)")
    if not llvm_bin:
        missing.append("the patched clang (--llvm-xenon or LLVM_XENON_DIR)")
    if not xdk:
        missing.append("your extracted Xbox 360 SDK with lib/xbox/xboxkrnl.lib (--xdk360 or XDK_DIR)")
    if not missing and not fetch_sdl_headers():
        missing.append("SDL's headers (downloaded once into build/server/third_party)")
    if missing:
        _missing(n, missing)
        return

    config = port.load_port_config()

    clang = llvm_bin / "clang"
    objcopy = _llvm_tool(llvm_bin, "llvm-objcopy")
    oxdklink = oxdk360 / "tools" / "oxdklink" / "oxdklink.py"
    cxex = oxdk360 / "tools" / "cxex" / "cxex"
    linker_script = oxdk360 / "oxdk360" / "title.ld"
    builtins = oxdk360 / "oxdk360" / "builtins" / "builtins.c"

    build_dir: Path = sln.build_dir / "xbox360"
    obj_dir = build_dir / "obj"
    gen_dir = build_dir / "gen"
    xex = build_dir / "default.xex"
    stamp = gen_dir / "xbox360_build_stamp.h"
    semantics_header = gen_dir / "halo_msvc_semantics.h"
    platform_semantics_header = gen_dir / "platform_msvc_semantics.h"
    prefix_header = PORT_DIR / "include" / "halo_xbox360_prefix.h"
    xdk_include = game / XDK_INCLUDE
    xdk_headers = sorted(xdk_include.glob("*.h"))
    semantics_tool = game / "tools" / "linux_msvc_semantics.py"

    n.comment("Xbox 360 build (ninja xbox360)")
    n.variable("xbox360_cc", _quote(clang))
    n.variable("xbox360_objcopy", _quote(objcopy))
    n.variable("xbox360_link", f"$python {_quote(oxdklink)}")

    # (MS compatibility records #pragma comment(lib) in .deplibs, which
    # ld.lld would try to honor: oxdk360.mk strips it the same way)
    n.rule(
        name="xbox360_cc",
        command="$xbox360_cc -MMD -MF $out.d $cflags -c $in -o $out && $xbox360_objcopy --remove-section=.deplibs $out",
        description="XBOX360 CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="xbox360_xex",
        command="$xbox360_link $in -o $out $ldflags",
        description="XBOX360 XEX $out",
    )
    n.rule(
        name="xbox360_stamp",
        command=f"$python {_quote(TOOLS_DIR / 'xbox360_stamp.py')} $out",
        description="XBOX360 STAMP $out",
    )
    n.rule(
        name="xbox360_msvc_semantics",
        command=f"$python {_quote(semantics_tool)} --output $out $scan",
        description="XBOX360 MSVC SEMANTICS $out",
        restat=True,
    )
    # the fonts only: the high-res HUD's pictures are not the 360's (its 512
    # MB the Xbox window and the GPU share), and the maps' own HUD is drawn
    n.rule(
        name="xbox360_embed_assets",
        command=f"$python {_quote(game / 'tools' / 'embed_assets.py')} --no-hud $out",
        description="XBOX360 EMBED $out",
    )

    # the stamp is written on every build: the XEX's first log line says
    # which build ran
    n.build(outputs="xbox360_always", rule="phony")
    n.build(outputs=stamp, rule="xbox360_stamp", implicit=["xbox360_always", TOOLS_DIR / "xbox360_stamp.py"])

    game_headers = port.game_headers()
    n.build(
        outputs=semantics_header,
        rule="xbox360_msvc_semantics",
        implicit=[semantics_tool, *xdk_headers, *game_headers],
        variables={"scan": f"--all-inlines --tags {_quote(game / 'source')} --inlines {_quote(game / 'source')} "
                           f"--inlines {_quote(xdk_include)}"},
    )
    n.build(
        outputs=platform_semantics_header,
        rule="xbox360_msvc_semantics",
        implicit=[semantics_tool, *xdk_headers],
        variables={"scan": f"--inlines {_quote(xdk_include)}"},
    )

    # the console's SDK: its own headers, then its compiler's C runtime
    sdk_includes = [
        f"-I{_quote(oxdk360 / 'oxdk360' / 'header-shim')}",
        f"-I{_quote(xdk / 'include' / 'xbox')}",
        f"-I{_quote(xdk / 'TechPreview' / 'Jul12Compiler' / 'include' / 'xbox')}",
    ]
    # the C runtime alone, behind the Xbox SDK declarations, for the game and
    # the desktop platform layer
    crt_includes = [
        f"-I{_quote(xdk / 'TechPreview' / 'Jul12Compiler' / 'include' / 'xbox')}",
        f"-I{_quote(xdk / 'include' / 'xbox')}",
    ]
    posix_include = PORT_DIR / "include" / "posix"
    windows_posix_include = game / WINDOWS_DIR / "include" / "posix"
    crt_wrappers = f"{_quote(PORT_DIR / 'include' / 'crt')} -I{_quote(game / WINDOWS_DIR / 'include' / 'crt')}"
    version_define = f"'-DHALO_VERSION=\"{version()}\"'"
    # configure.py's console options (--release, --console-players,
    # --console-debug-tools, --console-no-debug-asserts: the game's
    # tools/console_build.py). The developer tools (--console-debug-tools)
    # add the startup probe triangle, the frame profiler, the GL layer's
    # timers and the bring-up logs; off by default, so a normal build shows
    # no debug drawing
    defines = console_defines(sln)
    # the game's file names as the game spells them ("source/game/game.c"):
    # the log's and the asserts' names stay the desktop builds'
    file_names = f"-fmacro-prefix-map={_quote(game.as_posix() + '/')}=" if game != Path(".") else ""
    # the headers the shared code's console hooks and the port's own name
    # (halo_port_limits.h, halo_windows_api_names.h, cseries/xbox_address.h)
    shared_quotes = [
        f"-iquote {_quote(game / LINUX_DIR / 'include')}",
        f"-iquote {_quote(game / WINDOWS_DIR / 'include')}",
    ]

    game_cflags = " ".join([
        *BASE_FLAGS,
        *HOSTED_FLAGS,
        *GAME_ABI_FLAGS,
        *GAME_FLAGS,
        *defines,
        *MAP_LIST_DEFINES,
        file_names,
        f"-include {_quote(prefix_header)}",
        f"-include {_quote(semantics_header)}",
        f"-I{crt_wrappers}",
        f"-I{_quote(PORT_DIR / 'include')}",
        f"-I{_quote(game / CONSOLE_DIR / 'include')}",
        *shared_quotes,
        f"-iquote {_quote(game / LINUX_DIR / 'game')}",
        f"-iquote {_quote(game)}",
        port.game_defines_and_includes(config, _quote),
        f"-I{_quote(xdk_include)}",
        *crt_includes,
    ])
    platform_cflags = " ".join([
        *BASE_FLAGS,
        *HOSTED_FLAGS,
        *GAME_ABI_FLAGS,
        *PLATFORM_FLAGS,
        *defines,
        *MAP_LIST_DEFINES,
        file_names,
        version_define,
        f"-include {_quote(prefix_header)}",
        f"-include {_quote(platform_semantics_header)}",
        f"-I{_quote(posix_include)}",
        f"-I{_quote(windows_posix_include)}",
        f"-I{crt_wrappers}",
        f"-I{_quote(game / LINUX_DIR / 'src')}",
        f"-I{_quote(PORT_DIR / 'include')}",
        f"-I{_quote(game / TOML_DIR)}",
        f"-I{_quote(game / EXPAT_DIR)}",
        f"-I{_quote(game / KCP_DIR)}",
        f"-I{_quote(game / QRCODEGEN_DIR)}",
        f"-I{_quote(game / MONOCYPHER_DIR)}",
        f"-I{_quote(game / ZLIB_DIR)}",
        *shared_quotes,
        f"-I{_quote(game / 'source')} -I{_quote(game / 'source' / 'cseries')}",
        f"-I{_quote(SDL_INCLUDE)}",
        f"-I{_quote(gen_dir)}",
        f"-I{_quote(PORT_DIR / 'src')}",
        f"-I{_quote(xdk_include)}",
        *crt_includes,
    ])
    console_cflags = " ".join([
        *BASE_FLAGS,
        *FREESTANDING_FLAGS,
        "-DHALO_XBOX360_CONSOLE",
        "-DHALO_64BIT",
        "-DHALO_WINDOW_TRANSLATED",
        *CONSOLE_FLAGS,
        *(["-DHALO_CONSOLE_DEBUG_TOOLS"] if getattr(sln, "console_debug_tools", False) else []),
        version_define,
        f"-I{_quote(posix_include)}",
        f"-I{_quote(windows_posix_include)}",
        f"-I{_quote(PORT_DIR / 'include' / 'crt')}",
        f"-I{_quote(game / LINUX_DIR / 'src')}",
        f"-I{_quote(PORT_DIR / 'include')}",
        f"-I{_quote(SDL_INCLUDE)}",
        f"-I{_quote(gen_dir)}",
        *sdk_includes,
    ])
    third_party_cflags = " ".join([*BASE_FLAGS, *FREESTANDING_FLAGS, *GAME_ABI_FLAGS, "-std=gnu11", "-w",
                                   f"-include {_quote(prefix_header)}", f"-I{crt_wrappers}",
                                   f"-I{_quote(PORT_DIR / 'include')}", *shared_quotes,
                                   f"-I{_quote(game / 'source')}", f"-I{_quote(xdk_include)}", *crt_includes])
    implicit_common = [*xdk_headers, prefix_header, PORT_DIR / "include" / "halo_xbox360_api_names.h"]

    objects: List[Path] = []

    def add_object(source: Path, cflags: str, implicit: List[Path]) -> None:
        obj = obj_dir / source.relative_to(build_dir) if build_dir in source.parents else port.output_path(obj_dir, source)
        obj = obj.with_suffix(".o")
        objects.append(obj)
        n.build(outputs=obj, rule="xbox360_cc", inputs=source, implicit=implicit,
                order_only=[stamp], variables={"cflags": cflags})

    game_objects_start = len(objects)
    # (the sources every 64-bit build leaves out: port.json "lp64")
    excluded = set(config.get("lp64", {}).get("exclude", []))
    for source, flags in port.game_units(sln, config):
        if port.game_relative(source) not in excluded:
            add_object(source, " ".join([game_cflags, *flags]), [*implicit_common, semantics_header])
    for source in sorted((game / config["game_sources"]).glob("*.c")):
        add_object(source, game_cflags, [*implicit_common, semantics_header])
    # the consoles' shared game code: the maps' byte order
    for source in sorted((game / CONSOLE_DIR / "game").glob("*.c")):
        add_object(source, game_cflags, [*implicit_common, semantics_header])
    # the port's own game code: ONLINE GAMES (game/x360_browser_screen.c)
    for source in sorted((PORT_DIR / "game").glob("*.c")):
        add_object(source, game_cflags, [*implicit_common, semantics_header, PORT_DIR / "include" / "x360_game_list.h"])
    game_objects = objects[game_objects_start:]

    platform_objects_start = len(objects)
    for source in sorted((game / LINUX_DIR / "src").glob("*.c")):
        if source.name in REPLACED_PLATFORM_SOURCES:
            continue
        add_object(source, platform_cflags, [*implicit_common, platform_semantics_header])
    # the C runtime's file functions with Xbox paths, as on Windows
    add_object(game / WINDOWS_DIR / "src" / "windows_crt.c", platform_cflags,
               [*implicit_common, platform_semantics_header])
    # the console's own units (x360_*.c, over its SDK), and the platform
    # layer's units the port stands in for (the dedicated server's few SDL
    # functions: files, and the audio stream x360_audio.c plays)
    for source in sorted((PORT_DIR / "src").glob("*.c")):
        if source.name.startswith("x360_"):
            add_object(source, console_cflags, [stamp])
        else:
            add_object(source, platform_cflags, [*implicit_common, platform_semantics_header])
    embedded = gen_dir / "hud_hires_assets.c"
    n.build(outputs=embedded, rule="xbox360_embed_assets",
            implicit=[game / "tools" / "embed_assets.py",
                      *(game / path if not Path(path).is_absolute() else Path(path) for path in hud_asset_inputs())])
    add_object(embedded, platform_cflags, [*implicit_common, platform_semantics_header])
    platform_objects = objects[platform_objects_start:]

    add_object(game / TOML_DIR / "tomlc17.c", third_party_cflags, [])
    # the menus' XML parser, as on Windows (its hash salt from rand_s), over
    # the console's SDK
    for name in ("xmlparse.c", "xmlrole.c", "xmltok.c", "random_rand_s.c"):
        add_object(game / EXPAT_DIR / name,
                   " ".join([*BASE_FLAGS, *FREESTANDING_FLAGS, "-std=gnu11", "-w",
                             f"-I{_quote(PORT_DIR / 'include' / 'sdk')}", f"-I{_quote(game / EXPAT_DIR)}",
                             *sdk_includes]), [])
    add_object(game / KCP_DIR / "ikcp.c", third_party_cflags, [])
    add_object(game / QRCODEGEN_DIR / "qrcodegen.c", third_party_cflags, [])
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        add_object(game / MONOCYPHER_DIR / name, third_party_cflags, [])
    for name in ZLIB_SOURCES:
        add_object(game / ZLIB_DIR / name, f"{third_party_cflags} {' '.join(ZLIB_DEFINES)}", [])
    musl = game / MUSL_MATH_DIR
    for source in sorted((musl / "src").glob("*.c")):
        add_object(source, f"{third_party_cflags} -I{_quote(musl / 'include')} "
                           f"-include {_quote(musl / 'include' / 'libm.h')}", [])

    builtins_obj = obj_dir / "oxdk360_builtins.o"
    objects.append(builtins_obj)
    n.build(outputs=builtins_obj, rule="xbox360_cc", inputs=builtins,
            variables={"cflags": " ".join([*BASE_FLAGS, *FREESTANDING_FLAGS, *sdk_includes])})

    # each part on its own, to see what compiles (and the game's units for
    # the tag swap tables: the game's tools/tag_swap_generate.py)
    n.build(outputs="xbox360-game-objects", rule="phony", inputs=game_objects)
    n.build(outputs="xbox360-platform-objects", rule="phony", inputs=platform_objects)

    ldflags_list = [
        f"-T {_quote(linker_script)}",
        f"--xdk {_quote(xdk)}",
        f"--libs {XDK_LIBS}",
        f"--base {XEX_BASE}",
        f"--stack {XEX_STACK}",
        "--name default.exe",
    ]

    n.build(
        outputs=xex,
        rule="xbox360_xex",
        inputs=objects,
        implicit=[p for p in (linker_script, cxex, oxdklink) if p.is_file()],
        variables={"ldflags": " ".join(ldflags_list)},
    )
    n.build(outputs="xbox360", rule="phony", inputs=xex)
    n.newline()
