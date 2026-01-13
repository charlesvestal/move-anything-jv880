/*
 * JV-880 Plugin for Move Anything
 * Based on mini-jv880 emulator by giulioz (based on Nuked-SC55 by nukeykt)
 * Multi-expansion support with unified patch list
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

#include "mcu.h"

extern "C" {
#include "plugin_api_v1.h"
}

/* The emulator instance */
static MCU *g_mcu = nullptr;

/* Plugin state */
static char g_module_dir[512];
static int g_initialized = 0;
static int g_rom_loaded = 0;
static int g_led_test_done = 0;
static int g_render_count = 0;
static int g_debug_sysex = 0;
static uint8_t g_sysex_buf[512];
static int g_sysex_len = 0;
static int g_sysex_capture = 0;

/* Keep ROM2 for internal patch data access */
static uint8_t *g_rom2 = nullptr;

/* Patch data constants */
#define PATCH_SIZE 0x16a  /* 362 bytes per patch */
#define PATCH_NAME_LEN 12
#define PATCH_OFFSET_INTERNAL   0x008ce0  /* Internal bank (JV Strings, etc.) */
#define PATCH_OFFSET_PRESET_A   0x010ce0  /* Preset A (A.Piano 1, etc.) */
#define PATCH_OFFSET_PRESET_B   0x018ce0  /* Preset B (Pizzicato, etc.) */
#define NVRAM_PATCH_OFFSET      0x0d70
#define NVRAM_MODE_OFFSET       0x11

/* Expansion ROM support */
#define EXPANSION_SIZE_8MB 0x800000  /* 8MB standard */
#define EXPANSION_SIZE_2MB 0x200000  /* 2MB (Experience series) */
#define MAX_EXPANSIONS 32
#define MAX_PATCHES_PER_EXP 256

typedef struct {
    char filename[256];
    char name[64];          /* Short name like "01 Pop" */
    int patch_count;
    uint32_t patches_offset;
    int first_global_index; /* First patch index in unified list */
    uint32_t rom_size;      /* ROM size (8MB or 2MB) */
    uint8_t *unscrambled;   /* Unscrambled ROM data (loaded on demand) */
} ExpansionInfo;

static ExpansionInfo g_expansions[MAX_EXPANSIONS];
static int g_expansion_count = 0;
static int g_current_expansion = -1;  /* Currently loaded expansion (-1 = none) */

/* Unified patch list */
#define MAX_TOTAL_PATCHES 4096
typedef struct {
    char name[PATCH_NAME_LEN + 1];
    int expansion_index;    /* -1 for internal, 0+ for expansion */
    int local_patch_index;  /* Index within bank/expansion */
    uint32_t rom_offset;    /* Offset in ROM2 or expansion ROM */
} PatchInfo;

static PatchInfo g_patches[MAX_TOTAL_PATCHES];
static int g_total_patches = 0;
static int g_current_patch = 0;

/* Performance mode support */
static int g_performance_mode = 0;  /* 0 = patch mode, 1 = performance mode */
static int g_current_performance = 0;  /* 0-47 for all performances (3 banks x 16) */
static int g_current_part = 0;  /* 0-7 for parts within performance */
static int g_perf_bank = 0;  /* 0=Preset A, 1=Preset B, 2=Internal */
#define NUM_PERF_BANKS 3
#define PERFS_PER_BANK 16
#define NUM_PERFORMANCES (NUM_PERF_BANKS * PERFS_PER_BANK)  /* 48 total */
#define PERF_NAME_LEN 12

/* Bank navigation */
#define MAX_BANKS 64
static int g_bank_starts[MAX_BANKS];  /* First patch index of each bank */
static char g_bank_names[MAX_BANKS][64];
static int g_bank_count = 0;

/* Loading status for UI */
static char g_loading_status[256] = "Initializing...";
static int g_loading_complete = 0;

/* UI State Machine for LED rendering
 *
 * JV-880 LED behavior:
 * - Buttons are IDEMPOTENT (re-pressing active state = no-op)
 * - LEDs reflect (mode, context) state only
 * - Mode change clears context
 * - EXIT clears context
 *
 * Mode: PATCH | PERFORM | RHYTHM (one always active, mutually exclusive)
 * Context: NONE | EDIT | SYSTEM | UTILITY (overlay, can be cleared)
 * Tone switches: 4 independent states (patch mode only)
 */
enum UiMode {
    MODE_PATCH = 0,    /* Single patch mode */
    MODE_PERFORM,      /* Performance (multi-timbral) mode */
    MODE_RHYTHM        /* Rhythm/drum mode */
};

enum UiContext {
    CTX_PLAY = 0,      /* Normal play mode - no context LEDs lit */
    CTX_EDIT,          /* EDIT - editing patch/performance/rhythm (toggles with EDIT button) */
    CTX_SYSTEM,        /* SYSTEM - system settings (idempotent) */
    CTX_UTILITY        /* UTILITY - utility menu (idempotent) */
};

static UiMode g_ui_mode = MODE_PATCH;
static UiContext g_ui_context = CTX_PLAY;
static int g_tone_switch[4] = {1, 1, 1, 1};  /* Tone 1-4 on/off state */

/* Progressive loading state machine */
enum LoadingPhase {
    PHASE_INIT = 0,
    PHASE_CHECK_CACHE,
    PHASE_SCAN_EXPANSION,
    PHASE_BUILD_PATCHES,
    PHASE_WARMUP,
    PHASE_COMPLETE
};
static int g_loading_phase = PHASE_INIT;
static int g_loading_subindex = 0;  /* For multi-step phases */
static int g_warmup_count = 0;

/* Cache file structure */
#define CACHE_MAGIC 0x4A563838  /* "JV88" */
#define CACHE_VERSION 2  /* v2: added rom_size field */
#define CACHE_FILENAME "patch_cache.bin"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t rom1_size;
    uint32_t rom2_size;
    uint32_t waverom1_size;
    uint32_t waverom2_size;
    uint32_t expansion_count;
    uint32_t total_patches;
    uint32_t bank_count;
} CacheHeader;

/* Expansion file list for fingerprinting */
#define MAX_EXP_FILES 64
static char g_expansion_files[MAX_EXP_FILES][256];
static uint32_t g_expansion_sizes[MAX_EXP_FILES];
static int g_expansion_file_count = 0;

/* Background emulation thread */
static pthread_t g_emu_thread;
static volatile int g_thread_running = 0;
static pthread_t g_load_thread;
static volatile int g_load_thread_running = 0;

/* Audio ring buffer (44.1kHz stereo output) */
#define AUDIO_RING_SIZE 192
static int16_t g_audio_ring[AUDIO_RING_SIZE * 2];
static volatile int g_ring_write = 0;
static volatile int g_ring_read = 0;
static pthread_mutex_t g_ring_mutex = PTHREAD_MUTEX_INITIALIZER;

/* MIDI queue */
#define MIDI_QUEUE_SIZE 256
#define MIDI_MSG_MAX_LEN 256
static uint8_t g_midi_queue[MIDI_QUEUE_SIZE][MIDI_MSG_MAX_LEN];
static int g_midi_queue_len[MIDI_QUEUE_SIZE];
static volatile int g_midi_write = 0;
static volatile int g_midi_read = 0;

/* Octave transpose */
static int g_octave_transpose = 0;

/* Note: MIDI clock is now generated by the host, not the plugin */

/* Sample rates */
/* JV-880 emulator core runs at ~64 kHz (matches jv880_juce timing). */
#define JV880_SAMPLE_RATE 64000
#define MOVE_SAMPLE_RATE 44100

/* Ring buffer helpers */
static int ring_available(void) {
    int avail = g_ring_write - g_ring_read;
    if (avail < 0) avail += AUDIO_RING_SIZE;
    return avail;
}

static int ring_free(void) {
    return AUDIO_RING_SIZE - 1 - ring_available();
}

static void *load_thread_func(void *arg);

/* Load ROM file */
static int load_rom(const char *filename, uint8_t *dest, size_t size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/%s", g_module_dir, filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "JV880: Cannot open: %s\n", path);
        return 0;
    }

    size_t got = fread(dest, 1, size, f);
    fclose(f);

    if (got != size) {
        fprintf(stderr, "JV880: Size mismatch: %s (%zu vs %zu)\n", filename, got, size);
        return 0;
    }

    fprintf(stderr, "JV880: Loaded %s\n", filename);
    return 1;
}

/* Unscramble SR-JV expansion ROM (from jv880_juce) */
static void unscramble_rom(const uint8_t *src, uint8_t *dst, int len) {
    for (int i = 0; i < len; i++) {
        int address = i & ~0xfffff;
        static const int aa[] = {2, 0, 3, 4, 1, 9, 13, 10, 18, 17,
                                 6, 15, 11, 16, 8, 5, 12, 7, 14, 19};
        for (int j = 0; j < 20; j++) {
            if (i & (1 << j))
                address |= 1 << aa[j];
        }
        uint8_t srcdata = src[address];
        uint8_t data = 0;
        static const int dd[] = {2, 0, 4, 5, 7, 6, 3, 1};
        for (int j = 0; j < 8; j++) {
            if (srcdata & (1 << dd[j]))
                data |= 1 << j;
        }
        dst[i] = data;
    }
}

/* Extract short name from filename like "SR-JV80-01_Pop.bin" -> "01 Pop" */
static void extract_expansion_name(const char *filename, char *name, int max_len) {
    /* Look for pattern SR-JV80-XX_Name.bin */
    const char *p = strstr(filename, "SR-JV80-");
    if (p) {
        p += 8;  /* Skip "SR-JV80-" */
        /* Copy number and name */
        int i = 0;
        while (*p && *p != '.' && i < max_len - 1) {
            if (*p == '_') {
                name[i++] = ' ';
            } else {
                name[i++] = *p;
            }
            p++;
        }
        name[i] = '\0';
    } else {
        strncpy(name, filename, max_len - 1);
        name[max_len - 1] = '\0';
    }
}

/* Get file size for fingerprinting */
static uint32_t get_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    fclose(f);
    return size;
}

/* Scan for expansion files (just filenames, no loading) */
static void scan_expansion_files(void) {
    char exp_dir[1024];
    snprintf(exp_dir, sizeof(exp_dir), "%s/roms/expansions", g_module_dir);

    g_expansion_file_count = 0;

    DIR *dir = opendir(exp_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && g_expansion_file_count < MAX_EXP_FILES) {
        if (strstr(entry->d_name, "SR-JV80") && strstr(entry->d_name, ".bin")) {
            strncpy(g_expansion_files[g_expansion_file_count], entry->d_name,
                    sizeof(g_expansion_files[0]) - 1);

            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", exp_dir, entry->d_name);
            g_expansion_sizes[g_expansion_file_count] = get_file_size(path);

            g_expansion_file_count++;
        }
    }
    closedir(dir);
}

/* Save patch cache to file */
static void save_cache(void) {
    char cache_path[1024];
    snprintf(cache_path, sizeof(cache_path), "%s/roms/%s", g_module_dir, CACHE_FILENAME);

    FILE *f = fopen(cache_path, "wb");
    if (!f) {
        fprintf(stderr, "JV880: Cannot write cache\n");
        return;
    }

    /* Write header */
    CacheHeader hdr;
    hdr.magic = CACHE_MAGIC;
    hdr.version = CACHE_VERSION;

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/jv880_rom1.bin", g_module_dir);
    hdr.rom1_size = get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_rom2.bin", g_module_dir);
    hdr.rom2_size = get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom1.bin", g_module_dir);
    hdr.waverom1_size = get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom2.bin", g_module_dir);
    hdr.waverom2_size = get_file_size(path);

    hdr.expansion_count = g_expansion_count;
    hdr.total_patches = g_total_patches;
    hdr.bank_count = g_bank_count;

    fwrite(&hdr, sizeof(hdr), 1, f);

    /* Write expansion file list */
    fwrite(&g_expansion_file_count, sizeof(g_expansion_file_count), 1, f);
    for (int i = 0; i < g_expansion_file_count; i++) {
        fwrite(g_expansion_files[i], sizeof(g_expansion_files[0]), 1, f);
        fwrite(&g_expansion_sizes[i], sizeof(g_expansion_sizes[0]), 1, f);
    }

    /* Write expansion info (without unscrambled data pointers) */
    for (int i = 0; i < g_expansion_count; i++) {
        fwrite(g_expansions[i].filename, sizeof(g_expansions[i].filename), 1, f);
        fwrite(g_expansions[i].name, sizeof(g_expansions[i].name), 1, f);
        fwrite(&g_expansions[i].patch_count, sizeof(g_expansions[i].patch_count), 1, f);
        fwrite(&g_expansions[i].patches_offset, sizeof(g_expansions[i].patches_offset), 1, f);
        fwrite(&g_expansions[i].first_global_index, sizeof(g_expansions[i].first_global_index), 1, f);
        fwrite(&g_expansions[i].rom_size, sizeof(g_expansions[i].rom_size), 1, f);
    }

    /* Write patches */
    fwrite(g_patches, sizeof(PatchInfo), g_total_patches, f);

    /* Write banks */
    fwrite(g_bank_starts, sizeof(g_bank_starts[0]), g_bank_count, f);
    fwrite(g_bank_names, sizeof(g_bank_names[0]), g_bank_count, f);

    fclose(f);
    fprintf(stderr, "JV880: Saved cache (%d patches, %d banks)\n", g_total_patches, g_bank_count);
}

/* Load patch cache from file, returns 1 if valid cache loaded */
static int load_cache(void) {
    char cache_path[1024];
    snprintf(cache_path, sizeof(cache_path), "%s/roms/%s", g_module_dir, CACHE_FILENAME);

    FILE *f = fopen(cache_path, "rb");
    if (!f) {
        fprintf(stderr, "JV880: No cache file\n");
        return 0;
    }

    /* Read and validate header */
    CacheHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != CACHE_MAGIC ||
        hdr.version != CACHE_VERSION) {
        fprintf(stderr, "JV880: Invalid cache header\n");
        fclose(f);
        return 0;
    }

    /* Verify ROM fingerprints */
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/jv880_rom1.bin", g_module_dir);
    if (get_file_size(path) != hdr.rom1_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_rom2.bin", g_module_dir);
    if (get_file_size(path) != hdr.rom2_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom1.bin", g_module_dir);
    if (get_file_size(path) != hdr.waverom1_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom2.bin", g_module_dir);
    if (get_file_size(path) != hdr.waverom2_size) { fclose(f); return 0; }

    /* Read and verify expansion file list */
    int cached_exp_count;
    if (fread(&cached_exp_count, sizeof(cached_exp_count), 1, f) != 1) {
        fclose(f);
        return 0;
    }

    if (cached_exp_count != g_expansion_file_count) {
        fprintf(stderr, "JV880: Expansion count changed (%d vs %d)\n",
                cached_exp_count, g_expansion_file_count);
        fclose(f);
        return 0;
    }

    /* Verify each expansion file */
    for (int i = 0; i < cached_exp_count; i++) {
        char cached_name[256];
        uint32_t cached_size;
        fread(cached_name, sizeof(cached_name), 1, f);
        fread(&cached_size, sizeof(cached_size), 1, f);

        /* Find matching file */
        int found = 0;
        for (int j = 0; j < g_expansion_file_count; j++) {
            if (strcmp(cached_name, g_expansion_files[j]) == 0 &&
                cached_size == g_expansion_sizes[j]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "JV880: Expansion %s changed\n", cached_name);
            fclose(f);
            return 0;
        }
    }

    /* Cache is valid - load data */
    g_expansion_count = hdr.expansion_count;
    g_total_patches = hdr.total_patches;
    g_bank_count = hdr.bank_count;

    /* Read expansion info */
    for (int i = 0; i < g_expansion_count; i++) {
        fread(g_expansions[i].filename, sizeof(g_expansions[i].filename), 1, f);
        fread(g_expansions[i].name, sizeof(g_expansions[i].name), 1, f);
        fread(&g_expansions[i].patch_count, sizeof(g_expansions[i].patch_count), 1, f);
        fread(&g_expansions[i].patches_offset, sizeof(g_expansions[i].patches_offset), 1, f);
        fread(&g_expansions[i].first_global_index, sizeof(g_expansions[i].first_global_index), 1, f);
        fread(&g_expansions[i].rom_size, sizeof(g_expansions[i].rom_size), 1, f);
        g_expansions[i].unscrambled = nullptr;  /* Will load on demand */
    }

    /* Read patches */
    fread(g_patches, sizeof(PatchInfo), g_total_patches, f);

    /* Read banks */
    fread(g_bank_starts, sizeof(g_bank_starts[0]), g_bank_count, f);
    fread(g_bank_names, sizeof(g_bank_names[0]), g_bank_count, f);

    fclose(f);
    fprintf(stderr, "JV880: Loaded cache (%d patches, %d banks, %d expansions)\n",
            g_total_patches, g_bank_count, g_expansion_count);
    return 1;
}

/* Load and parse expansion ROM to get patch info (doesn't keep it loaded) */
static int scan_expansion_rom(const char *filename, ExpansionInfo *info) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/expansions/%s", g_module_dir, filename);

    /* Update loading status */
    snprintf(g_loading_status, sizeof(g_loading_status), "Scanning: %.40s", filename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Support 8MB standard ROMs and 2MB Experience series */
    uint32_t rom_size = 0;
    if (size == EXPANSION_SIZE_8MB) {
        rom_size = EXPANSION_SIZE_8MB;
    } else if (size == EXPANSION_SIZE_2MB) {
        rom_size = EXPANSION_SIZE_2MB;
    } else {
        fprintf(stderr, "JV880: Skipping %s (size %ld, expected %d or %d)\n",
                filename, size, EXPANSION_SIZE_8MB, EXPANSION_SIZE_2MB);
        snprintf(g_loading_status, sizeof(g_loading_status), "Skipped: %.40s (wrong size)", filename);
        fclose(f);
        return 0;
    }

    /* Read and unscramble */
    uint8_t *scrambled = (uint8_t *)malloc(rom_size);
    uint8_t *unscrambled = (uint8_t *)malloc(rom_size);
    if (!scrambled || !unscrambled) {
        free(scrambled);
        free(unscrambled);
        fclose(f);
        return 0;
    }

    fread(scrambled, 1, rom_size, f);
    fclose(f);

    unscramble_rom(scrambled, unscrambled, rom_size);
    free(scrambled);

    /* Parse expansion header (from jv880_juce) */
    /* Patch count at 0x66-0x67 (big endian) */
    int patch_count = unscrambled[0x67] | (unscrambled[0x66] << 8);

    /* Patches offset at 0x8c-0x8f (big endian) */
    uint32_t patches_offset = unscrambled[0x8f] |
                              (unscrambled[0x8e] << 8) |
                              (unscrambled[0x8d] << 16) |
                              (unscrambled[0x8c] << 24);

    if (patch_count <= 0 || patch_count > MAX_PATCHES_PER_EXP) {
        fprintf(stderr, "JV880: Invalid patch count %d in %s\n", patch_count, filename);
        free(unscrambled);
        return 0;
    }

    /* Validate patches_offset is within ROM bounds */
    if (patches_offset >= rom_size) {
        fprintf(stderr, "JV880: Invalid patch offset 0x%x in %s (size 0x%x)\n",
                patches_offset, filename, rom_size);
        free(unscrambled);
        return 0;
    }

    /* Store info */
    strncpy(info->filename, filename, sizeof(info->filename) - 1);
    extract_expansion_name(filename, info->name, sizeof(info->name));
    info->patch_count = patch_count;
    info->patches_offset = patches_offset;
    info->rom_size = rom_size;
    info->unscrambled = unscrambled;  /* Keep unscrambled data */

    /* Update loading status */
    snprintf(g_loading_status, sizeof(g_loading_status), "Loaded: %s (%d patches)", info->name, patch_count);

    fprintf(stderr, "JV880: Scanned %s: %d patches at offset 0x%x (%dMB ROM)\n",
            info->name, patch_count, patches_offset, rom_size / (1024 * 1024));

    return 1;
}

/* Compare function for sorting expansions alphabetically by name */
static int compare_expansions(const void *a, const void *b) {
    const ExpansionInfo *exp_a = (const ExpansionInfo *)a;
    const ExpansionInfo *exp_b = (const ExpansionInfo *)b;
    return strcasecmp(exp_a->name, exp_b->name);
}

/* Scan for expansion ROMs and build patch list */
static void scan_expansions(void) {
    char exp_dir[1024];
    snprintf(exp_dir, sizeof(exp_dir), "%s/roms/expansions", g_module_dir);

    DIR *dir = opendir(exp_dir);
    if (!dir) {
        fprintf(stderr, "JV880: No expansions directory\n");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && g_expansion_count < MAX_EXPANSIONS) {
        /* Look for .bin files starting with SR-JV80 */
        if (strstr(entry->d_name, "SR-JV80") && strstr(entry->d_name, ".bin")) {
            if (scan_expansion_rom(entry->d_name, &g_expansions[g_expansion_count])) {
                g_expansion_count++;
            }
        }
    }
    closedir(dir);

    /* Sort expansions alphabetically by name */
    if (g_expansion_count > 1) {
        qsort(g_expansions, g_expansion_count, sizeof(ExpansionInfo), compare_expansions);
    }

    fprintf(stderr, "JV880: Found %d expansion ROMs\n", g_expansion_count);
}

/* Build unified patch list */
static void build_patch_list(void) {
    g_total_patches = 0;
    g_bank_count = 0;

    /* Preset Bank A (0-63) - Factory presets starting with A.Piano 1 */
    g_bank_starts[g_bank_count] = g_total_patches;
    strncpy(g_bank_names[g_bank_count], "Preset A", sizeof(g_bank_names[0]) - 1);
    g_bank_count++;

    for (int i = 0; i < 64 && g_total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &g_patches[g_total_patches];
        uint32_t offset = PATCH_OFFSET_PRESET_A + (i * PATCH_SIZE);

        memcpy(p->name, &g_rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';

        p->expansion_index = -1;
        p->local_patch_index = i;
        p->rom_offset = offset;
        g_total_patches++;
    }

    /* Preset Bank B (64-127) - Factory presets starting with Pizzicato */
    g_bank_starts[g_bank_count] = g_total_patches;
    strncpy(g_bank_names[g_bank_count], "Preset B", sizeof(g_bank_names[0]) - 1);
    g_bank_count++;

    for (int i = 0; i < 64 && g_total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &g_patches[g_total_patches];
        uint32_t offset = PATCH_OFFSET_PRESET_B + (i * PATCH_SIZE);

        memcpy(p->name, &g_rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';

        p->expansion_index = -1;
        p->local_patch_index = 64 + i;
        p->rom_offset = offset;
        g_total_patches++;
    }

    /* Internal Bank (128-191) - User patches starting with JV Strings */
    g_bank_starts[g_bank_count] = g_total_patches;
    strncpy(g_bank_names[g_bank_count], "Internal", sizeof(g_bank_names[0]) - 1);
    g_bank_count++;

    for (int i = 0; i < 64 && g_total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &g_patches[g_total_patches];
        uint32_t offset = PATCH_OFFSET_INTERNAL + (i * PATCH_SIZE);

        memcpy(p->name, &g_rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';

        p->expansion_index = -1;
        p->local_patch_index = 128 + i;
        p->rom_offset = offset;
        g_total_patches++;
    }

    /* Expansion patches */
    for (int e = 0; e < g_expansion_count && g_bank_count < MAX_BANKS; e++) {
        ExpansionInfo *exp = &g_expansions[e];
        exp->first_global_index = g_total_patches;

        /* Add bank entry for this expansion */
        g_bank_starts[g_bank_count] = g_total_patches;
        strncpy(g_bank_names[g_bank_count], exp->name, sizeof(g_bank_names[0]) - 1);
        g_bank_count++;

        for (int i = 0; i < exp->patch_count && g_total_patches < MAX_TOTAL_PATCHES; i++) {
            PatchInfo *p = &g_patches[g_total_patches];
            uint32_t offset = exp->patches_offset + (i * PATCH_SIZE);

            /* Copy patch name from unscrambled expansion data */
            if (exp->unscrambled) {
                memcpy(p->name, &exp->unscrambled[offset], PATCH_NAME_LEN);
                p->name[PATCH_NAME_LEN] = '\0';
            } else {
                snprintf(p->name, sizeof(p->name), "Patch %d", i);
            }

            p->expansion_index = e;
            p->local_patch_index = i;
            p->rom_offset = offset;
            g_total_patches++;
        }
    }

    fprintf(stderr, "JV880: Total patches: %d (192 internal + %d expansion) in %d banks\n",
            g_total_patches, g_total_patches - 192, g_bank_count);
}

/* Send All Notes Off to prevent stuck notes */
static void send_all_notes_off(void) {
    /* Send All Notes Off (CC 123) on all channels */
    for (int ch = 0; ch < 16; ch++) {
        uint8_t msg[3] = { (uint8_t)(0xB0 | ch), 123, 0 };
        int next = (g_midi_write + 1) % MIDI_QUEUE_SIZE;
        if (next != g_midi_read) {
            memcpy(g_midi_queue[g_midi_write], msg, 3);
            g_midi_queue_len[g_midi_write] = 3;
            g_midi_write = next;
        }
    }
}

/* Load expansion ROM data on demand (for cache hits) */
static int load_expansion_data(int exp_index) {
    if (exp_index < 0 || exp_index >= g_expansion_count) return 0;

    ExpansionInfo *exp = &g_expansions[exp_index];
    if (exp->unscrambled) return 1;  /* Already loaded */

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/expansions/%s", g_module_dir, exp->filename);

    fprintf(stderr, "JV880: Loading expansion %s on demand...\n", exp->name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "JV880: Cannot open %s\n", path);
        return 0;
    }

    uint8_t *scrambled = (uint8_t *)malloc(exp->rom_size);
    uint8_t *unscrambled = (uint8_t *)malloc(exp->rom_size);
    if (!scrambled || !unscrambled) {
        free(scrambled);
        free(unscrambled);
        fclose(f);
        return 0;
    }

    fread(scrambled, 1, exp->rom_size, f);
    fclose(f);

    unscramble_rom(scrambled, unscrambled, exp->rom_size);
    free(scrambled);

    exp->unscrambled = unscrambled;
    fprintf(stderr, "JV880: Loaded expansion %s (%dMB)\n", exp->name, exp->rom_size / (1024 * 1024));
    return 1;
}

/* Load expansion ROM into emulator's waverom_exp and cardram */
static void load_expansion_to_emulator(int exp_index) {
    if (exp_index < 0 || exp_index >= g_expansion_count) return;
    if (exp_index == g_current_expansion) return;  /* Already loaded */

    ExpansionInfo *exp = &g_expansions[exp_index];

    /* Load expansion data if not already in memory (cache hit case) */
    if (!exp->unscrambled) {
        if (!load_expansion_data(exp_index)) {
            fprintf(stderr, "JV880: Failed to load expansion %s\n", exp->name);
            return;
        }
    }

    /* Send All Notes Off before reset to prevent stuck keys */
    send_all_notes_off();

    /* Clear the buffers first */
    memset(g_mcu->pcm.waverom_exp, 0, EXPANSION_SIZE_8MB);
    memset(g_mcu->pcm.waverom_card, 0, 0x200000);
    memset(g_mcu->cardram, 0, CARDRAM_SIZE);

    /* Copy first 32KB to cardram (patch/tone definitions for CPU) */
    size_t cardram_size = (exp->rom_size < CARDRAM_SIZE) ? exp->rom_size : CARDRAM_SIZE;
    memcpy(g_mcu->cardram, exp->unscrambled, cardram_size);

    /* SR-JV80 expansion boards use banks 3-6 (waverom_exp), NOT bank 2 (waverom_card) */
    /* Bank 2 (waverom_card) is for SO-PCM1 type 2MB cards */

    /* Copy full expansion data to waverom_exp (PCM banks 3-6, up to 8MB) */
    memcpy(g_mcu->pcm.waverom_exp, exp->unscrambled, exp->rom_size);

    g_current_expansion = exp_index;
    /* Reset emulator so it detects the new expansion */
    g_mcu->SC55_Reset();
    fprintf(stderr, "JV880: Loaded expansion %s to emulator (%dMB, cardram + waverom, with reset)\n",
            exp->name, exp->rom_size / (1024 * 1024));
}

/* Load patch to NVRAM and switch expansion if needed */
static void select_patch(int global_index) {
    if (global_index < 0 || global_index >= g_total_patches) return;

    PatchInfo *p = &g_patches[global_index];
    g_current_patch = global_index;

    /* Load expansion if needed */
    if (p->expansion_index >= 0) {
        load_expansion_to_emulator(p->expansion_index);

        /* Copy patch from expansion to NVRAM */
        ExpansionInfo *exp = &g_expansions[p->expansion_index];
        if (exp->unscrambled) {
            memcpy(&g_mcu->nvram[NVRAM_PATCH_OFFSET],
                   &exp->unscrambled[p->rom_offset], PATCH_SIZE);
        }
    } else {
        /* Internal patch - copy from ROM2 */
        memcpy(&g_mcu->nvram[NVRAM_PATCH_OFFSET], &g_rom2[p->rom_offset], PATCH_SIZE);
    }

    /* Set patch mode */
    g_mcu->nvram[NVRAM_MODE_OFFSET] = 1;

    /* Trigger patch load */
    uint8_t pc_msg[2] = { 0xC0, 0x00 };
    int next = (g_midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != g_midi_read) {
        memcpy(g_midi_queue[g_midi_write], pc_msg, 2);
        g_midi_queue_len[g_midi_write] = 2;
        g_midi_write = next;
    }

    fprintf(stderr, "JV880: Selected patch %d: %s\n", global_index, p->name);
}

/* Automated LED reverse engineering test
 * Minimal test - just cycle modes and watch stderr for IO writes
 */
static void run_automated_led_test() {
    fprintf(stderr, "JV880: LED test starting, g_mcu=%p\n", (void*)g_mcu);

    FILE *f = fopen("/data/UserData/jv880_led_auto.txt", "w");
    if (!f) {
        fprintf(stderr, "JV880: Failed to open LED test file\n");
        return;
    }

    fprintf(f, "JV-880 LED Test - Watch stderr for IO writes\n");
    fprintf(f, "=============================================\n\n");
    fflush(f);

    if (!g_mcu) {
        fprintf(f, "ERROR: g_mcu is NULL\n");
        fclose(f);
        return;
    }

    /* Helper to log LED state */
    auto log_leds = [&](const char* label) {
        fprintf(f, "  LED state: mode=%02X tone=%02X\n",
                g_mcu->led_mode_state, g_mcu->led_tone_state);
        fprintf(stderr, "JV880: %s LEDs: mode=%02X tone=%02X\n",
                label, g_mcu->led_mode_state, g_mcu->led_tone_state);
    };

    /* Helper to press and release a button */
    auto do_button = [&](int btn, const char* name) {
        fprintf(stderr, "JV880: >>> Pressing button %d (%s)\n", btn, name);
        g_mcu->mcu_button_pressed |= (1 << btn);
        for (int i = 0; i < 5; i++) g_mcu->updateSC55(1000);
        g_mcu->mcu_button_pressed &= ~(1 << btn);
        for (int i = 0; i < 15; i++) g_mcu->updateSC55(1000);  /* More cycles for LED update */
        fprintf(stderr, "JV880: LCD=[%.20s]\n", g_mcu->lcd.GetLine(0));
        log_leds(name);
    };

    fprintf(f, "Initial LCD: [%.24s]\n", g_mcu->lcd.GetLine(0));
    fprintf(stderr, "JV880: Initial LCD=[%.20s]\n", g_mcu->lcd.GetLine(0));
    log_leds("Initial");

    /* Test EDIT */
    fprintf(f, "\n=== EDIT ===\n");
    do_button(MCU_BUTTON_EDIT, "EDIT");
    fprintf(f, "After EDIT: LCD=[%.24s]\n", g_mcu->lcd.GetLine(0));

    /* Test SYSTEM */
    fprintf(f, "\n=== SYSTEM ===\n");
    do_button(MCU_BUTTON_SYSTEM, "SYSTEM");
    fprintf(f, "After SYSTEM: LCD=[%.24s]\n", g_mcu->lcd.GetLine(0));

    /* Test RHYTHM */
    fprintf(f, "\n=== RHYTHM ===\n");
    do_button(MCU_BUTTON_RHYTHM, "RHYTHM");
    fprintf(f, "After RHYTHM: LCD=[%.24s]\n", g_mcu->lcd.GetLine(0));

    /* Test UTILITY */
    fprintf(f, "\n=== UTILITY ===\n");
    do_button(MCU_BUTTON_UTILITY, "UTILITY");
    fprintf(f, "After UTILITY: LCD=[%.24s]\n", g_mcu->lcd.GetLine(0));

    /* Test PATCH/PERFORM toggle - should switch modes */
    fprintf(stderr, "JV880: About to test PATCH/PERFORM button\n");
    fprintf(f, "\n=== PATCH/PERFORM ===\n");
    fflush(f);
    do_button(MCU_BUTTON_PATCH_PERFORM, "PATCH/PERFORM");
    fprintf(f, "After PATCH/PERFORM: LCD=[%.24s]\n", g_mcu->lcd.GetLine(0));
    fflush(f);

    /* Return to PATCH mode by pressing PATCH/PERFORM from main screen */
    /* First exit UTILITY by pressing it again */
    fprintf(f, "\n=== Return to PATCH ===\n");
    do_button(MCU_BUTTON_UTILITY, "EXIT_UTILITY");  /* Exit utility */
    do_button(MCU_BUTTON_PATCH_PERFORM, "PATCH/PERFORM");
    fprintf(f, "After return: LCD=[%.24s]\n", g_mcu->lcd.GetLine(0));
    log_leds("Final");

    fflush(f);

    fprintf(f, "\n=== TEST COMPLETE ===\n");
    fclose(f);

    fprintf(stderr, "JV880: LED test complete -> /data/UserData/jv880_led_auto.txt\n");
}

/* Switch between patch and performance mode
 * Sets NVRAM mode flag and simulates PATCH/PERFORM button press
 */
static void set_mode(int performance_mode) {
    if (!g_mcu) return;

    int new_mode = performance_mode ? 1 : 0;

    /* Only switch if mode is actually changing */
    if (g_performance_mode == new_mode) return;

    g_performance_mode = new_mode;

    /* Set mode in NVRAM: 0 = performance, 1 = patch */
    g_mcu->nvram[NVRAM_MODE_OFFSET] = g_performance_mode ? 0 : 1;

    /* Simulate pressing PATCH/PERFORM button to trigger mode switch in emulator */
    g_mcu->mcu_button_pressed |= (1 << MCU_BUTTON_PATCH_PERFORM);

    fprintf(stderr, "JV880: Switched to %s mode (NVRAM[0x11]=0x%02x, button pressed)\n",
            g_performance_mode ? "Performance" : "Patch",
            g_mcu->nvram[NVRAM_MODE_OFFSET]);
}

/* Simulate pressing an MCU button (set bit to trigger press, then clear) */
static void press_mcu_button(int button) {
    if (!g_mcu || button < 0 || button > 31) return;
    g_mcu->mcu_button_pressed |= (1 << button);
}

static void release_mcu_button(int button) {
    if (!g_mcu || button < 0 || button > 31) return;
    g_mcu->mcu_button_pressed &= ~(1 << button);
}

/* Select a performance (0-47 across 3 banks) using MIDI bank select + program change.
 *
 * JV-880 Performance selection via MIDI (on Control Channel, default ch 16):
 *   Bank 80 + PC 1-16  → Internal Performances 1-16
 *   Bank 81 + PC 1-16  → Preset A Performances 1-16
 *   Bank 81 + PC 65-80 → Preset B Performances 1-16
 *
 * Our mapping (0-indexed):
 *   perf_index 0-15  (bank 0) = Preset A  → Bank 81, PC 0-15
 *   perf_index 16-31 (bank 1) = Preset B  → Bank 81, PC 64-79
 *   perf_index 32-47 (bank 2) = Internal  → Bank 80, PC 0-15
 */
static void select_performance(int perf_index) {
    if (!g_mcu || perf_index < 0 || perf_index >= NUM_PERFORMANCES) return;

    g_current_performance = perf_index;
    g_perf_bank = perf_index / PERFS_PER_BANK;
    int perf_in_bank = perf_index % PERFS_PER_BANK;

    /* Ensure we're in performance mode */
    if (!g_performance_mode) {
        set_mode(1);
    }

    /* Calculate bank select and program change values per JV-880 MIDI spec */
    uint8_t bank_msb;
    uint8_t pc_value;

    switch (g_perf_bank) {
        case 0:  /* Preset A */
            bank_msb = 81;
            pc_value = perf_in_bank;  /* 0-15 */
            break;
        case 1:  /* Preset B */
            bank_msb = 81;
            pc_value = 64 + perf_in_bank;  /* 64-79 */
            break;
        case 2:  /* Internal */
        default:
            bank_msb = 80;
            pc_value = perf_in_bank;  /* 0-15 */
            break;
    }

    /* Send on Control Channel (channel 16 = 0x0F, so status bytes are 0xBF/0xCF) */
    uint8_t ctrl_ch = 0x0F;  /* Channel 16 (0-indexed) */
    uint8_t bank_msg[3] = { (uint8_t)(0xB0 | ctrl_ch), 0x00, bank_msb };
    uint8_t pc_msg[2] = { (uint8_t)(0xC0 | ctrl_ch), pc_value };

    /* Queue Bank Select (CC#0) */
    int next = (g_midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != g_midi_read) {
        memcpy(g_midi_queue[g_midi_write], bank_msg, 3);
        g_midi_queue_len[g_midi_write] = 3;
        g_midi_write = next;
    }

    /* Queue Program Change */
    next = (g_midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != g_midi_read) {
        memcpy(g_midi_queue[g_midi_write], pc_msg, 2);
        g_midi_queue_len[g_midi_write] = 2;
        g_midi_write = next;
    }

    const char* bank_names[] = { "Preset A", "Preset B", "Internal" };
    fprintf(stderr, "JV880: Selected %s performance %d (ch16: Bank=%d PC=%d)\n",
            bank_names[g_perf_bank], perf_in_bank + 1, bank_msb, pc_value);
}

/* Select a part within the current performance (0-7) */
static void select_part(int part_index) {
    if (part_index < 0 || part_index > 7) return;
    g_current_part = part_index;
    fprintf(stderr, "JV880: Selected part %d\n", part_index + 1);
}

/* Get current bank name for display */
static const char* get_current_bank_name(void) {
    if (g_current_patch < 0 || g_current_patch >= g_total_patches) {
        return "JV-880";
    }

    PatchInfo *p = &g_patches[g_current_patch];
    if (p->expansion_index < 0) {
        if (p->local_patch_index < 64) {
            return "Internal A";
        } else {
            return "Internal B";
        }
    } else {
        return g_expansions[p->expansion_index].name;
    }
}

/*
 * Emulation thread - runs MCU+PCM together as designed
 * Note: MIDI clock is now generated by the host
 */
static void *emu_thread_func(void *arg) {
    (void)arg;
    fprintf(stderr, "JV880: Emulation thread started\n");

    float resample_acc = 0.0f;
    const float resample_ratio = (float)JV880_SAMPLE_RATE / (float)MOVE_SAMPLE_RATE;

    while (g_thread_running) {
        /* Process MIDI queue */
        while (g_midi_read != g_midi_write) {
            int idx = g_midi_read;
            g_mcu->postMidiSC55(g_midi_queue[idx], g_midi_queue_len[idx]);
            g_midi_read = (g_midi_read + 1) % MIDI_QUEUE_SIZE;
        }

        /* Check if we need more audio */
        int free_space = ring_free();
        if (free_space < 64) {
            usleep(50);
            continue;
        }

        g_mcu->updateSC55(64);

        /* Resample and copy to ring buffer */
        int avail = g_mcu->sample_write_ptr;
        for (int i = 0; i < avail && ring_free() > 0; i += 2) {
            resample_acc += 1.0f;
            if (resample_acc >= resample_ratio) {
                resample_acc -= resample_ratio;

                pthread_mutex_lock(&g_ring_mutex);
                int wr = g_ring_write;
                g_audio_ring[wr * 2 + 0] = g_mcu->sample_buffer[i];
                g_audio_ring[wr * 2 + 1] = g_mcu->sample_buffer[i + 1];
                g_ring_write = (wr + 1) % AUDIO_RING_SIZE;
                pthread_mutex_unlock(&g_ring_mutex);
            }
        }
    }

    fprintf(stderr, "JV880: Emulation thread stopped\n");
    return NULL;
}

/* Plugin callbacks */

static int jv880_on_load(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    strncpy(g_module_dir, module_dir, sizeof(g_module_dir) - 1);
    fprintf(stderr, "JV880: Loading from %s\n", module_dir);
    char debug_path[1024];
    snprintf(debug_path, sizeof(debug_path), "%s/debug_sysex_test", module_dir);
    if (access(debug_path, F_OK) == 0) {
        g_debug_sysex = 1;
        fprintf(stderr, "JV880: SysEx debug enabled (%s)\n", debug_path);
    }

    /* Create emulator instance */
    g_mcu = new MCU();

    /* Load ROMs */
    uint8_t *rom1 = (uint8_t *)malloc(ROM1_SIZE);
    uint8_t *rom2 = (uint8_t *)malloc(ROM2_SIZE);
    uint8_t *waverom1 = (uint8_t *)malloc(0x200000);
    uint8_t *waverom2 = (uint8_t *)malloc(0x200000);
    uint8_t *nvram = (uint8_t *)malloc(NVRAM_SIZE);

    if (!rom1 || !rom2 || !waverom1 || !waverom2 || !nvram) {
        fprintf(stderr, "JV880: Memory allocation failed\n");
        return -1;
    }

    memset(nvram, 0xFF, NVRAM_SIZE);

    int ok = 1;
    ok = ok && load_rom("jv880_rom1.bin", rom1, ROM1_SIZE);
    ok = ok && load_rom("jv880_rom2.bin", rom2, ROM2_SIZE);
    ok = ok && load_rom("jv880_waverom1.bin", waverom1, 0x200000);
    ok = ok && load_rom("jv880_waverom2.bin", waverom2, 0x200000);

    /* NVRAM is optional */
    char nvram_path[1024];
    snprintf(nvram_path, sizeof(nvram_path), "%s/roms/jv880_nvram.bin", module_dir);
    FILE *nf = fopen(nvram_path, "rb");
    if (nf) {
        fread(nvram, 1, NVRAM_SIZE, nf);
        fclose(nf);
        fprintf(stderr, "JV880: Loaded NVRAM\n");
    }

    if (!ok) {
        fprintf(stderr, "JV880: ROM loading failed\n");
        free(rom1); free(rom2); free(waverom1); free(waverom2); free(nvram);
        delete g_mcu;
        g_mcu = nullptr;
        return -1;
    }

    /* Initialize emulator */
    g_mcu->startSC55(rom1, rom2, waverom1, waverom2, nvram);

    /* Keep ROM2 for internal patch access */
    g_rom2 = rom2;

    free(rom1); free(waverom1); free(waverom2); free(nvram);

    g_rom_loaded = 1;

    /* Set patch mode */
    g_mcu->nvram[NVRAM_MODE_OFFSET] = 1;

    g_loading_complete = 0;
    snprintf(g_loading_status, sizeof(g_loading_status), "Loading...");

    /* Load patches/expansions and warmup in background so UI can render. */
    g_load_thread_running = 1;
    pthread_create(&g_load_thread, NULL, load_thread_func, NULL);
    return 0;
}

static void jv880_on_unload(void) {
    if (g_load_thread_running) {
        g_load_thread_running = 0;
        pthread_join(g_load_thread, NULL);
    }

    if (g_thread_running) {
        g_thread_running = 0;
        pthread_join(g_emu_thread, NULL);
    }

    if (g_mcu) {
        delete g_mcu;
        g_mcu = nullptr;
    }

    if (g_rom2) {
        free(g_rom2);
        g_rom2 = nullptr;
    }

    /* Free expansion data */
    for (int i = 0; i < g_expansion_count; i++) {
        if (g_expansions[i].unscrambled) {
            free(g_expansions[i].unscrambled);
            g_expansions[i].unscrambled = nullptr;
        }
    }
    g_expansion_count = 0;
    g_current_expansion = -1;
    g_total_patches = 0;

    g_initialized = 0;
    g_rom_loaded = 0;
}

static void jv880_on_midi(const uint8_t *msg, int len, int source) {
    if (!g_initialized || !g_thread_running) return;
    if (len < 1) return;

    uint8_t status = msg[0] & 0xF0;
    if (g_debug_sysex) {
        for (int i = 0; i < len; i++) {
            uint8_t b = msg[i];
            if (b == 0xF0) {
                g_sysex_capture = 1;
                g_sysex_len = 0;
            }
            if (g_sysex_capture) {
                if (g_sysex_len < (int)sizeof(g_sysex_buf)) {
                    g_sysex_buf[g_sysex_len++] = b;
                }
                if (b == 0xF7 || g_sysex_len >= (int)sizeof(g_sysex_buf)) {
                    uint8_t dev = (g_sysex_len > 2) ? g_sysex_buf[2] : 0;
                    uint8_t model = (g_sysex_len > 3) ? g_sysex_buf[3] : 0;
                    uint8_t cmd = (g_sysex_len > 4) ? g_sysex_buf[4] : 0;
                    int data_len = g_sysex_len - 11;
                    if (data_len < 0) data_len = 0;
                    fprintf(stderr,
                            "JV880: SysEx packet len=%d dev=%02x model=%02x cmd=%02x data=%d\n",
                            g_sysex_len, dev, model, cmd, data_len);
                    if (g_sysex_len >= 12 && cmd == 0x12) {
                        uint8_t a0 = g_sysex_buf[5];
                        uint8_t a1 = g_sysex_buf[6];
                        uint8_t a2 = g_sysex_buf[7];
                        uint8_t a3 = g_sysex_buf[8];
                        uint8_t d0 = g_sysex_buf[9];
                        uint8_t mode = g_mcu ? g_mcu->nvram[NVRAM_MODE_OFFSET] : 0;
                        fprintf(stderr,
                                "JV880: SysEx addr=%02x %02x %02x %02x data0=%02x mode_nvram=%02x\n",
                                a0, a1, a2, a3, d0, mode);
                    }
                    g_sysex_capture = 0;
                    g_sysex_len = 0;
                }
            }
        }
    }

    /* Filter Move control notes (steps/track rows/knob touch) from internal MIDI. */
    if (source == MOVE_MIDI_SOURCE_INTERNAL && (status == 0x90 || status == 0x80) && len >= 2) {
        const uint8_t note = msg[1];
        const bool is_step = (note >= 16 && note <= 31);
        const bool is_track_row = (note >= 40 && note <= 43);
        const bool is_knob_touch = (note < 10);
        if (is_step || is_track_row || is_knob_touch) return;
    }

    /* Copy message so we can modify it */
    uint8_t modified[MIDI_MSG_MAX_LEN];
    int n = (len > MIDI_MSG_MAX_LEN) ? MIDI_MSG_MAX_LEN : len;
    memcpy(modified, msg, n);

    /* Apply octave transpose to note messages */
    if ((status == 0x90 || status == 0x80) && n >= 2) {
        int note = modified[1];
        note += g_octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
        modified[1] = (uint8_t)note;
    }

    int next = (g_midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != g_midi_read) {
        memcpy(g_midi_queue[g_midi_write], modified, n);
        g_midi_queue_len[g_midi_write] = n;
        g_midi_write = next;
    }
}

/* Find which bank a patch belongs to */
static int get_bank_for_patch(int patch_index) {
    for (int i = g_bank_count - 1; i >= 0; i--) {
        if (patch_index >= g_bank_starts[i]) {
            return i;
        }
    }
    return 0;
}

/* Jump to next/previous bank */
static void jump_to_bank(int direction) {
    int current_bank = get_bank_for_patch(g_current_patch);
    int new_bank = current_bank + direction;

    /* Wrap around */
    if (new_bank < 0) new_bank = g_bank_count - 1;
    if (new_bank >= g_bank_count) new_bank = 0;

    select_patch(g_bank_starts[new_bank]);
    fprintf(stderr, "JV880: Jumped to bank %d: %s\n", new_bank, g_bank_names[new_bank]);
}

static void enforce_min_loading_time(const struct timespec *start_ts, int min_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - start_ts->tv_sec) * 1000L +
                      (now.tv_nsec - start_ts->tv_nsec) / 1000000L;
    if (elapsed_ms >= min_ms) return;
    int remain_ms = min_ms - (int)elapsed_ms;
    struct timespec sleep_ts;
    sleep_ts.tv_sec = remain_ms / 1000;
    sleep_ts.tv_nsec = (remain_ms % 1000) * 1000000L;
    nanosleep(&sleep_ts, NULL);
}

static void *load_thread_func(void *arg) {
    (void)arg;

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* Scan expansion file list (just filenames, fast) */
    snprintf(g_loading_status, sizeof(g_loading_status), "Checking expansions...");
    scan_expansion_files();
    fprintf(stderr, "JV880: Found %d expansion files\n", g_expansion_file_count);

    /* Try to load from cache first */
    int cache_valid = load_cache();

    if (!cache_valid) {
        /* Cache miss - do full scan */
        fprintf(stderr, "JV880: Cache miss, scanning expansions...\n");
        snprintf(g_loading_status, sizeof(g_loading_status), "Scanning expansions...");
        scan_expansions();
        build_patch_list();
        save_cache();
    } else {
        /* Cache hit - expansion ROM data will be loaded on demand */
        fprintf(stderr, "JV880: Using cached patch data\n");
    }

    /* Load first expansion BEFORE warmup so firmware detects the card at boot */
    if (g_expansion_count > 0) {
        fprintf(stderr, "JV880: Pre-loading expansion %s for boot detection...\n",
                g_expansions[0].name);
        /* Load expansion data if needed */
        if (!g_expansions[0].unscrambled) {
            load_expansion_data(0);
        }
        if (g_expansions[0].unscrambled) {
            /* Copy to cardram, waverom_card, and waverom_exp before first MCU run */
            memset(g_mcu->pcm.waverom_exp, 0, EXPANSION_SIZE_8MB);
            memset(g_mcu->pcm.waverom_card, 0, 0x200000);
            memset(g_mcu->cardram, 0, CARDRAM_SIZE);
            size_t cardram_size = (g_expansions[0].rom_size < CARDRAM_SIZE) ?
                                   g_expansions[0].rom_size : CARDRAM_SIZE;
            memcpy(g_mcu->cardram, g_expansions[0].unscrambled, cardram_size);
            /* Don't copy to waverom_card (bank 2) - only use waverom_exp (banks 3-6) for SR-JV80 */
            /* size_t card_wave_size = (g_expansions[0].rom_size < 0x200000) ?
                                     g_expansions[0].rom_size : 0x200000;
            memcpy(g_mcu->pcm.waverom_card, g_expansions[0].unscrambled, card_wave_size); */
            memcpy(g_mcu->pcm.waverom_exp, g_expansions[0].unscrambled, g_expansions[0].rom_size);
            g_current_expansion = 0;
            fprintf(stderr, "JV880: Expansion pre-loaded to cardram + waverom_card + waverom_exp\n");

            /* Debug: dump first 256 bytes of cardram to see what data is there */
            fprintf(stderr, "JV880: cardram first 256 bytes:\n");
            for (int i = 0; i < 256; i += 16) {
                fprintf(stderr, "  %04X: ", i);
                for (int j = 0; j < 16; j++) {
                    fprintf(stderr, "%02X ", g_mcu->cardram[i + j]);
                }
                fprintf(stderr, "\n");
            }

            /* Reset MCU so firmware re-detects expansion card */
            g_mcu->SC55_Reset();
            fprintf(stderr, "JV880: MCU reset to detect expansion card\n");
        }
    }

    /* Ensure a known starting patch ("A. Piano 1" in Internal A) */
    if (g_total_patches > 0) {
        select_patch(0);
    }

    /* Warmup */
    fprintf(stderr, "JV880: Running warmup...\n");
    snprintf(g_loading_status, sizeof(g_loading_status), "Warming up...");
    for (int i = 0; i < 100000; i++) {
        g_mcu->updateSC55(1);
    }
    fprintf(stderr, "JV880: Warmup done\n");

    /* Sync g_tone_switch from MCU LED state (active LOW: bit=0 means ON) */
    if (g_mcu) {
        uint8_t tone_leds = g_mcu->led_tone_state;
        g_tone_switch[0] = ((tone_leds >> 0) & 1) == 0 ? 1 : 0;
        g_tone_switch[1] = ((tone_leds >> 1) & 1) == 0 ? 1 : 0;
        g_tone_switch[2] = ((tone_leds >> 2) & 1) == 0 ? 1 : 0;
        g_tone_switch[3] = ((tone_leds >> 3) & 1) == 0 ? 1 : 0;
        fprintf(stderr, "JV880: Tone switch synced from LED state: %d%d%d%d\n",
                g_tone_switch[0], g_tone_switch[1], g_tone_switch[2], g_tone_switch[3]);
    }

    /* Pre-fill audio buffer */
    g_ring_write = 0;
    g_ring_read = 0;

    float resample_acc = 0.0f;
    const float ratio = (float)JV880_SAMPLE_RATE / (float)MOVE_SAMPLE_RATE;

    fprintf(stderr, "JV880: Pre-filling buffer...\n");
    snprintf(g_loading_status, sizeof(g_loading_status), "Preparing audio...");
    for (int i = 0; i < 256; i++) {
        g_mcu->updateSC55(8);
        int avail = g_mcu->sample_write_ptr;
        for (int j = 0; j < avail; j += 2) {
            resample_acc += 1.0f;
            if (resample_acc >= ratio) {
                resample_acc -= ratio;
                g_audio_ring[g_ring_write * 2 + 0] = g_mcu->sample_buffer[j];
                g_audio_ring[g_ring_write * 2 + 1] = g_mcu->sample_buffer[j + 1];
                g_ring_write = (g_ring_write + 1) % AUDIO_RING_SIZE;
            }
        }
    }
    fprintf(stderr, "JV880: Buffer pre-filled: %d samples\n", g_ring_write);

    /* Start emulation thread */
    g_thread_running = 1;
    pthread_create(&g_emu_thread, NULL, emu_thread_func, NULL);

    /* Ensure the loading screen is visible at least briefly. */
    enforce_min_loading_time(&start_ts, 200);

    g_initialized = 1;
    g_loading_complete = 1;
    snprintf(g_loading_status, sizeof(g_loading_status),
             "Ready: %d patches in %d banks", g_total_patches, g_bank_count);

    fprintf(stderr, "JV880: Ready!\n");

    g_load_thread_running = 0;
    return NULL;
}

static void jv880_set_param(const char *key, const char *val) {
    if (strcmp(key, "octave_transpose") == 0) {
        g_octave_transpose = atoi(val);
        if (g_octave_transpose < -4) g_octave_transpose = -4;
        if (g_octave_transpose > 4) g_octave_transpose = 4;
        fprintf(stderr, "JV880: Octave transpose set to %d\n", g_octave_transpose);
    } else if (strcmp(key, "program_change") == 0 || strcmp(key, "preset") == 0) {
        int program = atoi(val);
        if (program < 0) program = 0;
        if (program >= g_total_patches) program = g_total_patches - 1;
        select_patch(program);
        fprintf(stderr, "JV880: Preset set to %d\n", program);
    } else if (strcmp(key, "next_bank") == 0) {
        jump_to_bank(1);
    } else if (strcmp(key, "prev_bank") == 0) {
        jump_to_bank(-1);
    } else if (strcmp(key, "mode") == 0) {
        /* Switch between patch (0) and performance (1) mode */
        int mode = atoi(val);
        set_mode(mode);
    } else if (strcmp(key, "performance") == 0) {
        /* Select performance 0-7 */
        int perf = atoi(val);
        if (perf < 0) perf = 0;
        if (perf >= NUM_PERFORMANCES) perf = NUM_PERFORMANCES - 1;
        select_performance(perf);
    } else if (strcmp(key, "part") == 0) {
        /* Select part 0-7 within performance */
        int part = atoi(val);
        if (part < 0) part = 0;
        if (part > 7) part = 7;
        select_part(part);
    } else if (strcmp(key, "led_test") == 0) {
        /* Fully automated LED reverse engineering test
         * Runs without any user interaction, writes summary to file */
        if (!g_mcu) return;

        FILE *f = fopen("/data/UserData/jv880_led_auto.txt", "w");
        if (!f) f = stderr;

        fprintf(f, "JV-880 Automated LED Test\n");
        fprintf(f, "=========================\n\n");

        /* Structure to capture state */
        struct Snapshot {
            uint8_t p7dr_raw[4];
            uint8_t io_sd;
            uint8_t dev_p7ddr;
            uint8_t dev_p7dr;
            char lcd0[32];
            char lcd1[32];
        };

        auto capture = [&](Snapshot* s) {
            memcpy(s->p7dr_raw, g_mcu->led_state_raw, 4);
            s->io_sd = g_mcu->io_sd;
            s->dev_p7ddr = g_mcu->dev_register[0x0C];
            s->dev_p7dr = g_mcu->dev_register[0x0E];
            strncpy(s->lcd0, g_mcu->lcd.GetLine(0), 31);
            strncpy(s->lcd1, g_mcu->lcd.GetLine(1), 31);
            s->lcd0[31] = s->lcd1[31] = 0;
        };

        auto print_snapshot = [&](const char* label, Snapshot* s) {
            fprintf(f, "%s:\n", label);
            fprintf(f, "  P7DR_raw: [%02X %02X %02X %02X]\n",
                    s->p7dr_raw[0], s->p7dr_raw[1], s->p7dr_raw[2], s->p7dr_raw[3]);
            fprintf(f, "  io_sd=%02X P7DDR=%02X P7DR=%02X\n",
                    s->io_sd, s->dev_p7ddr, s->dev_p7dr);
            fprintf(f, "  LCD: [%s] [%s]\n\n", s->lcd0, s->lcd1);
        };

        auto press_and_release = [&](int btn, int samples) {
            g_mcu->mcu_button_pressed |= (1 << btn);
            g_mcu->updateSC55(samples);
            g_mcu->mcu_button_pressed &= ~(1 << btn);
            g_mcu->updateSC55(samples);
        };

        Snapshot before, after;

        /* Test each button */
        const int buttons[] = {MCU_BUTTON_EDIT, MCU_BUTTON_SYSTEM, MCU_BUTTON_RHYTHM, MCU_BUTTON_UTILITY};
        const char* names[] = {"EDIT", "SYSTEM", "RHYTHM", "UTILITY"};

        /* Settle first */
        g_mcu->updateSC55(10000);

        for (int i = 0; i < 4; i++) {
            fprintf(f, "=== TEST: %s (button %d) ===\n", names[i], buttons[i]);

            /* Capture before */
            capture(&before);
            print_snapshot("BEFORE", &before);

            /* Press and release */
            press_and_release(buttons[i], 5000);

            /* Capture after */
            capture(&after);
            print_snapshot("AFTER", &after);

            /* Show what changed */
            fprintf(f, "CHANGES:\n");
            for (int j = 0; j < 4; j++) {
                if (before.p7dr_raw[j] != after.p7dr_raw[j]) {
                    fprintf(f, "  p7dr_raw[%d]: %02X -> %02X\n", j, before.p7dr_raw[j], after.p7dr_raw[j]);
                }
            }
            if (before.dev_p7dr != after.dev_p7dr) {
                fprintf(f, "  P7DR: %02X -> %02X\n", before.dev_p7dr, after.dev_p7dr);
            }
            if (strcmp(before.lcd0, after.lcd0) != 0) {
                fprintf(f, "  LCD changed\n");
            }
            fprintf(f, "\n");

            /* Return to base state - press again to toggle back */
            press_and_release(buttons[i], 5000);
            g_mcu->updateSC55(5000);
        }

        fprintf(f, "=== TEST COMPLETE ===\n");

        if (f != stderr) fclose(f);
        fprintf(stderr, "JV880: LED test complete -> /data/UserData/jv880_led_auto.txt\n");
    } else if (strcmp(key, "encoder") == 0) {
        /* Data entry dial/encoder - used by jog wheel */
        int dir = atoi(val);  /* 0 = increment (CW), 1 = decrement (CCW) */
        if (g_mcu) {
            g_mcu->MCU_EncoderTrigger(dir);
        }
    } else if (strcmp(key, "button_press") == 0) {
        /* Simulate MCU button press */
        int button = atoi(val);
        if (g_mcu && button >= 0 && button < 32) {
            g_mcu->mcu_button_pressed |= (1 << button);
            fprintf(stderr, "JV880: Button %d pressed\n", button);

            /* UI State Machine: process button events
             * JV-880 behavior per owner's manual:
             * - PATCH/PERFORM: TOGGLES between Patch and Performance each press
             * - RHYTHM: Enters Rhythm mode (idempotent)
             * - EDIT: TOGGLES between play and edit (this is the exit mechanism)
             * - UTILITY: Enters utility (idempotent)
             * - SYSTEM: Enters system (idempotent)
             * - Mode change returns to PLAY context
             */
            switch (button) {
                /* MODE BUTTONS */
                case MCU_BUTTON_PATCH_PERFORM:
                    /* Toggle between PATCH and PERFORM each press */
                    if (g_ui_mode == MODE_PATCH) {
                        g_ui_mode = MODE_PERFORM;
                        g_performance_mode = 1;  /* Update LED state tracking */
                    } else {
                        /* From PERFORM or RHYTHM, go to PATCH */
                        g_ui_mode = MODE_PATCH;
                        g_performance_mode = 0;  /* Update LED state tracking */
                    }
                    g_ui_context = CTX_PLAY;  /* Mode change returns to play */
                    break;

                case MCU_BUTTON_RHYTHM:
                    /* Enter RHYTHM mode (idempotent - no-op if already in RHYTHM) */
                    if (g_ui_mode != MODE_RHYTHM) {
                        g_ui_mode = MODE_RHYTHM;
                        g_ui_context = CTX_PLAY;  /* Mode change returns to play */
                    }
                    break;

                /* CONTEXT BUTTONS */
                case MCU_BUTTON_EDIT:
                    /* TOGGLE: Play ↔ Edit for current mode */
                    if (g_ui_context == CTX_EDIT) {
                        g_ui_context = CTX_PLAY;  /* Exit edit, return to play */
                    } else {
                        g_ui_context = CTX_EDIT;  /* Enter edit */
                    }
                    break;

                case MCU_BUTTON_SYSTEM:
                    /* TOGGLE: Play ↔ System (press again to exit) */
                    if (g_ui_context == CTX_SYSTEM) {
                        g_ui_context = CTX_PLAY;
                    } else {
                        g_ui_context = CTX_SYSTEM;
                    }
                    break;

                case MCU_BUTTON_UTILITY:
                    /* TOGGLE: Play ↔ Utility (press again to exit) */
                    if (g_ui_context == CTX_UTILITY) {
                        g_ui_context = CTX_PLAY;
                    } else {
                        g_ui_context = CTX_UTILITY;
                    }
                    break;

                /* TONE SWITCH BUTTONS (3-6 per schematic) - only toggle in Patch/Play mode
                 * These are physical buttons that have different functions per mode:
                 * - Patch Play: Tone Switch 1-4 (mute individual tones)
                 * - Perform: Part select
                 * We only update g_tone_switch[] in Patch/Play mode.
                 */
                case MCU_BUTTON_TONE_SW1:  /* button 3 */
                case MCU_BUTTON_TONE_SW2:  /* button 4 */
                case MCU_BUTTON_TONE_SW3:  /* button 5 */
                case MCU_BUTTON_TONE_SW4:  /* button 6 */
                    if (g_ui_mode == MODE_PATCH && g_ui_context == CTX_PLAY) {
                        int tone_idx = button - MCU_BUTTON_TONE_SW1;  /* 0-3 */
                        g_tone_switch[tone_idx] = !g_tone_switch[tone_idx];
                        fprintf(stderr, "JV880: Tone %d = %s\n", tone_idx + 1,
                                g_tone_switch[tone_idx] ? "ON" : "OFF");
                    }
                    /* In other modes/contexts, button does something else (handled by MCU) */
                    break;
            }
        }
    } else if (strcmp(key, "button_release") == 0) {
        /* Release MCU button */
        int button = atoi(val);
        if (g_mcu && button >= 0 && button < 32) {
            g_mcu->mcu_button_pressed &= ~(1 << button);

            /* Log state AFTER release (MCU has had time to process) */
            FILE *logf = fopen("/data/UserData/jv880_debug.log", "a");
            if (logf) {
                const char* btn_names[] = {
                    "CURSOR_L", "CURSOR_R", "TONE_SELECT", "MUTE",
                    "TONE_SW1", "TONE_SW2", "TONE_SW3", "TONE_SW4",
                    "UTILITY", "PREVIEW", "PATCH_PERF", "EDIT",
                    "SYSTEM", "RHYTHM", "DATA_INC", "DATA_DEC"
                };
                const char* btn_name = (button < 16) ? btn_names[button] : "UNKNOWN";
                fprintf(logf, "--- AFTER %s RELEASE ---\n", btn_name);
                fprintf(logf, "LCD0: [%s]\n", g_mcu->lcd.GetLine(0));
                fprintf(logf, "LCD1: [%s]\n", g_mcu->lcd.GetLine(1));
                /* Dump more memory to find mode state */
                fprintf(logf, "SRAM[0x00-0x3F]: ");
                for (int i = 0; i < 64; i++) fprintf(logf, "%02X ", g_mcu->sram[i]);
                fprintf(logf, "\n");
                fprintf(logf, "SRAM[0x40-0x7F]: ");
                for (int i = 64; i < 128; i++) fprintf(logf, "%02X ", g_mcu->sram[i]);
                fprintf(logf, "\n");
                fprintf(logf, "SRAM[0x80-0xBF]: ");
                for (int i = 128; i < 192; i++) fprintf(logf, "%02X ", g_mcu->sram[i]);
                fprintf(logf, "\n");
                fprintf(logf, "SRAM[0xC0-0xFF]: ");
                for (int i = 192; i < 256; i++) fprintf(logf, "%02X ", g_mcu->sram[i]);
                fprintf(logf, "\n");
                fprintf(logf, "RAM[0x00-0x3F]: ");
                for (int i = 0; i < 64; i++) fprintf(logf, "%02X ", g_mcu->ram[i]);
                fprintf(logf, "\n");
                fprintf(logf, "RAM[0x40-0x7F]: ");
                for (int i = 64; i < 128; i++) fprintf(logf, "%02X ", g_mcu->ram[i]);
                fprintf(logf, "\n");
                fprintf(logf, "RAM[0x80-0xBF]: ");
                for (int i = 128; i < 192; i++) fprintf(logf, "%02X ", g_mcu->ram[i]);
                fprintf(logf, "\n");
                fprintf(logf, "RAM[0xC0-0xFF]: ");
                for (int i = 192; i < 256; i++) fprintf(logf, "%02X ", g_mcu->ram[i]);
                fprintf(logf, "\n\n");
                fclose(logf);
            }
        }
    } else if (strcmp(key, "tone_switch") == 0) {
        /* Toggle tone switch 1-4 (patch mode only)
         * val = "1", "2", "3", or "4" */
        int tone = atoi(val);
        if (tone >= 1 && tone <= 4) {
            g_tone_switch[tone - 1] = !g_tone_switch[tone - 1];
            /* Also send to MCU - press TONE_SELECT then the number */
            if (g_mcu) {
                /* The JV-880 uses TONE_SELECT button for this
                 * For now just track state; MCU integration TBD */
            }
            fprintf(stderr, "JV880: Tone %d = %s\n", tone,
                    g_tone_switch[tone - 1] ? "ON" : "OFF");
        }
    } else if (strcmp(key, "exit_context") == 0) {
        /* Exit current context to play mode (for UI convenience) */
        g_ui_context = CTX_PLAY;
    } else if (strcmp(key, "next_expansion") == 0) {
        /* Cycle to next expansion (or back to internal-only)
         * g_current_expansion: -1 = internal only, 0+ = expansion index */
        if (g_expansion_count > 0) {
            int next_exp = g_current_expansion + 1;
            if (next_exp >= g_expansion_count) {
                next_exp = -1;  /* Wrap back to internal */
            }
            if (next_exp < 0) {
                /* Clear expansion and reset to internal only */
                g_current_expansion = -1;
                send_all_notes_off();
                memset(g_mcu->pcm.waverom_exp, 0, EXPANSION_SIZE_8MB);
                memset(g_mcu->cardram, 0, CARDRAM_SIZE);
                g_mcu->SC55_Reset();
                fprintf(stderr, "JV880: Switched to internal patches only\n");
            } else {
                /* load_expansion_to_emulator sets g_current_expansion */
                load_expansion_to_emulator(next_exp);
                fprintf(stderr, "JV880: Switched to expansion: %s\n",
                        g_expansions[g_current_expansion].name);
            }
        }
    }
    /* Note: tempo and clock_mode are now host settings */
}

static int jv880_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "buffer_fill") == 0) {
        snprintf(buf, buf_len, "%d", ring_available());
        return 1;
    }
    if (strcmp(key, "lcd_line0") == 0 && g_mcu) {
        snprintf(buf, buf_len, "%s", g_mcu->lcd.GetLine(0));
        return 1;
    }
    if (strcmp(key, "lcd_line1") == 0 && g_mcu) {
        snprintf(buf, buf_len, "%s", g_mcu->lcd.GetLine(1));
        return 1;
    }
    if (strcmp(key, "lcd_cursor") == 0 && g_mcu) {
        /* Returns "visible,row,col" e.g. "1,0,5" for cursor on row 0, col 5 */
        int visible = g_mcu->lcd.IsCursorVisible() ? 1 : 0;
        int row = g_mcu->lcd.GetCursorRow();
        int col = g_mcu->lcd.GetCursorCol();
        snprintf(buf, buf_len, "%d,%d,%d", visible, row, col);
        return 1;
    }
    if (strcmp(key, "total_patches") == 0) {
        snprintf(buf, buf_len, "%d", g_total_patches);
        return 1;
    }
    if (strcmp(key, "current_patch") == 0) {
        snprintf(buf, buf_len, "%d", g_current_patch);
        return 1;
    }
    if (strcmp(key, "patch_name") == 0 || strcmp(key, "preset_name") == 0) {
        /* In performance mode, return the performance name from LCD line 0
         * which shows something like "PA:01 Perf Name" or "INT:01 Perf Name"
         */
        if (g_performance_mode && g_mcu) {
            const char* lcd_line = g_mcu->lcd.GetLine(0);
            /* Skip bank prefix (e.g., "PA:01 " or "INT:01 ") - find first space after colon */
            const char* name_start = lcd_line;
            const char* colon = strchr(lcd_line, ':');
            if (colon) {
                const char* space = strchr(colon, ' ');
                if (space) {
                    name_start = space + 1;
                }
            }
            /* Trim trailing spaces */
            int len = strlen(name_start);
            while (len > 0 && name_start[len - 1] == ' ') len--;
            if (len > 0 && len < buf_len) {
                memcpy(buf, name_start, len);
                buf[len] = '\0';
            } else {
                snprintf(buf, buf_len, "---");
            }
        } else if (g_current_patch >= 0 && g_current_patch < g_total_patches) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].name);
        } else {
            snprintf(buf, buf_len, "---");
        }
        return 1;
    }
    if (strcmp(key, "performance_name") == 0) {
        /* Get performance name from LCD line 0 */
        if (g_mcu) {
            const char* lcd_line = g_mcu->lcd.GetLine(0);
            snprintf(buf, buf_len, "%s", lcd_line);
        } else {
            snprintf(buf, buf_len, "---");
        }
        return 1;
    }
    if (strcmp(key, "octave_transpose") == 0) {
        snprintf(buf, buf_len, "%d", g_octave_transpose);
        return 1;
    }
    if (strcmp(key, "bank_name") == 0) {
        snprintf(buf, buf_len, "%s", get_current_bank_name());
        return 1;
    }
    if (strcmp(key, "loading_status") == 0) {
        snprintf(buf, buf_len, "%s", g_loading_status);
        return 1;
    }
    if (strcmp(key, "loading_complete") == 0) {
        snprintf(buf, buf_len, "%d", g_loading_complete);
        return 1;
    }
    if (strcmp(key, "bank_count") == 0) {
        snprintf(buf, buf_len, "%d", g_bank_count);
        return 1;
    }
    if (strcmp(key, "mode") == 0) {
        snprintf(buf, buf_len, "%d", g_performance_mode);
        return 1;
    }
    if (strcmp(key, "performance_mode") == 0) {
        snprintf(buf, buf_len, "%d", g_performance_mode);
        return 1;
    }
    if (strcmp(key, "current_performance") == 0) {
        snprintf(buf, buf_len, "%d", g_current_performance);
        return 1;
    }
    if (strcmp(key, "current_part") == 0) {
        snprintf(buf, buf_len, "%d", g_current_part);
        return 1;
    }
    if (strcmp(key, "num_performances") == 0) {
        snprintf(buf, buf_len, "%d", NUM_PERFORMANCES);
        return 1;
    }
    if (strcmp(key, "num_parts") == 0) {
        snprintf(buf, buf_len, "8");
        return 1;
    }
    if (strcmp(key, "perf_bank") == 0) {
        snprintf(buf, buf_len, "%d", g_perf_bank);
        return 1;
    }
    if (strcmp(key, "expansion_name") == 0) {
        if (g_current_expansion >= 0 && g_current_expansion < g_expansion_count) {
            snprintf(buf, buf_len, "%s", g_expansions[g_current_expansion].name);
        } else {
            snprintf(buf, buf_len, "Internal");
        }
        return 1;
    }
    if (strcmp(key, "expansion_count") == 0) {
        snprintf(buf, buf_len, "%d", g_expansion_count);
        return 1;
    }
    if (strcmp(key, "perf_bank_name") == 0) {
        const char* names[] = { "Preset A", "Preset B", "Internal" };
        snprintf(buf, buf_len, "%s", names[g_perf_bank % 3]);
        return 1;
    }
    if (strcmp(key, "perfs_per_bank") == 0) {
        snprintf(buf, buf_len, "%d", PERFS_PER_BANK);
        return 1;
    }
    if (strcmp(key, "led_state") == 0) {
        /* Return LED state as comma-separated values:
         * edit,system,rhythm,utility,patch,unused,tone1,tone2,tone3,tone4
         * Each is 0 or 1
         *
         * LED state decoded from P7DR matrix writes per switch board schematic:
         * LED columns (from CN102): LED0=PATCH, LED1=EDIT, LED2=SYSTEM, LED3=RHYTHM, LED4=UTILITY
         * - led_mode_state: single-bit value indicating which mode LED is ON
         *   bit 0 (0x01)=PATCH, bit 1 (0x02)=EDIT, bit 2 (0x04)=SYSTEM, bit 3 (0x08)=RHYTHM, bit 4 (0x10)=UTILITY
         * - led_tone_state: bit per tone (active LOW - bit=0 means ON)
         *   LED0=TONE1, LED1=TONE2, LED2=TONE3, LED3=TONE4
         */
        int patch = 0, edit = 0, system = 0, rhythm = 0, utility = 0;
        int tone1 = 0, tone2 = 0, tone3 = 0, tone4 = 0;

        if (g_mcu) {
            uint8_t mode_leds = g_mcu->led_mode_state;
            uint8_t tone_leds = g_mcu->led_tone_state;
            /* Decode mode LEDs - check individual bits (may be active LOW) */
            patch   = (mode_leds & 0x01) ? 1 : 0;  /* LED0 */
            edit    = (mode_leds & 0x02) ? 1 : 0;  /* LED1 */
            system  = (mode_leds & 0x04) ? 1 : 0;  /* LED2 */
            rhythm  = (mode_leds & 0x08) ? 1 : 0;  /* LED3 */
            utility = (mode_leds & 0x10) ? 1 : 0;  /* LED4 */
            /* Decode tone LEDs - active LOW, per schematic */
            tone1   = ((tone_leds >> 0) & 1) == 0 ? 1 : 0;  /* LED0 */
            tone2   = ((tone_leds >> 1) & 1) == 0 ? 1 : 0;  /* LED1 */
            tone3   = ((tone_leds >> 2) & 1) == 0 ? 1 : 0;  /* LED2 */
            tone4   = ((tone_leds >> 3) & 1) == 0 ? 1 : 0;  /* LED3 */
        }

        /* PATCH/PERFORM share LED0 - use g_performance_mode to distinguish
         * Per JV-880 manual: LED lights when in PATCH mode */
        int patch_led = patch && !g_performance_mode ? 1 : 0;

        /* Step 5 is the PATCH/PERFORM LED - lights when in PATCH mode per JV-880 manual */
        snprintf(buf, buf_len, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                 edit, system, rhythm, utility,
                 patch_led, 0,  /* Step 5 = PATCH/PERFORM LED (lit in PATCH mode), step 6 unused */
                 tone1, tone2, tone3, tone4);
        return 1;
    }
    if (strcmp(key, "led_debug") == 0) {
        /* Debug: show LED state from P7DR matrix writes
         * mode_state: bits for PATCH,EDIT,SYS,RHYTHM,UTIL (active LOW)
         * tone_state: bits for TONE1-4 (active LOW)
         * raw[0-1]: captured P7DR values for rows 0,1 */
        if (g_mcu) {
            snprintf(buf, buf_len, "mode=%02X tone=%02X raw=[%02X %02X]",
                     g_mcu->led_mode_state, g_mcu->led_tone_state,
                     g_mcu->led_state_raw[0], g_mcu->led_state_raw[1]);
        } else {
            snprintf(buf, buf_len, "no mcu");
        }
        return 1;
    }
    if (strcmp(key, "tone_sw_state") == 0) {
        /* Debug: show plugin's g_tone_switch state (1=ON, 0=OFF for each tone) */
        snprintf(buf, buf_len, "%d%d%d%d",
                 g_tone_switch[0], g_tone_switch[1], g_tone_switch[2], g_tone_switch[3]);
        return 1;
    }
    if (strcmp(key, "lcd_debug") == 0) {
        /* Debug: return raw LCD content for troubleshooting */
        if (g_mcu) {
            const char* line0 = g_mcu->lcd.GetLine(0);
            const char* line1 = g_mcu->lcd.GetLine(1);
            uint8_t nvram_mode = g_mcu->nvram[NVRAM_MODE_OFFSET];
            snprintf(buf, buf_len, "L0:[%s] L1:[%s] NV:%d", line0, line1, nvram_mode);
        } else {
            snprintf(buf, buf_len, "no mcu");
        }
        return 1;
    }
    if (strcmp(key, "nvram_dump") == 0) {
        /* Debug: dump NVRAM bytes 0x00-0x3F to find mode/context state */
        if (g_mcu) {
            char *p = buf;
            int remaining = buf_len;
            for (int i = 0; i < 64 && remaining > 3; i++) {
                int written = snprintf(p, remaining, "%02X ", g_mcu->nvram[i]);
                p += written;
                remaining -= written;
            }
        } else {
            snprintf(buf, buf_len, "no mcu");
        }
        return 1;
    }
    if (strcmp(key, "ram_dump") == 0) {
        /* Debug: dump RAM bytes 0x00-0x3F to find mode/context state */
        if (g_mcu) {
            char *p = buf;
            int remaining = buf_len;
            for (int i = 0; i < 64 && remaining > 3; i++) {
                int written = snprintf(p, remaining, "%02X ", g_mcu->ram[i]);
                p += written;
                remaining -= written;
            }
        } else {
            snprintf(buf, buf_len, "no mcu");
        }
        return 1;
    }
    /* Note: tempo and clock_mode are now host settings */
    return 0;
}

/* Output gain (reduce to prevent clipping) */
#define OUTPUT_GAIN_SHIFT 2  /* Divide by 4 (-12dB) */

static void jv880_render_block(int16_t *out, int frames) {
    if (!g_initialized || !g_thread_running) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Automated LED test disabled - was interfering with normal operation
     * Can be triggered manually via set_param("led_test", "1") */
    g_render_count++;

    pthread_mutex_lock(&g_ring_mutex);
    int avail = ring_available();
    int to_read = (avail < frames) ? avail : frames;

    for (int i = 0; i < to_read; i++) {
        out[i * 2 + 0] = g_audio_ring[g_ring_read * 2 + 0] >> OUTPUT_GAIN_SHIFT;
        out[i * 2 + 1] = g_audio_ring[g_ring_read * 2 + 1] >> OUTPUT_GAIN_SHIFT;
        g_ring_read = (g_ring_read + 1) % AUDIO_RING_SIZE;
    }
    pthread_mutex_unlock(&g_ring_mutex);

    /* Pad with silence if underrun */
    for (int i = to_read; i < frames; i++) {
        out[i * 2 + 0] = 0;
        out[i * 2 + 1] = 0;
    }
}

static plugin_api_v1_t jv880_api = {
    1,
    jv880_on_load,
    jv880_on_unload,
    jv880_on_midi,
    jv880_set_param,
    jv880_get_param,
    jv880_render_block
};

extern "C" plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    (void)host;
    return &jv880_api;
}
