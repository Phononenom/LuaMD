#include "shim.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- state -- */

static struct shim_ps5 P;
static int  ready;

static u8 *arena_base;
static u64 arena_size;
static u64 arena_pos;

/* ------------------------------------------------------------- logging --- */

void klog(const char *msg) {
    if (!ready || P.log_fd < 0 || !P.sendto) return;
    u64 n = 0;
    while (msg[n]) n++;
    NC(P.gadget, P.sendto, (u64)P.log_fd, (u64)msg, n, 0, (u64)P.log_sa, 16);
}

/* --------------------------------------------------------------- memory -- */

void shim_init(const struct shim_ps5 *ps5, void *arena, u64 size) {
    P = *ps5;
    arena_base = (u8 *)arena;
    arena_size = size;
    arena_pos  = 0;
    ready = 1;
}

void arena_reset(void) { arena_pos = 0; }
u64  arena_used(void)  { return arena_pos; }

void *malloc(size_t n) {
    if (!arena_base) return NULL;
    u64 need = ((u64)n + 15) & ~15ULL;          /* keep 16-byte alignment */
    if (arena_pos + need > arena_size) {
        klog("shim: arena exhausted\n");
        return NULL;
    }
    void *p = arena_base + arena_pos;
    arena_pos += need;
    return p;
}

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

/* The arena is rewound wholesale between games, so an individual free() has
   nothing to do -- except when it releases the most recent allocation, which
   is cheap to reclaim and keeps repeated load/unload cycles from creeping. */
void free(void *p) {
    (void)p;
}

void *realloc(void *p, size_t n) {
    void *q = malloc(n);
    if (q && p) memcpy(q, p, n);
    return q;
}

int abs(int v) { return v < 0 ? -v : v; }

/* --------------------------------------------------------------- string -- */

void *memcpy(void *d, const void *s, size_t n) {
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    if (dd == ss || n == 0) return d;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}

void *memset(void *d, int c, size_t n) {
    u8 *dd = (u8 *)d;
    while (n--) *dd++ = (u8)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

/* Standard strncpy semantics: pads with NULs, does not guarantee termination. */
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = '\0';
    return r;
}

char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) {}
    return r;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}

/* --------------------------------------------------------------- printf -- */

struct out {
    char  *buf;      /* NULL -> accumulate into line[] and send to klog */
    size_t cap;
    size_t len;
};

static void out_ch(struct out *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void out_str(struct out *o, const char *s) {
    while (*s) out_ch(o, *s++);
}

static void out_num(struct out *o, u64 v, int base, int upper,
                    int width, int zero, int neg) {
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;

    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % (u64)base]; v /= (u64)base; }
    if (neg) tmp[n++] = '-';

    for (int i = n; i < width; i++) out_ch(o, zero ? '0' : ' ');
    while (n--) out_ch(o, tmp[n]);
}

/* Supports the conversions fixGB actually emits -- %d %i %u %x %X %s %c %% --
   plus %p and an l/ll length modifier for this project's own logging. Width
   and zero padding are honoured; precision is parsed and ignored. */
static int shim_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    struct out o;
    o.buf = buf; o.cap = cap; o.len = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_ch(&o, *fmt); continue; }
        fmt++;
        if (*fmt == '%') { out_ch(&o, '%'); continue; }

        int zero = 0, width = 0, lng = 0;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0') {
            if (*fmt == '0') zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; while (*fmt >= '0' && *fmt <= '9') fmt++; }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l' || *fmt == 'z') lng = 1; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            s64 v = lng ? va_arg(ap, s64) : (s64)va_arg(ap, int);
            int neg = v < 0;
            out_num(&o, (u64)(neg ? -v : v), 10, 0, width, zero, neg);
            break;
        }
        case 'u': {
            u64 v = lng ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned int);
            out_num(&o, v, 10, 0, width, zero, 0);
            break;
        }
        case 'x': case 'X': {
            u64 v = lng ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned int);
            out_num(&o, v, 16, *fmt == 'X', width, zero, 0);
            break;
        }
        case 'p': {
            out_str(&o, "0x");
            out_num(&o, (u64)va_arg(ap, void *), 16, 0, 16, 1, 0);
            break;
        }
        case 'c': out_ch(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            out_str(&o, s);
            break;
        }
        case '\0': fmt--; break;
        default: out_ch(&o, '%'); out_ch(&o, *fmt); break;
        }
    }

    if (cap) o.buf[o.len < cap ? o.len : cap - 1] = '\0';
    return (int)o.len;
}

int snprintf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = shim_vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = shim_vsnprintf(out, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
#if GB_VERBOSE
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int r = shim_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    klog(line);
    return r;
#else
    (void)fmt;
    return 0;
#endif
}

int puts(const char *s) {
#if GB_VERBOSE
    klog(s);
    klog("\n");
#else
    (void)s;
#endif
    return 0;
}

/* ----------------------------------------------------------------- file -- */

#define MAX_FILES 8

struct _FILE {
    int used;
    s32 fd;
    s64 pos;
};

static struct _FILE file_slots[MAX_FILES];

/* FreeBSD open() flags -- the PS5 kernel is FreeBSD-derived, so these are the
   BSD values, not the Linux ones. 0x601 = O_WRONLY|O_CREAT|O_TRUNC, matching
   what EmuC0re's FTP server uses on hardware. */
#define BSD_O_RDONLY 0x0000
#define BSD_O_WRONLY 0x0001
#define BSD_O_CREAT  0x0200
#define BSD_O_TRUNC  0x0400

FILE *fopen(const char *path, const char *mode) {
    if (!ready || !P.open) return NULL;

    /* "r+" means write to a file that must already exist -- no O_CREAT. The
       PS5 sandbox refuses to create files inside savedata but permits writing
       existing ones, so this is the mode that makes cartridge saves work. */
    int plus    = (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+'));
    int writing = (mode[0] == 'w' || mode[0] == 'a' || (mode[0] == 'r' && plus));

    s32 flags;
    if (mode[0] == 'r' && plus)      flags = BSD_O_WRONLY;
    else if (writing)                flags = BSD_O_WRONLY | BSD_O_CREAT | BSD_O_TRUNC;
    else                             flags = BSD_O_RDONLY;

    s32 fd = (s32)NC(P.gadget, P.open, (u64)path, (u64)flags, 0x1FF, 0, 0, 0);
    if (fd < 0) {
        /* Report the actual return value. A save failing with EROFS (30) means
           savedata is mounted read-only inside the sandbox, which is a very
           different problem from ENOENT (2) or EACCES (13). */
        if (writing) {
            char b[192];
            int p = 0;
            const char *lbl = "shim: open for write failed, ret ";
            while (*lbl) b[p++] = *lbl++;
            s64 v = fd;
            if (v < 0) { b[p++] = '-'; v = -v; }
            char t[24]; int n = 0;
            if (!v) t[n++] = '0';
            while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
            while (n) b[p++] = t[--n];
            b[p++] = ' ';
            for (int i = 0; path[i] && p < 180; i++) b[p++] = path[i];
            b[p++] = '\n'; b[p] = 0;
            klog(b);
        }
        return NULL;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_slots[i].used) {
            file_slots[i].used = 1;
            file_slots[i].fd   = fd;
            file_slots[i].pos  = 0;
            return &file_slots[i];
        }
    }

    NC(P.gadget, P.close, (u64)fd, 0, 0, 0, 0, 0);
    klog("shim: out of FILE slots\n");
    return NULL;
}

int fclose(FILE *f) {
    if (!f || !f->used) return -1;
    NC(P.gadget, P.close, (u64)f->fd, 0, 0, 0, 0, 0);
    f->used = 0;
    f->fd   = -1;
    return 0;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f) {
    if (!f || !f->used || !P.read) return 0;
    u64 want = (u64)size * (u64)count, done = 0;
    u8 *p = (u8 *)buf;

    while (done < want) {
        s64 n = (s64)NC(P.gadget, P.read, (u64)f->fd, (u64)(p + done), want - done, 0, 0, 0);
        if (n <= 0) break;
        done += (u64)n;
    }
    f->pos += (s64)done;
    return size ? (size_t)(done / size) : 0;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    if (!f || !f->used || !P.write) return 0;
    u64 want = (u64)size * (u64)count, done = 0;
    const u8 *p = (const u8 *)buf;

    while (done < want) {
        s64 n = (s64)NC(P.gadget, P.write, (u64)f->fd, (u64)(p + done), want - done, 0, 0, 0);
        if (n <= 0) break;
        done += (u64)n;
    }
    f->pos += (s64)done;
    return size ? (size_t)(done / size) : 0;
}

int fseek(FILE *f, long off, int whence) {
    if (!f || !f->used || !P.lseek) return -1;
    s64 r = (s64)NC(P.gadget, P.lseek, (u64)f->fd, (u64)off, (u64)whence, 0, 0, 0);
    if (r < 0) return -1;
    f->pos = r;
    return 0;
}

long ftell(FILE *f) {
    if (!f || !f->used) return -1;
    return (long)f->pos;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }

/* ----------------------------------------------------------------- time -- */

static time_t now_seconds(void) {
    if (!ready || !P.gettimeofday) return 0;
    struct { s64 sec; s64 usec; } tv;
    tv.sec = 0; tv.usec = 0;
    NC(P.gadget, P.gettimeofday, (u64)&tv, 0, 0, 0, 0, 0);
    return (time_t)tv.sec;
}

time_t time(time_t *t) {
    time_t v = now_seconds();
    if (t) *t = v;
    return v;
}

/* Days-from-civil, reversed: turn a Unix timestamp into a broken-down UTC
   time. mbc.c only reads tm_sec/min/hour/yday, but the rest is filled in so
   the struct is not a trap for later callers. */
static struct tm tm_static;

struct tm *gmtime(const time_t *tp) {
    s64 t = tp ? *tp : 0;

    s64 days = t / 86400;
    s64 rem  = t % 86400;
    if (rem < 0) { rem += 86400; days--; }

    tm_static.tm_hour = (int)(rem / 3600);
    tm_static.tm_min  = (int)((rem % 3600) / 60);
    tm_static.tm_sec  = (int)(rem % 60);

    /* 1970-01-01 was a Thursday. */
    tm_static.tm_wday = (int)((days + 4) % 7);
    if (tm_static.tm_wday < 0) tm_static.tm_wday += 7;

    int year = 1970;
    for (;;) {
        int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        s64 ylen = leap ? 366 : 365;
        if (days < ylen) break;
        days -= ylen;
        year++;
    }
    tm_static.tm_year = year - 1900;
    tm_static.tm_yday = (int)days;

    static const u8 mlen[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    int mon = 0;
    for (;;) {
        int len = mlen[mon] + ((mon == 1 && leap) ? 1 : 0);
        if (days < len) break;
        days -= len;
        mon++;
    }
    tm_static.tm_mon   = mon;
    tm_static.tm_mday  = (int)days + 1;
    tm_static.tm_isdst = 0;
    return &tm_static;
}

/* No timezone database on board; UTC is close enough for a cartridge RTC. */
struct tm *localtime(const time_t *t) { return gmtime(t); }
