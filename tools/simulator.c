/******************************************************************************

 Copyright (c) 2015, Focusrite Audio Engineering Ltd.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 * Neither the name of Focusrite Audio Engineering Ltd., nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

// ____________________________________________________________________________
//
// LED state — 100 pads (index 0-99) + 1 setup button (index 100)
// ____________________________________________________________________________

static u8 g_led_r[101];
static u8 g_led_g[101];
static u8 g_led_b[101];

// ____________________________________________________________________________
//
// Flash storage simulation
// ____________________________________________________________________________

static u8 g_flash[USER_AREA_SIZE];

// ____________________________________________________________________________
//
// HAL implementation
// ____________________________________________________________________________

void hal_plot_led(u8 type, u8 index, u8 red, u8 green, u8 blue)
{
    int i = (type == TYPESETUP) ? 100 : (index < 100 ? index : -1);
    if (i < 0) return;
    g_led_r[i] = red;
    g_led_g[i] = green;
    g_led_b[i] = blue;
}

void hal_read_led(u8 type, u8 index, u8 *red, u8 *green, u8 *blue)
{
    int i = (type == TYPESETUP) ? 100 : (index < 100 ? index : 0);
    *red   = g_led_r[i];
    *green = g_led_g[i];
    *blue  = g_led_b[i];
}

void hal_send_midi(u8 port, u8 status, u8 d1, u8 d2)
{
    const char *port_name = (port == USBSTANDALONE) ? "USB-SA"
                          : (port == USBMIDI)       ? "USB-MI" : "DIN   ";
    printf("  [MIDI out] %s  status=0x%02X  d1=0x%02X(%3d)  d2=0x%02X(%3d)\n",
           port_name, status, d1, d1, d2, d2);
}

void hal_send_sysex(u8 port, const u8 *data, u16 length)
{
    const char *port_name = (port == USBSTANDALONE) ? "USB-SA"
                          : (port == USBMIDI)       ? "USB-MI" : "DIN   ";
    printf("  [SYSEX out] %s  len=%d\n", port_name, length);
}

void hal_read_flash(u32 offset, u8 *data, u32 length)
{
    if (offset + length > USER_AREA_SIZE) return;
    memcpy(data, g_flash + offset, length);
}

void hal_write_flash(u32 offset, const u8 *data, u32 length)
{
    if (offset + length > USER_AREA_SIZE) return;
    memcpy(g_flash + offset, data, length);
    printf("  [FLASH] wrote %lu bytes at offset %lu\n", length, offset);
}

u8 hal_read_device_id() { return 0; }
u8 hal_read_layout_text() { return 0; }

// ____________________________________________________________________________
//
// Grid display (ANSI 24-bit colour)
// ____________________________________________________________________________

static void print_grid(void)
{
    // MAXLED is 63, scale to 0-255
    #define SCALE(v) ((int)(v) * 255 / 63)

    printf("\033[2J\033[H");  // clear screen, cursor home
    printf("=== Launchpad Pro Simulator ===\n");

    // Setup button
    int sr = SCALE(g_led_r[100]), sg = SCALE(g_led_g[100]), sb = SCALE(g_led_b[100]);
    printf("SETUP: \033[48;2;%d;%d;%dm   \033[0m\n\n", sr, sg, sb);

    // Grid: row 9 (top) down to row 0 (bottom)
    for (int row = 9; row >= 0; row--) {
        printf("  ");
        for (int col = 0; col <= 9; col++) {
            int idx = row * 10 + col;
            int r = SCALE(g_led_r[idx]);
            int g = SCALE(g_led_g[idx]);
            int b = SCALE(g_led_b[idx]);
            if (r == 0 && g == 0 && b == 0) {
                // dark pad — show index in dim grey
                printf("\033[48;2;30;30;30m\033[38;2;80;80;80m%02d\033[0m ", idx);
            } else {
                // lit pad — white text on coloured background
                printf("\033[48;2;%d;%d;%dm\033[38;2;255;255;255m%02d\033[0m ", r, g, b, idx);
            }
        }
        printf("\n");
    }
    printf("\n");

    #undef SCALE
}

// ____________________________________________________________________________
//
// Event wrappers
// ____________________________________________________________________________

static u16 raw_ADC[64];

static void sim_app_init(void)
{
    printf("calling app_init()...\n");
    app_init(raw_ADC);
}

static void sim_app_surface_event(u8 type, u8 index, u8 value)
{
    printf("calling app_surface_event(%d, %d, %d)...\n", type, index, value);
    app_surface_event(type, index, value);
}

static void sim_app_midi_event(u8 port, u8 status, u8 d1, u8 d2)
{
    printf("calling app_midi_event(%d, 0x%02X, 0x%02X, 0x%02X)...\n", port, status, d1, d2);
    app_midi_event(port, status, d1, d2);
}

static void sim_app_timer_event(void)
{
    app_timer_event();
}

// ____________________________________________________________________________
//
// Interactive mode
// ____________________________________________________________________________

static void print_help(void)
{
    printf("Commands:\n");
    printf("  p <idx>              press pad <idx> (value=127)\n");
    printf("  r <idx>              release pad <idx> (value=0)\n");
    printf("  s                    press setup button\n");
    printf("  m <port> <st> <d1> <d2>  send MIDI event (hex values OK)\n");
    printf("  t [n]                fire n timer ticks (default=1)\n");
    printf("  g                    redraw grid\n");
    printf("  q                    quit\n");
    printf("  ?                    show this help\n\n");
}

static void run_interactive(void)
{
    printf("Initialising...\n");
    sim_app_init();
    print_grid();
    print_help();

    char line[256];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        char cmd;
        if (sscanf(line, " %c", &cmd) != 1) continue;

        if (cmd == 'q') break;

        if (cmd == '?') {
            print_help();
            continue;
        }

        if (cmd == 'g') {
            print_grid();
            continue;
        }

        if (cmd == 'p') {
            int idx;
            if (sscanf(line, " p %d", &idx) == 1 && idx >= 0 && idx < 100) {
                sim_app_surface_event(TYPEPAD, (u8)idx, 127);
                print_grid();
            } else {
                printf("Usage: p <idx>  (0-99)\n");
            }
            continue;
        }

        if (cmd == 'r') {
            int idx;
            if (sscanf(line, " r %d", &idx) == 1 && idx >= 0 && idx < 100) {
                sim_app_surface_event(TYPEPAD, (u8)idx, 0);
                print_grid();
            } else {
                printf("Usage: r <idx>  (0-99)\n");
            }
            continue;
        }

        if (cmd == 's') {
            sim_app_surface_event(TYPESETUP, 0, 127);
            app_surface_event(TYPESETUP, 0, 0);  // auto-release
            print_grid();
            continue;
        }

        if (cmd == 'm') {
            int port, status, d1, d2;
            if (sscanf(line, " m %i %i %i %i", &port, &status, &d1, &d2) == 4) {
                sim_app_midi_event((u8)port, (u8)status, (u8)d1, (u8)d2);
                print_grid();
            } else {
                printf("Usage: m <port> <status> <d1> <d2>  (hex ok, e.g. m 1 0x90 60 127)\n");
            }
            continue;
        }

        if (cmd == 't') {
            int n = 1;
            sscanf(line, " t %d", &n);
            if (n < 1) n = 1;
            if (n > 10000) n = 10000;
            printf("Firing %d timer tick(s)...\n", n);
            for (int i = 0; i < n; i++) sim_app_timer_event();
            print_grid();
            continue;
        }

        printf("Unknown command '%c'. Type ? for help.\n", cmd);
    }

    printf("\nBye!\n");
}

// ____________________________________________________________________________
//
// Script mode (default — used by make)
// ____________________________________________________________________________

static void run_script(void)
{
    sim_app_init();

    sim_app_surface_event(TYPEPAD, 35, 127);
    sim_app_surface_event(TYPESETUP, 0, 127);
    sim_app_surface_event(TYPEPAD, 35, 0);

    sim_app_midi_event(USBSTANDALONE, NOTEON, 60, 127);
    sim_app_midi_event(USBSTANDALONE, NOTEON, 60, 0);

    const int timerTicks = 21;
    printf("sending %d timer events via app_timer_event()...\n", timerTicks);
    for (int i = 0; i < timerTicks; ++i)
        sim_app_timer_event();
}

// ____________________________________________________________________________

int main(int argc, char *argv[])
{
    int interactive = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) interactive = 1;
    }

    memset(g_flash, 0xFF, sizeof(g_flash));  // uninitialised flash = 0xFF

    if (interactive)
        run_interactive();
    else
        run_script();

    return 0;
}
