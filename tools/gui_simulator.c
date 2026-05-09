/******************************************************************************
 * Launchpad Pro MC-707 — SDL2 GUI Simulator  (resizable)
 *
 * Layout matches the physical device:
 *   - Corners (0, 9, 99) are virtual — nothing rendered there
 *   - Corner 90 (top-left) = SETUP button  (fires TYPESETUP events)
 *   - Edge pads (col 0/9, row 0/9) are round
 *   - Inner 8×8 are square
 *
 * Controls:
 *   Left-click  = press / release
 *   Space       = 1 timer tick
 *   T           = 100 timer ticks
 *   Q / Esc     = quit
 *   MIDI output → stdout
 ******************************************************************************/

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "app.h"

// ____________________________________________________________________________
// State

static u8  g_led_r[101];   // 0-99 = pad indices, 100 = setup LED
static u8  g_led_g[101];
static u8  g_led_b[101];
static u8  g_flash[USER_AREA_SIZE];
static u16 g_adc[64];

// ____________________________________________________________________________
// HAL

void hal_plot_led(u8 type, u8 index, u8 red, u8 green, u8 blue)
{
    int i = (type == TYPESETUP) ? 100 : (index < 100 ? (int)index : -1);
    if (i < 0) return;
    g_led_r[i] = red; g_led_g[i] = green; g_led_b[i] = blue;
}

void hal_read_led(u8 type, u8 index, u8 *red, u8 *green, u8 *blue)
{
    int i = (type == TYPESETUP) ? 100 : (index < 100 ? (int)index : 0);
    *red = g_led_r[i]; *green = g_led_g[i]; *blue = g_led_b[i];
}

void hal_send_midi(u8 port, u8 status, u8 d1, u8 d2)
{
    const char *p = port==USBSTANDALONE ? "USB-SA" : port==USBMIDI ? "USB-MI" : "DIN   ";
    printf("  [MIDI] %s  %02X  %02X(%3d)  %02X(%3d)\n", p, status, d1, d1, d2, d2);
    fflush(stdout);
}

void hal_send_sysex(u8 port, const u8 *data, u16 length)
{
    printf("  [SYSEX] port=%d len=%d\n", port, length); fflush(stdout);
}

void hal_read_flash(u32 offset, u8 *data, u32 length)
{
    if (offset + length <= USER_AREA_SIZE)
        memcpy(data, g_flash + offset, length);
}

void hal_write_flash(u32 offset, const u8 *data, u32 length)
{
    if (offset + length <= USER_AREA_SIZE) {
        memcpy(g_flash + offset, data, length);
        printf("  [FLASH] wrote %lu bytes at offset %lu\n",
               (unsigned long)length, (unsigned long)offset);
        fflush(stdout);
    }
}

u8 hal_read_device_id(void)   { return 0; }
u8 hal_read_layout_text(void) { return 0; }

// ____________________________________________________________________________
// Dynamic layout — recomputed whenever the window is resized

typedef struct {
    int pad_px;   // width/height of each pad cell
    int gap;      // pixels between cells
    int stride;   // pad_px + gap
    int ox;       // grid left edge (X offset for centering)
    int oy;       // grid top edge  (Y offset for centering)
    int fs;       // font scale (pixels per bitmap pixel)
    int cr;       // circle radius for round/setup buttons
} Layout;

static Layout g_layout;

static Layout compute_layout(int win_w, int win_h)
{
    Layout l;
    int space  = SDL_min(win_w, win_h);
    int margin = SDL_max(10, space / 38);
    l.gap      = SDL_max(3, space / 140);
    l.pad_px   = (space - 2*margin - 9*l.gap) / 10;
    l.stride   = l.pad_px + l.gap;
    int grid   = 10*l.pad_px + 9*l.gap;
    l.ox       = (win_w - grid) / 2;
    l.oy       = (win_h - grid) / 2;
    l.fs       = SDL_max(1, l.pad_px / 28);
    l.cr       = l.pad_px/2 - SDL_max(2, l.pad_px/20);
    return l;
}

// ____________________________________________________________________________
// Bitmap font — 4 columns × 5 rows, MSB = leftmost column

static const u8 DIGITS[10][5] = {
    {0x6,0x9,0x9,0x9,0x6},  // 0
    {0x2,0x6,0x2,0x2,0x7},  // 1
    {0x6,0x9,0x2,0x4,0xF},  // 2
    {0xE,0x1,0x6,0x1,0xE},  // 3
    {0x5,0x5,0xF,0x1,0x1},  // 4
    {0xF,0x8,0xE,0x1,0xE},  // 5
    {0x6,0x8,0xE,0x9,0x6},  // 6
    {0xF,0x1,0x2,0x4,0x4},  // 7
    {0x6,0x9,0x6,0x9,0x6},  // 8
    {0x6,0x9,0x7,0x1,0x6},  // 9
};

static const u8 LETTER_S[5] = {0x6, 0x8, 0x6, 0x1, 0xE};

static void draw_glyph(SDL_Renderer *r, const u8 rows[5],
                        int cx, int cy, int s, Uint8 cr, Uint8 cg, Uint8 cb)
{
    // 4×5 glyph centred on (cx, cy)
    int x0 = cx - (4*s)/2, y0 = cy - (5*s)/2;
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 4; col++)
            if (rows[row] & (0x8 >> col)) {
                SDL_Rect dot = {x0 + col*s, y0 + row*s, s, s};
                SDL_RenderFillRect(r, &dot);
            }
}

// Two-digit label centred on (cx, cy); total block is (9s)×(5s)
static void draw_number(SDL_Renderer *r, int n, int cx, int cy, int s,
                         Uint8 cr, Uint8 cg, Uint8 cb)
{
    int x0 = cx - (9*s)/2, y0 = cy - (5*s)/2;
    // first digit at x0, second at x0+5s  (4 wide + 1 gap = 5)
    draw_glyph(r, DIGITS[n/10], x0 + 2*s,       cy, s, cr, cg, cb);
    draw_glyph(r, DIGITS[n%10], x0 + 2*s + 5*s, cy, s, cr, cg, cb);
}

// ____________________________________________________________________________
// Drawing primitives

static void fill_circle(SDL_Renderer *r, int cx, int cy, int radius)
{
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrt((double)(radius*radius - dy*dy));
        SDL_RenderDrawLine(r, cx-dx, cy+dy, cx+dx, cy+dy);
    }
}

static void stroke_circle(SDL_Renderer *r, int cx, int cy, int radius)
{
    int x = radius, y = 0, err = 1 - radius;
    while (x >= y) {
        SDL_RenderDrawPoint(r, cx+x, cy+y); SDL_RenderDrawPoint(r, cx+y, cy+x);
        SDL_RenderDrawPoint(r, cx-y, cy+x); SDL_RenderDrawPoint(r, cx-x, cy+y);
        SDL_RenderDrawPoint(r, cx-x, cy-y); SDL_RenderDrawPoint(r, cx-y, cy-x);
        SDL_RenderDrawPoint(r, cx+y, cy-x); SDL_RenderDrawPoint(r, cx+x, cy-y);
        y++;
        if (err < 0) err += 2*y + 1;
        else { x--; err += 2*(y-x) + 1; }
    }
}

// ____________________________________________________________________________
// Layout helpers

static int cell_x(int idx)  { return g_layout.ox + (idx%10) * g_layout.stride; }
static int cell_y(int idx)  { return g_layout.oy + (9 - idx/10) * g_layout.stride; }
static int cell_cx(int idx) { return cell_x(idx) + g_layout.pad_px/2; }
static int cell_cy(int idx) { return cell_y(idx) + g_layout.pad_px/2; }

static int is_virtual_corner(int idx)
{
    // 0, 9, 99 are unused; 90 is the SETUP button so it IS rendered
    return (idx == 0 || idx == 9 || idx == 99);
}

static int is_setup_cell(int idx)  { return idx == 90; }

static int is_round(int idx)
{
    if (is_virtual_corner(idx) || is_setup_cell(idx)) return 0;
    int col = idx%10, row = idx/10;
    return col==0 || col==9 || row==0 || row==9;
}

static int is_square(int idx)
{
    int col = idx%10, row = idx/10;
    return col>=1 && col<=8 && row>=1 && row<=8;
}

// Pad index at pixel (mx, my), or -1
static int pad_at(int mx, int my)
{
    for (int idx = 0; idx < 100; idx++) {
        if (is_virtual_corner(idx)) continue;
        int cx = cell_cx(idx), cy = cell_cy(idx);
        if (is_square(idx)) {
            int x = cell_x(idx), y = cell_y(idx), p = g_layout.pad_px;
            if (mx>=x && mx<x+p && my>=y && my<y+p) return idx;
        } else {
            int r = g_layout.cr, dx = mx-cx, dy = my-cy;
            if (dx*dx + dy*dy <= r*r) return idx;
        }
    }
    return -1;
}

// ____________________________________________________________________________
// Colour helpers

#define LED2RGB(v) ((int)(v) * 255 / 63)

static Uint8 fg_color(int r, int g, int b)
{
    return (299*r + 587*g + 114*b) / 1000 > 110 ? 0 : 160;
}

// ____________________________________________________________________________
// Render a single cell

static void render_cell(SDL_Renderer *rend, int idx, int pressed, int hovered)
{
    int led_i = is_setup_cell(idx) ? 100 : idx;
    int ri = LED2RGB(g_led_r[led_i]);
    int gi = LED2RGB(g_led_g[led_i]);
    int bi = LED2RGB(g_led_b[led_i]);
    int dark = (ri < 8 && gi < 8 && bi < 8);

    int fr = dark ? 36 : ri;
    int fg = dark ? 36 : gi;
    int fb = dark ? 36 : bi;

    if (hovered && !pressed) {
        fr = SDL_min(255, fr+55); fg = SDL_min(255, fg+55); fb = SDL_min(255, fb+55);
    }
    if (pressed) {
        fr = fr*2/3; fg = fg*2/3; fb = fb*2/3;
    }

    int cx = cell_cx(idx), cy = cell_cy(idx);
    Uint8 tc = fg_color(dark ? 36 : ri, dark ? 36 : gi, dark ? 36 : bi);
    int s = g_layout.fs;

    if (is_round(idx) || is_setup_cell(idx)) {
        int r = g_layout.cr - (pressed ? 2 : 0);
        SDL_SetRenderDrawColor(rend, fr, fg, fb, 255);
        fill_circle(rend, cx, cy, r);
        SDL_SetRenderDrawColor(rend, dark ? 75 : 18, dark ? 75 : 18, dark ? 75 : 18, 255);
        stroke_circle(rend, cx, cy, r);
        if (is_setup_cell(idx))
            draw_glyph(rend, LETTER_S, cx, cy, s, tc, tc, tc);
        else
            draw_number(rend, idx, cx, cy, s, tc, tc, tc);
    } else {
        int inset = pressed ? SDL_max(2, g_layout.pad_px/20) : 0;
        SDL_Rect fill   = {cell_x(idx)+inset,   cell_y(idx)+inset,
                           g_layout.pad_px-2*inset, g_layout.pad_px-2*inset};
        SDL_Rect border = {cell_x(idx), cell_y(idx), g_layout.pad_px, g_layout.pad_px};
        SDL_SetRenderDrawColor(rend, fr, fg, fb, 255);
        SDL_RenderFillRect(rend, &fill);
        SDL_SetRenderDrawColor(rend, dark ? 62 : 12, dark ? 62 : 12, dark ? 62 : 12, 255);
        SDL_RenderDrawRect(rend, &border);
        draw_number(rend, idx, cx, cy, s, tc, tc, tc);
    }
}

static void render_all(SDL_Renderer *rend, int hover, int pressed)
{
    SDL_SetRenderDrawColor(rend, 12, 12, 12, 255);
    SDL_RenderClear(rend);
    for (int idx = 0; idx < 100; idx++) {
        if (is_virtual_corner(idx)) continue;
        render_cell(rend, idx, idx==pressed, idx==hover && idx!=pressed);
    }
    SDL_RenderPresent(rend);
}

// ____________________________________________________________________________

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    memset(g_flash, 0xFF, sizeof(g_flash));

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    int win_w = 900, win_h = 900;
    SDL_Window *win = SDL_CreateWindow(
        "Launchpad Pro – MC707 Simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_Renderer *rend = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend) rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!rend) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    g_layout = compute_layout(win_w, win_h);

    printf("Initialising...\n");
    app_init(g_adc);
    printf("Ready. Window is resizable.\n");
    printf("  Click any pad or the SETUP circle (top-left).\n");
    printf("  Space = 1 timer tick   T = 100 ticks   Q/Esc = quit\n\n");
    fflush(stdout);

    int pressed = -1, hover = -1, running = 1;
    render_all(rend, hover, pressed);

    while (running) {
        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 50)) continue;

        switch (ev.type) {
        case SDL_QUIT:
            running = 0;
            break;

        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                SDL_GetRendererOutputSize(rend, &win_w, &win_h);
                g_layout = compute_layout(win_w, win_h);
                hover = pad_at(0, 0);   // reset hover (mouse pos unknown after resize)
                hover = -1;
                render_all(rend, hover, pressed);
            }
            break;

        case SDL_MOUSEMOTION: {
            int prev = hover;
            hover = pad_at(ev.motion.x, ev.motion.y);
            if (hover != prev) render_all(rend, hover, pressed);
            break;
        }

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                int idx = pad_at(ev.button.x, ev.button.y);
                if (idx >= 0) {
                    pressed = idx;
                    if (is_setup_cell(idx)) {
                        printf("[SIM] SETUP press\n"); fflush(stdout);
                        app_surface_event(TYPESETUP, 0, 127);
                    } else {
                        printf("[SIM] pad %d press\n", idx); fflush(stdout);
                        app_surface_event(TYPEPAD, (u8)idx, 127);
                    }
                    render_all(rend, hover, pressed);
                }
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT && pressed >= 0) {
                if (is_setup_cell(pressed)) {
                    app_surface_event(TYPESETUP, 0, 0);
                } else {
                    printf("[SIM] pad %d release\n", pressed); fflush(stdout);
                    app_surface_event(TYPEPAD, (u8)pressed, 0);
                }
                pressed = -1;
                render_all(rend, hover, pressed);
            }
            break;

        case SDL_KEYDOWN:
            switch (ev.key.keysym.sym) {
            case SDLK_q: case SDLK_ESCAPE:
                running = 0; break;
            case SDLK_SPACE:
                app_timer_event();
                render_all(rend, hover, pressed);
                break;
            case SDLK_t:
                printf("[SIM] 100 timer ticks\n"); fflush(stdout);
                for (int i = 0; i < 100; i++) app_timer_event();
                render_all(rend, hover, pressed);
                break;
            }
            break;
        }
    }

    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
