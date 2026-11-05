#!/usr/bin/env python3
"""Give default.xex the DOS header Microsoft's linker writes (ninja xbox360
runs this on the XEX after linking):

    python3 tools/xbox360_dos_header.py build/xbox360/default.xex

The PE inside a XEX starts with a DOS header, and OXDK360's cxex writes only
its "MZ" and the PE header's offset there, the rest zero. Microsoft's linker
writes the full header and the stub program that prints "This program cannot
be run in DOS mode." Every XEX in the Xbox 360 SDK has it. The console does
not look at it, but Xenia does: it loads an image only when the first four
bytes are "MZ", 0x90, 0 (XexModule::is_valid_executable).

This writes the header and stub Microsoft's linker writes (0x00 to 0x7f;
the PE header follows at 0x80, where cxex puts it), and then the one hash
that covers those bytes: the first page run's link in the XEX's chain of
page hashes, kept in the security info as the image's digest. The header
digest does not cover that field, so it stays as it is, and the tool checks
both before and after. The Rich header (the build tools' version stamps
between the stub and the PE header) is left out: it says which Microsoft
tools made the file, and they did not.

The XEX must be what cxex writes: unencrypted, basic compression, one block.
Running it again changes nothing.
"""

import hashlib
import struct
import sys

# Microsoft's DOS header (e_magic to e_res2; e_lfanew at 0x3c is kept) and
# stub, as in every XEX the Xbox 360 SDK ships
DOS_HEADER = bytes.fromhex(
    "4d5a90000300000004000000ffff0000"
    "b8000000000000004000000000000000"
    "00000000000000000000000000000000"
    "000000000000000000000000")
DOS_STUB = bytes.fromhex(
    "0e1fba0e00b409cd21b8014ccd21") + b"This program cannot be run in DOS mode.\r\r\n$"
DOS_STUB += bytes(0x40 - len(DOS_STUB))

KEY_FILE_FORMAT_INFO = 0x000003FF
KEY_IMAGE_BASE = 0x00010201


def fail(message):
    sys.exit(f"xbox360_dos_header: {message}")


def be32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def header_digest(data, security, image):
    # (as the kernel checks it, and cxex's seal_header writes it)
    return hashlib.sha1(bytes(data[security + 0x17C:image]) + bytes(data[:security + 8])).digest()


def first_run_digest(data, security, image, page_size):
    descriptor = be32(data, security + 0x184)
    following = bytes(data[security + 0x184 + 4:security + 0x184 + 24])
    length = (descriptor >> 4) * page_size
    run = bytes(data[image:image + length])
    run += bytes(length - len(run))
    return hashlib.sha1(run + struct.pack(">I", descriptor) + following).digest()


def main():
    if len(sys.argv) != 2:
        print("usage: xbox360_dos_header.py default.xex", file=sys.stderr)
        return 2
    path = sys.argv[1]
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"XEX2":
        fail(f"{path} is not an XEX2 file")
    image = be32(data, 8)
    security = be32(data, 16)
    count = be32(data, 20)
    base = None
    file_format = None
    for i in range(count):
        key, value = struct.unpack_from(">II", data, 24 + i * 8)
        if key == KEY_FILE_FORMAT_INFO:
            file_format = value
        elif key == KEY_IMAGE_BASE:
            base = value
    if file_format is None or base is None:
        fail(f"{path} has no file format or image base header")
    encryption, compression = struct.unpack_from(">HH", data, file_format + 4)
    if encryption != 0 or compression != 1:
        fail(f"{path} is encrypted or compressed (only cxex's plain XEX is handled)")
    if data[image:image + 2] != b"MZ":
        fail(f"{path}: the image does not start with MZ")
    if be32(data, security + 0x180) == 0:
        fail(f"{path} has no page descriptors")
    page_size = 0x10000 if base < 0x90000000 else 0x1000
    pe = struct.unpack_from("<I", data, image + 0x3C)[0]
    if pe < 0x80:
        fail(f"{path}: the PE header is at 0x{pe:x}, inside the stub's room")

    # what is there must check out, or this is not a file the tool knows
    if bytes(data[security + 0x164:security + 0x178]) != header_digest(data, security, image):
        fail(f"{path}: the header digest does not match (not written by cxex?)")
    if bytes(data[security + 0x114:security + 0x128]) != first_run_digest(data, security, image, page_size):
        fail(f"{path}: the image digest does not match (not written by cxex?)")

    stamped = DOS_HEADER + bytes(data[image + 0x3C:image + 0x40]) + DOS_STUB
    if bytes(data[image:image + 0x80]) == stamped:
        return 0
    data[image:image + 0x80] = stamped
    data[security + 0x114:security + 0x128] = first_run_digest(data, security, image, page_size)
    if bytes(data[security + 0x164:security + 0x178]) != header_digest(data, security, image):
        fail("internal error: the header digest changed")
    with open(path, "wb") as f:
        f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
