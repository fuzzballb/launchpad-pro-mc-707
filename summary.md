# Project Summary — Launchpad Pro MC-707 Clip Launcher

## What it does

Custom open firmware for the original Novation Launchpad Pro (STM32F103, ARM Cortex-M3) that turns it into a clip launcher for the Roland MC-707. The MC-707 accepts MIDI Program Change messages to trigger clips per track. This firmware stores up to 8 presets (pad layouts) in flash and sends the right PC messages when pads are pressed.

---

## Hardware layout

The Launchpad Pro's pad grid is addressed as a flat index: `index = row * 10 + col`, where row and col both run 0–9.

```
Row 9:  90  91  92  93  94  95  96  97  98  99   ← top edge (round buttons + corners)
Row 8:  80  81  82  83  84  85  86  87  88  89
Row 7:  70  71  72  73  74  75  76  77  78  79
Row 6:  60  61  62  63  64  65  66  67  68  69
Row 5:  50  51  52  53  54  55  56  57  58  59
Row 4:  40  41  42  43  44  45  46  47  48  49
Row 3:  30  31  32  33  34  35  36  37  38  39
Row 2:  20  21  22  23  24  25  26  27  28  29
Row 1:  10  11  12  13  14  15  16  17  18  19
Row 0:   0   1   2   3   4   5   6   7   8   9   ← bottom edge (round buttons + corners)
         ^col0                           ^col9
```

- **Inner 8×8 grid**: rows 1–8, cols 1–8 (indices 11–88) — square pads
- **Left column** (col 0, rows 1–8): indices 10, 20, 30, 40, 50, 60, 70, 80 — round preset buttons
- **Top edge** (row 9, cols 1–8): indices 91–98 — round buttons; col 9 (99) is a corner (unused)
- **Bottom edge** (row 0, cols 1–8): indices 1–8 — round buttons
- **Corners** (0, 9, 90, 99): virtual / unused

---

## MIDI mapping

When an active inner-grid pad is pressed in play mode:

```
MIDI Program Change
  channel  = col - 1          (col 1 → ch 0, col 8 → ch 7)
  program  = 8 - row          (row 8 → prog 0, row 1 → prog 7)
```

Sent on both USBMIDI and DINMIDI ports simultaneously.

Bottom-edge buttons (row 0, col 1–8) send:
```
  channel  = col - 1
  program  = 8                (intended as the "empty/stop" slot on the MC-707)
```

---

## Firmware — `src/app.c`

### Modes

| Mode | Button 91 | Inner pads | Left column |
|------|-----------|------------|-------------|
| **Play** (default) | press to toggle | blue = active, yellow = playing; press sends MIDI | select preset slot |
| **Prog** | press to toggle | red = active; press toggles on/off | select slot to edit; hold btn 1 + slot = delete |

### Preset storage (RAM)

```c
#define PRESET_COUNT  8
static u8  g_preset[PRESET_COUNT][8];   // 64-bit bitmask per slot
static u8  g_preset_valid[PRESET_COUNT];
static u8  g_playing_row[9];            // per-column currently-playing row (0 = none)
```

Bitmask: `bit = (row-1)*8 + (col-1)`, spread over 8 bytes.

### Flash layout

```
Offset 0:       magic byte 0xAB
Offsets 1–8:    g_preset_valid[0..7]
Offsets 9–72:   g_preset[0..7][0..7]   (8 bytes × 8 slots)
Total: 73 bytes
```

Auto-saved to flash on every pad toggle. Loaded on boot if magic byte matches.

### LED colour scheme

| Situation | Colour |
|-----------|--------|
| Play mode — active pad | Blue |
| Play mode — currently playing pad | Yellow (stays until another pad or bottom-edge pressed) |
| Play mode — inactive pad | Off |
| Prog mode — active pad | Red |
| Prog mode — inactive pad | Off |
| Left column — selected slot in prog | Red |
| Left column — selected slot in play | Bright yellow |
| Left column — slot has data | Dim yellow |
| Left column — empty slot | Off |
| Button 91 — play mode | Dim teal |
| Button 91 — prog mode | Orange |

---

## Building

### Prerequisites (Linux)

```bash
# ARM cross-compiler
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-binutils arm-none-eabi-newlib
# SDL2 for GUI simulator
sudo pacman -S sdl2
```

### Targets

```bash
make               # build launchpad_pro.syx (runs basic simulator test as part of build)
make gui           # build and launch SDL2 GUI simulator
```

The `libintelhex` submodule must be initialised:
```bash
git submodule update --init --recursive
```

### Key build outputs

| File | Description |
|------|-------------|
| `build/launchpad_pro.syx` | Firmware ready to flash via SysEx |
| `build/simulator` | CLI simulator (run with `-i` for interactive mode) |
| `build/gui_simulator` | SDL2 GUI simulator |

---

## Simulators

### CLI simulator

```bash
./build/simulator -i
```

Commands: `p <idx>` press, `r <idx>` release, `s` setup button, `m <port> <st> <d1> <d2>` MIDI, `t [n]` timer ticks, `g` redraw, `q` quit.

### GUI simulator (`make gui`)

- Click pad to press/release
- Space = 1 timer tick, T = 100 ticks
- Q / Esc = quit
- Window is resizable

---

## Flashing to the device

1. Hold **Setup** while plugging in USB — device boots into bootloader (grid shows a dim pattern)
2. Check MIDI device renamed: `amidi -l` should show `Launchpad Pro` (not `Launchpad Open ...`)
3. Flash:
   ```bash
   amidi -p hw:2,0,0 -s build/launchpad_pro.syx
   ```
   Adjust `hw:2,0,0` to match your system's port from `amidi -l`.

To restore factory firmware: flash `resources/Launchpad Pro-1.0.154.syx` the same way.

---

## Key files

| Path | Purpose |
|------|---------|
| `src/app.c` | All application logic — edit this |
| `include/app.h` | HAL API declarations |
| `include/app_defs.h` | Type aliases, constants (MAXLED=63, TYPEPAD, etc.) |
| `tools/simulator.c` | CLI simulator + HAL stub |
| `tools/gui_simulator.c` | SDL2 GUI simulator + HAL stub |
| `tools/hextosyx.cpp` | Converts ELF hex to SysEx for flashing |
| `lib/launchpad_pro.a` | Pre-compiled HAL library (low-level LED/MIDI/flash drivers) |
| `resources/Launchpad Pro-1.0.154.syx` | Factory firmware (use to recover) |
