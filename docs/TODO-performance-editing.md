# TODO: Performance Editing & Patch Saving

This document tracks the work needed to fully support performance editing and patch saving.

## Current Status

**Working:**
- Patch NVRAM reading (temp patch at 0x0d70, 362 bytes)
- SysEx DT1 parameter writing (affects temp patch in real-time)
- Patch browsing and loading from ROM
- Performance browsing and loading
- Performance name reading from ROM2/NVRAM

**Not Working:**
- Performance parameter reading/writing (common + part params)
- Patch saving to permanent storage
- Performance saving to permanent storage

## SysEx Address Map (from Edisyn)

| Data Type | SysEx Address (AA BB CC DD) | Size |
|-----------|----------------------------|------|
| System Common | 00 00 00 00 | ~16 bytes |
| Temp Performance Common | 00 00 10 00 | 31 params |
| Temp Performance Part 1-8 | 00 00 18-1F 00 | 35 params each |
| Temp Patch Common | 00 08 20 00 | 45 bytes |
| Temp Patch Tone 1-4 | 00 08 28-2B 00 | 127 bytes each |
| Stored Patch (Internal) | 01 XX 20 00 | XX=patch# (0x40-0x7F) |
| Stored Patch (Card) | 02 XX 20 00 | XX=patch# |

## Known NVRAM Locations

| Offset | Size | Content |
|--------|------|---------|
| 0x0000 | 1 | Master Tune |
| 0x0002 | 1 | Reverb/Chorus flags |
| 0x000d | 1 | System settings (bit 5: LastSet) |
| 0x0011 | 1 | Mode flag (0=performance, 1=patch) |
| 0x00b0 | 3264 | Internal Performances (16 × 204 bytes) |
| 0x0d70 | 362 | Temporary Patch data |
| 0x67f0 | 2684 | Drum kit data |

## Performance Data Locations (DISCOVERED)

**Performance size: 0xCC (204 bytes)**
**Performance name: first 12 bytes**

| Bank | Location | Start Offset | Notes |
|------|----------|--------------|-------|
| Preset A | ROM2 | 0x10020 | "Jazz Split", "Softly...", etc. |
| Preset B | ROM2 | 0x18020 | "GTR Players", "YMBA Choir", etc. |
| Internal | NVRAM | 0x000b0 | "Syn Lead", "Encounter X", "Brass ComeOn", etc. |

ROM2 structure at 0x10000:
- 0x10000: ROM signature "Roland JV-80D"
- 0x10010: Bank header "JV-80 Preset A  " (16 bytes)
- 0x10020: First performance (204 bytes each)

Internal performances end at 0x0d70 (0x00b0 + 16×0xCC = 0x0d70), right where temp patch begins.

## Temp Performance SRAM Layout (DISCOVERED via Automated Mapping)

**Base offset: 0x206a (SRAM)**
**Total size: 204 bytes (same as stored format)**

### Performance Common Layout (offsets 0-27)

| Offset | SysEx | Name | Notes |
|--------|-------|------|-------|
| 0-11 | -- | Performance Name | 12 ASCII chars |
| 12 | 0x0C,0x0D,0x11 | **PACKED** | keymode (bits 0-1), reverbtype (bits 2-4), chorustype (bits 5-7) |
| 13 | 0x0E | reverblevel | |
| 14 | 0x0F | reverbtime | |
| 15 | 0x10 | reverbfeedback | |
| 16 | 0x12,0x16 | **PACKED** | choruslevel (bits 0-6), chorusoutput (bit 7) |
| 17 | 0x13 | chorusdepth | |
| 18 | 0x14 | chorusrate | |
| 19 | 0x15 | chorusfeedback | |
| 20-27 | 0x17-0x1E | voicereserve1-8 | 1 byte each |

### Part Data Layout (offsets 28-203)

**Part base offset: 28 bytes from temp perf start**
**Part stride: 22 bytes per part**

| Offset | SysEx | Name | Storage Notes |
|--------|-------|------|---------------|
| 0 | PACKED | **byte0** | See bit layout below |
| 1 | 0x02 | transmitprogramchange | Direct, 0-127 |
| 2 | 0x04 | transmitvolume | Direct, 0-127 |
| 3 | 0x06 | transmitpan | Direct, 0-127 |
| 4 | 0x08 | transmitkeyrangelower | Direct, 0-127 |
| 5 | 0x09 | transmitkeyrangeupper | Direct, 0-127 |
| 6 | 0x0A | transmitkeytranspose | Signed offset: stored = (val-64)&0xFF |
| 7 | 0x0B | transmitvelocitysense | Signed offset: stored = (val-64)&0xFF |
| 8 | 0x0C | transmitvelocitymax | Direct, 0-127 |
| 9 | 0x0D | transmitvelocitycurve | Direct, 0-6 |
| 10 | 0x0F | internalkeyrangelower | Direct, 0-127 |
| 11 | 0x10 | internalkeyrangeupper | Direct, 0-127 |
| 12 | 0x11 | internalkeytranspose | Signed offset: stored = (val-64)&0xFF |
| 13 | 0x12 | internalvelocitysense | Signed offset: stored = (val-64)&0xFF |
| 14 | 0x13 | internalvelocitymax | Direct, 0-127 |
| 15 | PACKED | **byte15** | See bit layout below |
| 16 | 0x17 | patchnumber | Direct, triggers patch load |
| 17 | 0x19 | partlevel | Direct, 0-127 |
| 18 | 0x1A | partpan | Direct, 0-127 |
| 19 | 0x1B | partcoarsetune | Signed offset: stored = (val-64)&0xFF |
| 20 | 0x1C | partfinetune | Signed offset: stored = (val-64)&0xFF |
| 21 | PACKED | **byte21** | See bit layout below |

### Packed Byte Bit Layouts (VERIFIED)

**Part byte 0** `[internalswitch:bit7][transmitswitch:bit6][outputselect:bits4-5][transmitchannel:bits0-3]`
- transmitswitch: bit 6 (0=off, 1=on)
- internalswitch: bit 7 (0=off, 1=on)
- outputselect: bits 4-5 (0-2)
- transmitchannel: bits 0-3 (0-15)

**Part byte 15** `[receiveprogramchange:bit7][receivevolume:bit6][receivehold1:bit5][??:bits3-4][internalvelocitycurve:bits0-2]`
- receiveprogramchange: bit 7 (0=off, 1=on)
- receivevolume: bit 6 (0=off, 1=on)
- receivehold1: bit 5 (0=off, 1=on)
- internalvelocitycurve: bits 0-2 (0-6)

**Part byte 21** `[receiveswitch:bit7][reverbswitch:bit6][chorusswitch:bit5][??:bit4][receivechannel:bits0-3]`
- receiveswitch: bit 7 (0=off, 1=on)
- reverbswitch: bit 6 (0=off, 1=on)
- chorusswitch: bit 5 (0=off, 1=on)
- receivechannel: bits 0-3 (0-15)

### Value Transformations

**Direct storage**: `stored = sysex_value`
**Signed offset storage**: `stored = (sysex_value - 64) & 0xFF`
- Used for: transpose, velocitysense, coarsetune, finetune
- Reading: `sysex_value = (stored + 64) & 0x7F` (or handle signed)
- Center value (64) stores as 0

## Implementation Tasks

### 1. Find Performance NVRAM Location
- [x] Search mini-jv880 source for performance data handling
- [x] Experiment by loading performances and dumping NVRAM
- [x] Check if jv880_juce has any clues
- [x] **FOUND**: See "Performance Data Locations" above

### 2. Implement Performance/Part Editing
- [x] SysEx builders for performance common params (`buildPerformanceCommonParam`)
- [x] SysEx builders for part params (`buildPartParam`)
- [x] UI menus for performance common (reverb, chorus, key mode)
- [x] UI menus for part editing (level, pan, tune, key range, etc.)
- [x] **FOUND temp performance in SRAM at offset 0x206a** (fixed buffer, same format as stored)
- [x] Add DSP handlers for reading performance common params from SRAM (`sram_perfCommon_<param>`)
- [x] **Automated parameter mapping completed** - see SRAM Layout above
- [x] Add DSP handlers for reading part params (level, pan, tune - working)
- [ ] Decode packed switch bit fields (offsets 15-16)
- [ ] Add DSP handlers for switch parameters once bit layout known

### 3. Implement Patch Saving
**Current state:** Edits only affect temp patch at NVRAM 0x0d70. Nothing persists.

Options to investigate:
- [ ] **Option A**: Write directly to NVRAM storage locations and save NVRAM file
  - Copy temp patch (0x0d70, 362 bytes) to Internal slot (0x008ce0 + slot*362 in ROM2 area?)
  - Need to find actual NVRAM locations for user patches
- [ ] **Option B**: Send "Write to Internal" SysEx command (if emulator supports it)
- [ ] **Option C**: Trigger emulator's internal save mechanism

### 4. Implement Performance Saving
**Current state:** Basic implementation complete.

- [x] Copy temp performance (SRAM 0x206a, 204 bytes) to Internal slot (NVRAM 0x00b0 + slot*204)
  - `write_performance_<0-15>` DSP command implemented
- [x] Add "Write Performance" UI action
  - Save submenu in Performance Edit with 16 Internal slots
- [x] NVRAM file persistence
  - Saves NVRAM immediately after writing performance

### 5. NVRAM Persistence
- [x] Add DSP command to save current NVRAM to file (`save_nvram`)
- [x] Call after explicit save action (Save menu triggers it)
- [x] Load NVRAM on startup (already implemented)
- [ ] Auto-save on module unload (not yet implemented)

### 6. Expansion + Performance Compatibility (RESOLVED)

**Solution implemented:** User can select which expansion card is "installed" from the Performance Edit menu.

**How it works:**
- "Expansion Card" submenu in Performance Edit shows available expansions
- User selects which expansion to use for this performance
- Matches real hardware: one card slot, but user chooses which card
- Patches in patchnumber 64-127 come from the selected expansion

**Behavior:**
- Patch browsing still works freely (selecting any patch loads its expansion)
- In Performance mode, parts using patchnumber 64-127 get patches from the selected expansion
- Switching expansions in performance mode resets the emulator (as on hardware)

**Implementation:**
- [x] `expansion_count`, `current_expansion`, `expansion_X_name` get_params
- [x] `load_expansion` set_param to change active expansion
- [x] "Expansion Card" menu in Performance Edit

## Resources

- [Edisyn JV-880 Editor Source](https://github.com/eclab/edisyn/tree/master/edisyn/synth/rolandjv880)
- [jv880_juce Source](https://github.com/giulioz/jv880_juce)
- [Roland JV-880 Owner's Manual](https://www.manualslib.com/manual/691699/Roland-Jv-880.html)
- Roland JV-880 MIDI Implementation (pages 217-230 of manual)

## Notes

The JV-880 emulator (mini-jv880) is based on Nuked-SC55 which emulates the hardware at a low level. The NVRAM is a 32KB SRAM that stores:
- System settings
- User patches (Internal bank)
- User performances
- Drum kit edits

The emulator loads NVRAM from file on startup and we need to implement saving it back.
