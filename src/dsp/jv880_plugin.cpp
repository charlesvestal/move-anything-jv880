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

/* Keep ROM2 for internal patch data access */
static uint8_t *g_rom2 = nullptr;

/* Patch data constants */
#define PATCH_SIZE 0x16a  /* 362 bytes per patch */
#define PATCH_NAME_LEN 12
#define PATCH_OFFSET_INTERNAL_A 0x010ce0
#define PATCH_OFFSET_INTERNAL_B 0x018ce0
#define NVRAM_PATCH_OFFSET      0x0d70
#define NVRAM_MODE_OFFSET       0x11

/* Expansion ROM support */
#define EXPANSION_SIZE 0x800000  /* 8MB */
#define MAX_EXPANSIONS 32
#define MAX_PATCHES_PER_EXP 256

typedef struct {
    char filename[256];
    char name[64];          /* Short name like "01 Pop" */
    int patch_count;
    uint32_t patches_offset;
    int first_global_index; /* First patch index in unified list */
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

/* Background emulation thread */
static pthread_t g_emu_thread;
static volatile int g_thread_running = 0;

/* Audio ring buffer (44.1kHz stereo output) */
#define AUDIO_RING_SIZE 2048
static int16_t g_audio_ring[AUDIO_RING_SIZE * 2];
static volatile int g_ring_write = 0;
static volatile int g_ring_read = 0;
static pthread_mutex_t g_ring_mutex = PTHREAD_MUTEX_INITIALIZER;

/* MIDI queue */
#define MIDI_QUEUE_SIZE 256
#define MIDI_MSG_MAX_LEN 32
static uint8_t g_midi_queue[MIDI_QUEUE_SIZE][MIDI_MSG_MAX_LEN];
static int g_midi_queue_len[MIDI_QUEUE_SIZE];
static volatile int g_midi_write = 0;
static volatile int g_midi_read = 0;

/* Octave transpose */
static int g_octave_transpose = 0;

/* Sample rates */
#define JV880_SAMPLE_RATE 66207
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

/* Load and parse expansion ROM to get patch info (doesn't keep it loaded) */
static int scan_expansion_rom(const char *filename, ExpansionInfo *info) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/expansions/%s", g_module_dir, filename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Only support 8MB ROMs for now */
    if (size != EXPANSION_SIZE) {
        fprintf(stderr, "JV880: Skipping %s (size %ld, expected %d)\n", filename, size, EXPANSION_SIZE);
        fclose(f);
        return 0;
    }

    /* Read and unscramble just enough to get patch info */
    uint8_t *scrambled = (uint8_t *)malloc(EXPANSION_SIZE);
    uint8_t *unscrambled = (uint8_t *)malloc(EXPANSION_SIZE);
    if (!scrambled || !unscrambled) {
        free(scrambled);
        free(unscrambled);
        fclose(f);
        return 0;
    }

    fread(scrambled, 1, EXPANSION_SIZE, f);
    fclose(f);

    unscramble_rom(scrambled, unscrambled, EXPANSION_SIZE);
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

    /* Store info */
    strncpy(info->filename, filename, sizeof(info->filename) - 1);
    extract_expansion_name(filename, info->name, sizeof(info->name));
    info->patch_count = patch_count;
    info->patches_offset = patches_offset;
    info->unscrambled = unscrambled;  /* Keep unscrambled data */

    fprintf(stderr, "JV880: Scanned %s: %d patches at offset 0x%x\n",
            info->name, patch_count, patches_offset);

    return 1;
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

    fprintf(stderr, "JV880: Found %d expansion ROMs\n", g_expansion_count);
}

/* Build unified patch list */
static void build_patch_list(void) {
    g_total_patches = 0;

    /* Internal Bank A (0-63) */
    for (int i = 0; i < 64 && g_total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &g_patches[g_total_patches];
        uint32_t offset = PATCH_OFFSET_INTERNAL_A + (i * PATCH_SIZE);

        /* Copy patch name from ROM2 */
        memcpy(p->name, &g_rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';

        p->expansion_index = -1;  /* Internal */
        p->local_patch_index = i;
        p->rom_offset = offset;
        g_total_patches++;
    }

    /* Internal Bank B (64-127) */
    for (int i = 0; i < 64 && g_total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &g_patches[g_total_patches];
        uint32_t offset = PATCH_OFFSET_INTERNAL_B + (i * PATCH_SIZE);

        memcpy(p->name, &g_rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';

        p->expansion_index = -1;
        p->local_patch_index = 64 + i;
        p->rom_offset = offset;
        g_total_patches++;
    }

    /* Expansion patches */
    for (int e = 0; e < g_expansion_count; e++) {
        ExpansionInfo *exp = &g_expansions[e];
        exp->first_global_index = g_total_patches;

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

    fprintf(stderr, "JV880: Total patches: %d (128 internal + %d expansion)\n",
            g_total_patches, g_total_patches - 128);
}

/* Load expansion ROM into emulator's waverom_exp */
static void load_expansion_to_emulator(int exp_index) {
    if (exp_index < 0 || exp_index >= g_expansion_count) return;
    if (exp_index == g_current_expansion) return;  /* Already loaded */

    ExpansionInfo *exp = &g_expansions[exp_index];

    if (exp->unscrambled) {
        memcpy(g_mcu->pcm.waverom_exp, exp->unscrambled, EXPANSION_SIZE);
        g_current_expansion = exp_index;
        fprintf(stderr, "JV880: Loaded expansion %s to emulator\n", exp->name);
    }
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

    /* Scan for expansion ROMs and build patch list */
    scan_expansions();
    build_patch_list();

    /* Warmup */
    fprintf(stderr, "JV880: Running warmup...\n");
    for (int i = 0; i < 100000; i++) {
        g_mcu->updateSC55(1);
    }
    fprintf(stderr, "JV880: Warmup done\n");

    /* Pre-fill audio buffer */
    g_ring_write = 0;
    g_ring_read = 0;

    float resample_acc = 0.0f;
    const float ratio = (float)JV880_SAMPLE_RATE / (float)MOVE_SAMPLE_RATE;

    fprintf(stderr, "JV880: Pre-filling buffer...\n");
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

    g_initialized = 1;
    fprintf(stderr, "JV880: Ready!\n");
    return 0;
}

static void jv880_on_unload(void) {
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
    (void)source;

    if (!g_initialized || !g_thread_running) return;
    if (len < 1) return;

    uint8_t status = msg[0] & 0xF0;

    /* Filter capacitive touch from knobs (notes 0-9) */
    if ((status == 0x90 || status == 0x80) && len >= 2) {
        if (msg[1] < 10) return;
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

static void jv880_set_param(const char *key, const char *val) {
    if (strcmp(key, "octave_transpose") == 0) {
        g_octave_transpose = atoi(val);
        if (g_octave_transpose < -4) g_octave_transpose = -4;
        if (g_octave_transpose > 4) g_octave_transpose = 4;
        fprintf(stderr, "JV880: Octave transpose set to %d\n", g_octave_transpose);
    } else if (strcmp(key, "program_change") == 0) {
        int program = atoi(val);
        if (program < 0) program = 0;
        if (program >= g_total_patches) program = g_total_patches - 1;
        select_patch(program);
    }
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
    if (strcmp(key, "total_patches") == 0) {
        snprintf(buf, buf_len, "%d", g_total_patches);
        return 1;
    }
    if (strcmp(key, "current_patch") == 0) {
        snprintf(buf, buf_len, "%d", g_current_patch);
        return 1;
    }
    if (strcmp(key, "patch_name") == 0) {
        if (g_current_patch >= 0 && g_current_patch < g_total_patches) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].name);
        } else {
            snprintf(buf, buf_len, "---");
        }
        return 1;
    }
    if (strcmp(key, "bank_name") == 0) {
        snprintf(buf, buf_len, "%s", get_current_bank_name());
        return 1;
    }
    return 0;
}

/* Output gain (reduce to prevent clipping) */
#define OUTPUT_GAIN_SHIFT 2  /* Divide by 4 (-12dB) */

static void jv880_render_block(int16_t *out, int frames) {
    if (!g_initialized || !g_thread_running) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

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
