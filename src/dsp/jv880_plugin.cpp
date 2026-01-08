/*
 * JV-880 Plugin for Move Anything
 * Based on mini-jv880 emulator by giulioz (based on Nuked-SC55 by nukeykt)
 * Single-threaded approach - simpler and more reliable
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

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

/* Keep ROM2 for patch data access */
static uint8_t *g_rom2 = nullptr;

/* Patch data offsets in ROM2 (from jv880_juce) */
#define PATCH_SIZE 0x16a  /* 362 bytes per patch */
#define PATCH_OFFSET_INTERNAL_A 0x010ce0  /* Patches 0-63 */
#define PATCH_OFFSET_INTERNAL_B 0x018ce0  /* Patches 64-127 */
#define PATCH_OFFSET_USER       0x008ce0  /* Patches 128-191 */
#define NVRAM_PATCH_OFFSET      0x0d70    /* Where to copy patch in NVRAM */
#define NVRAM_MODE_OFFSET       0x11      /* 1=patch mode, 0=drum mode */

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

/* Load expansion ROM (SR-JV80 card) */
#define EXPANSION_SIZE 0x800000  /* 8MB */
static int load_expansion_rom(const char *filename) {
    if (!g_mcu) return 0;

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/%s", g_module_dir, filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "JV880: Expansion ROM not found: %s\n", path);
        return 0;
    }

    uint8_t *scrambled = (uint8_t *)malloc(EXPANSION_SIZE);
    if (!scrambled) {
        fclose(f);
        return 0;
    }

    size_t got = fread(scrambled, 1, EXPANSION_SIZE, f);
    fclose(f);

    if (got != EXPANSION_SIZE) {
        fprintf(stderr, "JV880: Expansion ROM size mismatch: %zu vs %d\n", got, EXPANSION_SIZE);
        free(scrambled);
        return 0;
    }

    fprintf(stderr, "JV880: Unscrambling expansion ROM...\n");
    unscramble_rom(scrambled, g_mcu->pcm.waverom_exp, EXPANSION_SIZE);
    free(scrambled);

    fprintf(stderr, "JV880: Loaded expansion ROM: %s\n", filename);
    return 1;
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
            /* Ring buffer nearly full, wait a bit */
            usleep(50);
            continue;
        }

        /* Generate some samples using the integrated update function */
        /* updateSC55 generates samples into sample_buffer */
        g_mcu->updateSC55(64);  /* Generate ~64 samples at JV880 rate */

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

    /* Keep ROM2 for patch data access (don't free it) */
    g_rom2 = rom2;

    free(rom1); free(waverom1); free(waverom2); free(nvram);

    g_rom_loaded = 1;

    /* Set patch mode in NVRAM and load initial patch */
    g_mcu->nvram[NVRAM_MODE_OFFSET] = 1;  /* Patch mode */

    /* Try to load expansion ROM (SR-JV80 card) */
    /* Name it jv880_expansion.bin in the roms folder */
    load_expansion_rom("jv880_expansion.bin");

    /* Warmup - run emulator for a bit to initialize */
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
    for (int i = 0; i < 256; i++) {  /* Minimal pre-fill for low latency */
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
        if (msg[1] < 10) return;  /* Ignore knob touch notes */
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

/*
 * Load patch data from ROM2 to NVRAM (based on jv880_juce approach)
 * This bypasses the firmware's patch selection which doesn't work for all patches
 */
static void load_patch_to_nvram(int program) {
    if (!g_mcu || !g_rom2) return;

    /* Determine which bank and calculate ROM offset */
    uint32_t rom_offset;
    if (program < 64) {
        /* Internal Bank A */
        rom_offset = PATCH_OFFSET_INTERNAL_A + (program * PATCH_SIZE);
    } else if (program < 128) {
        /* Internal Bank B */
        rom_offset = PATCH_OFFSET_INTERNAL_B + ((program - 64) * PATCH_SIZE);
    } else {
        /* User patches (128-191) - not typically used via UI */
        rom_offset = PATCH_OFFSET_USER + ((program - 128) * PATCH_SIZE);
    }

    /* Set patch mode in NVRAM */
    g_mcu->nvram[NVRAM_MODE_OFFSET] = 1;

    /* Copy patch data from ROM2 to NVRAM */
    memcpy(&g_mcu->nvram[NVRAM_PATCH_OFFSET], &g_rom2[rom_offset], PATCH_SIZE);

    fprintf(stderr, "JV880: Loaded patch %d from ROM2 offset 0x%x to NVRAM\n",
            program, rom_offset);
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
        if (program > 127) program = 127;

        /* Load patch data from ROM2 to NVRAM */
        load_patch_to_nvram(program);

        /* Send program change MIDI to trigger the emulator to use it */
        uint8_t pc_msg[2] = { 0xC0, 0x00 };  /* Always send 0 - patch is already in NVRAM */
        int next = (g_midi_write + 1) % MIDI_QUEUE_SIZE;
        if (next != g_midi_read) {
            memcpy(g_midi_queue[g_midi_write], pc_msg, 2);
            g_midi_queue_len[g_midi_write] = 2;
            g_midi_write = next;
        }
        fprintf(stderr, "JV880: Program change to %d\n", program);
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
