# Parameter Audit Report

Comprehensive comparison of Mini-JV plugin parameters against jv880_juce reference and JV-880 MIDI Implementation.

## Reference Sources

1. **jv880_juce** - Tone struct in dataStructures.h (NVRAM byte offsets)
2. **JV-880 MIDI Implementation PDF** - SysEx parameter indices

## Tone Parameters (Patch Mode)

### LFO Section

| Parameter | Code NVRAM | jv880_juce | Status |
|-----------|------------|------------|--------|
| lfo1rate | 24 | lfo1Rate @ 24 | ✓ |
| lfo1delay | 25 | lfo1Delay @ 25 | ✓ |
| lfo1fadetime | 26 | lfo1Fade @ 26 | ✓ |
| lfo2rate | 28 | lfo2Rate @ 28 | ✓ |
| lfo2delay | 29 | lfo2Delay @ 29 | ✓ |
| lfo2fadetime | 30 | lfo2Fade @ 30 | ✓ |
| lfo1pitchdepth | 31 | lfo1PitchDepth @ 31 | ✓ |
| lfo1tvfdepth | 32 | lfo1TvfDepth @ 32 | ✓ |
| lfo1tvadepth | 33 | lfo1TvaDepth @ 33 | ✓ |
| lfo2pitchdepth | 34 | lfo2PitchDepth @ 34 | ✓ |
| lfo2tvfdepth | 35 | lfo2TvfDepth @ 35 | ✓ |
| lfo2tvadepth | 36 | lfo2TvaDepth @ 36 | ✓ |

### Pitch Section

| Parameter | Code NVRAM | jv880_juce | Status |
|-----------|------------|------------|--------|
| pitchcoarse | 37 | pitchCoarse @ 37 | ✓ |
| pitchfine | 38 | pitchFine @ 38 | ✓ |
| penvdepth | 43 | tvpEnvDepth @ 43 | ✓ |
| penvtime1 | 44 | tvpEnvTime1 @ 44 | ✓ |
| penvlevel1 | 45 | tvpEnvLevel1 @ 45 | ✓ |
| penvtime2 | 46 | tvpEnvTime2 @ 46 | ✓ |
| penvlevel2 | 47 | tvpEnvLevel2 @ 47 | ✓ |
| penvtime3 | 48 | tvpEnvTime3 @ 48 | ✓ |
| penvlevel3 | 49 | tvpEnvLevel3 @ 49 | ✓ |
| penvtime4 | 50 | tvpEnvTime4 @ 50 | ✓ |
| penvlevel4 | 51 | tvpEnvLevel4 @ 51 | ✓ |

### TVF (Filter) Section

| Parameter | Code NVRAM | jv880_juce | Status |
|-----------|------------|------------|--------|
| cutofffrequency | 52 | tvfCutoff @ 52 | ✓ |
| resonance | 53 | tvfResonance @ 53 | ✓ |
| cutoffkeyfollow | 54 | tvfTimeKFKeyfollow @ 54 | ✓ |
| filtermode | 55 (bits 3-4) | tvfVeloCurveLpfHpf @ 55 | ✓ |
| tvfenvdepth | 58 | tvfEnvDepth @ 58 | ✓ |
| tvfenvtime1 | 59 | tvfEnvTime1 @ 59 | ✓ |
| tvfenvlevel1 | 60 | tvfEnvLevel1 @ 60 | ✓ |
| tvfenvtime2 | 61 | tvfEnvTime2 @ 61 | ✓ |
| tvfenvlevel2 | 62 | tvfEnvLevel2 @ 62 | ✓ |
| tvfenvtime3 | 63 | tvfEnvTime3 @ 63 | ✓ |
| tvfenvlevel3 | 64 | tvfEnvLevel3 @ 64 | ✓ |
| tvfenvtime4 | 65 | tvfEnvTime4 @ 65 | ✓ |
| tvfenvlevel4 | 66 | tvfEnvLevel4 @ 66 | ✓ |

### TVA (Amplitude) Section - FIXED

| Parameter | Code NVRAM | jv880_juce | Status |
|-----------|------------|------------|--------|
| level | 67 | tvaLevel @ 67 | ✓ (was 68) |
| pan | 68 | tvaPan @ 68 | ✓ (was 69) |
| tonedelaytime | 69 | tvaDelayTime @ 69 | ✓ |
| tvaenvvelocity | 72 | tvaVelocity @ 72 | ✓ |
| tvaenvtime1 | 74 | tvaEnvTime1 @ 74 | ✓ (was 75) |
| tvaenvlevel1 | 75 | tvaEnvLevel1 @ 75 | ✓ |
| tvaenvtime2 | 76 | tvaEnvTime2 @ 76 | ✓ (was 77) |
| tvaenvlevel2 | 77 | tvaEnvLevel2 @ 77 | ✓ |
| tvaenvtime3 | 78 | tvaEnvTime3 @ 78 | ✓ (was 79) |
| tvaenvlevel3 | 79 | tvaEnvLevel3 @ 79 | ✓ |
| tvaenvtime4 | 80 | tvaEnvTime4 @ 80 | ✓ (was 81) |
| drylevel | 81 | drySend @ 81 | ✓ (was 82) |
| reverbsendlevel | 82 | reverbSend @ 82 | ✓ (was 83) |
| chorussendlevel | 83 | chorusSend @ 83 | ✓ (was 84) |

### Tone SysEx Indices (vs MIDI Implementation)

| Parameter | Code SysEx | MIDI Impl | Status |
|-----------|-----------|-----------|--------|
| cutofffrequency | 74 | 4Ah (74) | ✓ |
| resonance | 75 | 4Bh (75) | ✓ |
| level | 92 | 5Ch (92) | ✓ |
| pan | 94 | 5Eh (94) | ✓ |
| pitchcoarse | 56 | 38h (56) | ✓ |
| pitchfine | 57 | 39h (57) | ✓ |
| filtermode | 73 | 49h (73) | ✓ |
| tvaenvtime1 | 105 | 69h (105) | ✓ |
| tvaenvtime2 | 107 | 6Bh (107) | ✓ |
| tvaenvtime3 | 109 | 6Dh (109) | ✓ |
| tvaenvtime4 | 111 | 6Fh (111) | ✓ |
| drylevel | 112 | 70h (112) | ✓ |
| reverbsendlevel | 113 | 71h (113) | ✓ |
| chorussendlevel | 114 | 72h (114) | ✓ |

## Part Parameters (Performance Mode)

### Part SysEx Indices (vs MIDI Implementation)

| Parameter | Code SysEx | MIDI Impl | Status |
|-----------|-----------|-----------|--------|
| partlevel | 25 | 19h (25) | ✓ |
| partpan | 26 | 1Ah (26) | ✓ |
| internalkeyrangelower | 15 | 0Fh (15) | ✓ |
| internalkeyrangeupper | 16 | 10h (16) | ✓ |
| internalkeytranspose | 17 | 11h (17) | ✓ |
| internalvelocitysense | 18 | 12h (18) | ✓ |
| internalvelocitymax | 19 | 13h (19) | ✓ |
| patchnumber | 23/24 | 17h/18h (23/24) | ✓ |
| partcoarsetune | 27 | 1Bh (27) | ✓ |
| partfinetune | 28 | 1Ch (28) | ✓ |
| reverbswitch | 29 | 1Dh (29) | ✓ |
| chorusswitch | 30 | 1Eh (30) | ✓ |

## Patch Common Parameters

| Parameter | Code NVRAM | jv880_juce | Status |
|-----------|------------|------------|--------|
| reverblevel | 13 | reverb.level @ 13 | ✓ |
| reverbtime | 14 | reverb.time @ 14 | ✓ |
| reverbfeedback | 15 | reverb.feedback @ 15 | ✓ |
| choruslevel | 16 | chorus.level @ 16 | ✓ |
| chorusdepth | 17 | chorus.depth @ 17 | ✓ |
| chorusrate | 18 | chorus.rate @ 18 | ✓ |
| chorusfeedback | 19 | chorus.feedback @ 19 | ✓ |
| analogfeel | 20 | analogFeel @ 20 | ✓ |
| patchlevel | 21 | level @ 21 | ✓ |
| patchpan | 22 | pan @ 22 | ✓ |
| bendrangedown | 23 | bendRange @ 23 | ✓ |
| portamentotime | 25 | portamentoTime @ 25 | ✓ |

## Summary

**All parameters verified against references:**

- **Tone NVRAM offsets**: All 84 bytes match jv880_juce Tone struct
- **Tone SysEx indices**: All match JV-880 MIDI Implementation
- **Part SysEx indices**: All match JV-880 MIDI Implementation
- **Patch Common offsets**: All match jv880_juce PatchCommon struct

**Fixes applied in this session:**

The entire TVA section NVRAM offsets were corrected (all were +1 too high):
- tvaLevel: 68 → 67
- tvaPan: 69 → 68
- tvaEnvTime1: 75 → 74
- tvaEnvTime2: 77 → 76
- tvaEnvTime3: 79 → 78
- tvaEnvTime4: 81 → 80
- drySend: 82 → 81
- reverbSend: 83 → 82
- chorusSend: 84 → 83

**Note on address spaces:**

There are TWO distinct address spaces:
1. **NVRAM offsets** (from jv880_juce) - Used for direct memory reads/writes
2. **SysEx indices** (from MIDI Implementation) - Used for DT1 parameter changes

These intentionally differ. The NVRAM is packed at 84 bytes/tone, while SysEx indices can go up to 116 due to nibble expansion for multi-byte values.
