#ifndef MDGLUE_H
#define MDGLUE_H

#include "core.h"

/* The host layer clownmdemu expects.
 *
 * clownmdemu is a pure core: it allocates nothing, opens nothing and keeps no
 * global state beyond a handful of lookup tables. Everything it needs arrives
 * through one callbacks struct, and everything it produces leaves the same
 * way. This file is that struct, made concrete against the PS5. */

/* Bit positions match the ClownMDEmu_Button enum exactly, so input_requested
 * is a shift and a test rather than a switch. The order is fixed by
 * clownmdemu.h -- UP DOWN LEFT RIGHT A B C X Y Z START MODE -- and this must
 * follow it. Twelve buttons is why the pad mask is 16 bits here where the
 * Game Boy port could use 8.
 *
 * X, Y, Z and MODE only exist on the 6-button pad. Games poll for it and fall
 * back to 3-button when it is absent, so exposing them is free. */
#define MD_BTN_UP     0x0001
#define MD_BTN_DOWN   0x0002
#define MD_BTN_LEFT   0x0004
#define MD_BTN_RIGHT  0x0008
#define MD_BTN_A      0x0010
#define MD_BTN_B      0x0020
#define MD_BTN_C      0x0040
#define MD_BTN_X      0x0080
#define MD_BTN_Y      0x0100
#define MD_BTN_Z      0x0200
#define MD_BTN_START  0x0400
#define MD_BTN_MODE   0x0800

/* Out-of-band pad values. 0xFFFF and 0xFFFE cannot collide with a real button
   combination because only the low 12 bits are ever set. */
#define MD_PAD_QUIT   0xFFFF
#define MD_PAD_MENU   0xFFFE

struct md_audio_iface {
    void *gadget;
    void *audio_out;    /* sceAudioOutOutput */
    s32   handle;
};

/* Called once from _start, after shim_init. Builds clownmdemu's lookup tables
   (about 96KB of .bss) and wires up the callbacks. */
void md_glue_init(const struct md_audio_iface *audio);

/* Load a .bin/.md/.gen/.smd from an absolute path. Returns 1 on success.
   Rewinds the shim arena first, so the previous game's ROM goes away. */
int  md_load_rom(const char *path);

/* Runs the emulated machine for exactly one frame. ClownMDEmu_Iterate is
   frame-granular by construction, so unlike the Game Boy port there is no
   cycle budget to enforce here. The finished picture is in textureImage. */
void md_run_frame(void);

/* Hands the frame's resampled and mixed audio to sceAudioOutOutput. Blocking,
   and that is deliberate: it is the emulator's master clock. */
void md_audio_flush(void);

/* Applies a bitmask of MD_BTN_* to the emulated pad. */
void md_set_input(u16 mask);

/* Writes cartridge SRAM back out. Safe to call when the cartridge has none,
   which is most of them. */
void md_save_store(void);

/* Non-zero makes md_run_frame narrate its phases. */
extern int md_trace;

/* True once a ROM is running. */
int  md_is_loaded(void);

/* Title from the cartridge header, trimmed. */
const char *md_title(void);

/* The finished frame, always MD_FB_W stride regardless of the live width. */
extern u32 textureImage[MD_FB_W * MD_FB_H];

/* Input frames consumed from each chip and output frames handed to
   sceAudioOut. Their ratio is the resampler's correctness check. */
extern u64 md_fm_frames_in;
extern u64 md_psg_frames_in;
extern u64 md_out_frames;

/* Loudest sample submitted so far: proves the stream is not silence. */
extern s32 md_out_peak;

/* Live picture dimensions, updated by the scanline callback every frame.
   The blitter reads these to centre and scale. */
extern int md_fb_w;
extern int md_fb_h;

#endif
