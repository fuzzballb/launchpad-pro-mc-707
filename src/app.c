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

#include "app.h"
#include <string.h>

// ____________________________________________________________________________
// Constants

#define PRESET_COUNT  8       // slots 0-7, mapped to left-col buttons 10-80
#define MODE_PLAY     0       // default: blue active pads, press to send MIDI
#define MODE_PROG     1       // editing: red active pads, toggle on/off

// Button indices
#define BTN_MODE      91      // top row: toggle play/prog mode
#define BTN_DELETE    1       // bottom row: hold + left-col = delete that preset

// ____________________________________________________________________________
// State

static const u16 *g_ADC;

static u8  g_mode         = MODE_PLAY;
static s8  g_slot         = -1;    // currently selected preset slot (-1 = none)
static u8  g_btn_del_held = 0;     // is BTN_DELETE currently held?

// Each preset is a 64-bit bitmask over the 8×8 inner grid.
// Bit layout: bit = (row-1)*8 + (col-1), stored across 8 bytes.
static u8  g_preset[PRESET_COUNT][8];
static u8  g_preset_valid[PRESET_COUNT];  // 1 if slot has at least one active pad

// Per-column playing state: g_playing_row[col] = row currently playing (0 = none).
// col range 1-8; row range 1-8.
static u8  g_playing_row[9];  // index 0 unused

// ____________________________________________________________________________
// Preset bit helpers

static int pad_is_active(int slot, int col, int row)
{
    if (slot < 0 || slot >= PRESET_COUNT) return 0;
    int bit = (row-1)*8 + (col-1);
    return (g_preset[slot][bit >> 3] >> (bit & 7)) & 1;
}

static void pad_toggle(int slot, int col, int row)
{
    int bit = (row-1)*8 + (col-1);
    g_preset[slot][bit >> 3] ^= (u8)(1 << (bit & 7));

    // recompute valid flag
    g_preset_valid[slot] = 0;
    for (int b = 0; b < 8; b++)
        if (g_preset[slot][b]) { g_preset_valid[slot] = 1; break; }
}

static void delete_slot(int slot)
{
    memset(g_preset[slot], 0, 8);
    g_preset_valid[slot] = 0;
}

// ____________________________________________________________________________
// Flash  (auto-saved on every change)

#define FLASH_MAGIC  0xAB
// Layout: [0]=magic  [1..8]=valid_flags  [9..72]=preset_bits[0..7][0..7]
#define FLASH_SIZE   (1 + PRESET_COUNT + PRESET_COUNT * 8)   // 73 bytes

static void flash_save(void)
{
    u8 buf[FLASH_SIZE];
    buf[0] = FLASH_MAGIC;
    for (int s = 0; s < PRESET_COUNT; s++) {
        buf[1 + s] = g_preset_valid[s];
        for (int b = 0; b < 8; b++)
            buf[1 + PRESET_COUNT + s*8 + b] = g_preset[s][b];
    }
    hal_write_flash(0, buf, FLASH_SIZE);
}

static void flash_load(void)
{
    u8 buf[FLASH_SIZE];
    hal_read_flash(0, buf, FLASH_SIZE);
    if (buf[0] != FLASH_MAGIC) return;
    for (int s = 0; s < PRESET_COUNT; s++) {
        g_preset_valid[s] = buf[1 + s] ? 1 : 0;
        for (int b = 0; b < 8; b++)
            g_preset[s][b] = buf[1 + PRESET_COUNT + s*8 + b];
    }
}

// ____________________________________________________________________________
// LED helpers

static void set_inner_leds(void)
{
    for (int row = 1; row <= 8; row++) {
        for (int col = 1; col <= 8; col++) {
            u8 idx = (u8)(row*10 + col);
            int active = (g_slot >= 0) && pad_is_active(g_slot, col, row);

            if (g_mode == MODE_PLAY) {
                if (active && g_playing_row[col] == (u8)row)
                    hal_plot_led(TYPEPAD, idx, MAXLED, MAXLED, 0);  // yellow = playing
                else if (active)
                    hal_plot_led(TYPEPAD, idx, 0, 0, MAXLED);       // blue = ready
                else
                    hal_plot_led(TYPEPAD, idx, 0, 0, 0);
            } else {
                hal_plot_led(TYPEPAD, idx, active ? MAXLED : 0, 0, 0);
            }
        }
    }
}

static void set_left_col_leds(void)
{
    for (int slot = 0; slot < PRESET_COUNT; slot++) {
        u8 idx = (u8)((slot + 1) * 10);  // 10, 20, ..., 80

        if (slot == (int)g_slot && g_mode == MODE_PROG) {
            hal_plot_led(TYPEPAD, idx, MAXLED, 0, 0);          // red  = being edited
        } else if (slot == (int)g_slot) {
            hal_plot_led(TYPEPAD, idx, MAXLED, MAXLED, 0);     // bright yellow = active in play
        } else if (g_preset_valid[slot]) {
            hal_plot_led(TYPEPAD, idx, MAXLED/2, MAXLED/2, 0); // dim yellow = has data
        } else {
            hal_plot_led(TYPEPAD, idx, 0, 0, 0);               // off = empty
        }
    }
}

static void set_mode_btn_led(void)
{
    if (g_mode == MODE_PROG)
        hal_plot_led(TYPEPAD, BTN_MODE, MAXLED, MAXLED/3, 0);  // orange = prog active
    else
        hal_plot_led(TYPEPAD, BTN_MODE, 0, 8, 12);             // dim teal = play (tap to edit)
}

static void update_all_leds(void)
{
    set_inner_leds();
    set_left_col_leds();
    set_mode_btn_led();
}

// ____________________________________________________________________________
// App callbacks

void app_surface_event(u8 type, u8 index, u8 value)
{
    if (type != TYPEPAD) return;

    int col = index % 10;
    int row = index / 10;

    // ---- Mode toggle button (91) ----
    if (index == BTN_MODE) {
        if (!value) return;
        g_mode = (g_mode == MODE_PLAY) ? MODE_PROG : MODE_PLAY;
        update_all_leds();
        return;
    }

    // ---- Bottom edge (row 0, col 1-8): send MIDI in play mode ----
    // In play mode these trigger an empty/stop clip per track.
    // In prog mode, col 1 (BTN_DELETE) tracks hold state for delete gesture.
    if (row == 0 && col >= 1 && col <= 8) {
        if (g_mode == MODE_PLAY) {
            if (!value) return;
            hal_send_midi(USBMIDI, (u8)(0xC0 + col - 1), 8, 0);
            hal_send_midi(DINMIDI, (u8)(0xC0 + col - 1), 8, 0);
            g_playing_row[col] = 0;  // clear playing state for this track
            set_inner_leds();
        } else {
            // prog mode: only button 1 is the delete-hold
            if (index == BTN_DELETE)
                g_btn_del_held = value ? 1 : 0;
        }
        return;
    }

    // ---- Inner 8×8 pads ----
    if (col >= 1 && col <= 8 && row >= 1 && row <= 8) {
        if (g_mode == MODE_PLAY) {
            if (!value) return;
            if (g_slot >= 0 && pad_is_active(g_slot, col, row)) {
                g_playing_row[col] = (u8)row;
                set_inner_leds();
                hal_send_midi(USBMIDI, (u8)(0xC0 + col - 1), (u8)(8 - row), 0);
                hal_send_midi(DINMIDI, (u8)(0xC0 + col - 1), (u8)(8 - row), 0);
            }
        } else {  // MODE_PROG
            if (value && g_slot >= 0) {
                pad_toggle(g_slot, col, row);
                set_inner_leds();
                set_left_col_leds();  // valid flag may have changed
                flash_save();
            }
        }
        return;
    }

    if (!value) return;   // left column only acts on press

    // ---- Left column: preset slot buttons (col 0, rows 1-8) ----
    if (col == 0 && row >= 1 && row <= 8) {
        int slot = row - 1;
        if (g_mode == MODE_PROG && g_btn_del_held) {
            delete_slot(slot);
            update_all_leds();
            flash_save();
        } else {
            g_slot = (s8)slot;
            update_all_leds();
        }
        return;
    }
}

void app_midi_event(u8 port, u8 status, u8 d1, u8 d2)
{
    if (port == USBMIDI) hal_send_midi(DINMIDI, status, d1, d2);
    if (port == DINMIDI)  hal_send_midi(USBMIDI, status, d1, d2);
}

void app_sysex_event(u8 port, u8 *data, u16 count)  { }
void app_aftertouch_event(u8 index, u8 value)        { }
void app_cable_event(u8 type, u8 value)              { }
void app_timer_event(void)                           { }

void app_init(const u16 *adc_raw)
{
    g_ADC             = adc_raw;
    g_mode            = MODE_PLAY;
    g_slot            = -1;
    g_btn_del_held    = 0;
    memset(g_preset,       0, sizeof(g_preset));
    memset(g_preset_valid, 0, sizeof(g_preset_valid));
    memset(g_playing_row,  0, sizeof(g_playing_row));

    flash_load();
    update_all_leds();
}
