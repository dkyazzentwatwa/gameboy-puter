# Gameboy-Puter

Arduino CLI-first Game Boy / Game Boy Color emulator firmware for the M5Stack
Cardputer and Cardputer ADV. It uses Walnut-CGB for emulation and the Cardputer
microSD slot for ROM files.

This repo does **not** include commercial games or copyrighted ROMs. Put your
own legally obtained `.gb` and `.gbc` files on a FAT32 microSD card.

## Current Status

This fork is now a source-first Arduino sketch, not an M5Burner binary package.
The firmware currently provides:

- Arduino CLI compile/upload from the repo root
- microSD ROM launcher for `.gb` files in the default fast build
- Preferred `/roms` folder with root-folder fallback
- Basic Cardputer keyboard controls
- Guarded ROM loading with clear error screens
- Hybrid RAM-backed ROM loading with SD cache fallback for larger Game Boy ROMs
- Serial performance output for frame timing, draw timing, ROM backing, and cache hits
- Battery-backed SRAM persistence to `.sav` files
- Return-to-launcher shortcut during gameplay
- No-SD boot screen with Enter-to-retry after inserting a card

The default wrapper build is fast DMG/Game Boy mode for classic `.gb` ROMs.
`GBPUTER_MODE=compat` keeps the older GB/GBC-compatible build, but it can be
slow on Cardputer-class hardware. Audio, savestates, BLE controllers, advanced
scaling, and the full feature set described by older upstream release notes are
not implemented in this source tree yet.

## ROM Setup

1. Format a microSD card as FAT32.
2. Create a folder named `/roms`.
3. Copy your `.gb` files into `/roms`.
4. Insert the card into the Cardputer before booting the firmware.

The launcher also scans the SD card root for `.gb` files, but `/roms` is the
recommended layout. Save RAM files are written beside the ROM as `.sav`.
The default fast build hides `.gbc` files instead of launching them. Use compat
mode when you intentionally want to test Game Boy Color ROMs.

## Arduino CLI Setup

Install Arduino CLI, then install the M5Stack ESP32 board package and required
libraries:

```bash
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Cardputer M5Unified M5GFX
```

Compile the recommended fast `.gb` build from the repo root:

```bash
arduino-cli compile \
  --fqbn 'm5stack:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc' \
  --build-property 'build.extra_flags=-DGBPUTER_FAST_DMG=1' \
  /Users/cypher/Documents/GitHub/gameboy-puter
```

Or use the wrapper:

```bash
./scripts/compile.sh
```

Compile the compatibility build when you want to test `.gbc` support:

```bash
GBPUTER_MODE=compat ./scripts/compile.sh
```

## Flash

Find the current serial port:

```bash
arduino-cli board list
```

Flash by passing the port explicitly:

```bash
./scripts/flash.sh /dev/cu.usbmodem3101
```

You can also set `PORT`:

```bash
PORT=/dev/cu.usbmodem3101 ./scripts/flash.sh
```

Flash the compatibility build by adding `GBPUTER_MODE=compat` to the same
command.

Monitor serial output:

```bash
arduino-cli monitor -p /dev/cu.usbmodem3101 -c baudrate=115200
```

## Controls

Launcher:

| Action | Keys |
|---|---|
| Move up | `;`, `e`, or `w` |
| Move down | `.`, or `s` |
| Launch ROM | `Enter` |

In game:

| Game Boy input | Cardputer keys |
|---|---|
| Up | `e`, `w`, or `;` |
| Down | `s` or `.` |
| Left / Right | `a` / `d` |
| B / A | `k`, `j`, `,` / `l`, `i`, `/`, `Space` |
| Start | `1`, `p`, or `Enter` |
| Select | `2`, `0`, `o`, or `Tab` |
| Toggle frame skip | `3` or `f` |
| Exit to launcher | `Fn` + `Del` |

The fast build may enable Walnut's 30 FPS frame skip automatically if the first
few seconds of measured frame work exceed the Game Boy frame budget. Press `f`
or `3` during gameplay to toggle it manually.

## Performance Output

Open the serial monitor while a ROM is running to see timing lines:

```text
[perf] fps=58.9 avg_us=14200 emu_us=11800 draw_us=2100 backing=ram cache_h=0 cache_m=0 frame_skip=off free_heap=92112
```

`backing=ram` means the full ROM fit in heap. `backing=sd` means the firmware is
using the SD page cache, and `cache_m` should not climb rapidly during normal
play after the intro settles.

## Troubleshooting

- **`main file missing from sketch`**: make sure the sketch file is named
  `gameboy-puter.ino` and you are compiling the repo root.
- **No SD card**: the firmware still boots. Insert a FAT32 microSD card and
  press `Enter` to retry mounting it.
- **No ROMs found**: verify the SD card is FAT32 and files end in `.gb`. The
  default fast build intentionally hides `.gbc` files.
- **Invalid checksum**: the ROM header did not validate. Try a clean dump with
  an unchanged `.gb` or `.gbc` extension.
- **ROM/cache memory error**: restart the device and try again. The firmware
  loads smaller ROMs into RAM and falls back to SD paging for larger ROMs, but
  still needs enough heap for the page cache and cartridge save RAM.
- **No sound**: audio is intentionally disabled in this reliability pass.

## Third-Party Code And Assets

- Walnut-CGB is included under `walnut-cgb/` and is MIT licensed.
- MiniGB APU code is included under `minigb_apu_cardputer/` and is MIT licensed.
- Cardputer arcade and stereo TV border assets include their original
  CC BY-SA 4.0 license notes in this repo.
- The root firmware code in this fork does not currently declare a separate
  project license. See [LICENSES.md](LICENSES.md) before redistributing.
