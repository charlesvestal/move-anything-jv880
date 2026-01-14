/*
 * Performance Part Parameter Mapper
 *
 * Automatically discovers the mapping between SysEx addresses and SRAM offsets
 * by sending SysEx messages and monitoring SRAM changes.
 *
 * Usage: Call runParameterMapping() from the console after loading a performance.
 */

import { buildDT1 } from './jv880_sysex.mjs';

/* SysEx Part parameter definitions from MIDI Implementation (page 8-9)
 * Format: [sysex_offset, name, min, max, is_two_byte]
 */
const PART_PARAMS_SYSEX = [
    [0x00, 'transmitswitch', 0, 1, false],
    [0x01, 'transmitchannel', 0, 15, false],
    [0x02, 'transmitprogramchange', 0, 128, true],  // 2 bytes
    [0x04, 'transmitvolume', 0, 128, true],         // 2 bytes
    [0x06, 'transmitpan', 0, 128, true],            // 2 bytes
    [0x08, 'transmitkeyrangelower', 0, 127, false],
    [0x09, 'transmitkeyrangeupper', 0, 127, false],
    [0x0A, 'transmitkeytranspose', 28, 100, false],
    [0x0B, 'transmitvelocitysense', 1, 127, false],
    [0x0C, 'transmitvelocitymax', 0, 127, false],
    [0x0D, 'transmitvelocitycurve', 0, 6, false],
    [0x0E, 'internalswitch', 0, 1, false],
    [0x0F, 'internalkeyrangelower', 0, 127, false],
    [0x10, 'internalkeyrangeupper', 0, 127, false],
    [0x11, 'internalkeytranspose', 28, 100, false],
    [0x12, 'internalvelocitysense', 1, 127, false],
    [0x13, 'internalvelocitymax', 0, 127, false],
    [0x14, 'internalvelocitycurve', 0, 6, false],
    [0x15, 'receiveswitch', 0, 1, false],
    [0x16, 'receivechannel', 0, 15, false],
    [0x17, 'patchnumber', 0, 255, true],            // 2 bytes
    [0x19, 'partlevel', 0, 127, false],
    [0x1A, 'partpan', 0, 127, false],
    [0x1B, 'partcoarsetune', 16, 112, false],
    [0x1C, 'partfinetune', 14, 114, false],
    [0x1D, 'reverbswitch', 0, 1, false],
    [0x1E, 'chorusswitch', 0, 1, false],
    [0x1F, 'receiveprogramchange', 0, 1, false],
    [0x20, 'receivevolume', 0, 1, false],
    [0x21, 'receivehold1', 0, 1, false],
    [0x22, 'outputselect', 0, 2, false],
];

/* Performance Common parameters from MIDI Implementation (page 8) */
const PERF_COMMON_PARAMS_SYSEX = [
    // Name bytes 00-0B handled separately
    [0x0C, 'keymode', 0, 2, false],
    [0x0D, 'reverbtype', 0, 7, false],
    [0x0E, 'reverblevel', 0, 127, false],
    [0x0F, 'reverbtime', 0, 127, false],
    [0x10, 'reverbfeedback', 0, 127, false],
    [0x11, 'chorustype', 0, 2, false],
    [0x12, 'choruslevel', 0, 127, false],
    [0x13, 'chorusdepth', 0, 127, false],
    [0x14, 'chorusrate', 0, 127, false],
    [0x15, 'chorusfeedback', 0, 127, false],
    [0x16, 'chorusoutput', 0, 1, false],
    [0x17, 'voicereserve1', 0, 28, false],
    [0x18, 'voicereserve2', 0, 28, false],
    [0x19, 'voicereserve3', 0, 28, false],
    [0x1A, 'voicereserve4', 0, 28, false],
    [0x1B, 'voicereserve5', 0, 28, false],
    [0x1C, 'voicereserve6', 0, 28, false],
    [0x1D, 'voicereserve7', 0, 28, false],
    [0x1E, 'voicereserve8', 0, 28, false],
];

const SRAM_TEMP_PERF_OFFSET = 0x206a;
const SRAM_SCAN_RANGE = 256;  // Bytes to scan around temp perf

/* Build a DT1 SysEx message for Performance Part parameter */
function buildPartSysEx(partIndex, sysexOffset, value, isTwoByte = false) {
    // Part addresses: 00 00 18+part 00
    const addr = [0x00, 0x00, 0x18 + partIndex, sysexOffset];
    const data = isTwoByte ? [0x00, value & 0x7F] : [value & 0x7F];
    return buildDT1(addr, data);
}

/* Build a DT1 SysEx for Performance Common parameter */
function buildPerfCommonSysEx(sysexOffset, value) {
    const addr = [0x00, 0x00, 0x10, sysexOffset];
    return buildDT1(addr, [value & 0x7F]);
}

/* Read SRAM region and return as array */
function readSramRegion(startOffset, length) {
    const bytes = [];
    for (let i = 0; i < length; i++) {
        const val = host_module_get_param(`debug_sram_${startOffset + i}`);
        bytes.push(parseInt(val) || 0);
    }
    return bytes;
}

/* Find differences between two SRAM snapshots */
function findSramChanges(before, after, baseOffset) {
    const changes = [];
    for (let i = 0; i < before.length; i++) {
        if (before[i] !== after[i]) {
            changes.push({
                offset: baseOffset + i,
                oldVal: before[i],
                newVal: after[i]
            });
        }
    }
    return changes;
}

/* Send SysEx via MIDI */
function sendSysEx(sysexBytes) {
    if (!sysexBytes) return;
    // Convert to format expected by host
    const hex = sysexBytes.map(b => b.toString(16).padStart(2, '0')).join(' ');
    print(`Sending SysEx: ${hex}`);
    // The host_midi_send function sends raw MIDI
    // We need to queue it through the module
    for (const byte of sysexBytes) {
        host_midi_send([byte]);
    }
}

/* Wait for emulator to process (simple delay using busy loop) */
function wait(ms) {
    const start = Date.now();
    while (Date.now() - start < ms) {
        // Busy wait - not ideal but works for testing
    }
}

/* Map a single parameter by changing it and observing SRAM */
function mapParameter(paramDef, buildSysExFn, partIndex = null) {
    const [sysexOffset, name, min, max, isTwoByte] = paramDef;

    print(`\nMapping ${name} (SysEx offset 0x${sysexOffset.toString(16)})...`);

    // Read SRAM before
    const before = readSramRegion(SRAM_TEMP_PERF_OFFSET, SRAM_SCAN_RANGE);

    // Choose a test value that's different from typical defaults
    // Most defaults are 0, 64, or 127, so use a distinctive value
    const testValue = (min === 0 && max >= 99) ? 99 : Math.min(max, min + 1);

    // Build and send SysEx
    const sysex = partIndex !== null
        ? buildPartSysEx(partIndex, sysexOffset, testValue, isTwoByte)
        : buildPerfCommonSysEx(sysexOffset, testValue);

    if (!sysex) {
        print(`  ERROR: Failed to build SysEx`);
        return null;
    }

    sendSysEx(sysex);

    // Wait for emulator to process
    wait(100);

    // Read SRAM after
    const after = readSramRegion(SRAM_TEMP_PERF_OFFSET, SRAM_SCAN_RANGE);

    // Find changes
    const changes = findSramChanges(before, after, SRAM_TEMP_PERF_OFFSET);

    if (changes.length === 0) {
        print(`  No SRAM changes detected`);
        return null;
    } else if (changes.length === 1) {
        const c = changes[0];
        const relOffset = c.offset - SRAM_TEMP_PERF_OFFSET;
        print(`  FOUND: SRAM offset ${relOffset} (0x${relOffset.toString(16)}) changed: ${c.oldVal} -> ${c.newVal}`);
        return { sysexOffset, name, sramOffset: relOffset, isTwoByte };
    } else {
        print(`  Multiple changes (${changes.length}):`);
        changes.forEach(c => {
            const relOffset = c.offset - SRAM_TEMP_PERF_OFFSET;
            print(`    offset ${relOffset} (0x${relOffset.toString(16)}): ${c.oldVal} -> ${c.newVal}`);
        });
        // Return first change as likely candidate
        const c = changes[0];
        return { sysexOffset, name, sramOffset: c.offset - SRAM_TEMP_PERF_OFFSET, isTwoByte, multipleChanges: true };
    }
}

/* Map all Performance Common parameters */
export function mapPerformanceCommon() {
    print('=== Mapping Performance Common Parameters ===\n');
    print('Make sure a performance is loaded first!\n');

    const results = [];
    for (const param of PERF_COMMON_PARAMS_SYSEX) {
        const result = mapParameter(param, buildPerfCommonSysEx);
        if (result) results.push(result);
        wait(50);  // Small delay between params
    }

    print('\n=== Results ===');
    print('SysEx -> SRAM Offset Mapping:');
    for (const r of results) {
        print(`  ${r.name}: SysEx 0x${r.sysexOffset.toString(16)} -> SRAM +${r.sramOffset} (0x${r.sramOffset.toString(16)})`);
    }

    return results;
}

/* Map all parameters for a single part */
export function mapPart(partIndex) {
    print(`=== Mapping Part ${partIndex + 1} Parameters ===\n`);
    print('Make sure a performance is loaded first!\n');

    const results = [];
    for (const param of PART_PARAMS_SYSEX) {
        const result = mapParameter(param, null, partIndex);
        if (result) {
            result.partIndex = partIndex;
            results.push(result);
        }
        wait(50);
    }

    print(`\n=== Part ${partIndex + 1} Results ===`);
    print('SysEx -> SRAM Offset Mapping:');
    for (const r of results) {
        print(`  ${r.name}: SysEx 0x${r.sysexOffset.toString(16)} -> SRAM +${r.sramOffset} (0x${r.sramOffset.toString(16)})`);
    }

    return results;
}

/* Run full parameter mapping for performance common and part 1 */
export function runParameterMapping() {
    print('========================================');
    print('Performance Parameter SRAM Mapper');
    print('========================================\n');

    const allResults = {
        common: mapPerformanceCommon(),
        parts: []
    };

    // Map Part 1 as a sample
    allResults.parts.push(mapPart(0));

    // Generate C code for DSP
    print('\n\n=== Generated C Code ===\n');
    generateCCode(allResults);

    return allResults;
}

/* Generate C code snippet for DSP implementation */
function generateCCode(results) {
    print('/* Performance Common SRAM offsets (relative to SRAM_TEMP_PERF_OFFSET) */');
    for (const r of results.common) {
        print(`#define PERF_COMMON_${r.name.toUpperCase()}_OFFSET ${r.sramOffset}`);
    }

    if (results.parts.length > 0) {
        print('\n/* Part SRAM offsets (relative to part base) */');
        print('/* Part base = SRAM_TEMP_PERF_OFFSET + 31 + (partIndex * PART_SIZE) */');
        const partResults = results.parts[0];
        // Calculate part base offset
        const partBaseGuess = Math.min(...partResults.map(r => r.sramOffset));
        print(`#define PERF_PART_BASE_OFFSET ${partBaseGuess}`);
        for (const r of partResults) {
            const relOffset = r.sramOffset - partBaseGuess;
            print(`#define PART_${r.name.toUpperCase()}_OFFSET ${relOffset}`);
        }
    }
}

// Export for use in console
globalThis.mapPerformanceCommon = mapPerformanceCommon;
globalThis.mapPart = mapPart;
globalThis.runParameterMapping = runParameterMapping;
