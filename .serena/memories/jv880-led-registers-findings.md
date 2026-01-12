# JV-880 LED Register Findings

## Hardware Architecture (from Switch Board Schematic)

### Button Matrix
- **3 rows × 4 columns = 12 buttons**
- Row select via `io_sd`:
  - `0xFB` (0b11111011) = Row 0: CURSOR_L, CURSOR_R, TONE_SELECT, TONE1
  - `0xF7` (0b11110111) = Row 1: TONE2, TONE3, TONE4, UTILITY
  - `0xEF` (0b11101111) = Row 2: PATCH_PERFORM, EDIT, SYSTEM, RHYTHM

### LED Matrix
- **2 rows × 5 columns**
- Row select via `io_sd`:
  - `0xFE` (0b11111110) = Mode LEDs (LC0): PATCH, EDIT, SYSTEM, RHYTHM, UTILITY
  - `0xFD` (0b11111101) = Tone LEDs (LC1): TONE1, TONE2, TONE3, TONE4
- Data written to P7DR (DEV 0x0E)

### Schematic LED Bit Mapping (theoretical)
Mode LEDs (`led_mode_state`):
- bit 0 (0x01) = PATCH/PERF (LED0)
- bit 1 (0x02) = EDIT (LED1)
- bit 2 (0x04) = SYSTEM (LED2)
- bit 3 (0x08) = RHYTHM (LED3)
- bit 4 (0x10) = UTILITY (LED4)

Tone LEDs (`led_tone_state`) - **ACTIVE LOW**:
- bit 0 = TONE1 (LED0) - 0=ON, 1=OFF
- bit 1 = TONE2 (LED1)
- bit 2 = TONE3 (LED2)
- bit 3 = TONE4 (LED3)

## DSP LED State Format

The `get_param('led_state')` returns comma-separated values:
```
edit,system,rhythm,utility,patch,unused,tone1,tone2,tone3,tone4
 [0]  [1]    [2]    [3]    [4]   [5]   [6]   [7]   [8]   [9]
```

## Observed Issues

### Bit Order Mismatch
Testing revealed the actual bit positions don't match the schematic. User's working mapping:
```javascript
/* Shifted mapping based on testing - bits are offset */
setStepLed(MoveStep1, leds[2] ? LedOn : LedOff);  /* edit */
setStepLed(MoveStep2, leds[3] ? LedOn : LedOff);  /* system */
setStepLed(MoveStep3, leds[0] ? LedOn : LedOff);  /* rhythm */
setStepLed(MoveStep4, leds[1] ? LedOn : LedOff);  /* utility */

setStepLed(MoveStep9, leds[8] ? LedOn : LedOff);   /* tone 1 */
setStepLed(MoveStep10, leds[9] ? LedOn : LedOff);  /* tone 2 */
setStepLed(MoveStep11, leds[6] ? LedOn : LedOff);  /* tone 3 */
setStepLed(MoveStep12, leds[7] ? LedOn : LedOff);  /* tone 4 */
```

### DSP Code Issue (Fixed)
Original code used exact equality which failed when multiple bits set:
```cpp
// WRONG - only works if exactly one LED on
patch = (mode_leds == 0x01) ? 1 : 0;

// CORRECT - check individual bits
patch = (mode_leds & 0x01) ? 1 : 0;
```

## Move Hardware LED Control

### White LEDs (CC messages)
Format: `move_midi_internal_send([0x0b, 0xB0, cc, brightness])`

| CC  | Button   |
|-----|----------|
| 49  | Shift    |
| 50  | Menu     |
| 51  | Back     |
| 52  | Capture  |
| 60  | Copy     |
| 119 | Delete   |

Brightness values:
- `0x00` = OFF
- `0x10` = DIM
- `0x40` = MEDIUM
- `0x7c` (124) = BRIGHT
- `127` = MAX

### RGB LEDs (Note messages)
Format: `move_midi_internal_send([0x09, 0x90, note, color])`

Step buttons are notes 16-31 (MoveSteps array).

Color values are palette indices:
- `0` (Black) = OFF
- `3` (Bright) = Bright Orange
- `120` (White) = White

### Known Issue: Menu LED (CC50) Not Responding
Despite using identical format to other white LEDs:
- Shift (CC49), Delete (CC119), Copy (CC60) respond correctly
- Menu (CC50) does NOT respond
- Back (CC51), Capture (CC52) status unclear

Tried:
- Direct MIDI sends
- setButtonLED function
- Note messages instead of CC
- Maximum brightness (127)
- Off-then-on sequences

**Status: Unresolved** - possibly hardware-specific or requires different approach.

## MCU Code Locations

- `mcu.h`: Button enum, `led_mode_state`, `led_tone_state` fields
- `mcu.cpp`: Button matrix decode in `DEV_P7DR` case, LED state capture
- `jv880_plugin.cpp`: `led_state` param handler around line 1825

## Files Modified

- `src/dsp/mcu.h` - Button enum updated to match schematic
- `src/dsp/mcu.cpp` - Button matrix decode changed to 4×3
- `src/dsp/jv880_plugin.cpp` - LED decoding with bit checking
- `src/ui.js` - LED control functions
