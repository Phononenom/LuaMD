#ifndef CORE_H
#define CORE_H

/* -------------------------------------------------------------------------
 * LuaMD -- Mega Drive / Genesis emulator running as native x86_64 shellcode
 * on PS5 through the Luac0re JIT exploit.
 *
 * Emulation core: clownmdemu by Clownacy (AGPLv3 -- see LICENSE).
 * PS5 runtime pattern: EmuC0re by egycnq / EgyDevTeam.
 * Delivery substrate: Luac0re by Gezine.
 * Runtime carried over from LuaGB, the Game Boy port that came first.
 * ------------------------------------------------------------------------- */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef long           s64;
typedef int            s32;
typedef short          s16;
typedef signed char    s8;

/* Offsets into the host game's eboot (Star Wars Racer Revenge). Properties of
   the host title, not of the emulator, so they carry over unchanged. */
#define GADGET_OFFSET    0x31AA9     /* the native_call trampoline gadget    */
#define LIBKERNEL_HANDLE 0x2001
#define EBOOT_GS_THREAD  0x057F89B0  /* pthread handle of the ps2emu GS thread */
#define EBOOT_VIDOUT     0x02d695d0  /* ps2emu's own sceVideoOut handle        */

/* Output geometry. */
#define SCR_W       1920
#define SCR_H       1080
#define FB_SIZE     (SCR_W * SCR_H * 4)
#define FB_ALIGNED  ((FB_SIZE + 0x1FFFFF) & ~0x1FFFFF)
#define FB_TOTAL    (FB_ALIGNED * 2)

/* The Mega Drive's picture is not one fixed size the way the Game Boy's is.
 * Width is 320 (H40) or 256 (H32) and height is 224 (V28) or 240 (V30), all
 * switchable by the running game -- Sonic 2's split-screen versus mode flips
 * height mid-frame. So the framebuffer is allocated at the maximum and the
 * live dimensions arrive with every scanline callback.
 *
 * 4x is the largest integer scale that still fits the tallest mode: 240*4=960
 * against 1080. 320*4=1280 leaves pillarboxing, which is correct -- the Mega
 * Drive is a 4:3 machine and stretching it to fill 1920 would be wrong.
 * Centring is computed per frame from the live dimensions, so an H32 game
 * stays centred rather than hugging the left edge. */
#define MD_FB_W  320
#define MD_FB_H  240
#define SCALE    4

/* The menu is drawn at the same resolution and goes through the same scaler,
   so the picker and the game reach the screen by one code path. */
#define UI_W   MD_FB_W
#define UI_H   MD_FB_H

/* Audio. sceAudioOut is opened at 48kHz stereo; the Mega Drive produces two
 * streams at neither rate -- see mdglue.c. SAMPLES_PER_BUF is the
 * sceAudioOutOutput grain. */
#define SAMPLE_RATE      48000
#define SAMPLES_PER_BUF  256
#define AUDIO_S16_STEREO 1

/* Native rates of the two sound chips, NTSC. Both are exact integer divisions
   of the master clock, and both are above 48kHz, so the resampler only ever
   has to decimate. From clownmdemu.h:
     FM  = (53693175 / 7) / 144 = 53267 Hz, stereo
     PSG = (53693175 / 15) / 16 = 223721 Hz, mono */
#define MD_FM_RATE   53267
#define MD_PSG_RATE  223721

/* Everything lives in the game's savedata and nowhere else.
 *
 * /av_contents/content_tmp is deliberately NOT used: it is per-sandbox scratch
 * outside the save, so anything left there is unmanaged and can vanish. The
 * savedata image is the unit that gets backed up, resigned and reasoned about,
 * so ROMs are read only from ROM_DIR and cartridge SRAM is written only to
 * SAVE_DIR. Both are inside the image; current budget is ~49MB total. */
#define ROM_DIR   "/savedata0/roms/"
#define SAVE_DIR  "/savedata0/saves/"

/* Calls a native function through the host eboot's argument-shuffling gadget.
   rdi = gadget, rsi = target fn, then the six real arguments. The gadget
   expects the callee in rbx and the first argument already in rdi. */
__attribute__((naked))
static u64 native_call(void *gadget, void *fn,
                       u64 a1, u64 a2, u64 a3,
                       u64 a4, u64 a5, u64 a6)
{
    __asm__ volatile (
        "pushq %%rbx\n\t"
        "movq %%rsi, %%rbx\n\t"
        "movq %%rdi, %%rax\n\t"
        "movq %%rdx, %%rdi\n\t"
        "movq %%rcx, %%rsi\n\t"
        "movq %%r8,  %%rdx\n\t"
        "movq %%r9,  %%rcx\n\t"
        "movq 16(%%rsp), %%r8\n\t"
        "movq 24(%%rsp), %%r9\n\t"
        "callq *%%rax\n\t"
        "popq %%rbx\n\t"
        "retq" ::: "memory"
    );
}

static void *resolve_sym(void *gadget, void *dlsym_fn, s32 handle, const char *name) {
    void *addr = 0;
    native_call(gadget, dlsym_fn, (u64)handle, (u64)name, (u64)&addr, 0, 0, 0);
    return addr;
}

#define NC  native_call
#define SYM resolve_sym

/* Arguments handed in by the Lua launcher. Layout is mirrored by lua/md.lua
 * -- keep the two in sync. */
struct ext_args {
    s64 status;
    s64 step;
    u32 frame_count;
    u32 _pad;
    s32 log_fd;
    s32 pad_fd;
    u8  log_addr[16];
    u64 dbg[8];
};

/* One entry in the ROM picker. */
#define MAX_ROMS 4096
#define MAX_NAME 34

struct rom_entry {
    char filename[128];
    char display[MAX_NAME];
};

#endif
