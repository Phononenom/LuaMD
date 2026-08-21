/* clownmdemu <-> PS5 host layer.
 *
 * clownmdemu is a pure core: it allocates nothing, opens nothing, and reaches
 * the outside world only through one callbacks struct. That makes it a good
 * fit for a payload with no libc, but it also means every single thing the
 * machine produces -- pixels, both audio streams, controller reads -- arrives
 * here as a callback in the middle of ClownMDEmu_Iterate. */

#include "core.h"
#include "mdglue.h"
#include "shim.h"
#include "savedata.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "clownmdemu.h"

/* ------------------------------------------------------------- state ----- */

static ClownMDEmu                     md;
static ClownMDEmu_Callbacks           cbs;
static ClownMDEmu_InitialConfiguration cfg;
static struct md_audio_iface          AUD;

static int  loaded;
static u16  pad_state;
static char title_buf[64];
static char save_path[224];

int md_trace;
int md_fb_w = 320;
int md_fb_h = 224;

u32 textureImage[MD_FB_W * MD_FB_H];

/* 4 palette lines x 16 colours x 3 brightnesses = 192 entries. The index the
   VDP hands us is (brightness << 6) | colour, so it never exceeds 190, but
   the table is 256 deep so a masked index can never walk off it. */
static u32 palette[256];

/* The cartridge, as 16-bit big-endian words. clownmdemu indexes the buffer by
   WORD, not by byte -- cartridge_buffer_length is in words too, which is easy
   to get wrong in the direction of a buffer twice as long as it should be. */
static cc_u16l *rom_words;
static u32      rom_word_count;

/* ------------------------------------------------------------- audio ----- */

/* The Mega Drive has two sound chips running at unrelated rates, neither of
 * them 48kHz and neither an integer multiple of the other:
 *
 *     YM2612 (FM)   53267 Hz  stereo
 *     SN76489 (PSG) 223721 Hz mono
 *
 * Both are above the output rate, so both only ever need decimating. Each gets
 * its own box-filter resampler feeding its own ring, and the two rings are
 * mixed at flush time. Mixing before resampling is not an option: the chips
 * are clocked independently and their callbacks arrive interleaved, several
 * times per video frame, with unrelated sample counts.
 *
 * Rings are deliberately generous. Draining is limited to whatever BOTH rings
 * can supply, so if one runs ahead it has to sit in its ring until the other
 * catches up. 8192 frames is about 170ms of slack. */
#define RING_FRAMES 8192

static s32 ring_fm[RING_FRAMES * 2];
static s32 ring_psg[RING_FRAMES * 2];
static u32 fm_head, fm_tail, psg_head, psg_tail;

/* Running totals, exposed so both the offline harness and the console log can
   check the resampler is producing the right RATIO rather than merely
   producing something. FM in / out should settle at 53267:48000 = 1.110, and
   PSG in / out at 223721:48000 = 4.661. */
u64 md_fm_frames_in, md_psg_frames_in, md_out_frames;

/* Loudest absolute sample handed to sceAudioOut since the last ROM load.
   Rates being right does not mean anything is audible -- a resampler faithfully
   decimating silence passes every ratio check there is. */
s32 md_out_peak;

static u32 fm_pos, psg_pos;
static s64 fm_acc_l, fm_acc_r, psg_acc;
static u32 fm_acc_n, psg_acc_n;

/* Scratch the generators fill. Sized well past one video frame's worth
   (53267/59.92 = 889 FM frames, 3734 PSG frames) so the chunking loop below
   almost never has to go round twice. */
#define GEN_CHUNK 4096
static cc_s16l gen_buf[GEN_CHUNK * 2];

static s16 clamp16(s32 v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (s16)v;
}

static void audio_reset(void) {
    md_fm_frames_in = md_psg_frames_in = md_out_frames = 0;
    md_out_peak = 0;
    fm_head = fm_tail = psg_head = psg_tail = 0;
    fm_pos = psg_pos = 0;
    fm_acc_l = fm_acc_r = psg_acc = 0;
    fm_acc_n = psg_acc_n = 0;
    memset(ring_fm, 0, sizeof(ring_fm));
    memset(ring_psg, 0, sizeof(ring_psg));
}

/* Box-filter decimation. Accumulate input samples, and every time the phase
   accumulator crosses the input rate emit their average. Over N input samples
   this emits exactly round(N * 48000 / in_rate) outputs, with the averaging
   doing double duty as the anti-aliasing filter.
   The FM ratio is only 1.11, so its boxes hold one or two samples; the PSG
   ratio is 4.66, so its boxes hold four or five. */
static void fm_push(s32 l, s32 r) {
    md_fm_frames_in++;
    fm_acc_l += l;
    fm_acc_r += r;
    fm_acc_n++;

    fm_pos += SAMPLE_RATE;
    if (fm_pos >= MD_FM_RATE) {
        fm_pos -= MD_FM_RATE;

        if (fm_head - fm_tail < RING_FRAMES) {
            u32 i = (fm_head % RING_FRAMES) * 2;
            ring_fm[i + 0] = (s32)(fm_acc_l / (s64)fm_acc_n);
            ring_fm[i + 1] = (s32)(fm_acc_r / (s64)fm_acc_n);
            fm_head++;
        }
        fm_acc_l = fm_acc_r = 0;
        fm_acc_n = 0;
    }
}

static void psg_push(s32 m) {
    md_psg_frames_in++;
    psg_acc += m;
    psg_acc_n++;

    psg_pos += SAMPLE_RATE;
    if (psg_pos >= MD_PSG_RATE) {
        psg_pos -= MD_PSG_RATE;

        if (psg_head - psg_tail < RING_FRAMES) {
            u32 i = (psg_head % RING_FRAMES) * 2;
            s32 v = (s32)(psg_acc / (s64)psg_acc_n);
            ring_psg[i + 0] = v;      /* the PSG is mono; both ears get it */
            ring_psg[i + 1] = v;
            psg_head++;
        }
        psg_acc = 0;
        psg_acc_n = 0;
    }
}

/* Both generators ACCUMULATE into the buffer with += and neither clears it
 * first -- FM_OutputSamples sums six channels, PSG_Update sums four. Handing
 * them a dirty buffer does not fail loudly; it just adds this frame's audio to
 * whatever was left in the scratch, which sounds like the emulator is
 * screaming. The memset is not defensive, it is part of the contract. */
static void cb_fm(void *ud, ClownMDEmu *emu, size_t total_frames,
                  void (*generate)(ClownMDEmu *, cc_s16l *, size_t)) {
    (void)ud;

    while (total_frames != 0) {
        size_t n = total_frames > GEN_CHUNK ? GEN_CHUNK : total_frames;
        size_t i;

        memset(gen_buf, 0, n * 2 * sizeof(gen_buf[0]));
        generate(emu, gen_buf, n);

        for (i = 0; i < n; i++)
            fm_push(gen_buf[i * 2 + 0], gen_buf[i * 2 + 1]);

        total_frames -= n;
    }
}

static void cb_psg(void *ud, ClownMDEmu *emu, size_t total_frames,
                   void (*generate)(ClownMDEmu *, cc_s16l *, size_t)) {
    (void)ud;

    while (total_frames != 0) {
        size_t n = total_frames > GEN_CHUNK ? GEN_CHUNK : total_frames;
        size_t i;

        memset(gen_buf, 0, n * sizeof(gen_buf[0]));   /* mono: one per frame */
        generate(emu, gen_buf, n);

        for (i = 0; i < n; i++)
            psg_push(gen_buf[i]);

        total_frames -= n;
    }
}

void md_audio_flush(void) {
    static s16 grain[SAMPLES_PER_BUF * 2];

    u32 have_fm  = fm_head  - fm_tail;
    u32 have_psg = psg_head - psg_tail;
    u32 n = have_fm < have_psg ? have_fm : have_psg;

    while (n >= SAMPLES_PER_BUF) {
        int i;
        for (i = 0; i < SAMPLES_PER_BUF; i++) {
            u32 fi = ((fm_tail  + (u32)i) % RING_FRAMES) * 2;
            u32 pi = ((psg_tail + (u32)i) % RING_FRAMES) * 2;

            /* The divisors come from clownmdemu.h and exist so the two streams
               sum without clipping: FM is 1, PSG is 8. */
            s32 l = ring_fm[fi + 0] / CLOWNMDEMU_FM_VOLUME_DIVISOR
                  + ring_psg[pi + 0] / CLOWNMDEMU_PSG_VOLUME_DIVISOR;
            s32 r = ring_fm[fi + 1] / CLOWNMDEMU_FM_VOLUME_DIVISOR
                  + ring_psg[pi + 1] / CLOWNMDEMU_PSG_VOLUME_DIVISOR;

            grain[i * 2 + 0] = clamp16(l);
            grain[i * 2 + 1] = clamp16(r);

            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > md_out_peak) md_out_peak = l;
            if (r > md_out_peak) md_out_peak = r;
        }

        fm_tail  += SAMPLES_PER_BUF;
        psg_tail += SAMPLES_PER_BUF;
        n        -= SAMPLES_PER_BUF;
        md_out_frames += SAMPLES_PER_BUF;

        /* Blocking, and that is the point: this call is what paces the
           emulator to the Mega Drive's 59.92Hz instead of the display's 60. */
        if (AUD.audio_out && AUD.handle >= 0)
            NC(AUD.gadget, AUD.audio_out, (u64)AUD.handle, (u64)grain, 0, 0, 0, 0);
    }
}

/* ------------------------------------------------------------- video ----- */

/* CRAM entries reach us already normalised to 12 bits, one nibble per
   channel. The Mega Drive packs colour as ----BBB-GGG-RRR-, so as nibbles the
   order is R, G, B from the bottom. Scaling a nibble by 17 maps 0..15 onto
   0..255 exactly (0xF * 17 = 255), which is why it is 17 and not 16. */
static void cb_colour(void *ud, cc_u16f index, cc_u16f colour) {
    u32 r = (u32)((colour >> 0) & 0xF) * 17;
    u32 g = (u32)((colour >> 4) & 0xF) * 17;
    u32 b = (u32)((colour >> 8) & 0xF) * 17;

    (void)ud;
    palette[index & 0xFF] = 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Called up to TWICE per scanline -- once for the window plane's span and
 * once for plane A's -- with disjoint [left, right) ranges, and skipped
 * entirely for an empty span. The two calls share one scanline buffer that is
 * overwritten between them, so the span has to be consumed now; there is no
 * "end of line" callback to defer to.
 *
 * Copying the full width instead of just the span would therefore paint one
 * plane's pixels over the other's. */
static void cb_scanline(void *ud, cc_u16f scanline, const cc_u8l *pixels,
                        cc_u16f left, cc_u16f right,
                        cc_u16f screen_width, cc_u16f screen_height) {
    u32 *row;
    cc_u16f x;

    (void)ud;

    /* Geometry is not fixed and games change it mid-run, so it is republished
       on every line rather than latched at reset. */
    md_fb_w = screen_width  > MD_FB_W ? MD_FB_W : (int)screen_width;
    md_fb_h = screen_height > MD_FB_H ? MD_FB_H : (int)screen_height;

    if (scanline >= MD_FB_H) return;
    if (right > MD_FB_W) right = MD_FB_W;
    if (left >= right)   return;

    row = &textureImage[(u32)scanline * MD_FB_W];
    for (x = left; x < right; x++)
        row[x] = palette[pixels[x] & 0xFF];
}

/* ------------------------------------------------------------- input ----- */

static cc_bool cb_input(void *ud, cc_u8f player_id, ClownMDEmu_Button button) {
    (void)ud;
    if (player_id != 0) return cc_false;          /* player 2 is unwired */
    return (pad_state & (1u << (unsigned)button)) != 0 ? cc_true : cc_false;
}

void md_set_input(u16 mask) {
    pad_state = mask;
}

/* ------------------------------------------------- Mega CD stubs --------- */

/* LuaMD is cartridge-only: cd_add_on_enabled is never set, so the Mega CD
 * address ranges are never mapped and nothing below is ever reached. They
 * exist because the callbacks struct has no optional members -- a null here
 * would be a jump to zero the moment a disc image did appear.
 *
 * The save_file_* group belongs to this family too, which is not obvious:
 * every caller lives in bus-sub-m68k.c and they serve Mega CD Backup RAM, NOT
 * cartridge SRAM. Cartridge saves are handled by md_save_store reading
 * external_ram directly. */
static void    cb_pcm(void *ud, ClownMDEmu *e, size_t n, void (*g)(ClownMDEmu *, cc_s16l *, size_t))  { (void)ud; (void)e; (void)n; (void)g; }
static void    cb_cdda(void *ud, ClownMDEmu *e, size_t n, void (*g)(ClownMDEmu *, cc_s16l *, size_t)) { (void)ud; (void)e; (void)n; (void)g; }
static void    cb_cd_seeked(void *ud, cc_u32f sector)                     { (void)ud; (void)sector; }
static void    cb_cd_sector_read(void *ud, cc_u16l *buffer)               { (void)ud; (void)buffer; }
static cc_bool cb_cd_track_seeked(void *ud, cc_u16f t, ClownMDEmu_CDDAMode m) { (void)ud; (void)t; (void)m; return cc_false; }
static size_t  cb_cd_audio_read(void *ud, cc_s16l *buf, size_t n)         { (void)ud; (void)buf; (void)n; return 0; }

static cc_bool cb_save_open_read(void *ud, const char *f)                 { (void)ud; (void)f; return cc_false; }
static cc_s16f cb_save_read(void *ud)                                     { (void)ud; return -1; }
static cc_bool cb_save_open_write(void *ud, const char *f)                { (void)ud; (void)f; return cc_false; }
static void    cb_save_written(void *ud, cc_u8f byte)                     { (void)ud; (void)byte; }
static void    cb_save_closed(void *ud)                                   { (void)ud; }
static cc_bool cb_save_removed(void *ud, const char *f)                   { (void)ud; (void)f; return cc_false; }
static cc_bool cb_save_size(void *ud, const char *f, size_t *sz)          { (void)ud; (void)f; (void)sz; return cc_false; }

/* The core logs bus errors and unimplemented instructions through here. The
   format string goes out without its arguments: a real vsnprintf is a lot of
   code to carry for diagnostics, and the bare message already identifies the
   site. */
static void cb_log(void *ud, const char *format, va_list arg) {
    (void)ud; (void)arg;
    if (md_trace) { klog("md: "); klog(format); klog("\n"); }
}

/* ------------------------------------------------------------ loading ---- */

static u8 rom_byte(u32 off) {
    u32 w = off >> 1;
    if (w >= rom_word_count) return 0;
    return (off & 1) ? (u8)(rom_words[w] & 0xFF) : (u8)(rom_words[w] >> 8);
}

/* Cartridge headers carry two titles, domestic at 0x120 and overseas at 0x150,
   both 48 bytes of space-padded ASCII. */
static void read_title(void) {
    int i, len = 0;
    u32 base = 0x150;

    for (i = 0; i < 48; i++) {
        u8 c = rom_byte(base + (u32)i);
        if (c < 0x20 || c > 0x7E) c = ' ';
        title_buf[i] = (char)c;
    }
    title_buf[48] = 0;

    for (i = 0; i < 48; i++)
        if (title_buf[i] != ' ') len = i + 1;
    title_buf[len] = 0;

    if (len == 0) {
        for (i = 0; i < 48; i++) {
            u8 c = rom_byte(0x120 + (u32)i);
            title_buf[i] = (c < 0x20 || c > 0x7E) ? ' ' : (char)c;
        }
        title_buf[48] = 0;
        for (i = 0; i < 48; i++)
            if (title_buf[i] != ' ') len = i + 1;
        title_buf[len] = 0;
    }

    if (title_buf[0] == 0) strcpy(title_buf, "UNKNOWN");
}

/* Region field is three bytes at 0x1F0. Older carts spell it out as some
   subset of "JUE"; newer ones use a single hex digit bitfield where bit 0 is
   Japan, bit 2 the Americas and bit 3 Europe. Both forms are handled by just
   looking for the letters, since the hex-digit form still lands on characters
   that are not J, U or E and falls through to the default.
   Preference order is U, then J, then E, because only E implies 50Hz and
   running a multi-region cart at PAL speed would be a 17% slowdown. */
static void pick_region(void) {
    int i;
    int has_u = 0, has_j = 0, has_e = 0;

    for (i = 0; i < 3; i++) {
        u8 c = rom_byte(0x1F0 + (u32)i);
        if (c == 'U') has_u = 1;
        if (c == 'J') has_j = 1;
        if (c == 'E') has_e = 1;
        if (c >= '0' && c <= '9') {
            int v = c - '0';
            if (v & 1) has_j = 1;
            if (v & 4) has_u = 1;
            if (v & 8) has_e = 1;
        } else if (c >= 'A' && c <= 'F') {
            int v = c - 'A' + 10;
            if (v & 1) has_j = 1;
            if (v & 4) has_u = 1;
            if (v & 8) has_e = 1;
        }
    }

    if (has_u) {
        cfg.general.region = CLOWNMDEMU_REGION_OVERSEAS;
        cfg.general.tv_standard = CLOWNMDEMU_TV_STANDARD_NTSC;
    } else if (has_j) {
        cfg.general.region = CLOWNMDEMU_REGION_DOMESTIC;
        cfg.general.tv_standard = CLOWNMDEMU_TV_STANDARD_NTSC;
    } else if (has_e) {
        cfg.general.region = CLOWNMDEMU_REGION_OVERSEAS;
        cfg.general.tv_standard = CLOWNMDEMU_TV_STANDARD_PAL;
    } else {
        cfg.general.region = CLOWNMDEMU_REGION_OVERSEAS;
        cfg.general.tv_standard = CLOWNMDEMU_TV_STANDARD_NTSC;
    }
}

/* SMD is a dump format from the Super Magic Drive copier, and it is not a
 * container -- the bytes themselves are permuted. After a 512 byte header the
 * file is a series of 16KB blocks, each holding all 8192 odd bytes first and
 * all 8192 even bytes second.
 *
 * Detected by size rather than by header contents: an SMD file is always 512
 * bytes past a multiple of 16KB, and no raw dump is. */
static int is_smd(long size) {
    return size > 512 && ((size - 512) % 16384) == 0;
}

static void smd_deinterleave(const u8 *in, u8 *out, long blocks) {
    long b;
    for (b = 0; b < blocks; b++) {
        const u8 *src = in + b * 16384;
        u8       *dst = out + b * 16384;
        int i;
        for (i = 0; i < 8192; i++) {
            dst[i * 2 + 1] = src[i];
            dst[i * 2 + 0] = src[i + 8192];
        }
    }
}

static void build_save_path(const char *rom_path) {
    const char *base = rom_path;
    const char *p;
    int i;
    char *dot;

    for (p = rom_path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    strcpy(save_path, SAVE_DIR);
    i = (int)strlen(save_path);
    strncpy(save_path + i, base, sizeof(save_path) - (size_t)i - 8);
    save_path[sizeof(save_path) - 8] = 0;

    dot = strrchr(save_path + i, '.');
    if (dot) *dot = 0;
    strcat(save_path, ".srm");
}

/* Cartridge SRAM is not routed through the save_file_* callbacks -- those are
   Mega CD only. It lives in state.external_ram, sized and flagged by
   SetUpExternalRAM from the 'RA' block in the cartridge header, so it can only
   be read after HardReset. */
static u32 sram_size(void) {
    if (!md.state.external_ram.non_volatile) return 0;
    if (md.state.external_ram.size == 0)     return 0;
    if (md.state.external_ram.size > sizeof(md.state.external_ram.buffer))
        return (u32)sizeof(md.state.external_ram.buffer);
    return (u32)md.state.external_ram.size;
}

static void md_save_load(void) {
    u32 want = sram_size();
    FILE *f;
    long size;

    if (!want || !save_path[0]) return;

    /* Reading needs no write window: the container is already mounted
       read-only. */
    f = fopen(save_path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);

    if (size > (long)want) size = (long)want;
    if (size > 0) fread(md.state.external_ram.buffer, 1, (size_t)size, f);
    fclose(f);

    printf("Save: loaded %s (%d bytes)\n", save_path, (int)size);
}

static int copy_file(const char *from, const char *to) {
    static u8 buf[8192];
    FILE *in, *out;
    size_t n, total = 0;

    in = fopen(from, "rb");
    if (!in) return 0;

    out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) break;
        total += n;
    }
    fclose(in);
    fclose(out);
    return total > 0;
}

void md_save_store(void) {
    const char *staging = "/av_contents/content_tmp/md_tmp.srm";
    u32 want = sram_size();
    FILE *t;
    int ok;

    if (!loaded || !want || !save_path[0]) return;

    /* Build the file outside the container first. The write window unmounts
       and remounts savedata, so it is held open for as little time as
       possible -- do the slow part here, then copy in. */
    t = fopen(staging, "wb");
    if (!t) {
        printf("Save: cannot stage %s\n", staging);
        return;
    }
    fwrite(md.state.external_ram.buffer, 1, want, t);
    fclose(t);

    if (savedata_begin_write() != 0) {
        printf("Save: no write window, progress left at %s\n", staging);
        return;
    }

    ok = copy_file(staging, save_path);

    /* The unmount inside this call is what actually commits the bytes. */
    savedata_end_write();

    printf(ok ? "Save: wrote %s\n" : "Save: copy into container failed for %s\n",
           save_path);
}

int md_load_rom(const char *path) {
    FILE *f;
    long size, raw_size;
    u8 *raw;
    u32 i;

    loaded = 0;
    rom_words = 0;
    rom_word_count = 0;
    save_path[0] = 0;

    /* Everything the previous game allocated goes away in one step. */
    arena_reset();
    memset(textureImage, 0, sizeof(textureImage));
    memset(palette, 0, sizeof(palette));
    audio_reset();

    f = fopen(path, "rb");
    if (!f) { printf("ROM: cannot open %s\n", path); return 0; }

    fseek(f, 0, SEEK_END);
    raw_size = ftell(f);
    rewind(f);

    if (raw_size < 0x200) {
        printf("ROM: %s is too small to be a cartridge\n", path);
        fclose(f);
        return 0;
    }

    raw = (u8 *)malloc((size_t)raw_size);
    if (!raw) { printf("ROM: out of arena for %s\n", path); fclose(f); return 0; }

    if (fread(raw, 1, (size_t)raw_size, f) != (size_t)raw_size) {
        printf("ROM: short read on %s\n", path);
        fclose(f);
        return 0;
    }
    fclose(f);

    size = raw_size;

    if (is_smd(raw_size)) {
        long blocks = (raw_size - 512) / 16384;
        u8 *flat = (u8 *)malloc((size_t)(blocks * 16384));
        if (!flat) { printf("ROM: out of arena deinterleaving SMD\n"); return 0; }
        smd_deinterleave(raw + 512, flat, blocks);
        raw = flat;
        size = blocks * 16384;
        printf("ROM: SMD detected, deinterleaved %d blocks\n", (int)blocks);
    }

    /* An odd trailing byte cannot form a word and is dropped rather than read
       past the buffer. */
    rom_word_count = (u32)(size / 2);
    rom_words = (cc_u16l *)malloc((size_t)rom_word_count * sizeof(cc_u16l));
    if (!rom_words) { printf("ROM: out of arena for word buffer\n"); return 0; }

    /* 68000 memory is big-endian; the file is a byte image of it. */
    for (i = 0; i < rom_word_count; i++)
        rom_words[i] = (cc_u16l)(((u32)raw[i * 2] << 8) | raw[i * 2 + 1]);

    /* Order matters, and getting it wrong fails silently.
     *
     * ClownMDEmu_Initialise sets cartridge_buffer back to NULL -- it takes the
     * configuration, so it has to run after pick_region, but that also means
     * it has to run BEFORE SetCartridge or it throws the cartridge away. The
     * symptom is not an error: the VDP still runs and still renders, so the
     * picture comes out as a blank screen at the default H32 geometry while
     * the 68000 quietly executes zeroes.
     *
     * HardReset comes last because it runs SetUpExternalRAM, which reads the
     * SRAM metadata out of the cartridge header and therefore needs the
     * cartridge already in place.
     *
     * read_title and pick_region read rom_words directly rather than through
     * clownmdemu, so they are free of this ordering. */
    read_title();
    pick_region();

    ClownMDEmu_Initialise(&md, &cfg, &cbs);
    ClownMDEmu_SetCartridge(&md, rom_words, rom_word_count);
    ClownMDEmu_HardReset(&md, cc_true, cc_false);

    if (rom_byte(0x100) != 'S' || rom_byte(0x101) != 'E' ||
        rom_byte(0x102) != 'G' || rom_byte(0x103) != 'A')
        printf("ROM: no SEGA signature at 0x100 -- loading anyway\n");

    build_save_path(path);
    md_save_load();

    printf("ROM: %s\n", title_buf);
    printf("ROM: %d KB, %s, %s\n",
           (int)(size / 1024),
           cfg.general.tv_standard == CLOWNMDEMU_TV_STANDARD_PAL ? "PAL 50Hz" : "NTSC 60Hz",
           sram_size() ? "battery SRAM" : "no SRAM");

    loaded = 1;
    return 1;
}

/* ------------------------------------------------------------- frame ----- */

void md_run_frame(void) {
    if (!loaded) return;

    if (md_trace) klog("md: iterate begin\n");
    ClownMDEmu_Iterate(&md);
    if (md_trace) klog("md: iterate done\n");
}

int md_is_loaded(void) { return loaded; }

const char *md_title(void) { return title_buf; }

/* --------------------------------------------------------------- init ---- */

void md_glue_init(const struct md_audio_iface *audio) {
    AUD = *audio;

    /* Assigned here, never as an initialiser. linker.ld discards .rela.*, so
       a statically initialised function pointer would keep its link-time
       value -- a small file offset -- and the first call through it would
       jump into unmapped memory. tools/check_relocs.sh fails the build if any
       of these slip back into .data. */
    memset(&cbs, 0, sizeof(cbs));
    cbs.user_data                = 0;
    cbs.colour_updated           = cb_colour;
    cbs.scanline_rendered        = cb_scanline;
    cbs.input_requested          = cb_input;
    cbs.fm_audio_to_be_generated = cb_fm;
    cbs.psg_audio_to_be_generated = cb_psg;
    cbs.pcm_audio_to_be_generated = cb_pcm;
    cbs.cdda_audio_to_be_generated = cb_cdda;
    cbs.cd_seeked                = cb_cd_seeked;
    cbs.cd_sector_read           = cb_cd_sector_read;
    cbs.cd_track_seeked          = cb_cd_track_seeked;
    cbs.cd_audio_read            = cb_cd_audio_read;
    cbs.save_file_opened_for_reading = cb_save_open_read;
    cbs.save_file_read           = cb_save_read;
    cbs.save_file_opened_for_writing = cb_save_open_write;
    cbs.save_file_written        = cb_save_written;
    cbs.save_file_closed         = cb_save_closed;
    cbs.save_file_removed        = cb_save_removed;
    cbs.save_file_size_obtained  = cb_save_size;

    /* Zero is the right default throughout: NTSC, domestic, no widescreen,
       nothing disabled. pick_region overrides the first two per cartridge. */
    memset(&cfg, 0, sizeof(cfg));

    ClownMDEmu_SetLogCallback(cb_log, 0);

    /* Builds the Z80 and VDP lookup tables -- about 96KB of .bss, generated
       once. The FM and PSG tables upstream generates here are already
       compiled in as constants, which is why this port needs no libm. */
    ClownMDEmu_Constant_Initialise();

    ClownMDEmu_Initialise(&md, &cfg, &cbs);

    audio_reset();
    pad_state = 0;
    loaded = 0;
}
