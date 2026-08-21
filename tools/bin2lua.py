#!/usr/bin/env python3
"""Folds the compiled payload into the Lua launcher.

    bin2lua.py <template.lua.in> <payload.bin> <out.lua> [payload.elf]

Substitutions performed on the template:

    @@SC@@         hex of the payload blob (code + initial .data)
    @@JIT_SIZE@@   bytes the JIT mapping must reserve

The JIT mapping has to cover more than the blob. The payload executes from a
PROT_READ|PROT_EXECUTE alias, so its writable globals cannot live in that
mapping; linker.ld parks .data and .bss in a region straight after the code and
_start maps anonymous RW memory over it. Reserving the whole span up front
means that mmap replaces the tail of a mapping that already exists, instead of
asking the kernel for an address the process' vm_map might refuse.
"""

import os
import re
import subprocess
import sys

PAGE = 0x4000


def linker_syms(elf_path, names):
    """Read the given linker-defined symbol addresses out of the ELF."""
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
    raise SystemExit("bin2lua: could not read %s from %r -- refusing to guess "
                     "the JIT reservation" % (", ".join(names), elf_path))


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)

    template, binary, out = sys.argv[1], sys.argv[2], sys.argv[3]
    elf = sys.argv[4] if len(sys.argv) > 4 else os.path.splitext(binary)[0] + ".elf"

    with open(binary, "rb") as f:
        blob = f.read()

    syms = linker_syms(elf, ["__data_start", "__bss_end"])
    data_start, bss_end = syms["__data_start"], syms["__bss_end"]

    # The JIT mapping only has to hold the code and .data's initial image --
    # everything from __data_start up is anonymous RW that _start maps itself.
    # Asking JIT shared memory for the whole span returns ENOMEM inside
    # ps2emu (0x8002000C); its JIT heap is far smaller than 1.6MB.
    jit_size = (data_start + PAGE - 1) & ~(PAGE - 1)

    # The blob must end below __data_start, or _start's RW mapping would land
    # on top of .data's initial image before it has been copied out.
    if len(blob) > data_start:
        raise SystemExit(
            "bin2lua: blob (%d B) overruns __data_start (0x%X) -- the .data "
            "init image would be clobbered by the RW mapping. Raise the "
            "ALIGN before .data in linker.ld." % (len(blob), data_start))

    # 32 bytes per line keeps the generated Lua readable and diff-able.
    hexstr = blob.hex()
    lines = [hexstr[i:i + 64] for i in range(0, len(hexstr), 64)]
    sc = "\n".join(lines)

    with open(template, "r", encoding="utf-8") as f:
        text = f.read()

    if "@@SC@@" not in text or "@@JIT_SIZE@@" not in text:
        raise SystemExit("bin2lua: template is missing @@SC@@ or @@JIT_SIZE@@")

    text = text.replace("@@JIT_SIZE@@", "0x%X" % jit_size)
    text = text.replace("@@SC@@", sc)

    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    print("bin2lua: %s" % out)
    print("  blob        %7d B  (code + .data init)" % len(blob))
    print("  JIT reserve   0x%05X  (%d B, headroom %d)"
          % (jit_size, jit_size, jit_size - len(blob)))
    print("  RW region   0x%05X..0x%05X  %d B, mapped by _start, not JIT"
          % (data_start, bss_end, bss_end - data_start))
    print("  lua         %7d B" % len(text))

    # remote_lua_loader caps a payload at 500 KiB.
    limit = 500 * 1024
    if len(text) > limit:
        raise SystemExit("bin2lua: %d B exceeds the %d B remote_lua_loader limit"
                         % (len(text), limit))


if __name__ == "__main__":
    main()
