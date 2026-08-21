#include "ui.h"
#include "tables.h"

int str_len(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

void ui_fill(u32 *scr, u32 color) {
    for (int i = 0; i < UI_W * UI_H; i++) scr[i] = color;
}

void clear_fb(u32 *fb) {
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000u;
}

void draw_char(u32 *scr, int x, int y, char ch, u32 color) {
    int idx = 0;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 32 && ch <= 90) idx = ch - 32;
    const u8 *glyph = font_data[idx];

    for (int r = 0; r < 8; r++) {
        u8 bits = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80 >> c)) {
                int px = x + c, py = y + r;
                if (px >= 0 && px < UI_W && py >= 0 && py < UI_H)
                    scr[py * UI_W + px] = color;
            }
        }
    }
}

void draw_str(u32 *scr, int x, int y, const char *s, u32 color) {
    while (*s) {
        draw_char(scr, x, y, *s, color);
        x += 8;
        s++;
    }
}

void draw_centered(u32 *scr, int y, const char *s, u32 color) {
    int x = (UI_W - str_len(s) * 8) / 2;
    if (x < 0) x = 0;
    draw_str(scr, x, y, s, color);
}

void draw_hline(u32 *scr, int y, int x1, int x2, u32 color) {
    if (y < 0 || y >= UI_H) return;
    for (int x = x1; x < x2 && x < UI_W; x++) {
        if (x >= 0) scr[y * UI_W + x] = color;
    }
}

/* The Mega Drive's picture size is not fixed, so the scale factor is a
   constant but the centring is not. Source stride is always MD_FB_W even when
   the live width is 256, because that is how mdglue.c lays the frame out. */
void blit_scale(u32 *fb, const u32 *src, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w > MD_FB_W) w = MD_FB_W;
    if (h > MD_FB_H) h = MD_FB_H;

    int off_x = (SCR_W - w * SCALE) / 2;
    int off_y = (SCR_H - h * SCALE) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    for (int gy = 0; gy < h; gy++) {
        int sy = off_y + gy * SCALE;
        const u32 *srow = &src[gy * MD_FB_W];
        for (int gx = 0; gx < w; gx++) {
            u32 c  = srow[gx];
            int sx = off_x + gx * SCALE;
            for (int dy = 0; dy < SCALE; dy++) {
                u32 *row = &fb[(sy + dy) * SCR_W + sx];
                for (int dx = 0; dx < SCALE; dx++) row[dx] = c;
            }
        }
    }
}

static char lower_ch(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int ext_is(const char *name, int len, const char *ext, int elen) {
    if (len < elen + 1) return 0;
    if (name[len - elen - 1] != '.') return 0;
    for (int i = 0; i < elen; i++)
        if (lower_ch(name[len - elen + i]) != ext[i]) return 0;
    return 1;
}

/* Raw dumps are .bin, .md or .gen depending on who made them; .smd is the
   Super Magic Drive format, which mdglue.c deinterleaves on load. */
int is_rom_file(const char *name) {
    int len = str_len(name);
    return ext_is(name, len, "bin", 3)
        || ext_is(name, len, "gen", 3)
        || ext_is(name, len, "smd", 3)
        || ext_is(name, len, "md", 2);
}

void extract_rom_name(const char *fn, char *out, int max) {
    int i = 0;
    while (fn[i] && fn[i] != '.' && i < max - 1) {
        out[i] = fn[i];
        i++;
    }
    out[i] = '\0';
}
