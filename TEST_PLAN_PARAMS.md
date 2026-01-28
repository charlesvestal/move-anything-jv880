# Mini-JV Parameter Test Plan

This test plan verifies the NVRAM offset corrections for Patch mode (tone) and Performance mode (part) parameters.

## Prerequisites

1. Build and install the updated module:
   ```bash
   ./scripts/build.sh && ./scripts/install.sh
   ```
2. Restart Move Anything on the device
3. Load Mini-JV in Signal Chain or standalone

---

## Part 1: Patch Mode Parameters (Tone Editing)

### Setup
1. Switch to **Patch Mode** (set mode = "Patch")
2. Select a patch with obvious tonal character (e.g., a bright pad or bass)
3. Select a tone using Track buttons 1-4

### TVF (Filter) Parameters

| Parameter | Test Procedure | Expected Result |
|-----------|----------------|-----------------|
| **Cutoff Frequency** | Turn knob mapped to cutofffrequency | Brightness changes - higher values = brighter, lower = duller |
| **Resonance** | Turn knob mapped to resonance | Peak/emphasis at cutoff increases - creates "wah" quality |
| **Filter Mode** | Cycle through filtermode | Off = no filter, LPF = low-pass (removes highs), HPF = high-pass (removes lows) |

### TVA (Amplitude) Parameters - CRITICAL (These were fixed)

| Parameter | Test Procedure | Expected Result |
|-----------|----------------|-----------------|
| **Level** (was at 68, now 67) | Adjust level parameter | Volume of selected tone increases/decreases |
| **Pan** (was at 69, now 68) | Adjust pan parameter | Sound moves left (0) to right (127), center at 64 |
| **TVA Env Time 1** (was at 75, now 74) | Adjust tvaenvtime1 | Attack time changes - higher = slower attack |
| **TVA Env Time 2** (was at 77, now 76) | Adjust tvaenvtime2 | Decay time changes - affects how sound fades after attack |
| **TVA Env Time 3** (was at 79, now 78) | Adjust tvaenvtime3 | Sustain/release time changes |
| **TVA Env Time 4** (was at 81, now 80) | Adjust tvaenvtime4 | Release time changes - how long sound rings after key release |
| **Dry Level** (was at 82, now 81) | Adjust drylevel | Dry/direct signal level changes |
| **Reverb Send** (was at 83, now 82) | Adjust reverbsendlevel | Reverb amount increases/decreases |
| **Chorus Send** (was at 84, now 83) | Adjust chorussendlevel | Chorus amount increases/decreases |

### Pitch Parameters

| Parameter | Test Procedure | Expected Result |
|-----------|----------------|-----------------|
| **Pitch Coarse** | Adjust pitchcoarse | Pitch shifts in semitones (obvious pitch change) |
| **Pitch Fine** | Adjust pitchfine | Subtle pitch change (cents) - useful for detuning |

### Switch Parameters (Enum Labels)

| Parameter | Test Procedure | Expected Result |
|-----------|----------------|-----------------|
| **Reverb Switch** | Toggle reverbswitch | Display shows "Off" or "On", reverb enables/disables |
| **Chorus Switch** | Toggle chorusswitch | Display shows "Off" or "On", chorus enables/disables |

---

## Part 2: Performance Mode Parameters (Part Editing)

### Setup
1. Switch to **Performance Mode** (set mode = "Performance")
2. Load a multi-part performance
3. Use Step buttons 1-8 to select different parts

### Part Parameters

| Parameter | Test Procedure | Expected Result |
|-----------|----------------|-----------------|
| **Part Level** | Adjust via Performance encoder macro | Selected part's volume changes |
| **Part Pan** | Adjust via Performance encoder macro | Selected part pans left/right |
| **Coarse Tune** | Adjust coarse tune parameter | Part pitch shifts in semitones |
| **Fine Tune** | Adjust fine tune parameter | Part pitch shifts slightly (cents) |
| **Key Range Low** | Adjust key range low | Part only sounds above this note |
| **Key Range High** | Adjust key range high | Part only sounds below this note |
| **Velocity Range** | Adjust velocity parameters | Part responds to different velocity ranges |

---

## Part 3: Verification Methods

### Method A: Auditory Verification
1. Play sustained notes while adjusting parameters
2. Listen for expected audio changes
3. Confirm changes are smooth and continuous (not glitchy)

### Method B: Visual Verification (if using debug logging)
1. Enable verbose logging in plugin
2. Watch for NVRAM writes at correct offsets
3. Watch for SysEx messages with correct indices

### Method C: State Persistence
1. Adjust several parameters
2. Save the state
3. Reload the patch/performance
4. Verify parameters restored to adjusted values

---

## Part 4: Regression Checks

Ensure these still work correctly:

| Feature | Test |
|---------|------|
| Preset browsing | Can cycle through patches |
| Bank navigation | Can navigate expansion banks |
| Tone selection | Track buttons 1-4 select tones in Patch mode |
| Part selection | Step buttons 1-8 select parts in Performance mode |
| Mode switching | Can switch between Patch and Performance modes |
| Octave transpose | Octave shift works correctly |

---

## Known Correct Values

From jv880_juce Tone struct (84 bytes per tone):

| Parameter | NVRAM Offset | SysEx Index |
|-----------|--------------|-------------|
| tvfCutoff | 52 | 74 |
| tvfResonance | 53 | 75 |
| tvaLevel | 67 | 92 |
| tvaPan | 68 | 94 |
| tvaEnvTime1 | 74 | 105 |
| tvaEnvTime2 | 76 | 107 |
| tvaEnvTime3 | 78 | 109 |
| tvaEnvTime4 | 80 | 111 |
| drySend | 81 | 112 |
| reverbSend | 82 | 113 |
| chorusSend | 83 | 114 |

---

## Troubleshooting

**Parameter has no effect:**
- Verify the correct tone/part is selected
- Check if the parameter makes sense for the current patch (e.g., filter mode might be "Off")
- Ensure the emulator has finished warming up

**Wrong parameter changes:**
- This indicates NVRAM offset is still incorrect
- Compare against jv880_juce dataStructures.h

**Sound glitches when adjusting:**
- SysEx message timing may need adjustment
- Check if emulator is keeping up with parameter changes
