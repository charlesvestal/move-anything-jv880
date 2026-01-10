# JV-880 Editor Manual (Move Anything)

This manual describes the on-device editor UI. It is text-only and matches the current control mapping.

## Quick Start

1. Load the JV-880 module.
2. Play pads to audition the current patch/part.
3. Turn encoders to shape sound. Touching an encoder shows its value in the HUD.
4. Press MENU to enter Edit pages. BACK returns one level; hold BACK to return to Play.

## Surface Map (ASCII)

Top row:
  [D-PAD] [JOG] [MENU] [BACK] [SHIFT]

Middle:
  Encoders 1..8 (touch + turn)
  Track buttons 1..4

Bottom:
  Steps 1..16
  Pads 8x4 (always playable)

## States

Play State (default)
  - Pads play selected patch/part.
  - Encoders map to performance macros (Cutoff, Reso, Attack, Release, LFO Rate, LFO Depth, FX Send, Level).
  - Steps are shortcuts (mode, octave, transpose, monitor toggles).

Edit State (MENU)
  - JV LCD pages shown on screen.
  - Encoders map to the 8 key parameters for the current page.
  - Steps are page tabs (HOME/COMMON/TONE/PITCH/TVF/TVA/LFO/OUT/FX/...)

Utility State (SHIFT+MENU)
  - Init, copy, naming, MIDI/system configuration.

BACK
  - Exits one level.
  - Hold BACK returns to Play State.

## HUD (2 lines, always visible)

Line 1: Activity
  - Encoder touches and edits
  - Incoming MIDI activity

Line 2: Context
  - Mode, patch/perf name
  - Selected tone/part
  - RX status

Example:
  Line 1: E4 Cutoff 76 (Tone1)
  Line 2: PATCH Warm Pad | Tone 1 | RX ON

## Global Navigation

D-PAD
  - Cursor navigation inside the JV LCD pages.

JOG
  - Value changes and list scroll.
  - SHIFT + JOG for fine/accelerated changes.

MENU
  - Enter Edit State.

SHIFT + MENU
  - Enter Utility State.

BACK
  - Cancel/exit (hold to return to Play State).

## Track Buttons (4)

Patch Mode
  - Track 1..4 = select Tone 1..4
  - SHIFT + Track = tone mute/enable

Performance Mode
  - Track 1..4 = select Part in current bank
  - SHIFT + Track = part mute/solo
  - Rhythm Part is Track 4 in Bank 2

Part Banks
  - Bank 1: Parts 1..4
  - Bank 2: Parts 5..7 + Rhythm

## Pads (8x4)

Always playable (except confirmations/naming).
  - Velocity -> TVA
  - Aftertouch/pressure routed per modulation config

Audition Controls
  - Octave shift
  - Transpose
  - Local audition channel selection (if enabled)

Pads are never repurposed for editing.

## Encoders (8)

General
  - Touch shows info without changing value.
  - Turn edits immediately.
  - SHIFT + turn = fine adjustment.

Play State (default)
  1. Cutoff
  2. Resonance
  3. Attack
  4. Release
  5. LFO Rate
  6. LFO Depth
  7. FX Send
  8. Level

Scope follows selected Tone (Patch) or Part (Performance).

Edit State
  - Encoders map to the 8 most important parameters of the current page.

## Step Buttons (16)

Play State shortcuts
  1. Patch Mode
  2. Performance Mode
  3. Rhythm Focus
  4. FX
  5. Favorites
  6. Part Bank Toggle
  7. Octave -
  8. Octave +
  9. Transpose -
 10. Transpose +
 11. Velocity Mode
 12. MIDI Monitor
 13. Output Assign View
 14. Local Control Toggle
 15. SysEx RX / Thru Toggle
 16. Utility Shortcut

Edit State page tabs
  1 HOME
  2 COMMON
  3 TONE / WG
  4 PITCH
  5 TVF
  6 TVA
  7 LFO
  8 OUT/FX
  9 MOD
 10 CTRL
 11 MIX
 12 PART
 13 RHYTHM
 14 FX
 15 UTIL
 16 (reserved)

## Transport / Utility Buttons

PLAY
  - Audition latch (held note/chord).

REC (seq)
  - MIDI monitor toggle.

LOOP
  - Sustain latch.

CAPTURE
  - Compare (edited vs stored).

REC (audio)
  - Panic / All Notes Off.
  - SHIFT + REC (audio) = Reset controllers.

MUTE
  - Tone/Part mute.

COPY
  - Copy Tone->Tone or Part->Part.

DELETE
  - Init Tone / Clear Part (confirm).

UNDO
  - Undo last edit.
  - SHIFT + UNDO = Redo.

## Confirmation Prompts

Destructive actions require confirmation.
  - MENU confirms.
  - BACK cancels.

## Troubleshooting

No response to encoders
  - Ensure you are in Play or Edit state (not in a confirmation prompt).
  - Check that SysEx RX is ON (Step 15).

No sound
  - Confirm ROMs are present in `roms/`.
  - Check patch level and output assignment on the OUT/FX page.
