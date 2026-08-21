# LuaMD

Mega Drive / Genesis emulator running as native x86_64 shellcode on PS5,
delivered through the [Luac0re](https://github.com/Gezine/Luac0re) JIT exploit.

**No kernel exploit required.** Runs entirely in userland inside the ps2emu
sandbox of *Star Wars Racer Revenge*.

Supports `.bin`, `.md`, `.gen` and `.smd` (Super Magic Drive, deinterleaved on
load), three- and six-button pads, and battery SRAM that persists into the
savedata container.

Sibling project to [LuaGB](https://github.com/soniciso1/LuaGB), which does the
same for the Game Boy and whose PS5 runtime this reuses.

---

## Credits

This project is glue around other people's work. The hard parts are theirs.

- **[Clownacy](https://github.com/Clownacy/clownmdemu-core)** — **clownmdemu**,
  the emulation core, along with the `clown68000` and `clownz80` CPU
  interpreters. AGPLv3, included in `md/`.
- **[Gezine](https://github.com/Gezine/Luac0re)** — **Luac0re**, the JIT exploit
  and Lua delivery framework this entire thing runs on. Without it there is no
  code execution and no project.
- **[egycnq](https://github.com/egycnq)** — the **savedata write method**.
  Writing to `/savedata0` from inside the sandbox is not obvious and not
  documented: the `sceSaveDataMount` pointer must come from the host eboot's
  import table (a `dlsym` lookup refuses every mount), the parameter block is
  `sceSaveDataMount`'s and not `sceSaveDataMount2`'s, and **the unmount is what
  commits the bytes**. That recipe is egycnq's; I would not have found it.
  Also **[EmuC0re](https://github.com/egycnq/EmuC0re)**, whose PS5 runtime
  structure — video/audio/pad bring-up, the shellcode ABI, the ROM picker — is
  the pattern this follows.
- **[CTurt](https://cturt.github.io/mast1c0re.html)** and
  **[McCaulay](https://mccaulay.co.uk/mast1c0re-part-2-arbitrary-ps2-code-execution/)**
  — mast1c0re, which Luac0re builds on.

---

## Requirements

- PS5 with **Luac0re** set up and its loader running (port 9026)
- *Star Wars Racer Revenge* — USA `CUSA03474` or EU `CUSA03492`
- Python 3 on a PC on the same network
- To build: `gcc`, `binutils`, `make` (WSL works on Windows)

---

## Where the ROMs go

**ROMs live inside the game's savedata, in a `roms/` folder at its root.**
Nothing is read from anywhere else.

```
/savedata0/
    lua/        <- Luac0re (already there, leave it alone)
    roms/       <- CREATE THIS. your .bin / .md / .gen / .smd files
    saves/      <- CREATE THIS, empty. cartridge SRAM lands here
    VMC0.card   <- leave alone
    VMC1.card   <- leave alone
    sce_sys/    <- leave alone
```

Two ways to get them in there:

**A save manager**, the same tool used to install Luac0re. Mount the savedata
image, create `roms/` and `saves/`, copy ROMs in, unmount to commit.

**FTP on port 1337.** The payload serves FTP itself while it waits on the ROM
screen and writes straight into the container, so no save manager is needed
after the initial setup. With an empty `roms/` it waits indefinitely; with ROMs
already present it waits a few seconds and moves on.

**Do it with the game closed** if you use a save manager. Mounting the savedata
image while the game is running desynchronises the game's own mount —
directories read back empty and writes fail.

**Watch the space.** The container is around 42 MB and the two PS2 virtual
memory cards take 16.5 MB before you start, leaving roughly 24 MB. A Mega Drive
cartridge is 512 KB to 4 MB.

---

## Running

Arm the loader — launch the game, then **OPTIONS → HALL OF FAME** — then:

```
python md_launcher.py <PS5_IP>
```

Add `--log` to watch the payload's UDP log on port 9027.

### Controls

| DualSense | Mega Drive |
| --- | --- |
| Square | A |
| Cross | B |
| Circle | C |
| Triangle | X |
| L2 / R2 | Y / Z |
| Options | Start |
| L3 | Mode |
| D-Pad | D-Pad |
| L1 | back to the ROM picker (saves first) |
| R1 *(hold 1 second)* | quit |

Six-button support is free: games that only understand three never poll for the
rest. R1 needs a deliberate full-second hold, because quitting ends the Luac0re
session and means relaunching the game.

A web controller is also served at `http://<PS5_IP>:9030` — touch, keyboard and
Gamepad API. The input source locks to whichever device you press first.

### Saving

Cartridge SRAM is written to `saves/<rom>.srm` inside the savedata container.
Press **L1** to return to the picker — that commits it. Pulling power will not.

Most cartridges have no battery at all; the load line says which.

---

## Building

```
make          # builds md_emu.bin and folds it into lua/md.lua
make check    # relocation guards (also part of a normal build)
make size     # section sizes against the JIT ceiling
```

---

## Notes for anyone porting something else

Four constraints shape this codebase and cost real effort to find.

**The JIT mapping tops out at 256 KB.** Probed on hardware: `0x40000` maps,
`0x60000` returns `ENOMEM` (`0x8002000C`). Everything with a load address has to
fit under it, so the build fails past it. `.bss` is free — `_start` maps it
separately as ordinary anonymous memory, which matters because the VDP's blit
tables alone are 96 KB.

**The Mega CD BIOS is hidden inside the Mega Drive bus code.** clownmdemu
`#include`s a 16,384-line array into the body of `bus-main-m68k.c`; it is
135,266 bytes, 70% of the entire core and more than half the budget. LuaMD is
cartridge-only, so `md/source/mega-cd-boot-rom.c` is replaced with zeroes — that
one edit takes the object from 140,928 bytes to 9,829 and is what makes the port
fit. The Mega CD `.c` files themselves stay: `clownmdemu.c` initialises the
sub-CPU, CDC, CDDA and PCM unconditionally, so removing them is a link error
rather than a saving.

**The payload runs from a `PROT_READ|PROT_EXECUTE` mapping, so it cannot have
writable globals.** The linker script parks `.data`/`.bss` immediately after the
code and `_start` maps anonymous RW memory over that range before touching a
single global. Link with `-static-pie`, never `-static`: plain `-static` relaxes
RIP-relative GOT loads into absolute immediates, which boots fine and then dies
on the first cross-TU function pointer. `tools/check_image.sh` guards this, and
`tools/check_relocs.sh` fails the build if initialised pointer data survives —
the linker script discards `.rela.*`, so such a table reads back as link-time
offsets.

**Three clownmdemu contracts fail silently if you get them wrong.**
`ClownMDEmu_Initialise` sets `cartridge_buffer` to `NULL`, so it must run
*before* `SetCartridge` and `HardReset` — get it backwards and the VDP still
renders, giving a blank screen while the 68000 executes zeroes. Both audio
generators accumulate into the sample buffer with `+=` and never clear it, so
the frontend must zero it every call. And cartridge SRAM does *not* use the
`save_file_*` callbacks — those are Mega CD Backup RAM; cartridge saves live in
`state.external_ram`, populated by `HardReset` from the header's `RA` block.

---

## Licence

**AGPLv3.** clownmdemu is AGPLv3 and LuaMD links it into a single binary, so the
whole work is AGPLv3 — including the PS5 runtime under `src/`, even though that
code originated in LuaGB. See `LICENSE`, and `md/LICENCE.txt` for the upstream
copy.

No ROMs, keys or copyrighted material are included or distributed.
