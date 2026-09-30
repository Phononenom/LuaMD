#!/usr/bin/env python3
"""Writes the Homebrew Browser manifest for this payload.

    mkmanifest.py <title> <payload.bin> <payload.elf> <out>

The browser installs each emulator as /savedata0/homebrew/<title>/ with a
payload.bin (the blob, verbatim) and this manifest, which is plain key=value
lines:

    title=<title>
    reserve=0x...   bytes of JIT shared memory the launch must reserve
    data_off=0x0    this blob is code + embedded .data image, copied whole
    data_size=0x0

reserve is EXACTLY the reservation bin2lua.py computes for @@JIT_SIZE@@ --
__data_start page-aligned up, i.e. code plus .data's initial image -- then
aligned up to whole 256KB chunks, the granularity the browser remaps JIT
memory at. .bss is deliberately NOT in the reservation: _start maps it
anonymously itself (boot_data_region in src/main.c), so charging it against
the JIT pool would only shrink the number of payloads a session can run.
"""

import os
import re
import subprocess
import sys

PAGE  = 0x4000
CHUNK = 0x40000

# The whole JIT region the browser can offer. A payload needing more cannot
# be launched by it at all, so exceeding it is a build failure, not a
# warning -- finding out on the console costs a game relaunch.
POOL_MAX = 0xC0000


def linker_syms(elf_path, names):
    """Read the given linker-defined symbol addresses out of the ELF.
       Same lookup bin2lua.py uses, so both tools agree by construction."""
    if elf_path and os.path.exists(elf_path):
        for tool in ("nm", "llvm-nm"):
            try:
                out = subprocess.check_output([tool, elf_path],
                                              stderr=subprocess.DEVNULL).decode()
            except (OSError, subprocess.CalledProcessError):
                continue
            found = {}
            for n in names:
                m = re.search(r"^([0-9a-fA-F]+)\s+\S+\s+%s$" % re.escape(n), out, re.M)
                if m:
                    found[n] = int(m.group(1), 16)
            if len(found) == len(names):
                return found
    raise SystemExit("mkmanifest: could not read %s from %r -- refusing to "
                     "guess the reservation" % (", ".join(names), elf_path))


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    title, binary, elf, out = sys.argv[1:5]

    with open(binary, "rb") as f:
        blob = f.read()

    data_start = linker_syms(elf, ["__data_start"])["__data_start"]

    jit_size = (data_start + PAGE - 1) & ~(PAGE - 1)
    reserve  = (jit_size + CHUNK - 1) & ~(CHUNK - 1)

    # The blob is copied into the JIT mappings whole, so it must fit the
    # reservation. bin2lua.py already enforces the tighter
    # blob <= __data_start bound at pack time; this catches a manifest made
    # from a stale bin/elf pair rather than re-litigating that check.
    if len(blob) > reserve:
        raise SystemExit(
            "mkmanifest: blob (%d B) exceeds its 0x%X reserve -- the browser "
            "would truncate the image loading it" % (len(blob), reserve))
    if reserve > POOL_MAX:
        raise SystemExit(
            "mkmanifest: reserve 0x%X exceeds the browser's 0x%X JIT region"
            % (reserve, POOL_MAX))

    text = ("title=%s\n"
            "reserve=0x%X\n"
            "data_off=0x0\n"
            "data_size=0x0\n" % (title, reserve))

    d = os.path.dirname(out)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    print("mkmanifest: %s" % out)
    print("  blob      %7d B  (code + .data init)" % len(blob))
    print("  reserve   0x%05X  (%d x 256KB chunks)"
          % (reserve, reserve // CHUNK))


if __name__ == "__main__":
    main()
