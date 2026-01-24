# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

Mini-JV module for Move Anything - a ROM-based synthesizer emulator based on mini-jv880.

## Architecture

```
src/
  dsp/
    jv880_plugin.cpp    # Main plugin, patch management, expansion support
    mcu.cpp/h           # H8/300 CPU emulator
    mcu_opcodes.cpp     # CPU instruction implementation
    pcm.cpp/h           # PCM wavetable synthesis
    lcd.cpp/h           # LCD emulation
  ui.js                 # JavaScript UI
  module.json           # Module metadata
  chain_patches/        # Signal Chain presets
```

## Key Implementation Details

### Plugin API

Implements Move Anything plugin_api_v1:
- `on_load`: Loads ROMs, initializes emulator, builds patch list
- `on_midi`: Queues MIDI for emulator thread
- `set_param`: preset, octave_transpose, program_change, next_bank, prev_bank
- `get_param`: preset_name, patch_name, octave_transpose, current_patch, bank_name, etc.
- `render_block`: Pulls audio from ring buffer filled by emulator thread

### Expansion ROM Support

- Expansions in `roms/expansions/` with "SR-JV80" in filename (case-insensitive .bin/.BIN)
- Sorted alphabetically by name for consistent ordering
- Auto-unscrambled on first load
- Patch cache (`patch_cache.bin`) speeds subsequent loads
- On-demand loading: expansion data loaded only when patch selected
- Bank pages for >64 patch expansions (select patches 1-64, 65-128, etc.)

### Threading Model

Background thread runs emulator at accelerated rate to fill audio ring buffer. Main thread pulls from buffer during render_block.

### Performance Mode

- Temp performance stored at SRAM offset 0x206a (204 bytes)
- Part data at offset 28 with 22-byte stride per part
- Supports reading/writing part parameters (level, pan, tune, patch, key range, velocity)
- Performances saved to NVRAM Internal slots (0x00b0, 16 slots × 204 bytes)
- Expansion card selection determines which expansion provides Card patches (patchnumber 64-127)

### Mode-Specific UI

- Patch mode: Track buttons 1-4 select tones (LEDs show enabled/selected)
- Performance mode: Track buttons disabled (LEDs off), Step buttons 1-8 select parts
- Encoder macros differ by mode:
  - Patch: Cutoff, Resonance, Attack, Release, LFO Rate, LFO Depth, FX Send, Level
  - Performance: Level, Pan, Coarse Tune, Fine Tune, Key Range Lo/Hi, Velocity Sense/Max

## ROM Requirements

- v1.0.0 ROMs required (1.0.1 causes CPU traps)
- Files: jv880_rom1.bin, jv880_rom2.bin, jv880_waverom1.bin, jv880_waverom2.bin
- Optional: jv880_nvram.bin

## Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "sound_generator"` in module.json. Chain presets installed to main repo's `modules/chain/patches/` by install script.
