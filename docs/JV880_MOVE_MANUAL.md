# JV-880 on Move (Module) Manual

This module turns Move into a JV-880 front panel and editor. It does not do sequencing or clip playback.

## Quick Start

1) Launch Move Anything.
2) Select the **JV-880** module.
3) Play pads and turn encoders to shape the sound.

## Surface Map (ASCII)

Top row:
  [D-PAD] [JOG] [MENU] [BACK] [SHIFT]

Middle:
  Encoders 1..8 (touch + turn)
  Track buttons 1..4

Bottom:
  Steps 1..16
  Pads 8x4 (always playable)

## UI States

- **Play** (default): performance control and patch/part selection.
- **Edit** (MENU): full JV parameter pages.
- **Utility** (SHIFT + MENU): system/utility access placeholder.

Navigation:
- **MENU** enters Edit (or loads a favorite if Favorites view is active).
- **SHIFT + MENU** enters Utility.
- **BACK** exits one level; hold BACK returns to Play.

## Display (4 Lines)

1) **HUD Line 1**: activity (encoder changes, incoming MIDI, etc.).
2) **HUD Line 2**: mode + patch name + tone/part + RX state.
3) **Line 3**:
   - Play: macro labels.
   - Edit: JV LCD line 1.
4) **Line 4**:
   - Play: macro labels.
   - Edit: JV LCD line 2.
   - Temporary help replaces line 4 for ~2 seconds after state changes.

## Encoders (8)

Play State (macros):
1 Cutoff
2 Resonance
3 Attack
4 Release
5 LFO Rate
6 LFO Depth
7 FX Send
8 Level

Edit/Utility State:
- Encoders map to the current page parameters (8 per page).
- Touch shows parameter info without changing value.
- SHIFT + turn = fine adjust.

## Pads

- Always play the engine (not repurposed for editing).
- Velocity affects level; aftertouch follows JV modulation routing.

## Jog + D-pad

Play State (Patch Mode):
- **Left/Right/Jog**: patch change (cycles through 192 internal patches + expansions).
- **SHIFT + Left/Right**: bank change (Preset A, Preset B, Internal, expansions).

Play State (Performance Mode):
- **Left/Right/Jog**: performance change (cycles through 8 user performances).
- **SHIFT + Left/Right**: no effect (performances have no banks).

Edit/Utility State:
- **Left/Right/Up/Down**: parameter selection.
- **Jog**: value change.

## Patch Mode vs Performance Mode

The JV-880 has two operating modes, switched via Step 1 and Step 2:

**Patch Mode** (Step 1):
- Single patch plays on MIDI channel 1.
- 192 internal patches (Preset A, Preset B, Internal) plus expansions.
- 4 tones per patch, each with independent sound parameters.
- Ideal for: solo instruments, layered sounds, split keyboards.

**Performance Mode** (Step 2):
- 8 parts, each on its own MIDI channel (1–8), with Part 8 as Rhythm.
- 8 user performances stored in NVRAM.
- Each part references a patch and has level/pan/key range settings.
- Ideal for: multitimbral setups, layered orchestrations, backing tracks.

## Track Buttons (4)

Patch Mode:
- Track 1–4 selects Tone 1–4.
- SHIFT + Track toggles tone mute.

Performance Mode:
- Track 1–4 selects Part 1–4 (or 5–8 with Step 6).
- SHIFT + Track toggles part mute.
- Step 6 toggles between Parts 1–4 and Parts 5–8.
- Part 8 is the Rhythm part (drums).

## Step Buttons (16)

Play State shortcuts:
1. **Patch Mode** - single patch, 4 tones (LED lit when active)
2. **Performance Mode** - 8 parts, multitimbral (LED lit when active)
3. Rhythm Focus
4. FX page
5. Favorites view (toggle)
6. Part Bank toggle (Parts 1–4 ↔ 5–8, LED lit for 5–8)
7. Octave -
8. Octave +
9. Transpose -
10. Transpose +
11. Velocity Mode (HUD only)
12. MIDI Monitor toggle (HUD only)
13. Output/FX page
14. Local Control toggle (HUD only)
15. SysEx RX/Thru toggle (HUD only)
16. Utility shortcut

Edit State tabs:
1. HOME
2. COMMON
3. TONE/WG
4. PITCH
5. TVF
6. TVA
7. LFO (SHIFT + this tab toggles LFO1/2)
8. OUT/FX
9. MOD
10. CTRL
11. STRUCT (Not used)
12. MIX
13. PART
14. RHYTHM
15. FX
16. UTIL

## Transport / Utility Buttons

- PLAY: audition latch (HUD only for now).
- REC (seq): MIDI monitor toggle (HUD only).
- LOOP: sustain latch (on/off).
- CAPTURE: compare (HUD only).
- REC (audio): Panic / All Notes Off.
- SHIFT + REC (audio): Reset controllers.
- MUTE: mute selected tone/part.
- COPY: copy (HUD only).
- DELETE: init/clear confirm (MENU = confirm, BACK = cancel).
- UNDO: undo (HUD only); SHIFT + UNDO: redo (HUD only).

## Favorites

- Step 5 toggles Favorites view.
- SHIFT + Step 5 adds/removes current patch to favorites.
- In Favorites view:
  - Jog or Left/Right selects.
  - MENU loads selection.
  - BACK exits.

Favorites save to `favorites.json` in the module folder.
