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

Implements Move Anything plugin_api_v1/v2:
- `on_load`/`create_instance`: Loads ROMs, initializes emulator, builds patch list
- `on_midi`: Queues MIDI for emulator thread
- `set_param`: preset, octave_transpose, program_change, next_bank, prev_bank, mode, performance, part, jump_to_expansion, load_user_patch, write_patch_N, write_performance_N, save_nvram
- `get_param`: preset_name, patch_name, octave_transpose, current_patch, bank_name, mode, expansion_list, user_patch_list, state, etc.
- `render_block`: Pulls audio from ring buffer filled by emulator thread

### State Serialization

State is saved/restored via `state` parameter containing JSON:
- mode (Patch/Performance)
- preset, performance, part indices
- octave_transpose
- expansion_index, expansion_bank_offset

**Deferred state restoration**: If state is set before loading completes, it's queued and applied after warmup finishes.

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

### User Patch Storage

- 64 user patch slots in NVRAM (0x1000-0x6A80)
- Save edited patches via Edit > Save Patch menu
- Load saved patches via Main > User Patches menu
- Patches persist to jv880_nvram.bin via save_nvram

### Jump to Expansion

- Main menu includes "Jump to Expansion" option
- Lists all loaded expansions with patch counts
- Selecting an expansion jumps to its first patch in the unified list
- Provides quick access without scrolling through full patch list

### Mode-Specific UI

- Patch mode: Track buttons 1-4 select tones (LEDs show enabled/selected)
- Performance mode: Track buttons disabled (LEDs off), Step buttons 1-8 select parts
- Encoder macros differ by mode:
  - Patch: Cutoff, Resonance, Attack, Release, LFO Rate, LFO Depth, FX Send, Level
  - Performance: Level, Pan, Coarse Tune, Fine Tune, Key Range Lo/Hi, Velocity Sense/Max

## ROM Requirements

- v1.0.0 ROMs required (1.0.1 causes CPU traps)
- Required files: jv880_rom1.bin, jv880_rom2.bin, jv880_waverom1.bin, jv880_waverom2.bin, jv880_nvram.bin

## Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "sound_generator"` in module.json. Chain presets installed to main repo's `modules/chain/patches/` by install script.
