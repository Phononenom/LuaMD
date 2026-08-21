#include "core.h"
#include "shim.h"
#include "mdglue.h"
#include "ui.h"
#include "ftp.h"
#include "savedata.h"

#define VERSION_STR "LUAMD v0.1"

/* Arena backing the shim's malloc. A Mega Drive ROM is held twice while it
   loads -- once as the raw byte image and once as the big-endian word array
   clownmdemu indexes -- so an 8MB cartridge peaks at 16MB, and an SMD file
   needs a third buffer while it is deinterleaved. */
#define ARENA_SIZE (32 * 1024 * 1024)

/* Frames R1 must be held before quitting actually happens. ~1s at 60Hz.
   See the play loop: an accidental quit costs a full Luac0re relaunch. */
#define QUIT_HOLD_FRAMES 60

static const char RESP_204K[] = "HTTP/1.1 204\r\nConnection:keep-alive\r\nAccess-Control-Allow-Origin:*\r\n\r\n";
static const char RESP_CORS[] = "HTTP/1.1 204\r\nAccess-Control-Allow-Origin:*\r\nAccess-Control-Allow-Methods:POST\r\nConnection:keep-alive\r\n\r\n";

/* Menu is composed here, then goes through blit_scale like an emulated frame. */
static u32 ui_screen[UI_W * UI_H];

/* ------------------------------------------------------- data bootstrap -- */

/* Laid out by linker.ld. Because everything is compiled -fpie, taking the
   address of one of these yields its real runtime address, so no manual base
   arithmetic is needed. */
extern char __data_load[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __got_start[];
extern char __got_end[];

/* Entry point, and also the image's load base: _start is the first thing in
   .text and the script links .text at 0, so its runtime address IS the base. */
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext);

/* FreeBSD mmap flags -- the PS5 kernel is FreeBSD-derived. */
#define BSD_MAP_PRIVATE   0x0002
#define BSD_MAP_FIXED     0x0010
#define BSD_MAP_ANON      0x1000
#define BSD_PROT_RW       0x0003

/* Sends "<label> 0x<hex>\n" using only stack and arguments, so it is usable
   before the .bss that klog() lives in has been mapped. */
static void bmsg(void *G, void *sendto, s32 fd, u8 *sa, const char *label, u64 v) {
    if (!sendto || fd < 0) return;

    char b[128];
    int p = 0;
    while (*label && p < 90) b[p++] = *label++;
    b[p++] = ' '; b[p++] = '0'; b[p++] = 'x';

    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        if (d || started || i == 0) {
            b[p++] = d < 10 ? (char)('0' + d) : (char)('a' + d - 10);
            started = 1;
        }
    }
    b[p++] = '\n';
    NC(G, sendto, (u64)fd, (u64)b, (u64)p, 0, (u64)sa, 16);
}

/* The payload executes from a PROT_READ|PROT_EXECUTE JIT alias, so every
 * global starts out unwritable. This maps anonymous RW memory over the address
 * range the linker reserved for .data and .bss -- placed directly after the
 * code so it stays inside RIP-relative range -- then installs .data's initial
 * contents and clears .bss.
 *
 * Nothing in here may touch a global: that is the very memory being set up.
 * Only arguments, locals and the linker symbols are in play.
 *
 * This memory is deliberately NOT part of the JIT mapping. ps5 JIT shared
 * memory is a scarce per-process resource -- asking for the whole 1.6MB span
 * returns ENOMEM (0x8002000C) inside ps2emu -- whereas plain anonymous pages
 * are cheap. The JIT mapping therefore covers only the 64KB of code, and the
 * writable region is requested separately at the address immediately after it.
 *
 * Two attempts, least destructive first: `addr` without MAP_FIXED is only a
 * hint, so it can never displace an existing mapping. MAP_FIXED is the
 * fallback, and it is what makes the placement non-negotiable.
 */
static int boot_data_region(void *G, void *mmap_fn, void *munmap_fn,
                            void *sendto, s32 fd, u8 *sa) {
    u64 base = (u64)__data_start;
    u64 span = (u64)__bss_end - base;

    if (span == 0) return 1;            /* nothing writable: nothing to do */

    bmsg(G, sendto, fd, sa, "boot: code base", (u64)__data_load);
    bmsg(G, sendto, fd, sa, "boot: want data at", base);
    bmsg(G, sendto, fd, sa, "boot: span", span);

    u64 got = NC(G, mmap_fn, base, span, BSD_PROT_RW,
                 BSD_MAP_ANON | BSD_MAP_PRIVATE, (u64)-1, 0);
    bmsg(G, sendto, fd, sa, "boot: hint mmap gave", got);

    if (got != base) {
        /* Landed somewhere else -- hand it back before forcing the address,
           otherwise the retry just leaks a 1.5MB mapping. */
        if (got != (u64)-1 && munmap_fn)
            NC(G, munmap_fn, got, span, 0, 0, 0, 0);

        got = NC(G, mmap_fn, base, span, BSD_PROT_RW,
                 BSD_MAP_FIXED | BSD_MAP_ANON | BSD_MAP_PRIVATE, (u64)-1, 0);
        bmsg(G, sendto, fd, sa, "boot: MAP_FIXED gave", got);
        if (got != base) return 0;
    }

    /* Copy .data's initial image out of the read-only code region. */
    const char *src = __data_load;
    char *dst = __data_start;
    u64 n = (u64)__data_end - (u64)__data_start;
    for (u64 i = 0; i < n; i++) dst[i] = src[i];

    /* Apply the relocations the discarded .rela.dyn would have done.
     *
     * -static-pie relaxes nearly every GOT reference into a RIP-relative lea,
     * but not all of them: a few real GOT slots survive holding link-time
     * addresses. Since .text is linked at 0, _start's runtime address is the
     * load base, and adding it to each non-zero slot is exactly the
     * R_X86_64_RELATIVE fixup a loader would apply.
     *
     * Zero slots are left alone -- those are the reserved .got.plt entries. */
    u64 img_base = (u64)&_start;
    u64 *gotp    = (u64 *)__got_start;
    u64 got_n    = ((u64)__got_end - (u64)__got_start) / 8;
    u64 fixed    = 0;
    for (u64 i = 0; i < got_n; i++) {
        if (gotp[i]) { gotp[i] += img_base; fixed++; }
    }
    bmsg(G, sendto, fd, sa, "boot: GOT entries relocated", fixed);

    /* Zero .bss. */
    char *b = __bss_start;
    u64 bn = (u64)__bss_end - (u64)__bss_start;
    for (u64 i = 0; i < bn; i++) b[i] = 0;

    return 1;
}

/* ---------------------------------------------------------- fault trap -- */

/* Enough of the PS5's FreeBSD-derived signal ABI to report a fault.
 *   siginfo_t: si_signo 0, si_errno 4, si_code 8, si_pid 12, si_uid 16,
 *              si_status 20, si_addr 24
 *   ucontext_t: uc_sigmask 0 (16 bytes), then mcontext_t, whose mc_rip sits
 *              160 bytes in -- so RIP is at ucontext + 176.
 * Both are sanity-checked against the known code range before being trusted. */
#define SIG_SEGV 11
#define SIG_BUS  10
#define SA_SIGINFO_BSD 0x0040
#define SI_ADDR_OFF 24
#define UC_RIP_OFF  176

static void *fault_G, *fault_sendto, *fault_sigaction;
static s32   fault_fd;
static u8   *fault_sa;

static void fault_handler(int sig, void *info, void *uctx) {
    u64 addr = info ? *(u64 *)((u8 *)info + SI_ADDR_OFF) : 0;
    u64 rip  = uctx ? *(u64 *)((u8 *)uctx + UC_RIP_OFF)  : 0;

    bmsg(fault_G, fault_sendto, fault_fd, fault_sa, "FAULT: signal", (u64)sig);
    bmsg(fault_G, fault_sendto, fault_fd, fault_sa, "FAULT: touched addr", addr);
    bmsg(fault_G, fault_sendto, fault_fd, fault_sa, "FAULT: rip", rip);

    /* Offsets relative to the image make the log directly usable with
       addr2line without knowing where the JIT mapping landed this run. */
    u64 code = (u64)__data_load;              /* end of .text, inside the image */
    bmsg(fault_G, fault_sendto, fault_fd, fault_sa, "FAULT: rip-image_off",
         rip - ((code) & ~0xFFFFULL));
    bmsg(fault_G, fault_sendto, fault_fd, fault_sa, "FAULT: data_start", (u64)__data_start);
    bmsg(fault_G, fault_sendto, fault_fd, fault_sa, "FAULT: bss_end", (u64)__bss_end);

    if (addr >= (u64)__bss_end)
        bmsg(fault_G, fault_sendto, fault_fd, fault_sa,
             "FAULT: PAST bss_end by", addr - (u64)__bss_end);

    /* Hand the signal back to the default action so the process dies on the
       re-executed instruction instead of looping in here forever. */
    if (fault_sigaction) {
        u8 sa[32];
        for (int i = 0; i < 32; i++) sa[i] = 0;   /* handler = SIG_DFL (0) */
        NC(fault_G, fault_sigaction, (u64)sig, (u64)sa, 0, 0, 0, 0);
    }
}

static void install_fault_trap(void *G, void *D, void *sendto, s32 fd, u8 *sa) {
    fault_G = G; fault_sendto = sendto; fault_fd = fd; fault_sa = sa;
    fault_sigaction = SYM(G, D, LIBKERNEL_HANDLE, "sigaction");
    if (!fault_sigaction) { klog("fault trap: no sigaction\n"); return; }

    u8 act[32];
    for (int i = 0; i < 32; i++) act[i] = 0;
    *(u64 *)(act + 0) = (u64)&fault_handler;
    *(u32 *)(act + 8) = SA_SIGINFO_BSD;

    s32 r1 = (s32)NC(G, fault_sigaction, SIG_SEGV, (u64)act, 0, 0, 0, 0);
    s32 r2 = (s32)NC(G, fault_sigaction, SIG_BUS,  (u64)act, 0, 0, 0, 0);
    if (r1 == 0 && r2 == 0) klog("fault trap: armed\n");
    else klog("fault trap: sigaction rejected\n");
}

/* ------------------------------------------------------------ web pad ---- */

static int poll_ready(void *G, void *poll, s32 fd, s32 timeout_ms) {
    u8 pfd[8];
    *(s32 *)pfd = fd;
    *(u16 *)(pfd + 4) = 0x0001;   /* POLLIN */
    *(u16 *)(pfd + 6) = 0;
    return (s32)NC(G, poll, (u64)pfd, 1, (u64)timeout_ms, 0, 0, 0) > 0;
}

static int parse_pad_last(u8 *buf, s32 len) {
    int val = -1;
    for (s32 i = len - 2; i >= 1; i--) {
        if (buf[i] == '/' && buf[i + 1] == 'b') {
            val = 0;
            for (s32 j = i + 2; j < len && j < i + 8; j++) {
                if (buf[j] >= '0' && buf[j] <= '9') val = val * 10 + (buf[j] - '0');
                else break;
            }
            break;
        }
    }
    return val;
}

static int count_posts(u8 *buf, s32 len) {
    int c = 0;
    for (s32 i = 0; i < len - 4; i++)
        if (buf[i] == 'P' && buf[i+1] == 'O' && buf[i+2] == 'S' && buf[i+3] == 'T') c++;
    return c;
}

/* Serves the controller page and drains any pending button POSTs. Returns
   non-zero when pad_out was updated this call. */
static u8 web_handle(void *G, void *poll, void *accept, void *recv,
                     void *send, void *close, void *sso, s32 listen_fd,
                     s32 *keep_fd, u8 *page, u64 page_len, u16 *pad_out) {
    u8 got_input = 0;
    u8 req[512];

    if (*keep_fd >= 0) {
        for (int r = 0; r < 8; r++) {
            if (!poll_ready(G, poll, *keep_fd, 0)) break;
            s32 n = (s32)NC(G, recv, (u64)*keep_fd, (u64)req, 512, 0x80, 0, 0);
            if (n <= 0) { NC(G, close, (u64)*keep_fd, 0,0,0,0,0); *keep_fd = -1; break; }
            int v = parse_pad_last(req, n);
            if (v >= 0) {
                *pad_out = (u16)v;
                got_input = 1;
                int np = count_posts(req, n);
                for (int k = 0; k < np; k++)
                    NC(G, send, (u64)*keep_fd, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        if (!poll_ready(G, poll, listen_fd, 0)) break;

        u8 sa[16]; s32 sa_len = 16;
        s32 client = (s32)NC(G, accept, (u64)listen_fd, (u64)sa, (u64)&sa_len, 0, 0, 0);
        if (client < 0) break;

        if (sso) { s32 one = 1; NC(G, sso, (u64)client, 6, 1, (u64)&one, 4, 0); }

        if (!poll_ready(G, poll, client, 0)) {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
            continue;
        }

        s32 n = (s32)NC(G, recv, (u64)client, (u64)req, 512, 0x80, 0, 0);

        if (n > 7 && req[0] == 'P' && req[5] == '/' && req[6] == 'b') {
            int v = parse_pad_last(req, n);
            if (v >= 0) { *pad_out = (u16)v; got_input = 1; }
            NC(G, send, (u64)client, (u64)RESP_204K, (u64)str_len(RESP_204K), 0, 0, 0);
            if (*keep_fd >= 0) NC(G, close, (u64)*keep_fd, 0,0,0,0,0);
            *keep_fd = client;
        } else if (n > 5 && req[0] == 'G' && req[4] == '/') {
            u64 off = 0;
            while (off < page_len) {
                u64 chunk = page_len - off;
                if (chunk > 2048) chunk = 2048;
                NC(G, send, (u64)client, (u64)(page + off), chunk, 0, 0, 0);
                off += chunk;
            }
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else if (n > 0 && req[0] == 'O') {
            NC(G, send, (u64)client, (u64)RESP_CORS, (u64)str_len(RESP_CORS), 0, 0, 0);
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        } else {
            NC(G, close, (u64)client, 0, 0, 0, 0, 0);
        }
    }
    return got_input;
}

/* ---------------------------------------------------------- native pad --- */

/* DualSense button bits -> the MD_BTN_* mask the emulator and the web page
   both speak. MD_PAD_MENU and MD_PAD_QUIT are the two out-of-band commands.

   A, B and C sit along the bottom row the way a real three-button pad does,
   and X, Y, Z fill the six-button row above. Games that only understand three
   buttons simply never poll for the rest, so exposing all six costs nothing.
   L1 and R1 are reserved for the menu and quit commands, which is why Y and Z
   land on the triggers rather than the shoulders. */
static u16 ds_to_md(u32 b) {
    u16 r = 0;
    if (b & 0x00008000) r |= MD_BTN_A;       /* SQUARE           */
    if (b & 0x00004000) r |= MD_BTN_B;       /* CROSS            */
    if (b & 0x00002000) r |= MD_BTN_C;       /* CIRCLE           */
    if (b & 0x00001000) r |= MD_BTN_X;       /* TRIANGLE         */
    if (b & 0x00000100) r |= MD_BTN_Y;       /* L2               */
    if (b & 0x00000200) r |= MD_BTN_Z;       /* R2               */
    if (b & 0x00000008) r |= MD_BTN_START;   /* OPTIONS          */
    if (b & 0x00000002) r |= MD_BTN_MODE;    /* L3               */
    if (b & 0x00000010) r |= MD_BTN_UP;
    if (b & 0x00000040) r |= MD_BTN_DOWN;
    if (b & 0x00000080) r |= MD_BTN_LEFT;
    if (b & 0x00000020) r |= MD_BTN_RIGHT;
    if (b & 0x00000400) r = MD_PAD_MENU;     /* L1 -> back to picker */
    if (b & 0x00000800) r = MD_PAD_QUIT;     /* R1 -> quit           */
    return r;
}

/* Also hands back the raw button word. A command button that ends the session
   is worth being able to prove after the fact rather than infer. */
static u32 last_pad_raw;

static s32 read_native_pad(void *G, void *pad_read, s32 pad_h, u8 *pbuf) {
    if (pad_h < 0 || !pad_read) return -1;
    for (int i = 0; i < 128; i++) pbuf[i] = 0;
    s32 n = (s32)NC(G, pad_read, (u64)pad_h, (u64)pbuf, 1, 0, 0, 0);
    if (n <= 0 || (u32)n >= 0x80000000) return -1;
    u32 raw = *(u32 *)pbuf;
    if (raw & 0x80000000) return -1;
    last_pad_raw = raw & 0x001FFFFF;
    return (s32)ds_to_md(last_pad_raw);
}

/* ------------------------------------------------------- ROM discovery -- */

/* Appends every ROM in one directory to the list, skipping duplicates. Each
   entry carries its full path, because ROMs are gathered from more than one
   directory and the picker has to be able to open any of them. */
static int scan_dir(void *G, void *kopen, void *kclose, void *getdents,
                    void *mmap_fn, void *munmap_fn,
                    const char *dir, struct rom_entry *roms, int count) {
    if (!kopen || !getdents) return count;

    s32 dfd = (s32)NC(G, kopen, (u64)dir, 0x20000 /* O_DIRECTORY */, 0, 0, 0, 0);
    if (dfd < 0) return count;

    u8 *dbuf = (u8 *)NC(G, mmap_fn, 0, 0x2000, 3, 0x1002, (u64)-1, 0);
    if ((s64)dbuf == -1) {
        NC(G, kclose, (u64)dfd, 0,0,0,0,0);
        return count;
    }

    int dirlen = str_len(dir);

    for (;;) {
        s32 nread = (s32)NC(G, getdents, (u64)dfd, (u64)dbuf, 0x2000, 0, 0, 0);
        if (nread <= 0) break;

        int off = 0;
        while (off < nread && count < MAX_ROMS) {
            u16 reclen = *(u16 *)(dbuf + off + 4);
            u8  namlen = *(u8 *)(dbuf + off + 7);
            char *name = (char *)(dbuf + off + 8);
            if (reclen == 0) break;

            if (namlen > 0 && is_rom_file(name)) {
                char full[128];
                int p = 0;
                for (int i = 0; i < dirlen && p < 120; i++) full[p++] = dir[i];
                for (int i = 0; name[i] && p < 127; i++) full[p++] = name[i];
                full[p] = 0;

                int dup = 0;
                for (int j = 0; j < count && !dup; j++) {
                    int match = 1;
                    for (int c = 0; c < 128; c++) {
                        if (roms[j].filename[c] != full[c]) { match = 0; break; }
                        if (!full[c]) break;
                    }
                    dup = match;
                }

                if (!dup) {
                    for (int c = 0; c <= p; c++) roms[count].filename[c] = full[c];
                    extract_rom_name(name, roms[count].display, MAX_NAME);
                    count++;
                }
            }
            off += reclen;
        }
        if (count >= MAX_ROMS) break;
    }

    if (munmap_fn) NC(G, munmap_fn, (u64)dbuf, 0x2000, 0,0,0,0);
    NC(G, kclose, (u64)dfd, 0,0,0,0,0);
    return count;
}

/* Only the save's roms directory. Scanning scratch locations outside the image
   made the library unmanageable -- what the picker showed depended on state
   that no backup captured. */
static int scan_all_dirs(void *G, void *kopen, void *kclose, void *getdents,
                         void *mmap_fn, void *munmap_fn,
                         struct rom_entry *roms, int count) {
    return scan_dir(G, kopen, kclose, getdents, mmap_fn, munmap_fn, ROM_DIR,
                    roms, count);
}

/* ------------------------------------------------------- FTP wait UI ----- */

/* Everything the idle callback needs to keep drawing while ftp_serve blocks.
   Assembled in _start and handed across as an opaque pointer. */
struct ftp_ui {
    void *G, *vid_flip, *wait_eq, *pad_read;
    u64   eq;
    s32   video, pad_h;
    void *fbs[2];
    u8   *pad_buf;
    u32  *screen;
    int   active;
    u32   frames;
    int   quit;
};

/* Runs about five times a second while FTP waits for a client.
 *
 * Replaces what used to be a blind 45 second stopwatch: the wait now ends when
 * a ROM actually arrives, or when the user says so, rather than when a timer
 * the user cannot see runs out. */
static int ftp_ui_idle(void *user, int roms_so_far) {
    struct ftp_ui *u = (struct ftp_ui *)user;
    s32 nb;

    /* A transfer finished and the client disconnected -- go and play it. */
    if (roms_so_far > 0) return 1;

    ui_fill(u->screen, COL_BG);
    draw_centered(u->screen, 30, "LUAMD", COL_BRAND);
    draw_centered(u->screen, 46, "MEGA DRIVE", COL_HEAD);
    draw_hline(u->screen, 62, 24, UI_W - 24, COL_LINE);
    draw_centered(u->screen, 78, "NO ROMS IN SAVEDATA", COL_WARN);
    draw_centered(u->screen, 100, "SEND THEM BY FTP TO", COL_NORM);
    draw_centered(u->screen, 114, "THIS CONSOLE, PORT 1337", COL_NORM);
    draw_centered(u->screen, 136, ".BIN .MD .GEN .SMD", COL_DIM);
    draw_centered(u->screen, 158, "THEY ARE SAVED INTO THE GAME", COL_DIM);
    draw_centered(u->screen, 172, "AND STAY THERE", COL_DIM);
    draw_hline(u->screen, 194, 24, UI_W - 24, COL_LINE);
    draw_centered(u->screen, 206, "WAITING -- R1 TO QUIT", COL_DIM);

    blit_scale((u32 *)u->fbs[u->active], u->screen, UI_W, UI_H);
    NC(u->G, u->vid_flip, (u64)u->video, (u64)u->active, 1, u->frames, 0, 0);
    if (u->eq && u->wait_eq) {
        u8 evt[64]; s32 cnt = 0;
        NC(u->G, u->wait_eq, u->eq, (u64)evt, 1, (u64)&cnt, 0, 0);
    }
    u->active ^= 1;
    u->frames++;

    /* Only R1 ends the wait, and it is checked per slice rather than held,
       because at five polls a second a hold counter would be unusable. */
    nb = read_native_pad(u->G, u->pad_read, u->pad_h, u->pad_buf);
    if (nb == (s32)MD_PAD_QUIT) { u->quit = 1; return 1; }

    return 0;
}

/* -------------------------------------------------------------- entry ---- */

__attribute__((section(".text._start")))
void _start(u64 eboot_base, u64 dlsym_addr, struct ext_args *ext) {
    void *G = (void *)(eboot_base + GADGET_OFFSET);
    void *D = (void *)dlsym_addr;
    ext->step = 1;

    /* Beacons for the window before shim_init, where klog() is not usable yet
       because it lives in the very .bss this code is about to map. ext->status
       and ext->step only come back if _start returns, so a hang in here would
       otherwise be completely silent -- and on this target a hang costs a
       reboot. Everything below uses arguments and stack only. */
    void *early_sendto = SYM(G, D, LIBKERNEL_HANDLE, "sendto");
    s32   early_fd = ext->log_fd;
    u8   *early_sa = ext->log_addr;

#define BEACON(m) do { \
        if (early_sendto && early_fd >= 0) \
            NC(G, early_sendto, (u64)early_fd, (u64)(m), sizeof(m) - 1, \
               0, (u64)early_sa, 16); \
    } while (0)

    BEACON("boot: _start entered\n");

    /* Must come first: until this returns, writing any global faults. */
    void *mmap = SYM(G, D, LIBKERNEL_HANDLE, "mmap");
    void *munmap = SYM(G, D, LIBKERNEL_HANDLE, "munmap");
    if (!mmap) {
        BEACON("boot: FAILED to resolve mmap\n");
        ext->status = -3; ext->step = 3; return;
    }
    BEACON("boot: mmap resolved, mapping data region\n");

    if (!boot_data_region(G, mmap, munmap, early_sendto, early_fd, early_sa)) {
        BEACON("boot: FAILED to map .data/.bss region\n");
        ext->status = -4; ext->step = 4; return;
    }
    BEACON("boot: data region live, globals now writable\n");
    ext->step = 2;

    void *usleep    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");
    void *cancel    = SYM(G, D, LIBKERNEL_HANDLE, "scePthreadCancel");
    void *load_mod  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLoadStartModule");
    void *alloc_dm  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelAllocateDirectMemory");
    void *map_dm    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMapDirectMemory");
    void *dm_size   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetDirectMemorySize");
    void *create_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelCreateEqueue");
    void *wait_eq   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWaitEqueue");
    void *delete_eq = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelDeleteEqueue");
    void *kopen     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelOpen");
    void *kread     = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelRead");
    void *kwrite    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelWrite");
    void *kclose    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelClose");
    void *kmkdir    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelMkdir");
    void *klseek    = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelLseek");
    if (!klseek) klseek = SYM(G, D, LIBKERNEL_HANDLE, "lseek");
    void *gettod    = SYM(G, D, LIBKERNEL_HANDLE, "gettimeofday");
    void *recvfrom  = SYM(G, D, LIBKERNEL_HANDLE, "recvfrom");
    void *sendto    = SYM(G, D, LIBKERNEL_HANDLE, "sendto");
    void *accept    = SYM(G, D, LIBKERNEL_HANDLE, "accept");
    void *poll      = SYM(G, D, LIBKERNEL_HANDLE, "poll");
    void *setsockopt_fn  = SYM(G, D, LIBKERNEL_HANDLE, "setsockopt");
    void *getsockname_fn = SYM(G, D, LIBKERNEL_HANDLE, "getsockname");
    void *getdents  = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelGetdents");
    if (!getdents) getdents = SYM(G, D, LIBKERNEL_HANDLE, "getdents");

    s32 log_fd = ext->log_fd;
    u8 log_sa[16];
    for (int i = 0; i < 16; i++) log_sa[i] = ext->log_addr[i];

    s32 web_fd      = (s32)ext->dbg[0];
    u8 *web_page    = (u8 *)ext->dbg[1];
    u64 web_len     = ext->dbg[2];
    s32 userId      = (s32)ext->dbg[3];
    s32 ftp_fd      = (s32)ext->dbg[4];
    s32 ftp_data_fd = (s32)ext->dbg[5];

    if (!usleep || !load_mod) { ext->status = -1; ext->step = 6; return; }

    void *arena = (void *)NC(G, mmap, 0, ARENA_SIZE, 3, 0x1002, (u64)-1, 0);
    if ((s64)arena == -1) { ext->status = -2; ext->step = 3; return; }

    struct shim_ps5 ps5;
    ps5.gadget = G;
    ps5.open   = kopen;
    ps5.read   = kread;
    ps5.write  = kwrite;
    ps5.close  = kclose;
    ps5.lseek  = klseek;
    ps5.mkdir  = kmkdir;
    ps5.sendto = sendto;
    ps5.gettimeofday = gettod;
    ps5.log_fd = log_fd;
    ps5.log_sa = log_sa;
    shim_init(&ps5, arena, ARENA_SIZE);

    klog("=== " VERSION_STR "\n");
    klog("boot: shim up, streaming log is live\n");
    install_fault_trap(G, D, sendto, log_fd, log_sa);

    /* Luac0re enters the payload through a ROP stack pivot, so this may not be
       the thread's real stack. Recording it lets a fault's si_addr be told
       apart: near this value means stack overflow, near __bss_end means a
       global overran. */
    {
        u64 rsp;
        __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
        bmsg(G, sendto, log_fd, log_sa, "boot: stack pointer", rsp);
    }

    s32 vid_mod = (s32)NC(G, load_mod, (u64)"libSceVideoOut.sprx", 0,0,0,0,0);
    s32 aud_mod = (s32)NC(G, load_mod, (u64)"libSceAudioOut.sprx", 0,0,0,0,0);

    void *vid_open  = SYM(G, D, vid_mod, "sceVideoOutOpen");
    void *vid_close = SYM(G, D, vid_mod, "sceVideoOutClose");
    void *vid_reg   = SYM(G, D, vid_mod, "sceVideoOutRegisterBuffers");
    void *vid_flip  = SYM(G, D, vid_mod, "sceVideoOutSubmitFlip");
    void *vid_rate  = SYM(G, D, vid_mod, "sceVideoOutSetFlipRate");
    void *vid_evt   = SYM(G, D, vid_mod, "sceVideoOutAddFlipEvent");
    void *aud_open  = SYM(G, D, aud_mod, "sceAudioOutOpen");
    void *aud_out   = SYM(G, D, aud_mod, "sceAudioOutOutput");
    void *aud_close = SYM(G, D, aud_mod, "sceAudioOutClose");

    ext->step = 5;

    /* Take the display away from ps2emu: stop its GS thread, then close the
       sceVideoOut handle it is holding. */
    if (cancel) {
        u64 gs = *(u64 *)(eboot_base + EBOOT_GS_THREAD);
        if (gs) NC(G, cancel, gs, 0,0,0,0,0);
    }
    NC(G, usleep, 300000, 0,0,0,0,0);

    s32 emu_vid = *(s32 *)(eboot_base + EBOOT_VIDOUT);
    if (vid_close && emu_vid >= 0) NC(G, vid_close, (u64)emu_vid, 0,0,0,0,0);
    NC(G, usleep, 100000, 0,0,0,0,0);

    klog("video: ps2emu GS thread stopped, opening our own output\n");
    s32 video = (s32)NC(G, vid_open, 0xFF, 0, 0, 0, 0, 0);
    if (video < 0) { ext->status = -10; ext->step = 11; return; }

    u64 eq = 0;
    if (create_eq) NC(G, create_eq, (u64)&eq, (u64)"gbq", 0,0,0,0);
    if (vid_evt && eq) NC(G, vid_evt, eq, (u64)video, 0,0,0,0);

    u64 mem_total = dm_size ? NC(G, dm_size, 0,0,0,0,0,0) : 0x300000000ULL;
    u64 phys = 0;
    NC(G, alloc_dm, 0, mem_total, FB_TOTAL, 0x200000, 3, (u64)&phys);
    void *vmem = 0;
    NC(G, map_dm, (u64)&vmem, FB_TOTAL, 0x33, 0, phys, 0x200000);
    if (!vmem) { ext->status = -21; ext->step = 22; return; }

    u8 attr[64];
    for (int i = 0; i < 64; i++) attr[i] = 0;
    *(u32 *)(attr + 0)  = 0x80000000;
    *(u32 *)(attr + 4)  = 1;
    *(u32 *)(attr + 12) = SCR_W;
    *(u32 *)(attr + 16) = SCR_H;
    *(u32 *)(attr + 20) = SCR_W;

    void *fbs[2];
    fbs[0] = vmem;
    fbs[1] = (u8 *)vmem + FB_ALIGNED;

    if (NC(G, vid_reg, (u64)video, 0, (u64)fbs, 2, (u64)attr, 0) != 0) {
        ext->status = -30; ext->step = 30; return;
    }
    if (vid_rate) NC(G, vid_rate, (u64)video, 0, 0,0,0,0);
    clear_fb((u32 *)fbs[0]);
    clear_fb((u32 *)fbs[1]);

    NC(G, load_mod, (u64)"libSceUserService.sprx", 0,0,0,0,0);

    /* ps2emu may still hold audio handles; free them before claiming one. */
    if (aud_close)
        for (int h = 0; h < 8; h++) NC(G, aud_close, (u64)h, 0,0,0,0,0);

    s32 audio_h = -1;
    if (aud_open)
        audio_h = (s32)NC(G, aud_open, 0xFF, 0, 0, SAMPLES_PER_BUF, SAMPLE_RATE, AUDIO_S16_STEREO);

    struct md_audio_iface ai;
    ai.gadget    = G;
    ai.audio_out = aud_out;
    ai.handle    = audio_h;
    md_glue_init(&ai);

    s32 pad_mod = (s32)NC(G, load_mod, (u64)"libScePad.sprx", 0,0,0,0,0);
    void *pad_init_fn = SYM(G, D, pad_mod, "scePadInit");
    void *pad_geth    = SYM(G, D, pad_mod, "scePadGetHandle");
    void *pad_read    = SYM(G, D, pad_mod, "scePadRead");
    if (pad_init_fn) NC(G, pad_init_fn, 0,0,0,0,0,0);
    s32 pad_h = -1;
    if (pad_geth) pad_h = (s32)NC(G, pad_geth, (u64)userId, 0, 0, 0, 0, 0);
    u8 pad_buf[128];

    klog(pad_h >= 0 ? "Native pad OK\n" : "Native pad N/A\n");
    klog(audio_h >= 0 ? "Audio out OK\n" : "Audio out FAILED\n");

    klog("video, audio and pad are up -- scanning for ROMs\n");

    /* Declared before the ROM listing because the FTP wait now draws real
       frames through the same double-buffered path the emulator uses, and has
       to hand the buffer index and frame counter back when it finishes. */
    u32 total_frames = 0;
    int active = 0;

    /* ------------------------------------------------------ ROM listing -- */

    struct rom_entry *roms = (struct rom_entry *)NC(G, mmap, 0,
        sizeof(struct rom_entry) * MAX_ROMS, 3, 0x1002, (u64)-1, 0);
    int rom_count = 0;

    /* Resolve the savedata mount primitive. Writing is done in short
       read-write windows around each save, not by holding one open. */
    savedata_init(G, D, load_mod, eboot_base, userId);

    /* Harmless if the directories already exist. */
    if (kmkdir) {
        NC(G, kmkdir, (u64)ROM_DIR,  0x1FF, 0, 0, 0, 0);
        NC(G, kmkdir, (u64)SAVE_DIR, 0x1FF, 0, 0, 0, 0);
    }

    if ((s64)roms != -1) {
        ui_fill(ui_screen, COL_BG);
        draw_centered(ui_screen, 40, "LUAMD", COL_BRAND);
        draw_centered(ui_screen, 56, "MEGA DRIVE", COL_HEAD);
        draw_centered(ui_screen, 84, "FTP ON PORT 1337", COL_NORM);
        draw_centered(ui_screen, 100, "WAITING FOR ROMS", COL_DIM);
        blit_scale((u32 *)fbs[0], ui_screen, UI_W, UI_H);
        blit_scale((u32 *)fbs[1], ui_screen, UI_W, UI_H);
        NC(G, vid_flip, (u64)video, 0, 1, 0, 0, 0);

        /* FTP writes straight into the savedata container, which is mounted
         * read-only for the game -- so without a write window every upload
         * comes back "550 Cannot create file" and the picker stays empty.
         *
         * The window is normally kept as short as possible, because closing it
         * is what commits and it unmounts and remounts to do so. Here it has
         * to span the whole session: files arrive over the network at
         * unpredictable times and reopening it per file would mean an unmount
         * and remount between every ROM.
         *
         * This is what lets ROMs be loaded without a jailbreak. The homebrew
         * save managers need a kernel exploit; this runs in the Luac0re
         * sandbox and reaches savedata through the eboot's own mount import. */
        /* Cheap look at what is already in the container, purely to size the
           wait below. ftp_serve refills the list from index 0 and the real
           scan runs again afterwards, so this result is deliberately thrown
           away. */
        int already = scan_all_dirs(G, kopen, kclose, getdents, mmap, munmap,
                                    roms, 0);
        /* With ROMs already in the container the wait is a short courtesy for
           adding more. With none, it is indefinite: quitting early would leave
           nothing to play and burn the session, since ending the payload means
           relaunching the game to send another. */
        s32 wait_ms = already > 0 ? FTP_WAIT_MS : 0;

        struct ftp_ui fui;
        fui.G = G; fui.vid_flip = vid_flip; fui.wait_eq = wait_eq;
        fui.pad_read = pad_read; fui.eq = eq; fui.video = video;
        fui.pad_h = pad_h; fui.fbs[0] = fbs[0]; fui.fbs[1] = fbs[1];
        fui.pad_buf = pad_buf; fui.screen = ui_screen;
        fui.active = active; fui.frames = total_frames; fui.quit = 0;

        int ftp_window = savedata_begin_write();
        klog(ftp_window == 0 ? "FTP: savedata open for writing\n"
                             : "FTP: NO write window -- uploads will fail\n");

        rom_count = ftp_serve(ftp_fd, ftp_data_fd,
                              G, D, load_mod, mmap, kopen, kwrite, kclose,
                              kmkdir, getdents, usleep,
                              recvfrom, sendto, accept,
                              getsockname_fn, poll, wait_ms,
                              log_fd, log_sa, userId,
                              roms, MAX_ROMS,
                              ftp_ui_idle, &fui);

        active       = fui.active;
        total_frames = fui.frames;

        /* Commits every uploaded ROM. Skipping this loses all of them. */
        if (ftp_window == 0) {
            savedata_end_write();
            klog("FTP: savedata committed\n");
        }
    }

    /* Pick up anything already on disk that FTP did not just deliver. */
    if ((s64)roms != -1)
        rom_count = scan_all_dirs(G, kopen, kclose, getdents, mmap, munmap,
                                  roms, rom_count);

    { char msg[32]; int k = 0; int v = rom_count;
      const char *lbl = "ROMs found: ";
      while (*lbl) msg[k++] = *lbl++;
      if (!v) msg[k++] = 48;
      else { char t[12]; int m = 0;
             while (v) { t[m++] = (char)(48 + v % 10); v /= 10; }
             while (m) msg[k++] = t[--m]; }
      msg[k++] = 10; msg[k] = 0; klog(msg); }

    /* ------------------------------------------------------- main loops -- */

    int has_web = (web_fd >= 0 && poll && accept);
    int input_src = 0;          /* 0 undecided, 1 DualSense, 2 web page */
    u16 web_pad = 0;
    s32 web_client = -1;

    for (;;) {
        int selected = 0;

        if (rom_count == 0) {
            /* The FTP wait only returns empty when the user asked to stop, so
               there is nothing left to offer -- the screen they just quit from
               already explained the situation. */
            klog("No ROMs in savedata and the wait was ended -- exiting\n");
            break;
        }

        /* ---------------------------------------------------- picker --- */
        {
            int cursor = 0, scroll = 0, mframe = 0, hold = 0;
            /* Start as if every button were already held, so a button still
               down on the way back from a game cannot read as a fresh press.
               Starting at 0 meant a held Cross re-selected the same ROM
               immediately, over and over, on returning to the picker. */
            u16 prev_btn = 0xFFFF;
            int visible = 18;
            if (visible > rom_count) visible = rom_count;

            for (;;) {
                u16 btn = 0;
                u16 wb = 0; int web_got = 0;

                if (has_web) {
                    web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                         setsockopt_fn, web_fd, &web_client,
                                         web_page, web_len, &wb);
                    if (web_got) web_pad = wb;
                }

                s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

                /* The first real button press decides which source owns the
                   session; mixing the two mid-game reads as stuck input. */
                if (input_src == 0) {
                    if (nb > 0 && nb < MD_PAD_MENU) {
                        input_src = 1; btn = (u16)nb;
                        klog("Input: native pad\n");
                    } else if (web_got && web_pad > 0 && web_pad < MD_PAD_MENU) {
                        input_src = 2; btn = web_pad;
                        klog("Input: web controller\n");
                    }
                    if (web_got && web_pad >= MD_PAD_MENU) btn = web_pad;
                    if (nb >= MD_PAD_MENU) btn = (u16)nb;
                } else if (input_src == 1) {
                    if (nb >= 0) btn = (u16)nb;
                } else {
                    btn = web_pad;
                }

                if (btn >= MD_PAD_MENU) web_pad = 0;
                if (btn == MD_PAD_QUIT) goto done;

                /* The out-of-band values share bits with the button masks --
                 * 0xFFFE has MD_BTN_A, DOWN, LEFT and RIGHT set among others --
                 * so they must never reach the decoding below. Without this,
                 * pressing L1 in the picker read as "select this ROM", and
                 * since L1 in a game returns to the picker, the two bounced
                 * off each other and reloaded the cartridge every frame.
                 *
                 * L1 has no meaning in the picker: it is the way back FROM a
                 * game, so dropping it to zero is the whole fix.
                 *
                 * The Game Boy port avoided this by accident, not by design:
                 * its sentinel 0xFE leaves bit 0 clear and its A button WAS
                 * bit 0. Widening the mask to 16 bits for the Mega Drive's
                 * twelve buttons moved A to 0x10, which 0xFFFE sets. */
                if (btn >= MD_PAD_MENU) btn = 0;

                /* u16, not u8: MD_BTN_START is 0x400 and was being truncated
                   away, so Start could never select a ROM. */
                u16 pressed = (u16)(btn & ~prev_btn);
                int move = 0;
                if (btn & MD_BTN_UP) {
                    hold++;
                    if ((pressed & MD_BTN_UP) || (hold > 12 && hold % 4 == 0)) move = -1;
                } else if (btn & MD_BTN_DOWN) {
                    hold++;
                    if ((pressed & MD_BTN_DOWN) || (hold > 12 && hold % 4 == 0)) move = 1;
                } else {
                    hold = 0;
                }

                if (move) {
                    cursor += move;
                    if (cursor < 0) cursor = rom_count - 1;
                    if (cursor >= rom_count) cursor = 0;
                    if (cursor < scroll) scroll = cursor;
                    if (cursor >= scroll + visible) scroll = cursor - visible + 1;
                }
                if ((pressed & MD_BTN_A) || (pressed & MD_BTN_START)) { selected = cursor; break; }
                prev_btn = btn;

                ui_fill(ui_screen, COL_BG);
                draw_centered(ui_screen, 4, "LUAMD", COL_BRAND);
                draw_hline(ui_screen, 14, 8, UI_W - 8, COL_LINE);
                draw_centered(ui_screen, 18, "SELECT A GAME", COL_HEAD);
                draw_hline(ui_screen, 28, 8, UI_W - 8, COL_LINE);

                if (scroll > 0) draw_centered(ui_screen, 30, "-", COL_DIM);

                int ly = 36;
                for (int i = 0; i < visible && scroll + i < rom_count; i++) {
                    int idx = scroll + i;
                    int iy  = ly + i * 10;
                    int sel = (idx == cursor);
                    if (sel && (mframe / 10) % 2)
                        draw_char(ui_screen, 1, iy, '>', COL_SEL);
                    draw_str(ui_screen, 10, iy, roms[idx].display, sel ? COL_SEL : COL_NORM);
                }

                if (scroll + visible < rom_count)
                    draw_centered(ui_screen, ly + visible * 10, "-", COL_DIM);

                draw_hline(ui_screen, UI_H - 14, 8, UI_W - 8, COL_LINE);
                draw_centered(ui_screen, UI_H - 10, "L1 MENU  R1 EXIT", COL_DIM);

                blit_scale((u32 *)fbs[active], ui_screen, UI_W, UI_H);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) {
                    u8 evt[64]; s32 cnt = 0;
                    NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
                }
                active ^= 1;
                mframe++;
                total_frames++;
            }
        }

        /* ------------------------------------------------------ load --- */

        const char *rom_path = roms[selected].filename;

        clear_fb((u32 *)fbs[0]);
        clear_fb((u32 *)fbs[1]);

        if (!md_load_rom(rom_path)) {
            for (int f = 0; f < 120; f++) {
                ui_fill(ui_screen, COL_BG);
                draw_centered(ui_screen, 60, "LOAD FAILED", COL_WARN);
                draw_centered(ui_screen, 80, roms[selected].display, COL_DIM);
                blit_scale((u32 *)fbs[active], ui_screen, UI_W, UI_H);
                NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
                if (eq && wait_eq) { u8 e[64]; s32 c = 0; NC(G, wait_eq, eq, (u64)e, 1, (u64)&c, 0, 0); }
                active ^= 1;
                total_frames++;
            }
            continue;
        }

        klog(md_title());
        klog("\n");

        /* ------------------------------------------------------ play --- */

        int back_to_menu = 0;
        int last_fb_w = 0, last_fb_h = 0;
        u16 cur_pad = 0;
        int quit_held = 0;
        int trace = 3;          /* phase-trace the opening frames, then quieten */

        for (;;) {
            u16 wb = 0; int web_got = 0;
            if (has_web) {
                web_got = web_handle(G, poll, accept, recvfrom, sendto, kclose,
                                     setsockopt_fn, web_fd, &web_client,
                                     web_page, web_len, &wb);
                if (web_got) web_pad = wb;
            }

            s32 nb = read_native_pad(G, pad_read, pad_h, pad_buf);

            if (input_src == 0) {
                if (nb > 0 && nb < MD_PAD_MENU) { input_src = 1; cur_pad = (u16)nb; klog("Input: native pad\n"); }
                else if (web_got && web_pad > 0 && web_pad < MD_PAD_MENU) { input_src = 2; cur_pad = web_pad; klog("Input: web controller\n"); }
                if (web_got && web_pad >= MD_PAD_MENU) cur_pad = web_pad;
                if (nb >= MD_PAD_MENU) cur_pad = (u16)nb;
            } else if (input_src == 1) {
                if (nb >= 0) cur_pad = (u16)nb;
            } else {
                cur_pad = web_pad;
            }

            if (cur_pad >= MD_PAD_MENU) web_pad = 0;

            /* Quitting ends the whole Luac0re session -- the payload cancels
               ps2emu's GS thread, so the loader cannot serve another payload
               afterwards and the game has to be relaunched. A brushed shoulder
               button must therefore not be enough: R1 has to be held. L1 back
               to the picker already saves, so quitting buys almost nothing. */
            if (cur_pad == MD_PAD_QUIT) {
                if (++quit_held == 1)
                    bmsg(G, sendto, log_fd, log_sa, "quit: R1 seen, raw pad", last_pad_raw);
                if (quit_held >= QUIT_HOLD_FRAMES) {
                    klog("quit: held long enough, exiting\n");
                    md_save_store();
                    goto done;
                }
                cur_pad = 0;            /* ignore while it is still building */
            } else {
                if (quit_held) klog("quit: released early, staying in game\n");
                quit_held = 0;
            }

            if (cur_pad == MD_PAD_MENU) {
                bmsg(G, sendto, log_fd, log_sa, "picker: L1, raw pad", last_pad_raw);
                back_to_menu = 1; cur_pad = 0; break;
            }

            md_set_input(cur_pad);

            /* The first few frames are traced phase by phase. Everything here
               runs for the first time only once a real cartridge is loaded, so
               a fault has no other way of telling us which stage it died in --
               the log otherwise just stops after the ROM loads. */
            if (trace) klog("frame: input applied\n");
            md_trace = trace;
            md_run_frame();
            md_trace = 0;
            if (trace) klog("frame: emulation ok\n");

            /* Games change resolution at runtime -- H40 to H32, or V28 to
               V30 -- and the picture is centred, so shrinking it would leave
               the previous frame's edges stranded around the new one. Clearing
               only on a change keeps that off the per-frame path. */
            if (md_fb_w != last_fb_w || md_fb_h != last_fb_h) {
                clear_fb((u32 *)fbs[0]);
                clear_fb((u32 *)fbs[1]);
                last_fb_w = md_fb_w;
                last_fb_h = md_fb_h;
            }

            blit_scale((u32 *)fbs[active], textureImage, md_fb_w, md_fb_h);
            if (trace) klog("frame: blit ok\n");

            NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
            if (trace) klog("frame: flip submitted\n");

            /* Submitted after the flip on purpose: this call blocks until the
               audio queue drains, which is what actually paces the emulator
               at the Game Boy's 59.7Hz rather than the display's 60Hz. */
            md_audio_flush();
            if (trace) klog("frame: audio ok\n");

            if (eq && wait_eq) {
                u8 evt[64]; s32 cnt = 0;
                NC(G, wait_eq, eq, (u64)evt, 1, (u64)&cnt, 0, 0);
            }
            if (trace) { klog("frame: vsync ok -- frame complete\n"); trace--; }

            active ^= 1;
            total_frames++;
            ext->frame_count = total_frames;
        }

        md_save_store();
        if (!back_to_menu) break;
    }

done:
    klog("Shutting down...\n");

    if (aud_close && audio_h >= 0) NC(G, aud_close, (u64)audio_h, 0,0,0,0,0);

    clear_fb((u32 *)fbs[0]);
    clear_fb((u32 *)fbs[1]);
    NC(G, vid_flip, (u64)video, (u64)active, 1, total_frames, 0, 0);
    if (usleep) NC(G, usleep, 50000, 0,0,0,0,0);

    if (vid_close && video >= 0) NC(G, vid_close, (u64)video, 0,0,0,0,0);
    if (delete_eq && eq)         NC(G, delete_eq, eq, 0,0,0,0,0);
    if (web_client >= 0 && kclose) NC(G, kclose, (u64)web_client, 0,0,0,0,0);
    if (web_fd >= 0 && kclose)     NC(G, kclose, (u64)web_fd, 0,0,0,0,0);

    if (munmap) {
        if ((s64)roms != -1)
            NC(G, munmap, (u64)roms, (u64)(sizeof(struct rom_entry) * MAX_ROMS), 0,0,0,0);
        NC(G, munmap, (u64)arena, ARENA_SIZE, 0,0,0,0);
    }

    klog("Clean exit\n");
    ext->status = 0;
    ext->step = 99;
    ext->frame_count = total_frames;
}
