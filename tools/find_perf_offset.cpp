/*
 * Standalone tool to find temp performance offset in SRAM
 *
 * Compile: clang++ -std=c++17 -O2 -I../src/dsp -o find_perf_offset find_perf_offset.cpp ../src/dsp/mcu.cpp ../src/dsp/mcu_opcodes.cpp ../src/dsp/pcm.cpp ../src/dsp/lcd.cpp
 * Run: ./find_perf_offset ../roms
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include "mcu.h"

static uint8_t* load_file(const char* path, size_t expected_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s\n", path);
        return nullptr;
    }
    uint8_t* data = new uint8_t[expected_size];
    size_t read = fread(data, 1, expected_size, f);
    fclose(f);
    if (read != expected_size) {
        fprintf(stderr, "Warning: %s is %zu bytes (expected %zu)\n", path, read, expected_size);
    }
    return data;
}

static int find_string_in_mem(const uint8_t* mem, size_t mem_size, const char* needle) {
    size_t needle_len = strlen(needle);
    for (size_t i = 0; i <= mem_size - needle_len; i++) {
        if (memcmp(&mem[i], needle, needle_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void dump_hex(const uint8_t* data, int offset, int len) {
    printf("  Offset 0x%04x:\n", offset);
    for (int i = 0; i < len; i += 16) {
        printf("    %04x: ", offset + i);
        for (int j = 0; j < 16 && i + j < len; j++) {
            printf("%02x ", data[offset + i + j]);
        }
        printf(" |");
        for (int j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[offset + i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <roms_dir>\n", argv[0]);
        return 1;
    }

    const char* roms_dir = argv[1];
    char path[512];

    printf("=== JV-880 SRAM Performance Finder ===\n\n");

    // Load ROMs
    printf("Loading ROMs from %s...\n", roms_dir);

    snprintf(path, sizeof(path), "%s/jv880_rom1.bin", roms_dir);
    uint8_t* rom1 = load_file(path, ROM1_SIZE);
    if (!rom1) return 1;

    snprintf(path, sizeof(path), "%s/jv880_rom2.bin", roms_dir);
    uint8_t* rom2 = load_file(path, ROM2_SIZE);
    if (!rom2) return 1;

    snprintf(path, sizeof(path), "%s/jv880_waverom1.bin", roms_dir);
    uint8_t* waverom1 = load_file(path, 0x200000);
    if (!waverom1) return 1;

    snprintf(path, sizeof(path), "%s/jv880_waverom2.bin", roms_dir);
    uint8_t* waverom2 = load_file(path, 0x200000);
    if (!waverom2) return 1;

    snprintf(path, sizeof(path), "%s/jv880_nvram.bin", roms_dir);
    uint8_t* nvram = load_file(path, NVRAM_SIZE);
    // NVRAM is optional

    printf("ROMs loaded.\n\n");

    // Initialize MCU
    printf("Initializing emulator...\n");
    MCU mcu;
    int result = mcu.startSC55(rom1, rom2, waverom1, waverom2, nvram);
    if (result != 0) {
        fprintf(stderr, "Error: Failed to start emulator\n");
        return 1;
    }
    printf("Emulator initialized.\n\n");

    // Run some cycles to let it boot
    printf("Running emulator for boot sequence...\n");
    for (int i = 0; i < 1000; i++) {
        mcu.updateSC55(256);  // 256 samples per update
    }
    printf("Boot sequence complete.\n\n");

    // Check LCD
    printf("LCD Line 0: %s\n", mcu.lcd.GetLine(0));
    printf("LCD Line 1: %s\n\n", mcu.lcd.GetLine(1));

    // Switch to performance mode and load Preset A:01
    printf("Switching to performance mode (Preset A:01)...\n");

    // Set NVRAM mode byte to performance mode (0 = perf, 1 = patch)
    mcu.nvram[0x11] = 0;

    // Send Bank Select (CC#0 = 81 for Preset A/B) on channel 16
    uint8_t bank_msg[3] = { 0xBF, 0x00, 81 };
    mcu.postMidiSC55(bank_msg, 3);

    // Send Program Change (0 for first performance) on channel 16
    uint8_t pc_msg[2] = { 0xCF, 0 };
    mcu.postMidiSC55(pc_msg, 2);

    // Run more cycles to process the change
    for (int i = 0; i < 500; i++) {
        mcu.updateSC55(256);
    }

    printf("LCD Line 0: %s\n", mcu.lcd.GetLine(0));
    printf("LCD Line 1: %s\n\n", mcu.lcd.GetLine(1));

    // Search for "Jazz" in SRAM
    printf("=== Searching SRAM for performance data ===\n\n");

    const char* search_terms[] = { "Jazz", "Split", "PA:01", "Perf" };
    for (const char* term : search_terms) {
        int offset = find_string_in_mem(mcu.sram, SRAM_SIZE, term);
        if (offset >= 0) {
            printf("Found '%s' in SRAM at offset 0x%04x\n", term, offset);
            dump_hex(mcu.sram, offset, 48);
            printf("\n");
        } else {
            printf("'%s' not found in SRAM\n\n", term);
        }
    }

    // Also search NVRAM
    printf("=== Searching NVRAM ===\n\n");
    for (const char* term : search_terms) {
        int offset = find_string_in_mem(mcu.nvram, NVRAM_SIZE, term);
        if (offset >= 0) {
            printf("Found '%s' in NVRAM at offset 0x%04x\n", term, offset);
            dump_hex(mcu.nvram, offset, 48);
            printf("\n");
        }
    }

    // Try sending a SysEx to change reverb level and see where it lands
    printf("=== Testing SysEx Parameter Change ===\n\n");

    // Save SRAM state
    uint8_t sram_before[SRAM_SIZE];
    memcpy(sram_before, mcu.sram, SRAM_SIZE);

    // Send DT1 SysEx to change Performance Common Reverb Level
    // Address: 00 00 10 0E (temp perf common, param index 14 = reverblevel)
    // Value: 0x7F (127)
    // Checksum: (0x100 - (0x00 + 0x00 + 0x10 + 0x0E + 0x7F)) & 0x7F = 0x63
    uint8_t sysex[] = {
        0xF0, 0x41, 0x10, 0x46, 0x12,  // Roland DT1 header
        0x00, 0x00, 0x10, 0x0E,        // Address: temp perf common, reverb level
        0x7F,                          // Value: 127
        0x63,                          // Checksum
        0xF7                           // End
    };

    printf("Sending SysEx to set reverb level to 127...\n");
    printf("SysEx: ");
    for (size_t i = 0; i < sizeof(sysex); i++) {
        printf("%02X ", sysex[i]);
    }
    printf("\n\n");

    mcu.postMidiSC55(sysex, sizeof(sysex));

    // Run cycles to process
    for (int i = 0; i < 200; i++) {
        mcu.updateSC55(256);
    }

    // Compare SRAM
    printf("Comparing SRAM before/after SysEx...\n");
    int changes = 0;
    for (int i = 0; i < SRAM_SIZE; i++) {
        if (mcu.sram[i] != sram_before[i]) {
            printf("  SRAM[0x%04x]: %02x -> %02x\n", i, sram_before[i], mcu.sram[i]);
            changes++;
            if (changes > 20) {
                printf("  ... (more changes)\n");
                break;
            }
        }
    }
    if (changes == 0) {
        printf("  No SRAM changes detected.\n");
    }
    printf("\n");

    // Also check NVRAM changes
    printf("Checking NVRAM for changes (shouldn't change for temp edits)...\n");
    // NVRAM was loaded from file, compare is tricky without saving original
    // Skip for now

    // Dump full SRAM to file for analysis
    printf("Dumping SRAM to sram_dump.bin...\n");
    FILE* f = fopen("sram_dump.bin", "wb");
    if (f) {
        fwrite(mcu.sram, 1, SRAM_SIZE, f);
        fclose(f);
        printf("Done. Use 'xxd sram_dump.bin | less' to examine.\n");
    }

    printf("\n=== Done ===\n");

    delete[] rom1;
    delete[] rom2;
    delete[] waverom1;
    delete[] waverom2;
    if (nvram) delete[] nvram;

    return 0;
}
