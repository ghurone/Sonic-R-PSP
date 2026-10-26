/**
 * menu_psp.c — the options' Graphics page on the PSP: FPS (30 or 60,
 * interp_psp.c), Draw Distance (Very Far, 1.5x Very Far, 2x Very Far —
 * platform_psp.c's g_pspFarQuarters) and Screen (4:3 or 16:9 in a race). The menu's words are pictures in its
 * two texture pages (OPT00.RAW, OPT01.RAW); the ones these need are made in
 * the pages' pixels once they are loaded (PSP_OptionsTextures):
 *   - "FPS" over the INTERLACE label (OPT01 128,0x70: the page's item 25,
 *     the one it now is), "SCREEN" (4:3 or 16:9, render_gu.c's
 *     g_pspWidescreen) over WINDOW (128,0x90: item 27), in their colours;
 *   - "VeryFar", "1.5xVeryFar", "2xVeryFar" over the screen sizes' values
 *     (OPT01 0,0x80..0xA0, the resolution the PSP has none of), from the
 *     page's own VeryFar and digits and an x and a point drawn in their style;
 *     "4:3" and "16:9" over Large and Fullscreen (0,0xB0..0xC0).
 */
#include <string.h>
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "r_types.h"
#include "r_state.h"

extern void R_MarkTextureDirty(int tpage);

#define PAGE_W 256
#define GREEN  0x07C0          /* the colour key, (0, 255, 0) as the pages keep it */

/* the pages' 16-bit pixels: red 15-11, green 10-6, blue 4-0 (texture.c) */
static inline int px_r(unsigned short p) { return (p >> 11) & 31; }
static inline int px_g(unsigned short p) { return (p >> 6) & 31; }
static inline int px_b(unsigned short p) { return p & 31; }
static inline unsigned short px(int r, int g, int b) { return (unsigned short)((r << 11) | (g << 6) | b); }

/* a label's letters: their fill, red or blue well over the pill's green */
static inline int label_fill(unsigned short p)
{
    return p != GREEN && (px_r(p) > 12 || px_b(p) > 12);
}

/* the label font's blocky capitals, 6 by 10, the ones the labels made here need */
static const struct {
    char c;
    const char *rows[10];
} s_capitals[] = {
    { 'F', { "######", "######", "##....", "##....", "#####.", "#####.", "##....", "##....", "##....", "##...." } },
    { 'P', { "#####.", "######", "##..##", "##..##", "######", "#####.", "##....", "##....", "##....", "##...." } },
    { 'S', { ".#####", "######", "##....", "###...", ".####.", "..####", "....##", "....##", "######", "#####." } },
    { 'C', { ".#####", "######", "##....", "##....", "##....", "##....", "##....", "##....", "######", ".#####" } },
    { 'R', { "#####.", "######", "##..##", "##..##", "######", "#####.", "##.##.", "##..##", "##..##", "##..##" } },
    { 'E', { "######", "######", "##....", "##....", "#####.", "#####.", "##....", "##....", "######", "######" } },
    { 'N', { "##..##", "###.##", "######", "######", "##.###", "##..##", "##..##", "##..##", "##..##", "##..##" } },
};

/* text over the label at (x0, y0) of page: its letters cleared (the pill's
 * shading across them taken straight from either side), the new ones drawn
 * in its letters' colour a row, with a shadow a pixel down and right as they
 * have. */
static void make_label(unsigned short *dst, int x0, int y0, const char *text)
{
    /* its letters' colour a row, and where they are */
    unsigned short rowFill[16];
    int left = x0 + 128, right = x0, top = -1;
    for (int y = 0; y < 16; y++) {
        rowFill[y] = 0;
        for (int x = x0 + 8; x < x0 + 120; x++) {
            unsigned short p = dst[(y0 + y) * PAGE_W + x];
            if (label_fill(p)) {
                rowFill[y] = rowFill[y] ? rowFill[y] : p;
                left = x < left ? x : left;
                right = x > right ? x : right;
            }
        }
        if (rowFill[y] && top < 0) {
            top = y;
        }
    }
    if (right < left || top < 0 || top + 10 > 16) {
        return;
    }
    left -= 2;
    right += 3;   /* (the shadow) */
    for (int y = 0; y < 16; y++) {
        unsigned short a = dst[(y0 + y) * PAGE_W + left], b = dst[(y0 + y) * PAGE_W + right];
        for (int x = left + 1; x < right; x++) {
            int t = (x - left) * 256 / (right - left);
            dst[(y0 + y) * PAGE_W + x] = px(px_r(a) + (px_r(b) - px_r(a)) * t / 256,
                                            px_g(a) + (px_g(b) - px_g(a)) * t / 256,
                                            px_b(a) + (px_b(b) - px_b(a)) * t / 256);
        }
    }

    static const int sh[3][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 } };
    int n = (int)strlen(text);
    int x = x0 + 64 - (n * 6 + (n - 1) * 2) / 2;
    for (int l = 0; l < n; l++, x += 8) {
        const char *const *L = NULL;
        for (unsigned int k = 0; k < sizeof(s_capitals) / sizeof(s_capitals[0]); k++) {
            if (s_capitals[k].c == text[l]) {
                L = s_capitals[k].rows;
            }
        }
        if (L == NULL) {
            continue;   /* (a space) */
        }
        for (int r = 0; r < 10; r++) {
            for (int c = 0; c < 6; c++) {
                if (L[r][c] != '#') {
                    continue;
                }
                for (int s = 0; s < 3; s++) {
                    int rr = r + sh[s][1], cc = c + sh[s][0];
                    if (!(rr < 10 && cc < 6 && L[rr][cc] == '#')) {
                        unsigned short *q = &dst[(y0 + top + rr) * PAGE_W + x + cc];
                        *q = px(px_r(*q) >> 2, px_g(*q) >> 2, px_b(*q) >> 2);
                    }
                }
            }
        }
        for (int r = 0; r < 10; r++) {
            for (int c = 0; c < 6; c++) {
                if (L[r][c] == '#') {
                    dst[(y0 + top + r) * PAGE_W + x + c] = rowFill[top + r];
                }
            }
        }
    }
}

/* the value font: anything not the key; its fill the bright of it */
static inline int value_pixel(unsigned short p)
{
    return p != GREEN;
}

static inline int value_fill(unsigned short p)
{
    return p != GREEN && px_r(p) > 20;
}

/* one cell of the value row (sx, sy) onto (dx, dy), its picture's columns only;
 * returns the columns copied */
static int copy_cell(unsigned short *page, int sx, int sy, int w, int dx, int dy)
{
    int from = -1, to = -1;
    for (int x = sx; x < sx + w; x++) {
        for (int y = 0; y < 16; y++) {
            if (value_pixel(page[(sy + y) * PAGE_W + x])) {
                from = from < 0 ? x : from;
                to = x;
                break;
            }
        }
    }
    if (from < 0) {
        return 0;
    }
    for (int x = from; x <= to; x++) {
        for (int y = 0; y < 16; y++) {
            unsigned short p = page[(sy + y) * PAGE_W + x];
            if (value_pixel(p) && dx + x - from < 128) {
                page[(dy + y) * PAGE_W + dx + x - from] = p;
            }
        }
    }
    return to - from + 1;
}

/* a mark drawn in the value font's style (rows of 8 columns, '#' its fill,
 * the colour of that row of VeryFar's; its shadow a pixel down and right) */
static int draw_mark(unsigned short *page, const char *const *rows, int top, int nrows, int dx, int dy,
                     const unsigned short *rowFill, unsigned short shadow)
{
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < 8; c++) {
            if (rows[r][c] != '#') {
                continue;
            }
            int y = top + r;
            static const int sh[3][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 } };
            for (int s = 0; s < 3; s++) {
                int qr = r + sh[s][1], qc = c + sh[s][0];
                int filled = qr < nrows && qc < 8 && rows[qr][qc] == '#';
                if (!filled && y + sh[s][1] < 16) {
                    page[(dy + y + sh[s][1]) * PAGE_W + dx + c + sh[s][0]] = shadow;
                }
            }
        }
        for (int c = 0; c < 8; c++) {
            if (rows[r][c] == '#') {
                page[(dy + top + r) * PAGE_W + dx + c] = rowFill[top + r];
            }
        }
    }
    int w = 0;
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < 8; c++) {
            if (rows[r][c] == '#' && c + 2 > w) {
                w = c + 2;
            }
        }
    }
    return w;
}

static void make_distance_values(unsigned short *page)
{
    const int veryFarY = 0x70, digitsY = 0xF0;
    /* VeryFar's colours a row, and its shadow */
    unsigned short rowFill[16], shadow = px(3, 2, 1);
    for (int y = 0; y < 16; y++) {
        rowFill[y] = px(31, 27, 20);
        for (int x = 0; x < 64; x++) {
            unsigned short p = page[(veryFarY + y) * PAGE_W + x];
            if (value_fill(p)) {
                rowFill[y] = p;
                break;
            }
            if (value_pixel(p)) {
                shadow = p;
            }
        }
    }
    static const char *const ex[8] = {
        "##....##", "###..###", ".######.", "..####..", "..####..", ".######.", "###..###", "##....##",
    };
    static const char *const point[3] = { "###.....", "###.....", "###....." };

    for (int v = 0; v < 3; v++) {
        int y0 = 0x80 + v * 16;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 128; x++) {
                page[(y0 + y) * PAGE_W + x] = GREEN;
            }
        }
        int x = 0;
        if (v == 1) {   /* 1.5x */
            x += copy_cell(page, 1 * 12, digitsY, 12, x, y0) + 1;
            x += draw_mark(page, point, 11, 3, x, y0, rowFill, shadow);
            x += copy_cell(page, 5 * 12, digitsY, 12, x, y0) + 1;
        } else if (v == 2) {   /* 2x */
            x += copy_cell(page, 2 * 12, digitsY, 12, x, y0) + 1;
        }
        if (v) {
            x += draw_mark(page, ex, 6, 8, x, y0, rowFill, shadow) + 4;
        }
        copy_cell(page, 0, veryFarY, 128 - x < 64 ? 128 - x : 64, x, y0);
    }
}

/* "4:3" and "16:9" over the screen sizes' Large and Fullscreen (OPT01
 * 0,0xB0 and 0,0xC0): the page's digits and a colon drawn in their style */
static void make_screen_values(unsigned short *page)
{
    unsigned short rowFill[16], shadow = px(3, 2, 1);
    for (int y = 0; y < 16; y++) {
        rowFill[y] = px(31, 27, 20);
        for (int x = 0; x < 64; x++) {
            unsigned short p = page[(0x70 + y) * PAGE_W + x];
            if (value_fill(p)) {
                rowFill[y] = p;
                break;
            }
            if (value_pixel(p)) {
                shadow = p;
            }
        }
    }
    static const char *const colon[8] = {
        "###.....", "###.....", "........", "........", "........", "###.....", "###.....", "........",
    };
    static const char digits[2][5] = { "4:3", "16:9" };
    for (int v = 0; v < 2; v++) {
        int y0 = 0xB0 + v * 16;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 128; x++) {
                page[(y0 + y) * PAGE_W + x] = GREEN;
            }
        }
        int x = 0;
        for (const char *c = digits[v]; *c; c++) {
            if (*c == ':') {
                x += draw_mark(page, colon, 4, 8, x, y0, rowFill, shadow) + 1;
            } else {
                x += copy_cell(page, (*c - '0') * 12, 0xF0, 12, x, y0) + 1;
            }
        }
    }
}

/* After the options menu loads its pages (screen_misc.c): OPT01's page */
void PSP_OptionsTextures(int tpageOpt01)
{
    unsigned short *opt01 = (unsigned short *)g_tpagePixelBuf[tpageOpt01];
    if (opt01 == NULL || g_bitsPerPixel != 0x10) {
        return;
    }
    make_label(opt01, 128, 0x70, "FPS");
    make_label(opt01, 128, 0x90, "SCREEN");
    make_distance_values(opt01);
    make_screen_values(opt01);
    R_MarkTextureDirty(tpageOpt01);
}
