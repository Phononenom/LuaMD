#ifndef LUAMD_UI_H
#define LUAMD_UI_H

#include "core.h"

/* The menu is drawn at Game Boy resolution into a 160x144 ARGB buffer and
   then goes through the same scaler as an emulated frame, so the picker and
   the game land on screen through exactly one code path. */

#define COL_BG     0xFF0F1410u
#define COL_BRAND  0xFF9BBC0Fu
#define COL_HEAD   0xFF8BAC0Fu
#define COL_LINE   0xFF306230u
#define COL_SEL    0xFFE0F8D0u
#define COL_NORM   0xFF9BBC0Fu
#define COL_DIM    0xFF306230u
#define COL_WARN   0xFFE8B040u

int  str_len(const char *s);
void ui_fill(u32 *scr, u32 color);
void draw_char(u32 *scr, int x, int y, char ch, u32 color);
void draw_str(u32 *scr, int x, int y, const char *s, u32 color);
void draw_centered(u32 *scr, int y, const char *s, u32 color);
void draw_hline(u32 *scr, int y, int x1, int x2, u32 color);

/* Nearest-neighbour integer scale of a live-sized Mega Drive frame (source
   stride MD_FB_W) into the centre of the 1920x1080 framebuffer, pillarboxed.
   This is the GAME path -- pointing the menu at it is what made the menu 4:3. */
void blit_scale(u32 *fb, const u32 *src, int w, int h);

/* The MENU path: UI_W x UI_H scaled by UI_SCALE, which covers 1920x1080
   exactly, so there is no centring offset and no border left to clear. */
void blit_ui(u32 *fb, const u32 *src);

void clear_fb(u32 *fb);

/* Accepts .gb, .gbc and .gbs (case-insensitive). */
int  is_rom_file(const char *name);
void extract_rom_name(const char *fn, char *out, int max);

#endif
