#!/usr/bin/env python3
"""The linker script the Xbox 360 build links with (ninja xbox360 makes it
from OXDK360's oxdk360/title.ld):

    python3 tools/xbox360_linker_script.py ~/OXDK360/oxdk360/title.ld build/xbox360/gen/title.ld

It is OXDK360's script with one change: the XDK's .XBMOVIE$ sections go
with the writable data instead of the read-only data. They hold variables
the XDK's libraries write: XAudio2 sets ?XBM__CaptureCompletionSignalXAudio
when it makes its mastering voice. Microsoft's linker puts them in a
writable section of their own (.XBMOVIE, read/write in every XEX of the
Xbox 360 SDK). In the read-only pages, the write faults where the pages'
protection is kept: Xenia stopped there, inside XAudio2, with no word in
its log.

They stay sorted by name and together (.XBMOVIE$A and $ZZZZZZZZ bracket the
entries), at the start of .data. A script that already places them
elsewhere is copied as it is.
"""

import re
import sys

XBMOVIE = "*(SORT_BY_NAME(.XBMOVIE$*))"


def fail(message):
    sys.exit(f"xbox360_linker_script: {message}")


def main():
    if len(sys.argv) != 3:
        print("usage: xbox360_linker_script.py title.ld out.ld", file=sys.stderr)
        return 2
    source, output = sys.argv[1], sys.argv[2]
    text = open(source).read()
    rdata = re.search(r"^\s*\.rdata\s*:\s*\{", text, re.M)
    data = re.search(r"^(\s*\.data\s*:\s*\{)", text, re.M)
    if not rdata or not data:
        fail(f"{source} has no .rdata or .data output section")
    at = text.find(XBMOVIE)
    if rdata.end() <= at < data.start():
        text = text[:at] + text[at + len(XBMOVIE):]
        data = re.search(r"^(\s*\.data\s*:\s*\{)", text, re.M)
        text = (text[:data.end()]
                + f"\n        /* (the XDK's writable .XBMOVIE$ sections: tools/xbox360_linker_script.py) */"
                + f"\n        . = ALIGN(4); {XBMOVIE}\n       "
                + text[data.end():])
    elif re.search(r"\(\s*\.XBMOVIE", text[rdata.end():data.start()]):
        fail(f"{source} places .XBMOVIE$ in its read-only data in a way this tool does not know")
    header = "/* made by tools/xbox360_linker_script.py from OXDK360's title.ld */\n"
    with open(output, "w") as f:
        f.write(header + text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
