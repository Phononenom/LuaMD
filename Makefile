# LuaMD -- Mega Drive / Genesis emulator as PS5 userland shellcode.
#
#   make            build md_emu.bin and fold it into lua/md.lua
#   make check      fail the build if any initialised pointer data survives
#   make size       report the image against the JIT mapping ceiling
#   make clean

CC      ?= gcc
OBJCOPY ?= objcopy
READELF ?= readelf
OBJDUMP ?= objdump
PYTHON  ?= python3

TARGET  := md_emu

# The payload is delivered into a PS5 JIT shared-memory mapping, and a probe on
# real hardware put the ceiling for that mapping at 0x40000 -- 256KB exactly,
# with 384KB returning ENOMEM. Everything with a load address has to fit under
# it. .bss does not count: _start maps it separately as ordinary anonymous
# memory, which is just as well, since the VDP's blit tables alone are 96KB.
JIT_LIMIT := 262144

# -fpie makes every symbol reference RIP-relative, which is what lets the flat
# blob run from whatever address the JIT mapping lands on.
BASEFLAGS = -Os -ffreestanding -fno-stack-protector -fno-builtin \
            -fpie -mno-red-zone -fomit-frame-pointer -fcf-protection=none \
            -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -fno-strict-aliasing -fvisibility=hidden

INCLUDES = -Isrc/libc -Isrc -Imd/source -Imd/libraries/clowncommon -Imd/libraries -Imd

# Our own code: warnings are signal.
SRCFLAGS = $(BASEFLAGS) $(INCLUDES) -Wall -Wno-unused-function

# clownmdemu is upstream code kept verbatim apart from one documented change
# (md/source/mega-cd-boot-rom.c), so it is not held to our warning bar. NDEBUG
# compiles out every assert, which also means <assert.h> never needs a real
# implementation.
MDFLAGS  = $(BASEFLAGS) $(INCLUDES) -DNDEBUG -w

# -static-pie, NOT "-static -pie": with plain -static the linker emits an
# ET_EXEC and then RELAXES every RIP-relative GOT load into an absolute
# immediate, which silently destroys position independence for cross-TU global
# function addresses. -static-pie produces ET_DYN and keeps the LEA form.
LDFLAGS = -T linker.ld -nostdlib -nostartfiles -static-pie \
          -Wl,--build-id=none -Wl,-z,norelro

SRC_SRCS = src/main.c src/mdglue.c src/shim.c src/setjmp.c src/ui.c \
           src/ftp.c src/savedata.c

# Mega CD support is compiled in even though LuaMD never enables it: clownmdemu.c
# initialises the sub-CPU, CDC, CDDA and PCM unconditionally, so leaving those
# files out is a link error rather than a saving. They cost about 10KB. What
# actually had to go was the Mega CD BOOT ROM, a 132KB array #included into
# bus-main-m68k.c -- see md/source/mega-cd-boot-rom.c.
MD_SRCS = md/source/bus-common.c md/source/bus-main-m68k.c \
          md/source/bus-sub-m68k.c md/source/bus-z80.c \
          md/source/cdc.c md/source/cdda.c md/source/clownmdemu.c \
          md/source/controller.c md/source/controller-manager.c \
          md/source/controller-multitap-ea.c md/source/controller-multitap-sega.c \
          md/source/fm.c md/source/fm-channel.c md/source/fm-lfo.c \
          md/source/fm-operator.c md/source/fm-phase.c \
          md/source/io-port.c md/source/log.c md/source/low-pass-filter.c \
          md/source/pcm.c md/source/psg.c md/source/sync.c md/source/vdp.c \
          md/libraries/clown68000/source/interpreter/clown68000.c \
          md/libraries/clown68000/source/common/opcode.c \
          md/libraries/clownz80/source/interpreter.c \
          md/libraries/clownz80/source/common.c

SRC_OBJS = $(SRC_SRCS:.c=.o)
MD_OBJS  = $(MD_SRCS:.c=.o)
OBJS     = $(SRC_OBJS) $(MD_OBJS)

HEADERS  = src/core.h src/shim.h src/mdglue.h src/ui.h src/ftp.h \
           src/tables.h src/savedata.h

all: $(TARGET).bin lua/md.lua

$(SRC_OBJS): %.o: %.c $(HEADERS)
	$(CC) $(SRCFLAGS) -c $< -o $@

$(MD_OBJS): %.o: %.c
	$(CC) $(MDFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(BASEFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(TARGET).bin: $(TARGET).elf check
	@READELF="$(READELF)" OBJDUMP="$(OBJDUMP)" sh tools/check_image.sh $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@sz=$$(wc -c < $@); \
	 echo "Built: $@ ($$sz bytes, JIT ceiling $(JIT_LIMIT))"; \
	 if [ $$sz -gt $(JIT_LIMIT) ]; then \
	     echo "FAIL: image exceeds the JIT mapping ceiling by $$((sz - $(JIT_LIMIT))) bytes"; \
	     exit 1; \
	 fi

lua/md.lua: $(TARGET).bin lua/md.lua.in tools/bin2lua.py
	$(PYTHON) tools/bin2lua.py lua/md.lua.in $(TARGET).bin lua/md.lua

# The linker script discards .rela.*, so an absolute relocation into a data
# section is not applied at load time and the pointer reads as its link-time
# value instead of a real address. gcc emits exactly that for initialised
# pointer data -- which is why mdglue.c fills the callbacks struct at runtime
# rather than with an initialiser. Catch it here rather than as a jump to a
# small integer on console, where the only symptom would be a silent hang.
check: $(OBJS)
	@READELF="$(READELF)" sh tools/check_relocs.sh $(OBJS)

size: $(TARGET).elf
	@$(READELF) -SW $(TARGET).elf | awk '/\.text|\.rodata|\.data|\.bss/ {printf "  %-12s %8d\n", $$2, strtonum("0x" $$6)}'

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin lua/md.lua 

.PHONY: all clean check size
