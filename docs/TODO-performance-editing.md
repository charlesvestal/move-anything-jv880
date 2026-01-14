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
- [ ] Add DSP handlers for reading part params (need to decode stored part format)
- [ ] Part format appears to be ~21 bytes/part, more compact than SysEx format

### 3. Implement Patch Saving
Options to investigate:
- [ ] **Option A**: Write directly to NVRAM storage locations and save NVRAM file
- [ ] **Option B**: Send "Write to Internal" SysEx command (if emulator supports it)
- [ ] **Option C**: Trigger emulator's internal save mechanism

### 4. Implement Performance Saving
- [ ] Same as patch saving, but for performance slots

### 5. NVRAM Persistence
- [ ] Add DSP command to save current NVRAM to file
- [ ] Call on module unload or explicit save action
- [ ] Load NVRAM on startup (already implemented)

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
