# Move Anything - JV-880 Module

Roland JV-880 synthesizer emulator module for [Move Anything](https://github.com/bobbydigitales/move-anything).

Based on [mini-jv880](https://github.com/giulioz/mini-jv880) by giulioz (which is based on [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) by nukeykt).

## Requirements

- Move Anything installed on your Ableton Move
- JV-880 ROM files (v1.0.0):
  - `jv880_rom1.bin` (32KB)
  - `jv880_rom2.bin` (256KB)
  - `jv880_waverom1.bin` (2MB)
  - `jv880_waverom2.bin` (2MB)
  - `jv880_nvram.bin` (optional, 32KB)
  - `jv880_expansion.bin` (optional, 8MB SR-JV80 expansion card)

**Note:** ROM version 1.0.0 is required. Version 1.0.1 ROMs do not work correctly.

## Expansion Cards

SR-JV80 expansion cards are supported. Rename your expansion ROM to `jv880_expansion.bin` and place it in the roms folder. The ROM will be automatically unscrambled on load.

Supported expansion cards include:
- SR-JV80-01 Pop through SR-JV80-19 House
- Other 8MB SR-JV80 format cards

## Building

```bash
./scripts/build.sh
```

This uses Docker for cross-compilation. The output will be in `dist/jv880/`.

## Installation

1. Place your ROM files in `dist/jv880/roms/`
2. Run:
```bash
./scripts/install.sh
```

Or manually copy `dist/jv880/` to `/data/UserData/move-anything/modules/` on your Move.

## Usage

- **Left/Right buttons or Jog wheel**: Change preset (MIDI program change)
- **Up/Down (+/-) buttons**: Octave transpose (±4 octaves)
- **Pads**: Play notes

## Performance

- CPU usage: ~80% on Move's CM4
- Latency: ~46ms

## License

This module includes code from:
- mini-jv880 by giulioz
- Nuked-SC55 by nukeykt

See LICENSE file for details.
