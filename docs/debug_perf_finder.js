/*
 * Debug script to find temp performance location in SRAM
 *
 * Run this from the Move Anything console after loading the JV-880 module.
 * Paste into console or load via: load('path/to/debug_perf_finder.js')
 */

function findTempPerformance() {
    print("=== Finding Temp Performance in SRAM ===\n");

    // 1. Switch to performance mode and load first performance
    print("1. Loading Performance mode, Preset A:01...");
    host_module_set_param('mode', '1');
    host_module_set_param('performance', '0');

    // Wait a moment for the emulator to process
    // (In real use, might need async handling)

    // 2. Get the performance name we expect
    let perfName = host_module_get_param('patch_name');
    print("   Current performance: " + perfName);

    // 3. Search for "Jazz" in SRAM (first word of Jazz Split)
    let searchTerm = "Jazz";
    let offset = host_module_get_param('debug_find_sram_' + searchTerm);
    print("\n2. Searching SRAM for '" + searchTerm + "'...");
    print("   Found at offset: " + offset);

    if (offset !== "-1") {
        let offsetNum = parseInt(offset, 16);
        print("\n3. Reading bytes around that location:");

        // Read 32 bytes starting from the found offset
        let bytes = [];
        for (let i = 0; i < 32; i++) {
            let val = parseInt(host_module_get_param('debug_sram_' + (offsetNum + i)));
            bytes.push(val);
        }

        // Show as hex
        let hexStr = bytes.map(b => b.toString(16).padStart(2, '0')).join(' ');
        print("   Hex: " + hexStr);

        // Show as ASCII where printable
        let asciiStr = bytes.map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : '.').join('');
        print("   ASCII: " + asciiStr);

        print("\n4. Performance structure should start at offset: 0x" + offsetNum.toString(16));
        print("   (Name is first 12 bytes, then common params, then 8 parts)");

        return offsetNum;
    } else {
        print("\n   ERROR: Could not find performance name in SRAM");
        print("   The temp performance may be in a different memory area.");
        return -1;
    }
}

function dumpAndCompare() {
    print("=== Dump SRAM Before/After SysEx Test ===\n");

    print("1. Dumping SRAM before change...");
    host_module_set_param('dump_sram', 'debug_sram_before.bin');

    print("2. Now send a SysEx to change a performance parameter.");
    print("   For example, from the Edit > Common > Reverb menu, change Level.");
    print("   Or use buildPerformanceCommonParam() from jv880_sysex.mjs");
    print("\n3. Then run: host_module_set_param('dump_sram', 'debug_sram_after.bin')");
    print("4. Copy files from Move and diff on Mac:");
    print("   xxd debug_sram_before.bin > before.txt");
    print("   xxd debug_sram_after.bin > after.txt");
    print("   diff before.txt after.txt");
}

// Run the finder
print("Run findTempPerformance() to search for perf data");
print("Run dumpAndCompare() to set up before/after dumps");
