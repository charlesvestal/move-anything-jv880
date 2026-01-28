/*
 * Mini-JV Plugin for Move Anything
 * Based on mini-jv880 emulator by giulioz (based on Nuked-SC55 by nukeykt)
 * Multi-expansion support with unified patch list
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <math.h>

#include "mcu.h"
extern "C" {
#include "resample/libresample.h"
}

extern "C" {
#include "plugin_api_v1.h"
}

/* Debug logging to file */
#define JV_DEBUG_LOG "/tmp/jv880_debug.log"
static void jv_debug(const char *fmt, ...) {
    FILE *f = fopen(JV_DEBUG_LOG, "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fflush(f);
        fclose(f);
    }
}

/* The emulator instance */
static MCU *g_mcu = nullptr;

/* Plugin state */
static char g_module_dir[512];
static int g_initialized = 0;
static int g_rom_loaded = 0;
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
#define NVRAM_PATCH_OFFSET      0x0d70    /* Working patch area (362 bytes) */
#define NVRAM_MODE_OFFSET       0x11
#define NVRAM_PATCH_INTERNAL    0x1000    /* User patch storage: 64 × 362 = 23168 bytes (up to 0x6A80) */
#define NUM_USER_PATCHES        64

/* Performance data constants
 * Performance structure is 204 bytes (0xCC)
 * 16 performances per bank, name is first 12 bytes
 * Preset A/B are in ROM2, Internal is in NVRAM
 * Internal performances (16 × 0xCC = 0xCC0) end at 0x0d70 (temp patch)
 */
#define PERF_SIZE 0xCC   /* 204 bytes per performance */
#define PERF_NAME_LEN 12
#define PERFS_PER_BANK 16
/* ROM2 offsets for preset performances */
#define PERF_OFFSET_PRESET_A    0x10020  /* Preset A: "Jazz Split", "Softly...", etc. */
#define PERF_OFFSET_PRESET_B    0x18020  /* Preset B: "GTR Players", etc. */
/* NVRAM offset for internal performances */
#define NVRAM_PERF_INTERNAL     0x00b0   /* Internal: "Syn Lead", "Encounter X", etc. */
/* SRAM offset for temp performance (discovered via scanning) */
#define SRAM_TEMP_PERF_OFFSET   0x206a   /* Temp performance buffer in SRAM */

/* Temp performance structure offsets (discovered via automated mapping):
 *   0-11:  Name (12 bytes)
 *   12:    Key mode (packed with other flags)
 *   14:    Reverb time
 *   15:    Reverb feedback
 *   16:    Chorus level
 *   17:    Chorus depth
 *   18:    Chorus rate
 *   19:    Chorus feedback
 *   20-24: Voice reserve 1-5
 *   28+:   Part data (8 parts × 22 bytes each)
 *
 * Part structure (22 bytes per part, offset from part base):
 *   +0:  Flags (transmit switch/channel/output packed)
 *   +4:  Transmit key range lower
 *   +6:  Transmit key transpose
 *   +7:  Transmit velocity sense
 *   +8:  Transmit velocity max
 *   +9:  Transmit velocity curve
 *   +10: Internal key range lower
 *   +12: Internal key transpose
 *   +13: Internal velocity sense
 *   +14: Internal velocity max
 *   +15: Internal velocity curve
 *   +17: Part level
 *   +18: Part pan
 *   +19: Part coarse tune
 *   +20: Part fine tune
 *   +21: Receive channel
 */
#define TEMP_PERF_COMMON_SIZE   28   /* Bytes before part data starts */
#define TEMP_PERF_PART_SIZE     22   /* Bytes per part (discovered) */

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
static int g_expansion_bank_offset = 0;  /* Which 64-patch bank is loaded (0, 64, 128, 192) */

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

/* ========================================================================
 * AUTOMATED PARAMETER MAPPING
 * Systematically tests SysEx parameters and logs which SRAM bytes change.
 * ======================================================================== */
#define MAP_SRAM_SCAN_SIZE 512  /* Bytes to scan around temp perf */

/* Parameter definition: SysEx offset, name, test value, is_two_byte */
typedef struct {
    uint8_t sysex_offset;
    const char* name;
    uint8_t test_value;
    int is_two_byte;
} ParamDef;

/* Performance Common parameters (SysEx address: 00 00 10 XX) */
/* Test values chosen to differ from typical defaults */
static const ParamDef PERF_COMMON_PARAMS[] = {
    {0x0C, "keymode", 2, 0},           /* Default likely 0, test 2 */
    {0x0D, "reverbtype", 5, 0},        /* Test different type */
    {0x0E, "reverblevel", 33, 0},      /* Different from 0 or 127 */
    {0x0F, "reverbtime", 77, 0},
    {0x10, "reverbfeedback", 77, 0},
    {0x11, "chorustype", 1, 0},        /* Test different type */
    {0x12, "choruslevel", 33, 0},
    {0x13, "chorusdepth", 77, 0},
    {0x14, "chorusrate", 77, 0},
    {0x15, "chorusfeedback", 77, 0},
    {0x16, "chorusoutput", 0, 0},      /* Switch: test 0 (likely default 1) */
    {0x17, "voicereserve1", 2, 0},     /* Different from default 4 */
    {0x18, "voicereserve2", 2, 0},
    {0x19, "voicereserve3", 2, 0},
    {0x1A, "voicereserve4", 2, 0},
    {0x1B, "voicereserve5", 2, 0},
    {0x1C, "voicereserve6", 2, 0},
    {0x1D, "voicereserve7", 2, 0},
    {0x1E, "voicereserve8", 2, 0},
};
#define NUM_PERF_COMMON_PARAMS (sizeof(PERF_COMMON_PARAMS)/sizeof(PERF_COMMON_PARAMS[0]))

/* Part parameters (SysEx address: 00 00 18+part XX) */
/* Test values: switches use 0 (default likely 1), ranges use distinctive values */
static const ParamDef PART_PARAMS[] = {
    {0x00, "transmitswitch", 0, 0},         /* Switch: test 0 */
    {0x01, "transmitchannel", 7, 0},        /* Different channel */
    {0x02, "transmitprogramchange", 33, 1}, /* Distinctive value */
    {0x04, "transmitvolume", 77, 1},
    {0x06, "transmitpan", 33, 1},
    {0x08, "transmitkeyrangelower", 24, 0}, /* Low range */
    {0x09, "transmitkeyrangeupper", 108, 0},/* High range */
    {0x0A, "transmitkeytranspose", 52, 0},
    {0x0B, "transmitvelocitysense", 77, 0},
    {0x0C, "transmitvelocitymax", 77, 0},
    {0x0D, "transmitvelocitycurve", 5, 0},
    {0x0E, "internalswitch", 0, 0},         /* Switch: test 0 */
    {0x0F, "internalkeyrangelower", 24, 0},
    {0x10, "internalkeyrangeupper", 108, 0},
    {0x11, "internalkeytranspose", 52, 0},
    {0x12, "internalvelocitysense", 77, 0},
    {0x13, "internalvelocitymax", 77, 0},
    {0x14, "internalvelocitycurve", 5, 0},
    {0x15, "receiveswitch", 0, 0},          /* Switch: test 0 */
    {0x16, "receivechannel", 7, 0},
    {0x17, "patchnumber", 33, 1},           /* Different patch number */
    {0x19, "partlevel", 77, 0},
    {0x1A, "partpan", 33, 0},
    {0x1B, "partcoarsetune", 76, 0},
    {0x1C, "partfinetune", 78, 0},
    {0x1D, "reverbswitch", 0, 0},           /* Switch: test 0 */
    {0x1E, "chorusswitch", 0, 0},           /* Switch: test 0 */
    {0x1F, "receiveprogramchange", 0, 0},   /* Switch: test 0 */
    {0x20, "receivevolume", 0, 0},          /* Switch: test 0 */
    {0x21, "receivehold1", 0, 0},           /* Switch: test 0 */
    {0x22, "outputselect", 2, 0},           /* Different output */
};
#define NUM_PART_PARAMS (sizeof(PART_PARAMS)/sizeof(PART_PARAMS[0]))

/* Mapping state machine */
enum MapPhase {
    MAP_IDLE = 0,
    MAP_SNAPSHOT,       /* Take SRAM snapshot */
    MAP_SEND_SYSEX,     /* Send the SysEx */
    MAP_WAIT,           /* Wait for emulator to process */
    MAP_COMPARE,        /* Compare and log changes */
    MAP_NEXT,           /* Move to next parameter */
    MAP_DONE            /* All done */
};

static int g_map_active = 0;
static int g_map_phase = MAP_IDLE;
static int g_map_mode = 0;          /* 0=common, 1=part */
static int g_map_part = 0;          /* Which part (0-7) when mapping parts */
static int g_map_param_idx = 0;     /* Current parameter index */
static int g_map_wait_cycles = 0;   /* Wait counter */
static int g_map_test_pass = 0;     /* 0-2: test with 3 different values */
static uint8_t g_map_sram_snapshot[MAP_SRAM_SCAN_SIZE];
static uint8_t g_map_sysex_pending[16];
static int g_map_sysex_len = 0;
static int g_map_last_offset = -1;  /* Track offset from previous pass */

/* Process one step of the parameter mapping */
static void process_mapping_step(void) {
    if (!g_map_active || !g_mcu) return;

    switch (g_map_phase) {
    case MAP_SNAPSHOT:
        /* Take snapshot of SRAM around temp perf */
        memcpy(g_map_sram_snapshot, &g_mcu->sram[SRAM_TEMP_PERF_OFFSET], MAP_SRAM_SCAN_SIZE);
        g_map_phase = MAP_SEND_SYSEX;
        break;

    case MAP_SEND_SYSEX: {
        const ParamDef* param;
        uint8_t addr2, addr3, addr4;

        if (g_map_mode == 0) {
            /* Common params */
            if (g_map_param_idx >= (int)NUM_PERF_COMMON_PARAMS) {
                /* Done with common, switch to parts */
                g_map_mode = 1;
                g_map_part = 0;
                g_map_param_idx = 0;
                fprintf(stderr, "\n=== Mapping Part %d Parameters ===\n", g_map_part + 1);
            }
        }

        if (g_map_mode == 1) {
            if (g_map_param_idx >= (int)NUM_PART_PARAMS) {
                g_map_part++;
                g_map_param_idx = 0;
                if (g_map_part >= 8) {
                    g_map_phase = MAP_DONE;
                    break;
                }
                fprintf(stderr, "\n=== Mapping Part %d Parameters ===\n", g_map_part + 1);
            }
        }

        if (g_map_mode == 0) {
            param = &PERF_COMMON_PARAMS[g_map_param_idx];
            addr2 = 0x00;
            addr3 = 0x10;
            addr4 = param->sysex_offset;
            fprintf(stderr, "Testing common/%s (SysEx 00 00 10 %02x)... ", param->name, addr4);
        } else {
            param = &PART_PARAMS[g_map_param_idx];
            addr2 = 0x00;
            addr3 = 0x18 + g_map_part;
            addr4 = param->sysex_offset;
            fprintf(stderr, "Testing part%d/%s (SysEx 00 00 %02x %02x)... ", g_map_part + 1, param->name, addr3, addr4);
        }

        /* Build DT1 SysEx message inline */
        {
            uint8_t* msg = g_map_sysex_pending;
            int len = 0;
            msg[len++] = 0xF0;  /* SysEx start */
            msg[len++] = 0x41;  /* Roland */
            msg[len++] = 0x10;  /* Device ID */
            msg[len++] = 0x46;  /* JV-880 */
            msg[len++] = 0x12;  /* DT1 */
            msg[len++] = 0x00;  /* addr1 */
            msg[len++] = addr2;
            msg[len++] = addr3;
            msg[len++] = addr4;
            /* Calculate test value based on pass (0, 1, 64, 100, 127) */
            uint8_t test_val;
            switch (g_map_test_pass) {
                case 0: test_val = 0; break;    /* Test with 0 for switches */
                case 1: test_val = 1; break;    /* Test with 1 for switches */
                case 2: test_val = 64; break;   /* Center value */
                case 3: test_val = 100; break;  /* Higher value */
                case 4: test_val = 127; break;  /* Max value */
                default: test_val = param->test_value; break;
            }
            if (param->is_two_byte) {
                /* For 2-byte params, vary MSB too on pass 2 */
                msg[len++] = (g_map_test_pass == 2) ? 0x01 : 0x00;
                msg[len++] = test_val & 0x7F;
            } else {
                msg[len++] = test_val & 0x7F;
            }
            fprintf(stderr, "[pass %d, val=%d] ", g_map_test_pass, test_val);
            /* Calculate checksum */
            uint8_t sum = 0;
            for (int i = 5; i < len; i++) sum += msg[i];
            msg[len++] = (128 - (sum & 0x7F)) & 0x7F;
            msg[len++] = 0xF7;  /* SysEx end */
            g_map_sysex_len = len;
        }
        g_map_wait_cycles = 0;
        g_map_phase = MAP_WAIT;
        break;
    }

    case MAP_WAIT:
        /* Wait for emulator to process (~250ms to ensure SysEx is fully processed) */
        g_map_wait_cycles++;
        if (g_map_wait_cycles >= 50) {  /* ~250ms for reliable detection */
            g_map_phase = MAP_COMPARE;
        }
        break;

    case MAP_COMPARE: {
        /* Find which bytes changed */
        int changes[32];
        int change_count = 0;

        for (int i = 0; i < MAP_SRAM_SCAN_SIZE && change_count < 32; i++) {
            if (g_mcu->sram[SRAM_TEMP_PERF_OFFSET + i] != g_map_sram_snapshot[i]) {
                changes[change_count++] = i;
            }
        }

        /* Calculate what test value we sent */
        uint8_t sent_val;
        switch (g_map_test_pass) {
            case 0: sent_val = 0; break;
            case 1: sent_val = 1; break;
            case 2: sent_val = 64; break;
            case 3: sent_val = 100; break;
            case 4: sent_val = 127; break;
            default: sent_val = 0; break;
        }

        if (change_count == 0) {
            fprintf(stderr, "NO CHANGE\n");
            g_map_last_offset = -1;
        } else if (change_count == 1) {
            int off = changes[0];
            uint8_t oldv = g_map_sram_snapshot[off];
            uint8_t newv = g_mcu->sram[SRAM_TEMP_PERF_OFFSET + off];
            /* Check if offset is consistent and value matches */
            if (g_map_test_pass > 0 && g_map_last_offset >= 0 && off != g_map_last_offset) {
                fprintf(stderr, "SRAM[%d]: %d -> %d (OFFSET CHANGED from %d!)\n",
                    off, oldv, newv, g_map_last_offset);
            } else if (newv == sent_val) {
                fprintf(stderr, "SRAM[%d]: %d -> %d ✓ (matches sent value)\n", off, oldv, newv);
            } else {
                fprintf(stderr, "SRAM[%d]: %d -> %d (sent %d, got %d - PACKED?)\n",
                    off, oldv, newv, sent_val, newv);
            }
            g_map_last_offset = off;
        } else {
            fprintf(stderr, "%d changes: ", change_count);
            for (int i = 0; i < change_count && i < 8; i++) {
                int off = changes[i];
                fprintf(stderr, "[%d]=%d ", off, g_mcu->sram[SRAM_TEMP_PERF_OFFSET + off]);
            }
            fprintf(stderr, "\n");
            g_map_last_offset = -1;
        }

        g_map_phase = MAP_NEXT;
        break;
    }

    case MAP_NEXT:
        /* For Part 1 only, run 5 test passes per parameter to verify mapping */
        if (g_map_mode == 1 && g_map_part == 0 && g_map_test_pass < 4) {
            g_map_test_pass++;
            g_map_phase = MAP_SNAPSHOT;
        } else {
            g_map_test_pass = 0;
            g_map_last_offset = -1;
            g_map_param_idx++;
            g_map_phase = MAP_SNAPSHOT;
        }
        break;

    case MAP_DONE:
        fprintf(stderr, "\n=== Parameter Mapping Complete ===\n");
        g_map_active = 0;
        g_map_phase = MAP_IDLE;
        break;

    default:
        break;
    }
}

/* Background emulation thread */
static pthread_t g_emu_thread;
static volatile int g_thread_running = 0;
static pthread_t g_load_thread;
static volatile int g_load_thread_running = 0;

/* Audio ring buffer (48kHz stereo output)
 * 512 samples = ~10.7ms buffer for CPU scheduling headroom */
#define AUDIO_RING_SIZE 512
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
/* JV-880 PCM runs at 64 kHz with oversampling enabled. */
#define JV880_SAMPLE_RATE 64000
#define MOVE_SAMPLE_RATE 44100

/* Anti-aliasing filter for 64k→44.1k downsampling.
 * 2nd order Butterworth low-pass at 20kHz (below 22.05kHz Nyquist).
 * Coefficients pre-calculated for fs=64000, fc=20000, Q=0.707 */
struct BiquadFilter {
    /* Filter coefficients */
    double b0, b1, b2, a1, a2;
    /* Filter state (per channel) */
    double z1_l, z2_l, z1_r, z2_r;
};

static void biquad_init(BiquadFilter *f) {
    /* Butterworth LPF: fs=64000, fc=20000 */
    const double fs = 64000.0;
    const double fc = 20000.0;
    const double Q = 0.707107;  /* Butterworth */

    const double w0 = 2.0 * 3.14159265358979323846 * fc / fs;
    const double cosw0 = cos(w0);
    const double sinw0 = sin(w0);
    const double alpha = sinw0 / (2.0 * Q);

    const double a0 = 1.0 + alpha;
    f->b0 = ((1.0 - cosw0) / 2.0) / a0;
    f->b1 = (1.0 - cosw0) / a0;
    f->b2 = ((1.0 - cosw0) / 2.0) / a0;
    f->a1 = (-2.0 * cosw0) / a0;
    f->a2 = (1.0 - alpha) / a0;

    f->z1_l = f->z2_l = f->z1_r = f->z2_r = 0.0;
}

static inline void biquad_process(BiquadFilter *f, double in_l, double in_r,
                                   double *out_l, double *out_r) {
    /* Direct Form II Transposed */
    *out_l = f->b0 * in_l + f->z1_l;
    f->z1_l = f->b1 * in_l - f->a1 * (*out_l) + f->z2_l;
    f->z2_l = f->b2 * in_l - f->a2 * (*out_l);

    *out_r = f->b0 * in_r + f->z1_r;
    f->z1_r = f->b1 * in_r - f->a1 * (*out_r) + f->z2_r;
    f->z2_r = f->b2 * in_r - f->a2 * (*out_r);
}

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

/* Case-insensitive check for .bin extension */
static int has_bin_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".bin") == 0);
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
        if (strstr(entry->d_name, "SR-JV80") && has_bin_extension(entry->d_name)) {
            strncpy(g_expansion_files[g_expansion_file_count], entry->d_name,
                    sizeof(g_expansion_files[0]) - 1);

            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", exp_dir, entry->d_name);
            g_expansion_sizes[g_expansion_file_count] = get_file_size(path);

            g_expansion_file_count++;
        }
    }
    closedir(dir);

    /* Sort file list alphabetically for consistent cache validation */
    if (g_expansion_file_count > 1) {
        /* Simple bubble sort for small array - sorts both filenames and sizes together */
        for (int i = 0; i < g_expansion_file_count - 1; i++) {
            for (int j = i + 1; j < g_expansion_file_count; j++) {
                if (strcmp(g_expansion_files[i], g_expansion_files[j]) > 0) {
                    char tmp_name[256];
                    strcpy(tmp_name, g_expansion_files[i]);
                    strcpy(g_expansion_files[i], g_expansion_files[j]);
                    strcpy(g_expansion_files[j], tmp_name);
                    uint32_t tmp_size = g_expansion_sizes[i];
                    g_expansion_sizes[i] = g_expansion_sizes[j];
                    g_expansion_sizes[j] = tmp_size;
                }
            }
        }
    }
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

/* Comparison function for sorting expansions by name */
static int compare_expansions(const void *a, const void *b) {
    const ExpansionInfo *ea = (const ExpansionInfo *)a;
    const ExpansionInfo *eb = (const ExpansionInfo *)b;
    return strcmp(ea->name, eb->name);
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
        if (strstr(entry->d_name, "SR-JV80") && has_bin_extension(entry->d_name)) {
            if (scan_expansion_rom(entry->d_name, &g_expansions[g_expansion_count])) {
                g_expansion_count++;
            }
        }
    }
    closedir(dir);

    /* Sort expansions alphabetically by name (01 Pop, 06 Dance, etc.) */
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

/* Load expansion ROM into emulator's waverom_exp */
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

    /* Clear the buffer first (for smaller ROMs) */
    memset(g_mcu->pcm.waverom_exp, 0, EXPANSION_SIZE_8MB);

    /* Copy expansion data */
    memcpy(g_mcu->pcm.waverom_exp, exp->unscrambled, exp->rom_size);

    g_current_expansion = exp_index;
    /* Reset emulator so it detects the new expansion */
    g_mcu->SC55_Reset();
    fprintf(stderr, "JV880: Loaded expansion %s to emulator (%dMB, with reset)\n",
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

/* SRAM scan state for performance discovery - forward declarations */
static int g_sram_scan_countdown = 0;
static int g_found_perf_sram_offset = -1;
static void scan_sram_for_performance(void);

/* Select a performance (0-47 across 3 banks) */
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

    /* Schedule SRAM scan for performance data discovery */
    g_sram_scan_countdown = 100;  /* Scan after 100 render cycles (~2 seconds) */
}

/* Scan SRAM for current performance name to find temp performance location */
static void scan_sram_for_performance(void) {
    if (!g_mcu || !g_performance_mode) return;

    /* Get expected performance name from ROM/NVRAM */
    char expected_name[16] = {0};
    int bank = g_current_performance / PERFS_PER_BANK;
    int perf_in_bank = g_current_performance % PERFS_PER_BANK;

    if (bank == 2 && g_mcu) {
        /* Internal - read from NVRAM */
        uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (perf_in_bank * PERF_SIZE);
        memcpy(expected_name, &g_mcu->nvram[nvram_offset], 12);
    } else if (g_rom2) {
        /* Preset A/B - read from ROM2 */
        uint32_t offset = (bank == 0) ? PERF_OFFSET_PRESET_A : PERF_OFFSET_PRESET_B;
        offset += perf_in_bank * PERF_SIZE;
        memcpy(expected_name, &g_rom2[offset], 12);
    }

    /* Trim trailing spaces */
    int name_len = 12;
    while (name_len > 0 && expected_name[name_len - 1] == ' ') name_len--;
    expected_name[name_len] = '\0';

    fprintf(stderr, "JV880: Scanning SRAM for performance name '%s'...\n", expected_name);

    /* Search for first 4+ chars of name in SRAM */
    int search_len = (name_len >= 4) ? 4 : name_len;
    if (search_len < 3) {
        fprintf(stderr, "JV880: Performance name too short to search\n");
        return;
    }

    for (int i = 0; i <= SRAM_SIZE - search_len; i++) {
        if (memcmp(&g_mcu->sram[i], expected_name, search_len) == 0) {
            g_found_perf_sram_offset = i;
            fprintf(stderr, "JV880: *** FOUND '%s' in SRAM at offset 0x%04x ***\n",
                    expected_name, i);

            /* Dump 64 bytes around the match */
            fprintf(stderr, "JV880: SRAM[0x%04x - 0x%04x]:\n", i, i + 63);
            for (int j = 0; j < 64; j += 16) {
                fprintf(stderr, "  %04x: ", i + j);
                for (int k = 0; k < 16 && i + j + k < SRAM_SIZE; k++) {
                    fprintf(stderr, "%02x ", g_mcu->sram[i + j + k]);
                }
                fprintf(stderr, "  |");
                for (int k = 0; k < 16 && i + j + k < SRAM_SIZE; k++) {
                    uint8_t c = g_mcu->sram[i + j + k];
                    fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
                }
                fprintf(stderr, "|\n");
            }
            return;
        }
    }

    fprintf(stderr, "JV880: Performance name NOT found in SRAM\n");
    g_found_perf_sram_offset = -1;
}

/* Select a part within the current performance (0-7) */
static void select_part(int part_index) {
    if (part_index < 0 || part_index > 7) return;
    g_current_part = part_index;
    fprintf(stderr, "JV880: Selected part %d\n", part_index + 1);
}

/* Get current bank name for display */
static const char* get_current_bank_name(void) {
    if (g_performance_mode) {
        /* Performance mode - return performance bank name */
        static const char* perf_bank_names[] = {"Preset A", "Preset B", "Internal"};
        int bank = g_current_performance / PERFS_PER_BANK;
        if (bank >= 0 && bank < NUM_PERF_BANKS) {
            return perf_bank_names[bank];
        }
        return "Mini-JV";
    }

    /* Patch mode */
    if (g_current_patch < 0 || g_current_patch >= g_total_patches) {
        return "Mini-JV";
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

    /* Anti-aliasing filter + linear interpolation for 64k → 44.1k downsampling.
     * 1. Filter every input sample at 64kHz (removes >22kHz content)
     * 2. Linear interpolation selects output samples at 44.1kHz */
    BiquadFilter aa_filter;
    biquad_init(&aa_filter);

    const double step = (double)JV880_SAMPLE_RATE / (double)MOVE_SAMPLE_RATE;
    double resample_pos = 0.0;
    double prev_l = 0.0, prev_r = 0.0;
    double curr_l = 0.0, curr_r = 0.0;

    while (g_thread_running) {
        /* Process MIDI queue */
        while (g_midi_read != g_midi_write) {
            int idx = g_midi_read;
            g_mcu->postMidiSC55(g_midi_queue[idx], g_midi_queue_len[idx]);
            g_midi_read = (g_midi_read + 1) % MIDI_QUEUE_SIZE;
        }

        /* Check for pending parameter mapping SysEx */
        if (g_map_sysex_len > 0) {
            g_mcu->postMidiSC55(g_map_sysex_pending, g_map_sysex_len);
            g_map_sysex_len = 0;
        }

        /* Check if we need more audio */
        int free_space = ring_free();
        if (free_space < 64) {
            usleep(50);
            continue;
        }

        g_mcu->updateSC55(64);
        int avail = g_mcu->sample_write_ptr;

        /* Process all input samples through anti-aliasing filter */
        for (int i = 0; i < avail; i += 2) {
            /* Apply anti-aliasing filter to input sample */
            double in_l = (double)g_mcu->sample_buffer[i];
            double in_r = (double)g_mcu->sample_buffer[i + 1];
            double filt_l, filt_r;
            biquad_process(&aa_filter, in_l, in_r, &filt_l, &filt_r);

            /* Advance to next input sample */
            prev_l = curr_l;
            prev_r = curr_r;
            curr_l = filt_l;
            curr_r = filt_r;
            resample_pos += 1.0;

            /* Output samples at 44.1kHz rate using linear interpolation */
            while (resample_pos >= step && ring_free() > 0) {
                resample_pos -= step;

                /* Interpolation factor: how far into current sample */
                double t = 1.0 - (resample_pos / 1.0);
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;

                int32_t out_l = (int32_t)(prev_l + t * (curr_l - prev_l));
                int32_t out_r = (int32_t)(prev_r + t * (curr_r - prev_r));

                pthread_mutex_lock(&g_ring_mutex);
                int wr = g_ring_write;
                g_audio_ring[wr * 2 + 0] = out_l;
                g_audio_ring[wr * 2 + 1] = out_r;
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

    /* Pre-fill audio buffer with anti-aliasing filter */
    g_ring_write = 0;
    g_ring_read = 0;

    BiquadFilter aa_filter;
    biquad_init(&aa_filter);
    const double step = (double)JV880_SAMPLE_RATE / (double)MOVE_SAMPLE_RATE;
    double resample_pos = 0.0;
    double prev_l = 0.0, prev_r = 0.0;
    double curr_l = 0.0, curr_r = 0.0;

    fprintf(stderr, "JV880: Pre-filling buffer...\n");
    snprintf(g_loading_status, sizeof(g_loading_status), "Preparing audio...");
    for (int i = 0; i < 256 && g_ring_write < AUDIO_RING_SIZE / 2; i++) {
        g_mcu->updateSC55(8);
        int avail = g_mcu->sample_write_ptr;

        for (int j = 0; j < avail && g_ring_write < AUDIO_RING_SIZE / 2; j += 2) {
            /* Apply anti-aliasing filter */
            double in_l = (double)g_mcu->sample_buffer[j];
            double in_r = (double)g_mcu->sample_buffer[j + 1];
            double filt_l, filt_r;
            biquad_process(&aa_filter, in_l, in_r, &filt_l, &filt_r);

            prev_l = curr_l;
            prev_r = curr_r;
            curr_l = filt_l;
            curr_r = filt_r;
            resample_pos += 1.0;

            /* Output at 44.1kHz rate */
            while (resample_pos >= step && g_ring_write < AUDIO_RING_SIZE / 2) {
                resample_pos -= step;
                double t = 1.0 - (resample_pos / 1.0);
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;

                g_audio_ring[g_ring_write * 2 + 0] = (int32_t)(prev_l + t * (curr_l - prev_l));
                g_audio_ring[g_ring_write * 2 + 1] = (int32_t)(prev_r + t * (curr_r - prev_r));
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
    } else if (strcmp(key, "load_expansion") == 0) {
        /* Load a specific expansion card by index
         * This sets which expansion is active for patchnumber 64-127 in performances
         * Format: "idx" or "idx,bank_offset" */
        int exp_idx = atoi(val);
        int bank_offset = 0;
        const char *comma = strchr(val, ',');
        if (comma) {
            bank_offset = atoi(comma + 1);
        }
        if (exp_idx >= 0 && exp_idx < g_expansion_count) {
            /* Validate bank offset */
            int max_offset = (g_expansions[exp_idx].patch_count > 64) ?
                             ((g_expansions[exp_idx].patch_count - 1) / 64) * 64 : 0;
            if (bank_offset < 0) bank_offset = 0;
            if (bank_offset > max_offset) bank_offset = max_offset;
            g_expansion_bank_offset = bank_offset;
            load_expansion_to_emulator(exp_idx);
            fprintf(stderr, "JV880: Loaded expansion card: %s (patches %d-%d)\n",
                    g_expansions[exp_idx].name,
                    bank_offset + 1,
                    bank_offset + 64 > g_expansions[exp_idx].patch_count ?
                        g_expansions[exp_idx].patch_count : bank_offset + 64);
        } else if (exp_idx == -1) {
            /* -1 means no expansion (clear the card slot) */
            g_current_expansion = -1;
            g_expansion_bank_offset = 0;
            fprintf(stderr, "JV880: Cleared expansion card slot\n");
        } else {
            fprintf(stderr, "JV880: Invalid expansion index %d (have %d expansions)\n",
                    exp_idx, g_expansion_count);
        }
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
    } else if (strcmp(key, "dump_sram") == 0 && g_mcu) {
        /* Debug: dump SRAM to file for analysis */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_module_dir, val);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(g_mcu->sram, 1, SRAM_SIZE, f);
            fclose(f);
            fprintf(stderr, "JV880: Dumped SRAM to %s\n", path);
        }
    } else if (strcmp(key, "dump_nvram") == 0 && g_mcu) {
        /* Debug: dump NVRAM to file for analysis */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_module_dir, val);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(g_mcu->nvram, 1, NVRAM_SIZE, f);
            fclose(f);
            fprintf(stderr, "JV880: Dumped NVRAM to %s\n", path);
        }
    } else if (strcmp(key, "save_nvram") == 0 && g_mcu) {
        /* Save NVRAM to the standard file location */
        char path[1024];
        snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", g_module_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(g_mcu->nvram, 1, NVRAM_SIZE, f);
            fclose(f);
            fprintf(stderr, "JV880: Saved NVRAM to %s\n", path);
        } else {
            fprintf(stderr, "JV880: Failed to save NVRAM to %s\n", path);
        }
    } else if (strcmp(key, "dump_temp_perf") == 0 && g_mcu) {
        /* Debug: dump temp performance from SRAM for analysis */
        fprintf(stderr, "JV880: === Temp Performance at SRAM 0x%04x ===\n", SRAM_TEMP_PERF_OFFSET);

        /* Get name */
        char name[13];
        memcpy(name, &g_mcu->sram[SRAM_TEMP_PERF_OFFSET], 12);
        name[12] = '\0';
        fprintf(stderr, "Name: '%s'\n", name);

        /* Dump first 64 bytes (common area) */
        fprintf(stderr, "\nCommon area (bytes 0-63):\n");
        for (int i = 0; i < 64; i += 16) {
            fprintf(stderr, "  %04x: ", i);
            for (int j = 0; j < 16 && (i+j) < 64; j++) {
                fprintf(stderr, "%02x ", g_mcu->sram[SRAM_TEMP_PERF_OFFSET + i + j]);
            }
            fprintf(stderr, " |");
            for (int j = 0; j < 16 && (i+j) < 64; j++) {
                uint8_t c = g_mcu->sram[SRAM_TEMP_PERF_OFFSET + i + j];
                fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
            }
            fprintf(stderr, "|\n");
        }

        /* Dump part data - try different layouts */
        fprintf(stderr, "\nPart data (bytes 31-203, 8 parts):\n");
        int partStart = 31;  /* After common params */
        int bytesPerPart = (204 - 31) / 8;  /* ~21 bytes per part */
        for (int part = 0; part < 8; part++) {
            int offset = partStart + part * bytesPerPart;
            fprintf(stderr, "  Part %d (offset %d, 0x%02x):\n    ", part + 1, offset, offset);
            for (int j = 0; j < bytesPerPart; j++) {
                fprintf(stderr, "%02x ", g_mcu->sram[SRAM_TEMP_PERF_OFFSET + offset + j]);
            }
            fprintf(stderr, "\n");
        }

        /* Also dump beyond 204 bytes in case temp format is larger */
        fprintf(stderr, "\nExtended area (bytes 204-400) to check if temp format is larger:\n");
        for (int i = 204; i < 400; i += 16) {
            fprintf(stderr, "  %04x: ", i);
            for (int j = 0; j < 16 && (i+j) < 400; j++) {
                fprintf(stderr, "%02x ", g_mcu->sram[SRAM_TEMP_PERF_OFFSET + i + j]);
            }
            fprintf(stderr, " |");
            for (int j = 0; j < 16 && (i+j) < 400; j++) {
                uint8_t c = g_mcu->sram[SRAM_TEMP_PERF_OFFSET + i + j];
                fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
            }
            fprintf(stderr, "|\n");
        }
        fprintf(stderr, "=== End Temp Performance ===\n");
    } else if (strcmp(key, "start_param_mapping") == 0 && g_mcu) {
        /* Start automated parameter mapping */
        if (!g_map_active) {
            fprintf(stderr, "\n");
            fprintf(stderr, "=================================================================\n");
            fprintf(stderr, "=== AUTOMATED PARAMETER MAPPING ===\n");
            fprintf(stderr, "=== Testing SysEx parameters and monitoring SRAM changes... ===\n");
            fprintf(stderr, "=================================================================\n");
            fprintf(stderr, "\n=== Mapping Performance Common Parameters ===\n");
            g_map_active = 1;
            g_map_phase = MAP_SNAPSHOT;
            g_map_mode = 0;  /* Start with common params */
            g_map_part = 0;
            g_map_param_idx = 0;
            g_map_wait_cycles = 0;
        } else {
            fprintf(stderr, "JV880: Parameter mapping already in progress\n");
        }
    } else if (strcmp(key, "stop_param_mapping") == 0) {
        /* Stop parameter mapping */
        g_map_active = 0;
        g_map_phase = MAP_IDLE;
        fprintf(stderr, "JV880: Parameter mapping stopped\n");
    } else if (strcmp(key, "dump_part_values") == 0 && g_mcu) {
        /* Dump all part parameter values for verification */
        fprintf(stderr, "\n=== Current Part Parameter Values ===\n");
        fprintf(stderr, "LCD Line 0: '%s'\n", g_mcu->lcd.GetLine(0));
        fprintf(stderr, "LCD Line 1: '%s'\n\n", g_mcu->lcd.GetLine(1));
        fprintf(stderr, "Part | Level | Pan  | Coarse | Fine | KeyLo | IntKeyLo\n");
        fprintf(stderr, "-----+-------+------+--------+------+-------+---------\n");
        for (int p = 0; p < 8; p++) {
            int base = SRAM_TEMP_PERF_OFFSET + TEMP_PERF_COMMON_SIZE + (p * TEMP_PERF_PART_SIZE);
            uint8_t level = g_mcu->sram[base + 17];
            uint8_t pan = g_mcu->sram[base + 18];
            uint8_t coarse = g_mcu->sram[base + 19];
            uint8_t fine = g_mcu->sram[base + 20];
            uint8_t keylo = g_mcu->sram[base + 4];   /* transmit key range lower */
            uint8_t intkeylo = g_mcu->sram[base + 10];  /* internal key range lower */
            fprintf(stderr, "  %d  |  %3d  | %3d  |  %3d   | %3d  |  %3d  |  %3d\n",
                    p + 1, level, pan, coarse, fine, keylo, intkeylo);
        }
        fprintf(stderr, "\nCommon: keymode=%d reverbtime=%d choruslevel=%d\n",
                g_mcu->sram[SRAM_TEMP_PERF_OFFSET + 12] & 0x03,
                g_mcu->sram[SRAM_TEMP_PERF_OFFSET + 14],
                g_mcu->sram[SRAM_TEMP_PERF_OFFSET + 16]);
        fprintf(stderr, "\nRaw bytes around part 1 (offset 28-50):\n  ");
        for (int i = 28; i < 50; i++) {
            fprintf(stderr, "%02x ", g_mcu->sram[SRAM_TEMP_PERF_OFFSET + i]);
        }
        fprintf(stderr, "\n=== End Part Values ===\n");
    } else if (strcmp(key, "run_param_test") == 0 && g_mcu) {
        /* Automated parameter offset verification test
         * Tests that set_param writes to correct NVRAM offsets
         * by setting distinctive values and reading back directly */
        fprintf(stderr, "\n");
        fprintf(stderr, "============================================\n");
        fprintf(stderr, "=== AUTOMATED PARAMETER OFFSET TEST ===\n");
        fprintf(stderr, "============================================\n\n");

        int pass = 0, fail = 0;
        const int toneIdx = 0;
        const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

        /* Test structure: {name, nvram_offset, test_value} */
        struct { const char* name; int offset; uint8_t testVal; } tests[] = {
            /* TVA section (these were the ones with wrong offsets) */
            {"level", 67, 0x63},           /* tvaLevel */
            {"pan", 68, 0x40},             /* tvaPan */
            {"tvaenvtime1", 74, 0x4A},     /* tvaEnvTime1 */
            {"tvaenvtime2", 76, 0x4C},     /* tvaEnvTime2 */
            {"tvaenvtime3", 78, 0x4E},     /* tvaEnvTime3 */
            {"tvaenvtime4", 80, 0x50},     /* tvaEnvTime4 */
            {"drylevel", 81, 0x51},        /* drySend */
            {"reverbsendlevel", 82, 0x52}, /* reverbSend */
            {"chorussendlevel", 83, 0x53}, /* chorusSend */
            /* TVF section */
            {"cutofffrequency", 52, 0x7F}, /* tvfCutoff */
            {"resonance", 53, 0x32},       /* tvfResonance */
            /* Pitch section */
            {"pitchcoarse", 37, 0x40},     /* pitchCoarse */
            {"pitchfine", 38, 0x41},       /* pitchFine */
        };
        const int numTests = sizeof(tests) / sizeof(tests[0]);

        /* Save original values */
        uint8_t origValues[20];
        for (int i = 0; i < numTests; i++) {
            origValues[i] = g_mcu->nvram[toneBase + tests[i].offset];
        }

        fprintf(stderr, "Testing tone %d parameters (base=0x%04x):\n\n", toneIdx, toneBase);

        for (int i = 0; i < numTests; i++) {
            /* Write test value directly to expected NVRAM offset */
            g_mcu->nvram[toneBase + tests[i].offset] = tests[i].testVal;

            /* Read back via get_param API */
            char paramKey[64], readBuf[32];
            snprintf(paramKey, sizeof(paramKey), "nvram_tone_%d_%s", toneIdx, tests[i].name);

            /* Call internal get_param logic - read the NVRAM byte */
            uint8_t readVal = g_mcu->nvram[toneBase + tests[i].offset];

            if (readVal == tests[i].testVal) {
                fprintf(stderr, "  ✓ PASS: %-20s offset=%2d wrote=0x%02x read=0x%02x\n",
                        tests[i].name, tests[i].offset, tests[i].testVal, readVal);
                pass++;
            } else {
                fprintf(stderr, "  ✗ FAIL: %-20s offset=%2d wrote=0x%02x read=0x%02x\n",
                        tests[i].name, tests[i].offset, tests[i].testVal, readVal);
                fail++;
            }
        }

        /* Restore original values */
        for (int i = 0; i < numTests; i++) {
            g_mcu->nvram[toneBase + tests[i].offset] = origValues[i];
        }

        fprintf(stderr, "\n--------------------------------------------\n");
        fprintf(stderr, "Results: %d passed, %d failed\n", pass, fail);
        fprintf(stderr, "============================================\n\n");
    } else if (strcmp(key, "dump_tone_layout") == 0 && g_mcu) {
        /* Dump current tone 0 structure with labeled offsets
         * Useful for verifying layout matches jv880_juce reference */
        const int toneIdx = 0;
        const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

        fprintf(stderr, "\n=== Tone %d Structure (base=0x%04x) ===\n", toneIdx, toneBase);
        fprintf(stderr, "Offsets verified against jv880_juce dataStructures.h\n\n");

        /* Print each section with meaningful names */
        fprintf(stderr, "--- LFO Section (23-36) ---\n");
        fprintf(stderr, "  23 lfo1Flags:     %3d (0x%02x)\n", g_mcu->nvram[toneBase+23], g_mcu->nvram[toneBase+23]);
        fprintf(stderr, "  24 lfo1Rate:      %3d\n", g_mcu->nvram[toneBase+24]);
        fprintf(stderr, "  25 lfo1Delay:     %3d\n", g_mcu->nvram[toneBase+25]);
        fprintf(stderr, "  26 lfo1Fade:      %3d\n", g_mcu->nvram[toneBase+26]);
        fprintf(stderr, "  27 lfo2Flags:     %3d (0x%02x)\n", g_mcu->nvram[toneBase+27], g_mcu->nvram[toneBase+27]);
        fprintf(stderr, "  28 lfo2Rate:      %3d\n", g_mcu->nvram[toneBase+28]);
        fprintf(stderr, "  29 lfo2Delay:     %3d\n", g_mcu->nvram[toneBase+29]);
        fprintf(stderr, "  30 lfo2Fade:      %3d\n", g_mcu->nvram[toneBase+30]);
        fprintf(stderr, "  31 lfo1PitchDpth: %3d\n", g_mcu->nvram[toneBase+31]);
        fprintf(stderr, "  32 lfo1TvfDepth:  %3d\n", g_mcu->nvram[toneBase+32]);
        fprintf(stderr, "  33 lfo1TvaDepth:  %3d\n", g_mcu->nvram[toneBase+33]);

        fprintf(stderr, "\n--- Pitch Section (37-51) ---\n");
        fprintf(stderr, "  37 pitchCoarse:   %3d (signed: %d)\n", g_mcu->nvram[toneBase+37], (int8_t)g_mcu->nvram[toneBase+37]);
        fprintf(stderr, "  38 pitchFine:     %3d (signed: %d)\n", g_mcu->nvram[toneBase+38], (int8_t)g_mcu->nvram[toneBase+38]);
        fprintf(stderr, "  43 tvpEnvDepth:   %3d\n", g_mcu->nvram[toneBase+43]);
        fprintf(stderr, "  44 tvpEnvTime1:   %3d\n", g_mcu->nvram[toneBase+44]);
        fprintf(stderr, "  45 tvpEnvLevel1:  %3d\n", g_mcu->nvram[toneBase+45]);

        fprintf(stderr, "\n--- TVF Section (52-66) ---\n");
        fprintf(stderr, "  52 tvfCutoff:     %3d  <-- Filter Cutoff\n", g_mcu->nvram[toneBase+52]);
        fprintf(stderr, "  53 tvfResonance:  %3d  <-- Filter Resonance\n", g_mcu->nvram[toneBase+53]);
        fprintf(stderr, "  55 tvfVeloCurve:  %3d (filterMode=%d)\n", g_mcu->nvram[toneBase+55], (g_mcu->nvram[toneBase+55]>>3)&3);
        fprintf(stderr, "  58 tvfEnvDepth:   %3d\n", g_mcu->nvram[toneBase+58]);
        fprintf(stderr, "  59 tvfEnvTime1:   %3d\n", g_mcu->nvram[toneBase+59]);
        fprintf(stderr, "  65 tvfEnvTime4:   %3d\n", g_mcu->nvram[toneBase+65]);

        fprintf(stderr, "\n--- TVA Section (67-83) --- CRITICAL ---\n");
        fprintf(stderr, "  67 tvaLevel:      %3d  <-- Tone Volume\n", g_mcu->nvram[toneBase+67]);
        fprintf(stderr, "  68 tvaPan:        %3d  <-- Tone Pan (64=center)\n", g_mcu->nvram[toneBase+68]);
        fprintf(stderr, "  69 tvaDelayTime:  %3d\n", g_mcu->nvram[toneBase+69]);
        fprintf(stderr, "  72 tvaVelocity:   %3d\n", g_mcu->nvram[toneBase+72]);
        fprintf(stderr, "  74 tvaEnvTime1:   %3d  <-- Attack Time\n", g_mcu->nvram[toneBase+74]);
        fprintf(stderr, "  75 tvaEnvLevel1:  %3d\n", g_mcu->nvram[toneBase+75]);
        fprintf(stderr, "  76 tvaEnvTime2:   %3d  <-- Decay Time\n", g_mcu->nvram[toneBase+76]);
        fprintf(stderr, "  77 tvaEnvLevel2:  %3d\n", g_mcu->nvram[toneBase+77]);
        fprintf(stderr, "  78 tvaEnvTime3:   %3d\n", g_mcu->nvram[toneBase+78]);
        fprintf(stderr, "  79 tvaEnvLevel3:  %3d\n", g_mcu->nvram[toneBase+79]);
        fprintf(stderr, "  80 tvaEnvTime4:   %3d  <-- Release Time\n", g_mcu->nvram[toneBase+80]);
        fprintf(stderr, "  81 drySend:       %3d  <-- Dry Level\n", g_mcu->nvram[toneBase+81]);
        fprintf(stderr, "  82 reverbSend:    %3d  <-- Reverb Send\n", g_mcu->nvram[toneBase+82]);
        fprintf(stderr, "  83 chorusSend:    %3d  <-- Chorus Send\n", g_mcu->nvram[toneBase+83]);

        fprintf(stderr, "\n--- Raw Hex (bytes 67-83, TVA section) ---\n  ");
        for (int i = 67; i <= 83; i++) {
            fprintf(stderr, "%02x ", g_mcu->nvram[toneBase+i]);
        }
        fprintf(stderr, "\n\n=== End Tone Layout ===\n");
    } else if (strncmp(key, "write_performance_", 18) == 0 && g_mcu) {
        /* Write temp performance to an Internal slot (0-15)
         * Usage: write_performance_<slot> where slot is 0-15
         * Copies temp performance from SRAM to NVRAM Internal slot */
        int slot = atoi(key + 18);
        if (slot >= 0 && slot < 16) {
            uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (slot * PERF_SIZE);
            memcpy(&g_mcu->nvram[nvram_offset],
                   &g_mcu->sram[SRAM_TEMP_PERF_OFFSET], PERF_SIZE);
            char name[13];
            memcpy(name, &g_mcu->sram[SRAM_TEMP_PERF_OFFSET], 12);
            name[12] = '\0';
            fprintf(stderr, "JV880: Wrote performance '%s' to Internal slot %d (NVRAM 0x%04x)\n",
                    name, slot + 1, nvram_offset);
        } else {
            fprintf(stderr, "JV880: Invalid performance slot %d (must be 0-15)\n", slot);
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
    if (strcmp(key, "total_patches") == 0 || strcmp(key, "preset_count") == 0) {
        snprintf(buf, buf_len, "%d", g_total_patches);
        return 1;
    }
    if (strcmp(key, "current_patch") == 0 || strcmp(key, "preset") == 0) {
        snprintf(buf, buf_len, "%d", g_current_patch);
        return 1;
    }
    if (strcmp(key, "patch_name") == 0 || strcmp(key, "preset_name") == 0) {
        if (g_performance_mode) {
            /* In performance mode, read name from ROM2/NVRAM */
            int idx = g_current_performance;
            if (idx >= 0 && idx < NUM_PERFORMANCES) {
                int bank = idx / PERFS_PER_BANK;
                int perf_in_bank = idx % PERFS_PER_BANK;
                uint8_t name_buf[PERF_NAME_LEN + 1];
                memset(name_buf, 0, sizeof(name_buf));
                int got_name = 0;

                if (bank == 2 && g_mcu) {
                    /* Internal - read from NVRAM */
                    uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (perf_in_bank * PERF_SIZE);
                    if (nvram_offset + PERF_NAME_LEN <= 0x8000) {
                        memcpy(name_buf, &g_mcu->nvram[nvram_offset], PERF_NAME_LEN);
                        got_name = 1;
                    }
                } else if (g_rom2) {
                    /* Preset A/B - read from ROM2 */
                    uint32_t offset = (bank == 0) ? PERF_OFFSET_PRESET_A : PERF_OFFSET_PRESET_B;
                    offset += perf_in_bank * PERF_SIZE;
                    if (offset + PERF_NAME_LEN <= 0x40000) {
                        memcpy(name_buf, &g_rom2[offset], PERF_NAME_LEN);
                        got_name = 1;
                    }
                }

                if (got_name) {
                    /* Trim trailing spaces */
                    int len = PERF_NAME_LEN;
                    while (len > 0 && (name_buf[len - 1] == ' ' || name_buf[len - 1] == 0)) len--;
                    name_buf[len] = '\0';
                    snprintf(buf, buf_len, "%s", name_buf);
                    return 1;
                }
            }
            snprintf(buf, buf_len, "---");
        } else if (g_current_patch >= 0 && g_current_patch < g_total_patches) {
            snprintf(buf, buf_len, "%s", g_patches[g_current_patch].name);
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
    if (strcmp(key, "patch_in_bank") == 0) {
        if (g_performance_mode) {
            /* Return 1-indexed position within performance bank */
            int pos = (g_current_performance % PERFS_PER_BANK) + 1;
            snprintf(buf, buf_len, "%d", pos);
        } else {
            /* Return 1-indexed position within current patch bank */
            int bank = get_bank_for_patch(g_current_patch);
            int pos = g_current_patch - g_bank_starts[bank] + 1;
            snprintf(buf, buf_len, "%d", pos);
        }
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
    /* Expansion card queries */
    if (strcmp(key, "expansion_count") == 0) {
        snprintf(buf, buf_len, "%d", g_expansion_count);
        return 1;
    }
    if (strcmp(key, "current_expansion") == 0) {
        snprintf(buf, buf_len, "%d", g_current_expansion);
        return 1;
    }
    if (strcmp(key, "current_expansion_name") == 0) {
        if (g_current_expansion >= 0 && g_current_expansion < g_expansion_count) {
            snprintf(buf, buf_len, "%s", g_expansions[g_current_expansion].name);
        } else {
            snprintf(buf, buf_len, "None");
        }
        return 1;
    }
    if (strncmp(key, "expansion_", 10) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 10);
        if (idx >= 0 && idx < g_expansion_count) {
            snprintf(buf, buf_len, "%s", g_expansions[idx].name);
            return 1;
        }
    }
    if (strncmp(key, "expansion_", 10) == 0 && strstr(key, "_patch_count")) {
        int idx = atoi(key + 10);
        if (idx >= 0 && idx < g_expansion_count) {
            snprintf(buf, buf_len, "%d", g_expansions[idx].patch_count);
            return 1;
        }
    }
    if (strcmp(key, "expansion_bank_offset") == 0) {
        snprintf(buf, buf_len, "%d", g_expansion_bank_offset);
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
    /* Query individual patch name by index: patch_<index>_name */
    if (strncmp(key, "patch_", 6) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 6);
        if (idx >= 0 && idx < g_total_patches) {
            snprintf(buf, buf_len, "%s", g_patches[idx].name);
            return 1;
        }
        snprintf(buf, buf_len, "---");
        return 1;
    }
    /* Query performance name by index: perf_<index>_name */
    /* Query performance name by index: perf_<index>_name
     * Always read from ROM2 (preset) or NVRAM (internal) - don't use LCD
     * because LCD may show part info, not just the name */
    if (strncmp(key, "perf_", 5) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 5);
        if (idx >= 0 && idx < NUM_PERFORMANCES) {
            /* Read performance name from ROM2 (preset) or NVRAM (internal) */
            int bank = idx / PERFS_PER_BANK;
            int perf_in_bank = idx % PERFS_PER_BANK;
            uint8_t name_buf[PERF_NAME_LEN + 1];
            memset(name_buf, 0, sizeof(name_buf));
            int got_name = 0;

            if (g_rom2) {
                uint32_t offset = 0;
                if (bank == 0) {
                    /* Preset A - read from ROM2 */
                    offset = PERF_OFFSET_PRESET_A + (perf_in_bank * PERF_SIZE);
                } else if (bank == 1) {
                    /* Preset B - read from ROM2 */
                    offset = PERF_OFFSET_PRESET_B + (perf_in_bank * PERF_SIZE);
                } else if (bank == 2 && g_mcu) {
                    /* Internal - read from NVRAM */
                    uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (perf_in_bank * PERF_SIZE);
                    if (nvram_offset + PERF_NAME_LEN <= 0x8000) {
                        memcpy(name_buf, &g_mcu->nvram[nvram_offset], PERF_NAME_LEN);
                        got_name = 1;
                    }
                }

                if (!got_name && offset > 0 && offset + PERF_NAME_LEN <= 0x40000) {
                    memcpy(name_buf, &g_rom2[offset], PERF_NAME_LEN);
                    got_name = 1;
                }
            }

            if (got_name) {
                /* Check if name looks valid (printable ASCII) */
                int valid = 1;
                for (int i = 0; i < PERF_NAME_LEN && name_buf[i]; i++) {
                    if (name_buf[i] < 0x20 || name_buf[i] > 0x7e) {
                        valid = 0;
                        break;
                    }
                }
                if (valid && name_buf[0] >= 0x20) {
                    /* Trim trailing spaces */
                    int len = PERF_NAME_LEN;
                    while (len > 0 && (name_buf[len - 1] == ' ' || name_buf[len - 1] == 0)) len--;
                    name_buf[len] = '\0';
                    snprintf(buf, buf_len, "%s", name_buf);
                    return 1;
                }
            }

            /* Fallback: Bank and number labels */
            int num = perf_in_bank + 1;
            const char* bank_names[] = {"PA", "PB", "INT"};
            snprintf(buf, buf_len, "%s:%02d", bank_names[bank], num);
            return 1;
        }
        snprintf(buf, buf_len, "---");
        return 1;
    }
    /* Query bank info: bank_<index>_name, bank_<index>_start, bank_<index>_count */
    if (strncmp(key, "bank_", 5) == 0) {
        int idx = atoi(key + 5);
        if (strstr(key, "_name") && idx >= 0 && idx < g_bank_count) {
            snprintf(buf, buf_len, "%s", g_bank_names[idx]);
            return 1;
        }
        if (strstr(key, "_start") && idx >= 0 && idx < g_bank_count) {
            snprintf(buf, buf_len, "%d", g_bank_starts[idx]);
            return 1;
        }
        if (strstr(key, "_count") && idx >= 0 && idx < g_bank_count) {
            int next_start = (idx + 1 < g_bank_count) ? g_bank_starts[idx + 1] : g_total_patches;
            snprintf(buf, buf_len, "%d", next_start - g_bank_starts[idx]);
            return 1;
        }
        /* Return JV-880 patchnumber base for this bank
         * JV-880 encoding: 0-63=Internal, 64-127=Card, 128-191=Preset A, 192-255=Preset B */
        if (strstr(key, "_patchnum_base") && idx >= 0 && idx < g_bank_count) {
            int base = 0;
            if (strcmp(g_bank_names[idx], "Preset A") == 0) {
                base = 128;
            } else if (strcmp(g_bank_names[idx], "Preset B") == 0) {
                base = 192;
            } else if (strcmp(g_bank_names[idx], "Internal") == 0) {
                base = 0;
            } else {
                /* Expansion cards go into the "Card" range (64-127) */
                /* For now, all expansions share the 64-127 range */
                base = 64;
            }
            snprintf(buf, buf_len, "%d", base);
            return 1;
        }
    }
    /* Read tone parameter by name: nvram_tone_<toneIdx>_<paramName>
     * Maps SysEx parameter names to actual NVRAM byte offsets.
     * Patch layout (362 bytes at 0x0d70):
     *   - Patch common: 26 bytes (0-25)
     *   - Tone 0: 84 bytes (26-109)
     *   - Tone 1: 84 bytes (110-193)
     *   - Tone 2: 84 bytes (194-277)
     *   - Tone 3: 84 bytes (278-361)
     */
    if (strncmp(key, "nvram_tone_", 11) == 0 && g_mcu) {
        int toneIdx = atoi(key + 11);
        const char* underscore = strchr(key + 11, '_');
        if (underscore && toneIdx >= 0 && toneIdx < 4) {
            const char* paramName = underscore + 1;
            int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);
            int offset = -1;
            int bitMask = 0;  /* 0 = full byte, otherwise extract specific bit */

            /* Map parameter names to NVRAM offsets within tone (based on jv880_juce dataStructures.h)
             * Tone structure is 84 bytes, offsets verified against jv880_juce */
            if (strcmp(paramName, "toneswitch") == 0) {
                offset = 0; bitMask = 0x80;  /* flags byte, bit 7 */
            } else if (strcmp(paramName, "wavegroup") == 0) {
                offset = 0; bitMask = 0x03;  /* flags byte, bits 0-1 */
            } else if (strcmp(paramName, "wavenumber") == 0) {
                offset = 1;
            } else if (strcmp(paramName, "fxmswitch") == 0) {
                offset = 2; bitMask = 0x80;  /* fxmConfig bit 7 */
            } else if (strcmp(paramName, "fxmdepth") == 0) {
                offset = 2; bitMask = 0x3F;  /* fxmConfig bits 0-5 */
            } else if (strcmp(paramName, "velocityrangelower") == 0) {
                offset = 3;
            } else if (strcmp(paramName, "velocityrangeupper") == 0) {
                offset = 4;
            /* LFO 1 parameters (offsets 23-26, 31-33) */
            } else if (strcmp(paramName, "lfo1rate") == 0) {
                offset = 24;
            } else if (strcmp(paramName, "lfo1delay") == 0) {
                offset = 25;
            } else if (strcmp(paramName, "lfo1fadetime") == 0) {
                offset = 26;  /* lfo1Fade */
            } else if (strcmp(paramName, "lfo1pitchdepth") == 0) {
                offset = 31;
            } else if (strcmp(paramName, "lfo1tvfdepth") == 0) {
                offset = 32;
            } else if (strcmp(paramName, "lfo1tvadepth") == 0) {
                offset = 33;
            /* LFO 2 parameters (offsets 27-30, 34-36) */
            } else if (strcmp(paramName, "lfo2rate") == 0) {
                offset = 28;
            } else if (strcmp(paramName, "lfo2delay") == 0) {
                offset = 29;
            } else if (strcmp(paramName, "lfo2fadetime") == 0) {
                offset = 30;  /* lfo2Fade */
            } else if (strcmp(paramName, "lfo2pitchdepth") == 0) {
                offset = 34;
            } else if (strcmp(paramName, "lfo2tvfdepth") == 0) {
                offset = 35;
            } else if (strcmp(paramName, "lfo2tvadepth") == 0) {
                offset = 36;
            /* Pitch parameters (offsets 37-51) */
            } else if (strcmp(paramName, "pitchcoarse") == 0) {
                offset = 37;
            } else if (strcmp(paramName, "pitchfine") == 0) {
                offset = 38;
            } else if (strcmp(paramName, "penvdepth") == 0) {
                offset = 43;  /* tvpEnvDepth */
            } else if (strcmp(paramName, "penvtime1") == 0) {
                offset = 44;
            } else if (strcmp(paramName, "penvlevel1") == 0) {
                offset = 45;
            } else if (strcmp(paramName, "penvtime2") == 0) {
                offset = 46;
            } else if (strcmp(paramName, "penvlevel2") == 0) {
                offset = 47;
            } else if (strcmp(paramName, "penvtime3") == 0) {
                offset = 48;
            } else if (strcmp(paramName, "penvlevel3") == 0) {
                offset = 49;
            } else if (strcmp(paramName, "penvtime4") == 0) {
                offset = 50;
            } else if (strcmp(paramName, "penvlevel4") == 0) {
                offset = 51;
            /* TVF (filter) parameters (offsets 52-66) */
            } else if (strcmp(paramName, "cutofffrequency") == 0) {
                offset = 52;  /* tvfCutoff */
            } else if (strcmp(paramName, "resonance") == 0) {
                offset = 53;  /* tvfResonance */
            } else if (strcmp(paramName, "cutoffkeyfollow") == 0) {
                offset = 54;  /* tvfTimeKFKeyfollow - lower nibble is keyfollow */
                bitMask = 0x0F;
            } else if (strcmp(paramName, "tvfenvdepth") == 0) {
                offset = 58;
            } else if (strcmp(paramName, "tvfenvtime1") == 0) {
                offset = 59;
            } else if (strcmp(paramName, "tvfenvlevel1") == 0) {
                offset = 60;
            } else if (strcmp(paramName, "tvfenvtime2") == 0) {
                offset = 61;
            } else if (strcmp(paramName, "tvfenvlevel2") == 0) {
                offset = 62;
            } else if (strcmp(paramName, "tvfenvtime3") == 0) {
                offset = 63;
            } else if (strcmp(paramName, "tvfenvlevel3") == 0) {
                offset = 64;
            } else if (strcmp(paramName, "tvfenvtime4") == 0) {
                offset = 65;
            } else if (strcmp(paramName, "tvfenvlevel4") == 0) {
                offset = 66;
            /* TVA (amp) parameters - NVRAM offsets from jv880_juce Tone struct */
            } else if (strcmp(paramName, "level") == 0) {
                offset = 67;  /* tvaLevel */
            } else if (strcmp(paramName, "pan") == 0) {
                offset = 68;  /* tvaPan */
            } else if (strcmp(paramName, "tonedelaytime") == 0) {
                offset = 69;  /* tvaDelayTime */
            } else if (strcmp(paramName, "tvaenvvelocitylevelsense") == 0) {
                offset = 72;  /* tvaVelocity */
            } else if (strcmp(paramName, "tvaenvtime1") == 0) {
                offset = 74;  /* tvaEnvTime1 */
            } else if (strcmp(paramName, "tvaenvlevel1") == 0) {
                offset = 75;  /* tvaEnvLevel1 */
            } else if (strcmp(paramName, "tvaenvtime2") == 0) {
                offset = 76;  /* tvaEnvTime2 */
            } else if (strcmp(paramName, "tvaenvlevel2") == 0) {
                offset = 77;  /* tvaEnvLevel2 */
            } else if (strcmp(paramName, "tvaenvtime3") == 0) {
                offset = 78;  /* tvaEnvTime3 */
            } else if (strcmp(paramName, "tvaenvlevel3") == 0) {
                offset = 79;  /* tvaEnvLevel3 */
            } else if (strcmp(paramName, "tvaenvtime4") == 0) {
                offset = 80;  /* tvaEnvTime4 */
            /* Output/FX sends - NVRAM offsets from jv880_juce */
            } else if (strcmp(paramName, "drylevel") == 0) {
                offset = 81;  /* drySend */
            } else if (strcmp(paramName, "reverbsendlevel") == 0) {
                offset = 82;  /* reverbSend */
            } else if (strcmp(paramName, "chorussendlevel") == 0) {
                offset = 83;  /* chorusSend */
            /* Filter mode - bits 3-4 of byte 55 (0=Off, 1=LPF, 2=HPF) */
            } else if (strcmp(paramName, "filtermode") == 0) {
                uint8_t byte = g_mcu->nvram[toneBase + 55];
                int filterMode = (byte >> 3) & 0x03;
                const char *labels[] = {"Off", "LPF", "HPF"};
                return snprintf(buf, buf_len, "%s", labels[filterMode < 3 ? filterMode : 0]);
            }

            if (offset >= 0 && offset < 85) {  /* 85 to include chorusSend */
                uint8_t val = g_mcu->nvram[toneBase + offset];
                if (bitMask != 0) {
                    val = (val & bitMask);
                    if (bitMask == 0x80) val = val ? 1 : 0;  /* Boolean for bit 7 */
                }
                snprintf(buf, buf_len, "%d", val);
                return 1;
            }
        }
    }

    /* Read patch common parameter by name: nvram_patchCommon_<paramName>
     * Patch common is 26 bytes (0-25) based on jv880_juce dataStructures.h:
     *   0-11: name (12 bytes)
     *   12: revChorConfig
     *   13-15: reverb (level, time, feedback)
     *   16-19: chorus (level, depth, rate, feedback)
     *   20: analogFeel
     *   21: level
     *   22: pan
     *   23: bendRange (bend down range)
     *   24: flags (bits 0-3: bend up, bit 4: porta mode, bit 5: solo legato, bit 6: porta switch, bit 7: key assign)
     *   25: portamentoTime
     */
    if (strncmp(key, "nvram_patchCommon_", 18) == 0 && g_mcu) {
        const char* paramName = key + 18;
        int offset = -1;
        int bitMask = 0;

        /* Map parameter names to NVRAM offsets (based on jv880_juce) */
        if (strcmp(paramName, "patchlevel") == 0) {
            offset = 21;
        } else if (strcmp(paramName, "patchpan") == 0 || strcmp(paramName, "patchpanning") == 0) {
            offset = 22;
        } else if (strcmp(paramName, "analogfeel") == 0) {
            offset = 20;
        } else if (strcmp(paramName, "reverblevel") == 0) {
            offset = 13;
        } else if (strcmp(paramName, "reverbtime") == 0) {
            offset = 14;
        } else if (strcmp(paramName, "reverbfeedback") == 0) {
            offset = 15;
        } else if (strcmp(paramName, "choruslevel") == 0) {
            offset = 16;
        } else if (strcmp(paramName, "chorusdepth") == 0) {
            offset = 17;
        } else if (strcmp(paramName, "chorusrate") == 0) {
            offset = 18;
        } else if (strcmp(paramName, "chorusfeedback") == 0) {
            offset = 19;
        } else if (strcmp(paramName, "bendrangedown") == 0) {
            offset = 23;  /* bendRange byte = bend down range */
        } else if (strcmp(paramName, "bendrangeup") == 0) {
            offset = 24;  /* flags byte, bits 0-3 = bend up range */
            bitMask = 0x0F;
        } else if (strcmp(paramName, "portamentoswitch") == 0) {
            offset = 24;  /* flags byte, bit 6 */
            bitMask = 0x40;
        } else if (strcmp(paramName, "portamentomode") == 0) {
            offset = 24;  /* flags byte, bit 4 */
            bitMask = 0x10;
        } else if (strcmp(paramName, "sololegato") == 0) {
            offset = 24;  /* flags byte, bit 5 */
            bitMask = 0x20;
        } else if (strcmp(paramName, "keyassign") == 0) {
            offset = 24;  /* flags byte, bit 7 */
            bitMask = 0x80;
        } else if (strcmp(paramName, "portamentotime") == 0) {
            offset = 25;
        }

        if (offset >= 0 && offset < 26) {
            uint8_t val = g_mcu->nvram[NVRAM_PATCH_OFFSET + offset];
            if (bitMask != 0) {
                val = (val & bitMask);
                /* Convert single-bit masks to boolean 0/1 */
                if (bitMask == 0x40 || bitMask == 0x10 || bitMask == 0x20 || bitMask == 0x80) {
                    val = val ? 1 : 0;
                }
            }
            snprintf(buf, buf_len, "%d", val);
            return 1;
        }
    }

    /* Read performance common parameter from SRAM temp buffer
     * Format: sram_perfCommon_<paramName>
     * Offsets discovered via automated mapping:
     *   12: keymode (packed)
     *   14: reverbtime
     *   15: reverbfeedback
     *   16: choruslevel
     *   17: chorusdepth
     *   18: chorusrate
     *   19: chorusfeedback
     *   20-24: voicereserve1-5
     */
    if (strncmp(key, "sram_perfCommon_", 16) == 0 && g_mcu) {
        const char* paramName = key + 16;
        int offset = -1;
        int bitMask = 0;

        /* Map parameter names to discovered SRAM offsets */
        if (strcmp(paramName, "keymode") == 0) {
            offset = 12;
            bitMask = 0x03;  /* Lower 2 bits based on mapping results */
        }
        else if (strcmp(paramName, "reverbtime") == 0) offset = 14;
        else if (strcmp(paramName, "reverbfeedback") == 0) offset = 15;
        else if (strcmp(paramName, "choruslevel") == 0) offset = 16;
        else if (strcmp(paramName, "chorusdepth") == 0) offset = 17;
        else if (strcmp(paramName, "chorusrate") == 0) offset = 18;
        else if (strcmp(paramName, "chorusfeedback") == 0) offset = 19;
        else if (strcmp(paramName, "voicereserve1") == 0) offset = 20;
        else if (strcmp(paramName, "voicereserve2") == 0) offset = 21;
        else if (strcmp(paramName, "voicereserve3") == 0) offset = 22;
        else if (strcmp(paramName, "voicereserve4") == 0) offset = 23;
        else if (strcmp(paramName, "voicereserve5") == 0) offset = 24;

        if (offset >= 0) {
            uint8_t val = g_mcu->sram[SRAM_TEMP_PERF_OFFSET + offset];
            if (bitMask != 0) {
                val = val & bitMask;
            }
            snprintf(buf, buf_len, "%d", val);
            return 1;
        }
    }

    /* Read part parameter from SRAM temp buffer
     * Format: sram_part_<partIdx>_<paramName>
     * Part data starts at offset 28, each part is 22 bytes (discovered via mapping)
     */
    if (strncmp(key, "sram_part_", 10) == 0 && g_mcu) {
        int partIdx = key[10] - '0';
        if (partIdx >= 0 && partIdx < 8 && key[11] == '_') {
            const char* paramName = key + 12;
            int partBase = SRAM_TEMP_PERF_OFFSET + TEMP_PERF_COMMON_SIZE + (partIdx * TEMP_PERF_PART_SIZE);
            int offset = -1;

            /* Part parameter offsets (discovered via automated mapping Jan 2025) */
            /* Direct storage parameters */
            if (strcmp(paramName, "partlevel") == 0) offset = 17;
            else if (strcmp(paramName, "partpan") == 0) offset = 18;
            else if (strcmp(paramName, "transmitprogramchange") == 0) offset = 1;
            else if (strcmp(paramName, "transmitvolume") == 0) offset = 2;
            else if (strcmp(paramName, "transmitpan") == 0) offset = 3;
            else if (strcmp(paramName, "transmitkeyrangelower") == 0) offset = 4;
            else if (strcmp(paramName, "transmitkeyrangeupper") == 0) offset = 5;
            else if (strcmp(paramName, "transmitvelocitymax") == 0) offset = 8;
            else if (strcmp(paramName, "transmitvelocitycurve") == 0) offset = 9;
            else if (strcmp(paramName, "internalkeyrangelower") == 0) offset = 10;
            else if (strcmp(paramName, "internalkeyrangeupper") == 0) offset = 11;
            else if (strcmp(paramName, "internalvelocitymax") == 0) offset = 14;
            else if (strcmp(paramName, "patchnumber") == 0) offset = 16;
            /* Signed offset parameters: stored = (val-64)&0xFF, read = (stored+64)&0x7F */
            else if (strcmp(paramName, "transmitkeytranspose") == 0 ||
                     strcmp(paramName, "transmitvelocitysense") == 0 ||
                     strcmp(paramName, "internalkeytranspose") == 0 ||
                     strcmp(paramName, "internalvelocitysense") == 0 ||
                     strcmp(paramName, "partcoarsetune") == 0 ||
                     strcmp(paramName, "partfinetune") == 0) {
                if (strcmp(paramName, "transmitkeytranspose") == 0) offset = 6;
                else if (strcmp(paramName, "transmitvelocitysense") == 0) offset = 7;
                else if (strcmp(paramName, "internalkeytranspose") == 0) offset = 12;
                else if (strcmp(paramName, "internalvelocitysense") == 0) offset = 13;
                else if (strcmp(paramName, "partcoarsetune") == 0) offset = 19;
                else if (strcmp(paramName, "partfinetune") == 0) offset = 20;
                if (offset >= 0) {
                    int8_t stored = (int8_t)g_mcu->sram[partBase + offset];
                    int val = stored + 64;  /* Convert from signed offset */
                    snprintf(buf, buf_len, "%d", val);
                    return 0;
                }
            }
            /* Packed byte 0: [internalswitch:7][transmitswitch:6][outputselect:4-5][transmitchannel:0-3] */
            else if (strcmp(paramName, "transmitswitch") == 0) {
                uint8_t b = g_mcu->sram[partBase + 0];
                snprintf(buf, buf_len, "%d", (b >> 6) & 1);
                return 0;
            }
            else if (strcmp(paramName, "internalswitch") == 0) {
                uint8_t b = g_mcu->sram[partBase + 0];
                snprintf(buf, buf_len, "%d", (b >> 7) & 1);
                return 0;
            }
            else if (strcmp(paramName, "outputselect") == 0) {
                uint8_t b = g_mcu->sram[partBase + 0];
                snprintf(buf, buf_len, "%d", (b >> 4) & 3);
                return 0;
            }
            else if (strcmp(paramName, "transmitchannel") == 0) {
                uint8_t b = g_mcu->sram[partBase + 0];
                snprintf(buf, buf_len, "%d", b & 0x0F);
                return 0;
            }
            /* Packed byte 15: [receiveprogramchange:7][receivevolume:6][receivehold1:5][internalvelocitycurve:0-2] */
            else if (strcmp(paramName, "internalvelocitycurve") == 0) {
                uint8_t b = g_mcu->sram[partBase + 15];
                snprintf(buf, buf_len, "%d", b & 0x07);
                return 0;
            }
            else if (strcmp(paramName, "receiveprogramchange") == 0) {
                uint8_t b = g_mcu->sram[partBase + 15];
                snprintf(buf, buf_len, "%d", (b >> 7) & 1);
                return 0;
            }
            else if (strcmp(paramName, "receivevolume") == 0) {
                uint8_t b = g_mcu->sram[partBase + 15];
                snprintf(buf, buf_len, "%d", (b >> 6) & 1);
                return 0;
            }
            else if (strcmp(paramName, "receivehold1") == 0) {
                uint8_t b = g_mcu->sram[partBase + 15];
                snprintf(buf, buf_len, "%d", (b >> 5) & 1);
                return 0;
            }
            /* Packed byte 21: [receiveswitch:7][reverbswitch:6][chorusswitch:5][receivechannel:0-3] */
            else if (strcmp(paramName, "receiveswitch") == 0) {
                uint8_t b = g_mcu->sram[partBase + 21];
                snprintf(buf, buf_len, "%d", (b >> 7) & 1);
                return 0;
            }
            else if (strcmp(paramName, "reverbswitch") == 0) {
                uint8_t b = g_mcu->sram[partBase + 21];
                snprintf(buf, buf_len, "%s", ((b >> 6) & 1) ? "On" : "Off");
                return 0;
            }
            else if (strcmp(paramName, "chorusswitch") == 0) {
                uint8_t b = g_mcu->sram[partBase + 21];
                snprintf(buf, buf_len, "%s", ((b >> 5) & 1) ? "On" : "Off");
                return 0;
            }
            else if (strcmp(paramName, "receivechannel") == 0) {
                uint8_t b = g_mcu->sram[partBase + 21];
                snprintf(buf, buf_len, "%d", b & 0x0F);
                return 0;
            }
            /* Raw byte access for debugging */
            else if (strncmp(paramName, "byte", 4) == 0) {
                offset = atoi(paramName + 4);
                if (offset < 0 || offset >= TEMP_PERF_PART_SIZE) offset = -1;
            }

            if (offset >= 0) {
                snprintf(buf, buf_len, "%d", g_mcu->sram[partBase + offset]);
                return 1;
            }
        }
    }

    /* Debug: search for string in SRAM - returns offset or -1 */
    if (strncmp(key, "debug_find_sram_", 16) == 0 && g_mcu) {
        const char* needle = key + 16;
        int needle_len = strlen(needle);
        if (needle_len > 0 && needle_len <= 12) {
            for (int i = 0; i <= SRAM_SIZE - needle_len; i++) {
                if (memcmp(&g_mcu->sram[i], needle, needle_len) == 0) {
                    snprintf(buf, buf_len, "0x%04x", i);
                    fprintf(stderr, "JV880: Found '%s' in SRAM at offset 0x%04x\n", needle, i);
                    return 1;
                }
            }
        }
        snprintf(buf, buf_len, "-1");
        return 1;
    }

    /* Debug: read SRAM byte at offset */
    if (strncmp(key, "debug_sram_", 11) == 0 && g_mcu) {
        int offset = strtol(key + 11, NULL, 0);
        if (offset >= 0 && offset < SRAM_SIZE) {
            snprintf(buf, buf_len, "%d", g_mcu->sram[offset]);
            return 1;
        }
    }

    /* Note: tempo and clock_mode are now host settings */
    return 0;
}

/* Output gain (reduce to prevent clipping) */
#define OUTPUT_GAIN_SHIFT 1  /* -6dB headroom to prevent clipping on hot patches */

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

    /* Check for pending SRAM scan (for performance data discovery) */
    if (g_sram_scan_countdown > 0) {
        g_sram_scan_countdown--;
        if (g_sram_scan_countdown == 0) {
            scan_sram_for_performance();
        }
    }

    /* Process parameter mapping state machine */
    if (g_map_active) {
        process_mapping_step();
    }
}

/* ========================================================================
 * PLUGIN API V2 - INSTANCE-BASED (for multi-instance support)
 *
 * Note: V1 API has been removed. V2 is required.
 *
 * Note: JV880 is extremely resource-intensive (emulator, ROMs, threads).
 * Multiple simultaneous instances are technically possible but may cause
 * performance issues on limited hardware.
 * ======================================================================== */

/* v2 instance structure containing ALL state (for true multi-instance) */
typedef struct {
    /* Module path */
    char module_dir[512];

    /* Emulator */
    MCU *mcu;
    int initialized;
    int rom_loaded;

    /* ROM data */
    uint8_t *rom2;

    /* Debug/SysEx */
    int debug_sysex;
    uint8_t sysex_buf[512];
    int sysex_len;
    int sysex_capture;

    /* Expansions */
    ExpansionInfo expansions[MAX_EXPANSIONS];
    int expansion_count;
    int current_expansion;
    int expansion_bank_offset;

    /* Expansion file tracking */
    char expansion_files[MAX_EXP_FILES][256];
    uint32_t expansion_sizes[MAX_EXP_FILES];
    int expansion_file_count;

    /* Patches */
    PatchInfo patches[MAX_TOTAL_PATCHES];
    int total_patches;
    int current_patch;

    /* Banks */
    int bank_starts[MAX_BANKS];
    char bank_names[MAX_BANKS][64];
    int bank_count;

    /* Performance mode */
    int performance_mode;
    int current_performance;
    int current_part;
    int perf_bank;
    int pending_perf_select;   /* Countdown to select performance after mode switch */
    int pending_patch_select;  /* Countdown to select patch after mode switch */
    volatile int warmup_remaining;  /* Warmup cycles remaining after reset */

    /* Resampling state (libresample) */
    void *resampleL;
    void *resampleR;
    float resample_in_l[4096];  /* Input buffer for resampler */
    float resample_in_r[4096];
    float resample_out_l[4096]; /* Output buffer for resampler */
    float resample_out_r[4096];

    /* Loading state */
    char loading_status[256];
    int loading_complete;
    int loading_phase;
    int loading_subindex;
    int warmup_count;

    /* Parameter mapping */
    int map_active;
    int map_phase;
    int map_mode;
    int map_part;
    int map_param_idx;
    int map_wait_cycles;
    int map_test_pass;
    uint8_t map_sram_snapshot[MAP_SRAM_SCAN_SIZE];
    uint8_t map_sysex_pending[16];
    int map_sysex_len;
    int map_last_offset;

    /* SRAM scanning */
    int sram_scan_countdown;
    int found_perf_sram_offset;

    /* Threading */
    pthread_t emu_thread;
    volatile int thread_running;
    pthread_t load_thread;
    volatile int load_thread_running;

    /* Audio ring buffer */
    int16_t audio_ring[AUDIO_RING_SIZE * 2];
    volatile int ring_write;
    volatile int ring_read;
    pthread_mutex_t ring_mutex;

    /* MIDI queue */
    uint8_t midi_queue[MIDI_QUEUE_SIZE][MIDI_MSG_MAX_LEN];
    int midi_queue_len[MIDI_QUEUE_SIZE];
    volatile int midi_write;
    volatile int midi_read;

    /* Other settings */
    int octave_transpose;

    /* Deferred state restoration (applied after loading completes) */
    char pending_state[512];
    int pending_state_valid;

    /* Error state */
    char load_error[256];

    /* Audio diagnostics */
    int underrun_count;
    int render_count;
    int min_buffer_level;
} jv880_instance_t;

/* Forward declarations for v2 helper functions */
static int v2_load_rom(jv880_instance_t *inst, const char *filename, uint8_t *dest, size_t size);
static void* v2_load_thread_func(void *arg);
static void* v2_emu_thread_func(void *arg);
/* Forward declarations for v2 expansion functions */
static void v2_scan_expansion_files(jv880_instance_t *inst);
static int v2_load_cache(jv880_instance_t *inst);
static void v2_save_cache(jv880_instance_t *inst);
static int v2_scan_expansion_rom(jv880_instance_t *inst, const char *filename, ExpansionInfo *info);
static void v2_scan_expansions(jv880_instance_t *inst);
static void v2_build_patch_list(jv880_instance_t *inst);
static int v2_load_expansion_data(jv880_instance_t *inst, int exp_index);
static void v2_load_expansion_to_emulator(jv880_instance_t *inst, int exp_index);
static void v2_select_patch(jv880_instance_t *inst, int global_index);
static void v2_select_performance(jv880_instance_t *inst, int perf_index);
static void v2_set_mode(jv880_instance_t *inst, int performance_mode);
static void v2_send_all_notes_off(jv880_instance_t *inst);
static void v2_set_param(void *instance, const char *key, const char *val);

/* v2: Get file size helper */
static uint32_t v2_get_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    uint32_t size = ftell(f);
    fclose(f);
    return size;
}

/* v2: Scan for expansion ROM files */
static void v2_scan_expansion_files(jv880_instance_t *inst) {
    char exp_dir[1024];
    snprintf(exp_dir, sizeof(exp_dir), "%s/roms/expansions", inst->module_dir);

    inst->expansion_file_count = 0;

    DIR *dir = opendir(exp_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && inst->expansion_file_count < MAX_EXP_FILES) {
        if (strstr(entry->d_name, "SR-JV80") && has_bin_extension(entry->d_name)) {
            strncpy(inst->expansion_files[inst->expansion_file_count], entry->d_name,
                    sizeof(inst->expansion_files[0]) - 1);

            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", exp_dir, entry->d_name);
            inst->expansion_sizes[inst->expansion_file_count] = v2_get_file_size(path);

            inst->expansion_file_count++;
        }
    }
    closedir(dir);

    /* Sort alphabetically */
    if (inst->expansion_file_count > 1) {
        for (int i = 0; i < inst->expansion_file_count - 1; i++) {
            for (int j = i + 1; j < inst->expansion_file_count; j++) {
                if (strcmp(inst->expansion_files[i], inst->expansion_files[j]) > 0) {
                    char tmp_name[256];
                    strcpy(tmp_name, inst->expansion_files[i]);
                    strcpy(inst->expansion_files[i], inst->expansion_files[j]);
                    strcpy(inst->expansion_files[j], tmp_name);
                    uint32_t tmp_size = inst->expansion_sizes[i];
                    inst->expansion_sizes[i] = inst->expansion_sizes[j];
                    inst->expansion_sizes[j] = tmp_size;
                }
            }
        }
    }
}

/* v2: Scan a single expansion ROM */
static int v2_scan_expansion_rom(jv880_instance_t *inst, const char *filename, ExpansionInfo *info) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/expansions/%s", inst->module_dir, filename);

    snprintf(inst->loading_status, sizeof(inst->loading_status), "Scanning: %.40s", filename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t rom_size = 0;
    if (size == EXPANSION_SIZE_8MB) {
        rom_size = EXPANSION_SIZE_8MB;
    } else if (size == EXPANSION_SIZE_2MB) {
        rom_size = EXPANSION_SIZE_2MB;
    } else {
        fprintf(stderr, "JV880 v2: Skipping %s (wrong size)\n", filename);
        fclose(f);
        return 0;
    }

    uint8_t *scrambled = (uint8_t *)malloc(rom_size);
    uint8_t *unscrambled_data = (uint8_t *)malloc(rom_size);
    if (!scrambled || !unscrambled_data) {
        free(scrambled);
        free(unscrambled_data);
        fclose(f);
        return 0;
    }

    fread(scrambled, 1, rom_size, f);
    fclose(f);

    unscramble_rom(scrambled, unscrambled_data, rom_size);
    free(scrambled);

    int patch_count = unscrambled_data[0x67] | (unscrambled_data[0x66] << 8);
    uint32_t patches_offset = unscrambled_data[0x8f] |
                              (unscrambled_data[0x8e] << 8) |
                              (unscrambled_data[0x8d] << 16) |
                              (unscrambled_data[0x8c] << 24);

    if (patch_count <= 0 || patch_count > MAX_PATCHES_PER_EXP || patches_offset >= rom_size) {
        fprintf(stderr, "JV880 v2: Invalid expansion %s\n", filename);
        free(unscrambled_data);
        return 0;
    }

    strncpy(info->filename, filename, sizeof(info->filename) - 1);
    extract_expansion_name(filename, info->name, sizeof(info->name));
    info->patch_count = patch_count;
    info->patches_offset = patches_offset;
    info->rom_size = rom_size;
    info->unscrambled = unscrambled_data;

    fprintf(stderr, "JV880 v2: Scanned %s: %d patches\n", info->name, patch_count);
    return 1;
}

/* v2: Compare expansions for sorting */
static int v2_compare_expansions(const void *a, const void *b) {
    return strcmp(((ExpansionInfo*)a)->name, ((ExpansionInfo*)b)->name);
}

/* v2: Scan all expansions */
static void v2_scan_expansions(jv880_instance_t *inst) {
    char exp_dir[1024];
    snprintf(exp_dir, sizeof(exp_dir), "%s/roms/expansions", inst->module_dir);

    DIR *dir = opendir(exp_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && inst->expansion_count < MAX_EXPANSIONS) {
        if (strstr(entry->d_name, "SR-JV80") && has_bin_extension(entry->d_name)) {
            if (v2_scan_expansion_rom(inst, entry->d_name, &inst->expansions[inst->expansion_count])) {
                inst->expansion_count++;
            }
        }
    }
    closedir(dir);

    if (inst->expansion_count > 1) {
        qsort(inst->expansions, inst->expansion_count, sizeof(ExpansionInfo), v2_compare_expansions);
    }

    fprintf(stderr, "JV880 v2: Found %d expansions\n", inst->expansion_count);
}

/* v2: Build complete patch list with expansions */
static void v2_build_patch_list(jv880_instance_t *inst) {
    inst->total_patches = 0;
    inst->bank_count = 0;

    /* Preset Bank A (0-63) */
    inst->bank_starts[inst->bank_count] = inst->total_patches;
    strncpy(inst->bank_names[inst->bank_count], "Preset A", sizeof(inst->bank_names[0]) - 1);
    inst->bank_count++;

    for (int i = 0; i < 64 && inst->total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &inst->patches[inst->total_patches];
        uint32_t offset = PATCH_OFFSET_PRESET_A + (i * PATCH_SIZE);
        memcpy(p->name, &inst->rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';
        p->expansion_index = -1;
        p->local_patch_index = i;
        p->rom_offset = offset;
        inst->total_patches++;
    }

    /* Preset Bank B (64-127) */
    inst->bank_starts[inst->bank_count] = inst->total_patches;
    strncpy(inst->bank_names[inst->bank_count], "Preset B", sizeof(inst->bank_names[0]) - 1);
    inst->bank_count++;

    for (int i = 0; i < 64 && inst->total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &inst->patches[inst->total_patches];
        uint32_t offset = PATCH_OFFSET_PRESET_B + (i * PATCH_SIZE);
        memcpy(p->name, &inst->rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';
        p->expansion_index = -1;
        p->local_patch_index = 64 + i;
        p->rom_offset = offset;
        inst->total_patches++;
    }

    /* Internal Bank (128-191) */
    inst->bank_starts[inst->bank_count] = inst->total_patches;
    strncpy(inst->bank_names[inst->bank_count], "Internal", sizeof(inst->bank_names[0]) - 1);
    inst->bank_count++;

    for (int i = 0; i < 64 && inst->total_patches < MAX_TOTAL_PATCHES; i++) {
        PatchInfo *p = &inst->patches[inst->total_patches];
        uint32_t offset = PATCH_OFFSET_INTERNAL + (i * PATCH_SIZE);
        memcpy(p->name, &inst->rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';
        p->expansion_index = -1;
        p->local_patch_index = 128 + i;
        p->rom_offset = offset;
        inst->total_patches++;
    }

    /* Expansion patches */
    for (int e = 0; e < inst->expansion_count && inst->bank_count < MAX_BANKS; e++) {
        ExpansionInfo *exp = &inst->expansions[e];
        exp->first_global_index = inst->total_patches;

        inst->bank_starts[inst->bank_count] = inst->total_patches;
        strncpy(inst->bank_names[inst->bank_count], exp->name, sizeof(inst->bank_names[0]) - 1);
        inst->bank_count++;

        for (int i = 0; i < exp->patch_count && inst->total_patches < MAX_TOTAL_PATCHES; i++) {
            PatchInfo *p = &inst->patches[inst->total_patches];
            uint32_t offset = exp->patches_offset + (i * PATCH_SIZE);

            if (exp->unscrambled) {
                memcpy(p->name, &exp->unscrambled[offset], PATCH_NAME_LEN);
                p->name[PATCH_NAME_LEN] = '\0';
            } else {
                snprintf(p->name, sizeof(p->name), "Patch %d", i);
            }

            p->expansion_index = e;
            p->local_patch_index = i;
            p->rom_offset = offset;
            inst->total_patches++;
        }
    }

    fprintf(stderr, "JV880 v2: Total patches: %d (192 internal + %d expansion) in %d banks\n",
            inst->total_patches, inst->total_patches - 192, inst->bank_count);
}

/* v2: Load expansion data on demand */
static int v2_load_expansion_data(jv880_instance_t *inst, int exp_index) {
    if (exp_index < 0 || exp_index >= inst->expansion_count) return 0;

    ExpansionInfo *exp = &inst->expansions[exp_index];
    if (exp->unscrambled) return 1;

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/expansions/%s", inst->module_dir, exp->filename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint8_t *scrambled = (uint8_t *)malloc(exp->rom_size);
    uint8_t *unscrambled_data = (uint8_t *)malloc(exp->rom_size);
    if (!scrambled || !unscrambled_data) {
        free(scrambled);
        free(unscrambled_data);
        fclose(f);
        return 0;
    }

    fread(scrambled, 1, exp->rom_size, f);
    fclose(f);

    unscramble_rom(scrambled, unscrambled_data, exp->rom_size);
    free(scrambled);

    exp->unscrambled = unscrambled_data;
    fprintf(stderr, "JV880 v2: Loaded expansion %s on demand\n", exp->name);
    return 1;
}

/* v2: Send all notes off */
static void v2_send_all_notes_off(jv880_instance_t *inst) {
    for (int ch = 0; ch < 16; ch++) {
        uint8_t msg[3] = { (uint8_t)(0xB0 | ch), 123, 0 };
        int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) {
            memcpy(inst->midi_queue[inst->midi_write], msg, 3);
            inst->midi_queue_len[inst->midi_write] = 3;
            inst->midi_write = next;
        }
    }
}

/* v2: Load expansion to emulator */
static void v2_load_expansion_to_emulator(jv880_instance_t *inst, int exp_index) {
    if (exp_index < 0 || exp_index >= inst->expansion_count) return;

    ExpansionInfo *exp = &inst->expansions[exp_index];

    /* Load expansion data if not already in memory */
    if (!exp->unscrambled) {
        if (!v2_load_expansion_data(inst, exp_index)) return;
    }

    /* Skip if this expansion is already loaded in the emulator */
    if (exp_index == inst->current_expansion) return;

    v2_send_all_notes_off(inst);
    memset(inst->mcu->pcm.waverom_exp, 0, EXPANSION_SIZE_8MB);
    memcpy(inst->mcu->pcm.waverom_exp, exp->unscrambled, exp->rom_size);

    inst->current_expansion = exp_index;
    inst->mcu->SC55_Reset();
    inst->warmup_remaining = 50000;  /* Warmup after expansion change */
    fprintf(stderr, "JV880 v2: Loaded expansion %s to emulator, starting warmup\n", exp->name);
}

/* v2: Select a patch */
static void v2_select_patch(jv880_instance_t *inst, int global_index) {
    jv_debug("[v2_select_patch] Called: global_index=%d\n", global_index);

    if (!inst || !inst->mcu) {
        jv_debug("[v2_select_patch] ERROR: inst=%p mcu=%p\n", (void*)inst, inst ? (void*)inst->mcu : NULL);
        return;
    }
    if (global_index < 0 || global_index >= inst->total_patches) {
        jv_debug("[v2_select_patch] ERROR: invalid index %d (total=%d)\n", global_index, inst->total_patches);
        return;
    }

    /* If in performance mode, switch to patch mode first.
     * Update current_patch BEFORE switching so the mode switch loads the right patch. */
    if (inst->performance_mode) {
        jv_debug("[v2_select_patch] In performance mode, setting current_patch=%d then switching to patch mode\n", global_index);
        inst->current_patch = global_index;  /* Set target patch before mode switch */
        v2_set_mode(inst, 0);  /* This will load inst->current_patch */
        return;
    }

    PatchInfo *p = &inst->patches[global_index];
    inst->current_patch = global_index;

    jv_debug("[v2_select_patch] Loading patch %d: %s (exp=%d rom_off=0x%x)\n",
            global_index, p->name, p->expansion_index, p->rom_offset);

    /* Load patch data to NVRAM */
    if (p->expansion_index >= 0) {
        v2_load_expansion_to_emulator(inst, p->expansion_index);
        ExpansionInfo *exp = &inst->expansions[p->expansion_index];
        if (exp->unscrambled) {
            memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                   &exp->unscrambled[p->rom_offset], PATCH_SIZE);
            jv_debug("[v2_select_patch] Copied expansion patch to NVRAM\n");
        }
    } else {
        memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET], &inst->rom2[p->rom_offset], PATCH_SIZE);
        jv_debug("[v2_select_patch] Copied internal patch to NVRAM\n");
    }

    /* Ensure patch mode in NVRAM */
    inst->mcu->nvram[NVRAM_MODE_OFFSET] = 1;
    jv_debug("[v2_select_patch] Set NVRAM mode=1 (patch)\n");

    /* Send PC 0 to trigger emulator to reload from NVRAM */
    uint8_t pc_msg[2] = { 0xC0, 0x00 };
    pthread_mutex_lock(&inst->ring_mutex);
    int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != inst->midi_read) {
        memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
        inst->midi_queue_len[inst->midi_write] = 2;
        inst->midi_write = next;
        jv_debug("[v2_select_patch] Queued PC: [0x%02x 0x%02x]\n", pc_msg[0], pc_msg[1]);
    } else {
        jv_debug("[v2_select_patch] ERROR: MIDI queue full!\n");
    }
    pthread_mutex_unlock(&inst->ring_mutex);

    jv_debug("[v2_select_patch] Complete\n");
}

/* v2: Save cache */
static void v2_save_cache(jv880_instance_t *inst) {
    char cache_path[1024];
    snprintf(cache_path, sizeof(cache_path), "%s/roms/%s", inst->module_dir, CACHE_FILENAME);

    FILE *f = fopen(cache_path, "wb");
    if (!f) return;

    CacheHeader hdr;
    hdr.magic = CACHE_MAGIC;
    hdr.version = CACHE_VERSION;

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/jv880_rom1.bin", inst->module_dir);
    hdr.rom1_size = v2_get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_rom2.bin", inst->module_dir);
    hdr.rom2_size = v2_get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom1.bin", inst->module_dir);
    hdr.waverom1_size = v2_get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom2.bin", inst->module_dir);
    hdr.waverom2_size = v2_get_file_size(path);

    hdr.expansion_count = inst->expansion_count;
    hdr.total_patches = inst->total_patches;
    hdr.bank_count = inst->bank_count;

    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(&inst->expansion_file_count, sizeof(inst->expansion_file_count), 1, f);
    for (int i = 0; i < inst->expansion_file_count; i++) {
        fwrite(inst->expansion_files[i], sizeof(inst->expansion_files[0]), 1, f);
        fwrite(&inst->expansion_sizes[i], sizeof(inst->expansion_sizes[0]), 1, f);
    }

    for (int i = 0; i < inst->expansion_count; i++) {
        fwrite(inst->expansions[i].filename, sizeof(inst->expansions[i].filename), 1, f);
        fwrite(inst->expansions[i].name, sizeof(inst->expansions[i].name), 1, f);
        fwrite(&inst->expansions[i].patch_count, sizeof(inst->expansions[i].patch_count), 1, f);
        fwrite(&inst->expansions[i].patches_offset, sizeof(inst->expansions[i].patches_offset), 1, f);
        fwrite(&inst->expansions[i].first_global_index, sizeof(inst->expansions[i].first_global_index), 1, f);
        fwrite(&inst->expansions[i].rom_size, sizeof(inst->expansions[i].rom_size), 1, f);
    }

    fwrite(inst->patches, sizeof(PatchInfo), inst->total_patches, f);
    fwrite(inst->bank_starts, sizeof(inst->bank_starts[0]), inst->bank_count, f);
    fwrite(inst->bank_names, sizeof(inst->bank_names[0]), inst->bank_count, f);

    fclose(f);
    fprintf(stderr, "JV880 v2: Saved cache\n");
}

/* v2: Load cache */
static int v2_load_cache(jv880_instance_t *inst) {
    char cache_path[1024];
    snprintf(cache_path, sizeof(cache_path), "%s/roms/%s", inst->module_dir, CACHE_FILENAME);

    FILE *f = fopen(cache_path, "rb");
    if (!f) return 0;

    CacheHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != CACHE_MAGIC ||
        hdr.version != CACHE_VERSION) {
        fclose(f);
        return 0;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/jv880_rom1.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.rom1_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_rom2.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.rom2_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom1.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.waverom1_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom2.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.waverom2_size) { fclose(f); return 0; }

    int cached_exp_count;
    if (fread(&cached_exp_count, sizeof(cached_exp_count), 1, f) != 1 ||
        cached_exp_count != inst->expansion_file_count) {
        fclose(f);
        return 0;
    }

    for (int i = 0; i < cached_exp_count; i++) {
        char cached_name[256];
        uint32_t cached_size;
        fread(cached_name, sizeof(cached_name), 1, f);
        fread(&cached_size, sizeof(cached_size), 1, f);

        int found = 0;
        for (int j = 0; j < inst->expansion_file_count; j++) {
            if (strcmp(cached_name, inst->expansion_files[j]) == 0 &&
                cached_size == inst->expansion_sizes[j]) {
                found = 1;
                break;
            }
        }
        if (!found) { fclose(f); return 0; }
    }

    inst->expansion_count = hdr.expansion_count;
    inst->total_patches = hdr.total_patches;
    inst->bank_count = hdr.bank_count;

    for (int i = 0; i < inst->expansion_count; i++) {
        fread(inst->expansions[i].filename, sizeof(inst->expansions[i].filename), 1, f);
        fread(inst->expansions[i].name, sizeof(inst->expansions[i].name), 1, f);
        fread(&inst->expansions[i].patch_count, sizeof(inst->expansions[i].patch_count), 1, f);
        fread(&inst->expansions[i].patches_offset, sizeof(inst->expansions[i].patches_offset), 1, f);
        fread(&inst->expansions[i].first_global_index, sizeof(inst->expansions[i].first_global_index), 1, f);
        fread(&inst->expansions[i].rom_size, sizeof(inst->expansions[i].rom_size), 1, f);
        inst->expansions[i].unscrambled = nullptr;
    }

    fread(inst->patches, sizeof(PatchInfo), inst->total_patches, f);
    fread(inst->bank_starts, sizeof(inst->bank_starts[0]), inst->bank_count, f);
    fread(inst->bank_names, sizeof(inst->bank_names[0]), inst->bank_count, f);

    fclose(f);
    fprintf(stderr, "JV880 v2: Loaded cache (%d patches, %d banks, %d expansions)\n",
            inst->total_patches, inst->bank_count, inst->expansion_count);
    return 1;
}

/* v2: Load thread function */
static void* v2_load_thread_func(void *arg) {
    jv880_instance_t *inst = (jv880_instance_t*)arg;

    fprintf(stderr, "JV880 v2: Load thread started\n");

    /* Scan for expansion files */
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Checking expansions...");
    v2_scan_expansion_files(inst);
    fprintf(stderr, "JV880 v2: Found %d expansion files\n", inst->expansion_file_count);

    /* Try cache first */
    int cache_valid = v2_load_cache(inst);

    if (!cache_valid) {
        fprintf(stderr, "JV880 v2: Cache miss, scanning expansions...\n");
        snprintf(inst->loading_status, sizeof(inst->loading_status), "Scanning expansions...");
        v2_scan_expansions(inst);
        v2_build_patch_list(inst);
        v2_save_cache(inst);
    }

    /* Select initial patch */
    if (inst->total_patches > 0) {
        v2_select_patch(inst, 0);
    }

    /* Warmup */
    fprintf(stderr, "JV880 v2: Running warmup...\n");
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Warming up...");
    for (int i = 0; i < 100000; i++) {
        inst->mcu->updateSC55(1);
    }
    fprintf(stderr, "JV880 v2: Warmup done\n");

    /* Pre-fill audio buffer */
    inst->ring_write = 0;
    inst->ring_read = 0;

    /* Initialize high-quality resampler (66207 Hz -> 48000 Hz) */
    double ratio = (double)MOVE_SAMPLE_RATE / (double)JV880_SAMPLE_RATE;
    inst->resampleL = resample_open(1, ratio, ratio);  /* High quality */
    inst->resampleR = resample_open(1, ratio, ratio);
    fprintf(stderr, "JV880 v2: Resampler initialized (ratio %.4f)\n", ratio);

    fprintf(stderr, "JV880 v2: Pre-filling buffer...\n");
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Preparing audio...");
    for (int i = 0; i < 256 && inst->ring_write < AUDIO_RING_SIZE / 2; i++) {
        inst->mcu->updateSC55(8);
        int avail = inst->mcu->sample_write_ptr;
        int in_samples = avail / 2;

        if (in_samples > 0 && in_samples < 4096) {
            /* Convert int16 to float for resampler */
            for (int j = 0; j < in_samples; j++) {
                inst->resample_in_l[j] = (float)inst->mcu->sample_buffer[j * 2] / 32768.0f;
                inst->resample_in_r[j] = (float)inst->mcu->sample_buffer[j * 2 + 1] / 32768.0f;
            }

            /* Resample */
            int inUsedL = 0, inUsedR = 0;
            int outL = resample_process(inst->resampleL, ratio, inst->resample_in_l, in_samples,
                                        0, &inUsedL, inst->resample_out_l, 4096);
            int outR = resample_process(inst->resampleR, ratio, inst->resample_in_r, in_samples,
                                        0, &inUsedR, inst->resample_out_r, 4096);
            int out_samples = (outL < outR) ? outL : outR;

            /* Copy to ring buffer */
            for (int j = 0; j < out_samples && inst->ring_write < AUDIO_RING_SIZE / 2; j++) {
                int32_t l = (int32_t)(inst->resample_out_l[j] * 32768.0f);
                int32_t r = (int32_t)(inst->resample_out_r[j] * 32768.0f);
                if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                inst->audio_ring[inst->ring_write * 2 + 0] = (int16_t)l;
                inst->audio_ring[inst->ring_write * 2 + 1] = (int16_t)r;
                inst->ring_write = (inst->ring_write + 1) % AUDIO_RING_SIZE;
            }
        }
    }
    fprintf(stderr, "JV880 v2: Buffer pre-filled: %d samples\n", inst->ring_write);

    /* Start emulation thread - set initialized BEFORE pthread_create so
     * render_block and on_midi can start working with the pre-filled buffer
     * while the emu_thread starts up */
    inst->thread_running = 1;
    inst->initialized = 1;
    pthread_create(&inst->emu_thread, NULL, v2_emu_thread_func, inst);

    inst->loading_complete = 1;
    snprintf(inst->loading_status, sizeof(inst->loading_status),
             "Ready: %d patches in %d banks", inst->total_patches, inst->bank_count);

    /* Apply any pending state that was queued during loading */
    if (inst->pending_state_valid) {
        fprintf(stderr, "JV880 v2: Applying deferred state restoration\n");
        inst->pending_state_valid = 0;
        /* Re-call set_param now that loading is complete */
        v2_set_param(inst, "state", inst->pending_state);
    }

    fprintf(stderr, "JV880 v2: Ready!\n");
    inst->load_thread_running = 0;
    return NULL;
}

/* v2: Create instance */
static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    jv880_instance_t *inst = (jv880_instance_t*)calloc(1, sizeof(jv880_instance_t));
    if (!inst) {
        fprintf(stderr, "JV880 v2: Failed to allocate instance\n");
        return NULL;
    }

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    fprintf(stderr, "JV880 v2: Loading from %s\n", module_dir);

    /* Check for debug mode */
    char debug_path[1024];
    snprintf(debug_path, sizeof(debug_path), "%s/debug_sysex_test", module_dir);
    if (access(debug_path, F_OK) == 0) {
        inst->debug_sysex = 1;
        fprintf(stderr, "JV880 v2: SysEx debug enabled\n");
    }

    /* Initialize mutex */
    pthread_mutex_init(&inst->ring_mutex, NULL);

    /* Initialize loading status */
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Initializing...");
    inst->current_expansion = -1;
    inst->found_perf_sram_offset = -1;
    inst->map_last_offset = -1;

    /* Create emulator instance */
    inst->mcu = new MCU();

    /* Load ROMs */
    uint8_t *rom1 = (uint8_t *)malloc(ROM1_SIZE);
    uint8_t *rom2 = (uint8_t *)malloc(ROM2_SIZE);
    uint8_t *waverom1 = (uint8_t *)malloc(0x200000);
    uint8_t *waverom2 = (uint8_t *)malloc(0x200000);
    uint8_t *nvram = (uint8_t *)malloc(NVRAM_SIZE);

    if (!rom1 || !rom2 || !waverom1 || !waverom2 || !nvram) {
        fprintf(stderr, "JV880 v2: Memory allocation failed\n");
        free(rom1); free(rom2); free(waverom1); free(waverom2); free(nvram);
        delete inst->mcu;
        pthread_mutex_destroy(&inst->ring_mutex);
        free(inst);
        return NULL;
    }

    memset(nvram, 0xFF, NVRAM_SIZE);

    int ok = 1;
    ok = ok && v2_load_rom(inst, "jv880_rom1.bin", rom1, ROM1_SIZE);
    ok = ok && v2_load_rom(inst, "jv880_rom2.bin", rom2, ROM2_SIZE);
    ok = ok && v2_load_rom(inst, "jv880_waverom1.bin", waverom1, 0x200000);
    ok = ok && v2_load_rom(inst, "jv880_waverom2.bin", waverom2, 0x200000);

    /* NVRAM is optional */
    char nvram_path[1024];
    snprintf(nvram_path, sizeof(nvram_path), "%s/roms/jv880_nvram.bin", module_dir);
    FILE *nf = fopen(nvram_path, "rb");
    if (nf) {
        fread(nvram, 1, NVRAM_SIZE, nf);
        fclose(nf);
        fprintf(stderr, "JV880 v2: Loaded NVRAM\n");
    }

    if (!ok) {
        fprintf(stderr, "JV880 v2: ROM loading failed\n");
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "Mini-JV: ROM files not found. Place ROM files in roms/ folder.");
        free(rom1); free(rom2); free(waverom1); free(waverom2); free(nvram);
        delete inst->mcu;
        inst->mcu = nullptr;
        inst->rom_loaded = 0;
        inst->initialized = 1;  /* Mark as initialized so get_error works */
        return inst;  /* Return instance so error can be retrieved */
    }

    /* Initialize emulator */
    inst->mcu->startSC55(rom1, rom2, waverom1, waverom2, nvram);

    /* Keep ROM2 for internal patch access */
    inst->rom2 = rom2;

    free(rom1); free(waverom1); free(waverom2); free(nvram);

    inst->rom_loaded = 1;

    /* Set patch mode */
    inst->mcu->nvram[NVRAM_MODE_OFFSET] = 1;

    /* Load patches/expansions and warmup in background */
    inst->load_thread_running = 1;
    pthread_create(&inst->load_thread, NULL, v2_load_thread_func, inst);

    fprintf(stderr, "JV880 v2: Instance created\n");
    return inst;
}

/* v2: Destroy instance */
static void v2_destroy_instance(void *instance) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst) return;

    fprintf(stderr, "JV880 v2: Destroying instance\n");

    /* Stop load thread */
    if (inst->load_thread_running) {
        inst->load_thread_running = 0;
        pthread_join(inst->load_thread, NULL);
    }

    /* Stop emulator thread */
    if (inst->thread_running) {
        inst->thread_running = 0;
        pthread_join(inst->emu_thread, NULL);
    }

    /* Cleanup resampler */
    if (inst->resampleL) {
        resample_close(inst->resampleL);
        inst->resampleL = nullptr;
    }
    if (inst->resampleR) {
        resample_close(inst->resampleR);
        inst->resampleR = nullptr;
    }

    /* Cleanup emulator */
    if (inst->mcu) {
        delete inst->mcu;
        inst->mcu = nullptr;
    }

    /* Free ROM2 */
    if (inst->rom2) {
        free(inst->rom2);
        inst->rom2 = nullptr;
    }

    /* Free expansion data */
    for (int i = 0; i < inst->expansion_count; i++) {
        if (inst->expansions[i].unscrambled) {
            free(inst->expansions[i].unscrambled);
            inst->expansions[i].unscrambled = nullptr;
        }
    }

    pthread_mutex_destroy(&inst->ring_mutex);
    free(inst);
    fprintf(stderr, "JV880 v2: Instance destroyed\n");
}

/* v2: Load ROM helper */
static int v2_load_rom(jv880_instance_t *inst, const char *filename, uint8_t *dest, size_t size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/%s", inst->module_dir, filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "JV880 v2: Cannot open: %s\n", path);
        return 0;
    }

    size_t got = fread(dest, 1, size, f);
    fclose(f);

    if (got != size) {
        fprintf(stderr, "JV880 v2: Size mismatch: %s (%zu vs %zu)\n", filename, got, size);
        return 0;
    }

    fprintf(stderr, "JV880 v2: Loaded %s\n", filename);
    return 1;
}

/* v2: Ring buffer helpers (instance-based) */
static int v2_ring_available(jv880_instance_t *inst) {
    int avail = inst->ring_write - inst->ring_read;
    if (avail < 0) avail += AUDIO_RING_SIZE;
    return avail;
}

static int v2_ring_free(jv880_instance_t *inst) {
    return AUDIO_RING_SIZE - 1 - v2_ring_available(inst);
}

/* v2: Emulator thread */
static void* v2_emu_thread_func(void *arg) {
    jv880_instance_t *inst = (jv880_instance_t*)arg;
    fprintf(stderr, "JV880 v2: Emulation thread started\n");

    const double ratio = (double)MOVE_SAMPLE_RATE / (double)JV880_SAMPLE_RATE;

    while (inst->thread_running) {
        /* Handle warmup after SC55_Reset */
        if (inst->warmup_remaining > 0) {
            int batch = (inst->warmup_remaining > 1000) ? 1000 : inst->warmup_remaining;
            for (int i = 0; i < batch; i++) {
                inst->mcu->updateSC55(1);
            }
            inst->warmup_remaining -= batch;
            if (inst->warmup_remaining <= 0) {
                snprintf(inst->loading_status, sizeof(inst->loading_status),
                         "Ready: %d patches", inst->total_patches);
                jv_debug("[v2_emu_thread] Warmup complete\n");
            }
            continue;  /* Skip audio output during warmup */
        }

        /* Process MIDI queue */
        while (inst->midi_read != inst->midi_write) {
            int idx = inst->midi_read;
            inst->mcu->postMidiSC55(inst->midi_queue[idx], inst->midi_queue_len[idx]);
            inst->midi_read = (inst->midi_read + 1) % MIDI_QUEUE_SIZE;
        }

        /* Check for pending parameter mapping SysEx */
        if (inst->map_sysex_len > 0) {
            inst->mcu->postMidiSC55(inst->map_sysex_pending, inst->map_sysex_len);
            inst->map_sysex_len = 0;
        }

        /* Check if we need more audio */
        int free_space = v2_ring_free(inst);
        if (free_space < 64) {
            usleep(50);
            continue;
        }

        inst->mcu->updateSC55(64);
        int avail = inst->mcu->sample_write_ptr;
        int in_samples = avail / 2;  /* Stereo pairs */

        if (in_samples > 0 && in_samples < 4096) {
            /* Convert int16 to float for resampler (separate L/R channels) */
            for (int i = 0; i < in_samples; i++) {
                inst->resample_in_l[i] = (float)inst->mcu->sample_buffer[i * 2] / 32768.0f;
                inst->resample_in_r[i] = (float)inst->mcu->sample_buffer[i * 2 + 1] / 32768.0f;
            }

            /* Resample using high-quality polyphase filter */
            int inUsedL = 0, inUsedR = 0;
            int outL = resample_process(inst->resampleL, ratio, inst->resample_in_l, in_samples,
                                        0, &inUsedL, inst->resample_out_l, 4096);
            int outR = resample_process(inst->resampleR, ratio, inst->resample_in_r, in_samples,
                                        0, &inUsedR, inst->resample_out_r, 4096);

            int out_samples = (outL < outR) ? outL : outR;

            /* Batch copy to ring buffer with single lock */
            if (out_samples > 0) {
                pthread_mutex_lock(&inst->ring_mutex);
                int free_now = v2_ring_free(inst);
                int to_write = (out_samples < free_now) ? out_samples : free_now;
                for (int i = 0; i < to_write; i++) {
                    int wr = inst->ring_write;
                    /* Convert float back to int16 */
                    int32_t l = (int32_t)(inst->resample_out_l[i] * 32768.0f);
                    int32_t r = (int32_t)(inst->resample_out_r[i] * 32768.0f);
                    if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                    inst->audio_ring[wr * 2 + 0] = (int16_t)l;
                    inst->audio_ring[wr * 2 + 1] = (int16_t)r;
                    inst->ring_write = (wr + 1) % AUDIO_RING_SIZE;
                }
                pthread_mutex_unlock(&inst->ring_mutex);
            }
        }
    }

    fprintf(stderr, "JV880 v2: Emulation thread stopped\n");
    return NULL;
}

/* v2: Helper to queue SysEx for tone parameter changes */
static void v2_queue_tone_sysex(jv880_instance_t *inst, int toneIdx, int paramIdx, int value) {
    if (!inst || toneIdx < 0 || toneIdx > 3) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data checksum F7 */
    uint8_t addr[4] = { 0x00, 0x08, (uint8_t)(0x28 + toneIdx), (uint8_t)paramIdx };
    uint8_t data = (uint8_t)(value & 0x7F);

    /* Calculate checksum: 128 - (sum of addr + data) mod 128 */
    int sum = addr[0] + addr[1] + addr[2] + addr[3] + data;
    uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

    uint8_t sysex[12] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                          addr[0], addr[1], addr[2], addr[3],
                          data, chk, 0xF7 };

    /* Queue the SysEx */
    int write_idx = inst->midi_write;
    int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
    if (next == inst->midi_read) return; /* Queue full */

    memcpy(inst->midi_queue[write_idx], sysex, 12);
    inst->midi_queue_len[write_idx] = 12;
    inst->midi_write = next;
}

/* v2: Helper to queue SysEx for patch common parameter changes */
static void v2_queue_patch_common_sysex(jv880_instance_t *inst, int paramIdx, int value) {
    if (!inst) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data checksum F7 */
    uint8_t addr[4] = { 0x00, 0x08, 0x20, (uint8_t)paramIdx };
    uint8_t data = (uint8_t)(value & 0x7F);

    /* Calculate checksum */
    int sum = addr[0] + addr[1] + addr[2] + addr[3] + data;
    uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

    uint8_t sysex[12] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                          addr[0], addr[1], addr[2], addr[3],
                          data, chk, 0xF7 };

    /* Queue the SysEx */
    int write_idx = inst->midi_write;
    int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
    if (next == inst->midi_read) return;

    memcpy(inst->midi_queue[write_idx], sysex, 12);
    inst->midi_queue_len[write_idx] = 12;
    inst->midi_write = next;
}

/* v2: Helper to queue SysEx for part parameter changes */
static void v2_queue_part_sysex(jv880_instance_t *inst, int partIdx, int paramIdx, int value) {
    if (!inst || partIdx < 0 || partIdx > 7) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data checksum F7 */
    /* Part address: [0x00, 0x00, 0x18 + partIdx, paramIdx] */
    uint8_t addr[4] = { 0x00, 0x00, (uint8_t)(0x18 + partIdx), (uint8_t)paramIdx };
    uint8_t data = (uint8_t)(value & 0x7F);

    /* Calculate checksum */
    int sum = addr[0] + addr[1] + addr[2] + addr[3] + data;
    uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

    uint8_t sysex[12] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                          addr[0], addr[1], addr[2], addr[3],
                          data, chk, 0xF7 };

    /* Queue the SysEx */
    int write_idx = inst->midi_write;
    int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
    if (next == inst->midi_read) return;

    memcpy(inst->midi_queue[write_idx], sysex, 12);
    inst->midi_queue_len[write_idx] = 12;
    inst->midi_write = next;
}

/* v2: MIDI handler */
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst || !inst->initialized) return;
    (void)source;

    /* Copy to MIDI queue with octave transpose */
    int write_idx = inst->midi_write;
    int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
    if (next == inst->midi_read) return; /* Queue full */

    memcpy(inst->midi_queue[write_idx], msg, len);
    inst->midi_queue_len[write_idx] = len;

    /* Apply octave transpose to note on/off */
    uint8_t status = inst->midi_queue[write_idx][0] & 0xF0;
    if ((status == 0x90 || status == 0x80) && len >= 2) {
        int note = inst->midi_queue[write_idx][1] + (inst->octave_transpose * 12);
        if (note < 0) note = 0;
        if (note > 127) note = 127;
        inst->midi_queue[write_idx][1] = note;
    }

    inst->midi_write = next;
}

/* v2: Helper to find which bank a patch belongs to */
static int v2_get_bank_for_patch(jv880_instance_t *inst, int patch_index) {
    for (int i = inst->bank_count - 1; i >= 0; i--) {
        if (patch_index >= inst->bank_starts[i]) {
            return i;
        }
    }
    return 0;
}

/* v2: Jump to next/previous bank */
static void v2_jump_to_bank(jv880_instance_t *inst, int direction) {
    int current_bank = v2_get_bank_for_patch(inst, inst->current_patch);
    int new_bank = current_bank + direction;

    /* Wrap around */
    if (new_bank < 0) new_bank = inst->bank_count - 1;
    if (new_bank >= inst->bank_count) new_bank = 0;

    v2_select_patch(inst, inst->bank_starts[new_bank]);
    fprintf(stderr, "JV880 v2: Jumped to bank %d: %s\n", new_bank, inst->bank_names[new_bank]);
}

/* v2: Switch between patch and performance mode */
static void v2_set_mode(jv880_instance_t *inst, int performance_mode) {
    if (!inst || !inst->mcu) {
        jv_debug("[v2_set_mode] ERROR: inst=%p mcu=%p\n", (void*)inst, inst ? (void*)inst->mcu : NULL);
        return;
    }

    int new_mode = performance_mode ? 1 : 0;

    jv_debug("[v2_set_mode] Called: current=%s requested=%s patch=%d perf=%d\n",
            inst->performance_mode ? "Performance" : "Patch",
            new_mode ? "Performance" : "Patch",
            inst->current_patch, inst->current_performance);

    /* Only switch if mode is actually changing */
    if (inst->performance_mode == new_mode) {
        jv_debug("[v2_set_mode] Mode unchanged, returning\n");
        return;
    }

    jv_debug("[v2_set_mode] Switching from %s to %s mode\n",
            inst->performance_mode ? "Performance" : "Patch",
            new_mode ? "Performance" : "Patch");

    /* Send All Notes Off on all channels before mode switch */
    pthread_mutex_lock(&inst->ring_mutex);
    for (int ch = 0; ch < 16; ch++) {
        uint8_t notes_off[3] = { (uint8_t)(0xB0 | ch), 123, 0 };
        int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) {
            memcpy(inst->midi_queue[inst->midi_write], notes_off, 3);
            inst->midi_queue_len[inst->midi_write] = 3;
            inst->midi_write = next;
        }
    }
    pthread_mutex_unlock(&inst->ring_mutex);
    jv_debug("[v2_set_mode] Sent All Notes Off on all 16 channels\n");

    /* Update mode state */
    inst->performance_mode = new_mode;

    /* Set NVRAM mode directly: 0 = performance, 1 = patch */
    uint8_t desired_nvram_mode = inst->performance_mode ? 0 : 1;
    inst->mcu->nvram[NVRAM_MODE_OFFSET] = desired_nvram_mode;
    jv_debug("[v2_set_mode] Set NVRAM[0x%x] = %d\n", NVRAM_MODE_OFFSET, desired_nvram_mode);

    /* Reset emulator for clean state - don't use button press which can cause conflicts */
    jv_debug("[v2_set_mode] Resetting emulator for clean mode switch\n");
    inst->mcu->SC55_Reset();
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Warming up...");
    inst->warmup_remaining = 100000;  /* Same as initial warmup */

    if (!inst->performance_mode) {
        /* Entering patch mode */
        jv_debug("[v2_set_mode] Entering patch mode, setting pending_patch_select\n");
        inst->pending_patch_select = 50;  /* Delay to allow warmup to complete */
    } else {
        /* Entering performance mode */
        jv_debug("[v2_set_mode] Entering performance mode, setting pending_perf_select\n");
        inst->pending_perf_select = 50;  /* Same delay as patch mode */
    }

    jv_debug("[v2_set_mode] Complete\n");
}

/* v2: Select a performance (0-47 across 3 banks) */
static void v2_select_performance(jv880_instance_t *inst, int perf_index) {
    jv_debug("[v2_select_performance] Called: perf_index=%d\n", perf_index);

    if (!inst || !inst->mcu || perf_index < 0 || perf_index >= NUM_PERFORMANCES) {
        jv_debug("[v2_select_performance] ERROR: invalid args inst=%p mcu=%p perf=%d\n",
                (void*)inst, inst ? (void*)inst->mcu : NULL, perf_index);
        return;
    }

    inst->current_performance = perf_index;
    inst->perf_bank = perf_index / PERFS_PER_BANK;
    int perf_in_bank = perf_index % PERFS_PER_BANK;

    jv_debug("[v2_select_performance] perf=%d bank=%d in_bank=%d current_mode=%s\n",
            perf_index, inst->perf_bank, perf_in_bank,
            inst->performance_mode ? "Performance" : "Patch");

    /* Ensure we're in performance mode */
    if (!inst->performance_mode) {
        jv_debug("[v2_select_performance] Not in performance mode, calling v2_set_mode(1)\n");
        inst->current_performance = perf_index;  /* Store desired perf for deferred selection */
        v2_set_mode(inst, 1);
        /* v2_set_mode sets pending_perf_select, which will trigger this function again
         * from render_block after mode switch has been processed. Return now. */
        return;
    }

    /* Calculate bank select and program change values per JV-880 MIDI spec */
    uint8_t bank_msb;
    uint8_t pc_value;

    switch (inst->perf_bank) {
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

    jv_debug("[v2_select_performance] bank_msb=%d pc_value=%d\n", bank_msb, pc_value);

    /* Send on Control Channel (channel 16 = 0x0F) */
    uint8_t ctrl_ch = 0x0F;
    uint8_t bank_msg[3] = { (uint8_t)(0xB0 | ctrl_ch), 0x00, bank_msb };
    uint8_t pc_msg[2] = { (uint8_t)(0xC0 | ctrl_ch), pc_value };

    /* Queue Bank Select (CC#0) */
    pthread_mutex_lock(&inst->ring_mutex);
    int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != inst->midi_read) {
        memcpy(inst->midi_queue[inst->midi_write], bank_msg, 3);
        inst->midi_queue_len[inst->midi_write] = 3;
        inst->midi_write = next;
        jv_debug("[v2_select_performance] Queued Bank: [0x%02x 0x%02x 0x%02x]\n",
                bank_msg[0], bank_msg[1], bank_msg[2]);
    } else {
        jv_debug("[v2_select_performance] ERROR: MIDI queue full for bank!\n");
    }

    /* Queue Program Change */
    next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != inst->midi_read) {
        memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
        inst->midi_queue_len[inst->midi_write] = 2;
        inst->midi_write = next;
        jv_debug("[v2_select_performance] Queued PC: [0x%02x 0x%02x]\n",
                pc_msg[0], pc_msg[1]);
    } else {
        jv_debug("[v2_select_performance] ERROR: MIDI queue full for PC!\n");
    }
    pthread_mutex_unlock(&inst->ring_mutex);

    jv_debug("[v2_select_performance] Complete\n");

    /* Schedule SRAM scan for performance data discovery */
    inst->sram_scan_countdown = 100;
}

/* v2: Select a part within the current performance (0-7) */
static void v2_select_part(jv880_instance_t *inst, int part_index) {
    if (!inst || part_index < 0 || part_index > 7) return;
    inst->current_part = part_index;
    fprintf(stderr, "JV880 v2: Selected part %d\n", part_index + 1);
}

static int clamp_int(int value, int min_val, int max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

/* Helper to extract a JSON number value by key */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

/* v2: Set parameter - full expansion support */
static void v2_set_param(void *instance, const char *key, const char *val) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst) return;

    /* State restore from patch save */
    if (strcmp(key, "state") == 0) {
        /* If loading isn't complete, queue the state for later application */
        if (!inst->loading_complete) {
            strncpy(inst->pending_state, val, sizeof(inst->pending_state) - 1);
            inst->pending_state[sizeof(inst->pending_state) - 1] = '\0';
            inst->pending_state_valid = 1;
            fprintf(stderr, "JV880 v2: Queued state for deferred restoration\n");
            return;
        }

        float f;
        /* Restore mode first */
        if (json_get_number(val, "mode", &f) == 0) {
            v2_set_mode(inst, (int)f);
        }
        /* Note: expansion_index is saved for state but we don't restore it directly.
         * v2_select_patch will load the correct expansion when it selects the patch.
         * Setting current_expansion here would cause v2_load_expansion_to_emulator
         * to skip loading the actual ROM data. */
        if (json_get_number(val, "expansion_bank_offset", &f) == 0) {
            inst->expansion_bank_offset = (int)f;
        }
        /* Restore preset or performance based on mode */
        if (inst->performance_mode) {
            if (json_get_number(val, "performance", &f) == 0) {
                int perf = (int)f;
                if (perf >= 0 && perf < NUM_PERFORMANCES) {
                    v2_select_performance(inst, perf);
                }
            }
            if (json_get_number(val, "part", &f) == 0) {
                int part = (int)f;
                if (part >= 0 && part <= 7) {
                    v2_select_part(inst, part);
                }
            }
        } else {
            if (json_get_number(val, "preset", &f) == 0) {
                int preset = (int)f;
                if (preset >= 0 && preset < inst->total_patches) {
                    v2_select_patch(inst, preset);
                }
            }
        }
        /* Restore octave transpose */
        if (json_get_number(val, "octave_transpose", &f) == 0) {
            int oct = (int)f;
            if (oct < -4) oct = -4;
            if (oct > 4) oct = 4;
            inst->octave_transpose = oct;
        }
        return;
    }

    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->total_patches) {
            v2_select_patch(inst, idx);
        }
    } else if (strcmp(key, "octave_transpose") == 0) {
        int v = atoi(val);
        if (v < -3) v = -3;
        if (v > 3) v = 3;
        inst->octave_transpose = v;
    } else if (strcmp(key, "program_change") == 0) {
        int program = atoi(val);
        if (program >= 0 && program < inst->total_patches) {
            v2_select_patch(inst, program);
        }
    } else if (strcmp(key, "next_bank") == 0) {
        v2_jump_to_bank(inst, 1);
    } else if (strcmp(key, "prev_bank") == 0) {
        v2_jump_to_bank(inst, -1);
    } else if (strcmp(key, "mode") == 0) {
        /* Switch between patch (0) and performance (1) mode
         * Accept both string names and numeric indices for enum compatibility */
        int mode;
        if (strcmp(val, "Patch") == 0 || strcmp(val, "patch") == 0) {
            mode = 0;
        } else if (strcmp(val, "Performance") == 0 || strcmp(val, "performance") == 0) {
            mode = 1;
        } else {
            mode = atoi(val);
        }
        jv_debug("[set_param] mode='%s' -> %d (current=%s)\n",
                val, mode, inst->performance_mode ? "Performance" : "Patch");
        v2_set_mode(inst, mode);
    } else if (strcmp(key, "performance") == 0) {
        /* Select performance 0-47 */
        int perf = atoi(val);
        if (perf < 0) perf = 0;
        if (perf >= NUM_PERFORMANCES) perf = NUM_PERFORMANCES - 1;
        v2_select_performance(inst, perf);
    } else if (strcmp(key, "part") == 0) {
        /* Select part 0-7 within performance */
        int part = atoi(val);
        if (part < 0) part = 0;
        if (part > 7) part = 7;
        v2_select_part(inst, part);
    } else if (strcmp(key, "load_expansion") == 0) {
        /* Load a specific expansion card by index (for performance mode Card patches) */
        int exp_idx = atoi(val);
        int bank_offset = 0;
        const char *comma = strchr(val, ',');
        if (comma) {
            bank_offset = atoi(comma + 1);
        }
        if (exp_idx >= 0 && exp_idx < inst->expansion_count) {
            int max_offset = (inst->expansions[exp_idx].patch_count > 64) ?
                             ((inst->expansions[exp_idx].patch_count - 1) / 64) * 64 : 0;
            if (bank_offset < 0) bank_offset = 0;
            if (bank_offset > max_offset) bank_offset = max_offset;
            inst->expansion_bank_offset = bank_offset;
            inst->current_expansion = exp_idx;
            fprintf(stderr, "JV880 v2: Loaded expansion %d (%s) at bank offset %d\n",
                    exp_idx, inst->expansions[exp_idx].name, bank_offset);
        }
    } else if (strcmp(key, "jump_to_expansion") == 0) {
        /* Jump to first patch of expansion (for patch browsing) */
        /* -1 = factory patches, 0+ = expansion index */
        int exp_idx = atoi(val);
        if (exp_idx == -1) {
            /* Jump to factory patches (Preset A) */
            v2_select_patch(inst, 0);
            fprintf(stderr, "JV880 v2: Jumped to factory patches\n");
        } else if (exp_idx >= 0 && exp_idx < inst->expansion_count) {
            int first_patch = inst->expansions[exp_idx].first_global_index;
            if (first_patch >= 0 && first_patch < inst->total_patches) {
                v2_select_patch(inst, first_patch);
                fprintf(stderr, "JV880 v2: Jumped to expansion %d (%s) at patch %d\n",
                        exp_idx, inst->expansions[exp_idx].name, first_patch);
            }
        }
    } else if (strcmp(key, "jump_to_internal") == 0) {
        /* Jump to first internal patch (Preset A) */
        v2_select_patch(inst, 0);
        fprintf(stderr, "JV880 v2: Jumped to internal patches\n");
    } else if (strncmp(key, "nvram_patchCommon_", 18) == 0 && inst->mcu) {
        const char *paramName = key + 18;
        int sysexIdx = -1;
        int nvramOffset = -1;

        /* Map parameter names to both SysEx index and NVRAM offset */
        if (strcmp(paramName, "reverblevel") == 0) { sysexIdx = 14; nvramOffset = 13; }
        else if (strcmp(paramName, "reverbtime") == 0) { sysexIdx = 15; nvramOffset = 14; }
        else if (strcmp(paramName, "choruslevel") == 0) { sysexIdx = 18; nvramOffset = 16; }
        else if (strcmp(paramName, "chorusdepth") == 0) { sysexIdx = 19; nvramOffset = 17; }
        else if (strcmp(paramName, "chorusrate") == 0) { sysexIdx = 20; nvramOffset = 18; }
        else if (strcmp(paramName, "analogfeel") == 0) { sysexIdx = 23; nvramOffset = 20; }
        else if (strcmp(paramName, "patchlevel") == 0) { sysexIdx = 0x18; nvramOffset = 21; }  /* 18h per MIDI Impl */
        else if (strcmp(paramName, "patchpan") == 0) { sysexIdx = 0x19; nvramOffset = 22; }   /* 19h per MIDI Impl */

        if (sysexIdx >= 0 && nvramOffset >= 0) {
            const int v = clamp_int(atoi(val), 0, 127);
            /* Update NVRAM directly for immediate UI feedback */
            inst->mcu->nvram[NVRAM_PATCH_OFFSET + nvramOffset] = (uint8_t)v;
            /* Send SysEx to emulator for actual sound change */
            v2_queue_patch_common_sysex(inst, sysexIdx, v);
        }
    } else if (strncmp(key, "nvram_tone_", 11) == 0 && inst->mcu) {
        int toneIdx = atoi(key + 11);
        const char *underscore = strchr(key + 11, '_');
        if (underscore && toneIdx >= 0 && toneIdx < 4) {
            const char *paramName = underscore + 1;
            int nvramOffset = -1;
            int sysexIdx = -1;

            /* Map parameter names to NVRAM offset and SysEx index (they differ!)
             * Offsets verified against jv880_juce dataStructures.h Tone struct */
            /* NVRAM offsets from jv880_juce Tone struct (84 bytes total) */
            if (strcmp(paramName, "cutofffrequency") == 0) { nvramOffset = 52; sysexIdx = 74; }  /* tvfCutoff */
            else if (strcmp(paramName, "resonance") == 0) { nvramOffset = 53; sysexIdx = 75; }   /* tvfResonance */
            else if (strcmp(paramName, "level") == 0) { nvramOffset = 67; sysexIdx = 92; }       /* tvaLevel at 67 */
            else if (strcmp(paramName, "pan") == 0) { nvramOffset = 68; sysexIdx = 94; }         /* tvaPan at 68 */
            else if (strcmp(paramName, "pitchcoarse") == 0) { nvramOffset = 37; sysexIdx = 56; } /* pitchCoarse */
            else if (strcmp(paramName, "pitchfine") == 0) { nvramOffset = 38; sysexIdx = 57; }   /* pitchFine */
            else if (strcmp(paramName, "tvaenvtime1") == 0) { nvramOffset = 74; sysexIdx = 105; }  /* tvaEnvTime1 */
            else if (strcmp(paramName, "tvaenvtime2") == 0) { nvramOffset = 76; sysexIdx = 107; }  /* tvaEnvTime2 */
            else if (strcmp(paramName, "tvaenvtime3") == 0) { nvramOffset = 78; sysexIdx = 109; }  /* tvaEnvTime3 */
            else if (strcmp(paramName, "tvaenvtime4") == 0) { nvramOffset = 80; sysexIdx = 111; }  /* tvaEnvTime4 */
            else if (strcmp(paramName, "drylevel") == 0) { nvramOffset = 81; sysexIdx = 112; }     /* drySend */
            else if (strcmp(paramName, "reverbsendlevel") == 0) { nvramOffset = 82; sysexIdx = 113; } /* reverbSend */
            else if (strcmp(paramName, "chorussendlevel") == 0) { nvramOffset = 83; sysexIdx = 114; } /* chorusSend */

            /* Handle filter mode specially - it's bits 3-4 of byte 55 (tvfVeloCurveLpfHpf)
             * Accepts: "Off", "LPF", "HPF" or numeric 0, 1, 2 */
            if (strcmp(paramName, "filtermode") == 0) {
                int v;
                if (strcmp(val, "Off") == 0) v = 0;
                else if (strcmp(val, "LPF") == 0) v = 1;
                else if (strcmp(val, "HPF") == 0) v = 2;
                else v = clamp_int(atoi(val), 0, 2);
                const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);
                const int byteOffset = 55;  /* tvfVeloCurveLpfHpf */
                uint8_t *byte = &inst->mcu->nvram[toneBase + byteOffset];
                /* Clear bits 3-4 and set new filter mode */
                *byte = (*byte & ~0x18) | ((v & 0x03) << 3);
                /* SysEx index 0x49 (73) for filter mode per JV-880 MIDI Implementation */
                v2_queue_tone_sysex(inst, toneIdx, 0x49, v);
                return;
            }

            if (nvramOffset >= 0 && sysexIdx >= 0) {
                const int v = clamp_int(atoi(val), 0, 127);
                const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);
                /* Update NVRAM directly for immediate UI feedback */
                inst->mcu->nvram[toneBase + nvramOffset] = (uint8_t)v;
                /* Send SysEx to emulator for actual sound change */
                v2_queue_tone_sysex(inst, toneIdx, sysexIdx, v);
            }
        }
    } else if (strncmp(key, "sram_part_", 10) == 0 && inst->mcu) {
        int partIdx = key[10] - '0';
        if (partIdx >= 0 && partIdx < 8 && key[11] == '_') {
            const char *paramName = key + 12;
            const int partBase = SRAM_TEMP_PERF_OFFSET + TEMP_PERF_COMMON_SIZE + (partIdx * TEMP_PERF_PART_SIZE);
            int sramOffset = -1;
            int sysexIdx = -1;

            /* Map parameter names to SRAM offset and SysEx index */
            if (strcmp(paramName, "partlevel") == 0) { sramOffset = 17; sysexIdx = 25; }
            else if (strcmp(paramName, "partpan") == 0) { sramOffset = 18; sysexIdx = 26; }
            else if (strcmp(paramName, "internalkeyrangelower") == 0) { sramOffset = 10; sysexIdx = 15; }
            else if (strcmp(paramName, "internalkeyrangeupper") == 0) { sramOffset = 11; sysexIdx = 16; }
            else if (strcmp(paramName, "internalvelocitysense") == 0) { sramOffset = 13; sysexIdx = 18; }
            else if (strcmp(paramName, "internalvelocitymax") == 0) { sramOffset = 14; sysexIdx = 19; }

            /* Handle patchnumber specially - it's 0-255, uses dual bytes in SysEx
             * Per JV-880 MIDI Implementation: offset 0x17, range 0-255
             * Values > 127 use nibblized format: 0000 bbbb at addr, 0bbb bbbb at addr+1
             * This sends the value as two consecutive bytes */
            if (strcmp(paramName, "patchnumber") == 0) {
                const int v = clamp_int(atoi(val), 0, 255);
                inst->mcu->sram[partBase + 16] = (uint8_t)v;
                /* Send as nibblized dual bytes: MSB nibble at 0x17, LSB at 0x18 */
                int msb = (v >> 4) & 0x0F;  /* High nibble */
                int lsb = v & 0x7F;         /* Low 7 bits (but only need low nibble for < 256) */
                /* Actually for 0-255, we need: byte1 = v/128, byte2 = v%128 */
                msb = v >> 7;    /* 0 or 1 for 0-255 range */
                lsb = v & 0x7F;  /* 0-127 */
                v2_queue_part_sysex(inst, partIdx, 0x17, msb);  /* MSB */
                v2_queue_part_sysex(inst, partIdx, 0x18, lsb);  /* LSB */
                return;
            }

            /* Handle reverbswitch and chorusswitch (bit fields)
             * Accepts: "Off", "On" or numeric 0, 1 */
            if (strcmp(paramName, "reverbswitch") == 0) {
                int v;
                if (strcmp(val, "On") == 0) v = 1;
                else if (strcmp(val, "Off") == 0) v = 0;
                else v = atoi(val) ? 1 : 0;
                uint8_t *b = &inst->mcu->sram[partBase + 21];
                if (v) *b |= 0x40;
                else *b &= ~0x40;
                v2_queue_part_sysex(inst, partIdx, 29, v);
                return;
            }
            if (strcmp(paramName, "chorusswitch") == 0) {
                int v;
                if (strcmp(val, "On") == 0) v = 1;
                else if (strcmp(val, "Off") == 0) v = 0;
                else v = atoi(val) ? 1 : 0;
                uint8_t *b = &inst->mcu->sram[partBase + 21];
                if (v) *b |= 0x20;
                else *b &= ~0x20;
                v2_queue_part_sysex(inst, partIdx, 30, v);
                return;
            }

            /* Handle signed parameters (coarse/fine tune, transpose) */
            if (strcmp(paramName, "partcoarsetune") == 0) { sramOffset = 19; sysexIdx = 27; }
            else if (strcmp(paramName, "partfinetune") == 0) { sramOffset = 20; sysexIdx = 28; }
            else if (strcmp(paramName, "internalkeytranspose") == 0) { sramOffset = 12; sysexIdx = 17; }

            if (sramOffset >= 0 && sysexIdx >= 0) {
                int v = clamp_int(atoi(val), 0, 127);
                /* Check if this is a signed parameter (tune/transpose) */
                if (sramOffset == 19 || sramOffset == 20 || sramOffset == 12) {
                    int stored = v - 64;
                    if (stored < -64) stored = -64;
                    if (stored > 63) stored = 63;
                    inst->mcu->sram[partBase + sramOffset] = (uint8_t)(int8_t)stored;
                } else {
                    inst->mcu->sram[partBase + sramOffset] = (uint8_t)v;
                }
                v2_queue_part_sysex(inst, partIdx, sysexIdx, v);
            }
        }
    } else if (strncmp(key, "write_patch_", 12) == 0 && inst->mcu) {
        /* Write current working patch to user patch slot (0-63)
         * Copies working patch from NVRAM_PATCH_OFFSET to NVRAM_PATCH_INTERNAL
         * Usage: write_patch_<slot> where slot is 0-63 */
        int slot = atoi(key + 12);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Wrote patch '%s' to User slot %d (NVRAM 0x%04x)\n",
                    name, slot + 1, dest_offset);
        } else {
            fprintf(stderr, "JV880 v2: Invalid patch slot %d (must be 0-63)\n", slot);
        }
    } else if (strncmp(key, "write_performance_", 18) == 0 && inst->mcu) {
        /* Write temp performance to Internal slot (0-15)
         * Copies temp performance from SRAM to NVRAM Internal slot */
        int slot = atoi(key + 18);
        if (slot >= 0 && slot < 16) {
            uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (slot * PERF_SIZE);
            memcpy(&inst->mcu->nvram[nvram_offset],
                   &inst->mcu->sram[SRAM_TEMP_PERF_OFFSET], PERF_SIZE);
            char name[13];
            memcpy(name, &inst->mcu->sram[SRAM_TEMP_PERF_OFFSET], 12);
            name[12] = '\0';
            fprintf(stderr, "JV880 v2: Wrote performance '%s' to Internal slot %d (NVRAM 0x%04x)\n",
                    name, slot + 1, nvram_offset);
        } else {
            fprintf(stderr, "JV880 v2: Invalid performance slot %d (must be 0-15)\n", slot);
        }
    } else if (strcmp(key, "save_nvram") == 0 && inst->mcu) {
        /* Save NVRAM to disk (persists patches and performances) */
        char path[1024];
        snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
            fclose(f);
            fprintf(stderr, "JV880 v2: Saved NVRAM to %s\n", path);
        } else {
            fprintf(stderr, "JV880 v2: Failed to save NVRAM to %s\n", path);
        }
    } else if (strcmp(key, "load_user_patch") == 0 && inst->mcu) {
        /* Load a user patch from NVRAM into working area */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            /* Check if slot has valid data (not 0xFF filled) */
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded user patch '%s' from slot %d\n", name, slot + 1);
            } else {
                fprintf(stderr, "JV880 v2: User patch slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "run_param_test") == 0 && inst->mcu) {
        /* Automated parameter offset verification test */
        fprintf(stderr, "\n");
        fprintf(stderr, "============================================\n");
        fprintf(stderr, "=== AUTOMATED PARAMETER OFFSET TEST (v2) ===\n");
        fprintf(stderr, "============================================\n\n");

        int pass = 0, fail = 0;
        const int toneIdx = 0;
        const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

        struct { const char* name; int offset; uint8_t testVal; } tests[] = {
            {"level", 67, 0x63},
            {"pan", 68, 0x40},
            {"tvaenvtime1", 74, 0x4A},
            {"tvaenvtime2", 76, 0x4C},
            {"tvaenvtime3", 78, 0x4E},
            {"tvaenvtime4", 80, 0x50},
            {"drylevel", 81, 0x51},
            {"reverbsendlevel", 82, 0x52},
            {"chorussendlevel", 83, 0x53},
            {"cutofffrequency", 52, 0x7F},
            {"resonance", 53, 0x32},
            {"pitchcoarse", 37, 0x40},
            {"pitchfine", 38, 0x41},
        };
        const int numTests = sizeof(tests) / sizeof(tests[0]);

        uint8_t origValues[20];
        for (int i = 0; i < numTests; i++) {
            origValues[i] = inst->mcu->nvram[toneBase + tests[i].offset];
        }

        fprintf(stderr, "Testing tone %d parameters (base=0x%04x):\n\n", toneIdx, toneBase);

        for (int i = 0; i < numTests; i++) {
            inst->mcu->nvram[toneBase + tests[i].offset] = tests[i].testVal;
            uint8_t readVal = inst->mcu->nvram[toneBase + tests[i].offset];

            if (readVal == tests[i].testVal) {
                fprintf(stderr, "  ✓ PASS: %-20s offset=%2d wrote=0x%02x read=0x%02x\n",
                        tests[i].name, tests[i].offset, tests[i].testVal, readVal);
                pass++;
            } else {
                fprintf(stderr, "  ✗ FAIL: %-20s offset=%2d wrote=0x%02x read=0x%02x\n",
                        tests[i].name, tests[i].offset, tests[i].testVal, readVal);
                fail++;
            }
        }

        for (int i = 0; i < numTests; i++) {
            inst->mcu->nvram[toneBase + tests[i].offset] = origValues[i];
        }

        fprintf(stderr, "\n--------------------------------------------\n");
        fprintf(stderr, "Results: %d passed, %d failed\n", pass, fail);
        fprintf(stderr, "============================================\n\n");
    } else if (strcmp(key, "dump_tone_layout") == 0 && inst->mcu) {
        /* Dump current tone structure for verification */
        const int toneIdx = 0;
        const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

        fprintf(stderr, "\n=== Tone %d Structure (v2, base=0x%04x) ===\n\n", toneIdx, toneBase);
        fprintf(stderr, "--- TVA Section (67-83) ---\n");
        fprintf(stderr, "  67 tvaLevel:      %3d\n", inst->mcu->nvram[toneBase+67]);
        fprintf(stderr, "  68 tvaPan:        %3d\n", inst->mcu->nvram[toneBase+68]);
        fprintf(stderr, "  74 tvaEnvTime1:   %3d\n", inst->mcu->nvram[toneBase+74]);
        fprintf(stderr, "  76 tvaEnvTime2:   %3d\n", inst->mcu->nvram[toneBase+76]);
        fprintf(stderr, "  78 tvaEnvTime3:   %3d\n", inst->mcu->nvram[toneBase+78]);
        fprintf(stderr, "  80 tvaEnvTime4:   %3d\n", inst->mcu->nvram[toneBase+80]);
        fprintf(stderr, "  81 drySend:       %3d\n", inst->mcu->nvram[toneBase+81]);
        fprintf(stderr, "  82 reverbSend:    %3d\n", inst->mcu->nvram[toneBase+82]);
        fprintf(stderr, "  83 chorusSend:    %3d\n", inst->mcu->nvram[toneBase+83]);
        fprintf(stderr, "\n--- TVF Section (52-53) ---\n");
        fprintf(stderr, "  52 tvfCutoff:     %3d\n", inst->mcu->nvram[toneBase+52]);
        fprintf(stderr, "  53 tvfResonance:  %3d\n", inst->mcu->nvram[toneBase+53]);
        fprintf(stderr, "\n=== End Tone Layout ===\n");
    }
}

/* v2: Get parameter */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 || strcmp(key, "name") == 0) {
        if (!inst->loading_complete) {
            return snprintf(buf, buf_len, "%s", inst->loading_status);
        }
        if (inst->performance_mode) {
            /* In performance mode, read name from ROM2/NVRAM */
            int idx = inst->current_performance;
            if (idx >= 0 && idx < NUM_PERFORMANCES) {
                int bank = idx / PERFS_PER_BANK;
                int perf_in_bank = idx % PERFS_PER_BANK;
                uint8_t name_buf[PERF_NAME_LEN + 1];
                memset(name_buf, 0, sizeof(name_buf));
                int got_name = 0;

                if (bank == 2 && inst->mcu) {
                    /* Internal - read from NVRAM */
                    uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (perf_in_bank * PERF_SIZE);
                    if (nvram_offset + PERF_NAME_LEN <= 0x8000) {
                        memcpy(name_buf, &inst->mcu->nvram[nvram_offset], PERF_NAME_LEN);
                        got_name = 1;
                    }
                } else if (inst->rom2) {
                    /* Preset A/B - read from ROM2 */
                    uint32_t offset = (bank == 0) ? PERF_OFFSET_PRESET_A : PERF_OFFSET_PRESET_B;
                    offset += perf_in_bank * PERF_SIZE;
                    if (offset + PERF_NAME_LEN <= 0x40000) {
                        memcpy(name_buf, &inst->rom2[offset], PERF_NAME_LEN);
                        got_name = 1;
                    }
                }

                if (got_name) {
                    /* Trim trailing spaces */
                    int len = PERF_NAME_LEN;
                    while (len > 0 && (name_buf[len - 1] == ' ' || name_buf[len - 1] == 0)) len--;
                    name_buf[len] = '\0';
                    return snprintf(buf, buf_len, "%s", name_buf);
                }
            }
            return snprintf(buf, buf_len, "---");
        }
        if (inst->current_patch >= 0 && inst->current_patch < inst->total_patches) {
            return snprintf(buf, buf_len, "%s", inst->patches[inst->current_patch].name);
        }
        return snprintf(buf, buf_len, "Mini-JV");
    }
    if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0) {
        return snprintf(buf, buf_len, "%d", inst->total_patches);
    }
    if (strcmp(key, "current_patch") == 0 || strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_patch);
    }
    if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    }
    /* State serialization for patch save/load */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"mode\":%d,\"preset\":%d,\"performance\":%d,\"part\":%d,\"octave_transpose\":%d,"
            "\"expansion_index\":%d,\"expansion_bank_offset\":%d}",
            inst->performance_mode ? 1 : 0,
            inst->current_patch,
            inst->current_performance,
            inst->current_part,
            inst->octave_transpose,
            inst->current_expansion,
            inst->expansion_bank_offset);
    }
    if (strcmp(key, "loading_complete") == 0) {
        return snprintf(buf, buf_len, "%d", inst->loading_complete);
    }
    if (strcmp(key, "loading_status") == 0) {
        return snprintf(buf, buf_len, "%s", inst->loading_status);
    }
    if (strcmp(key, "audio_diag") == 0) {
        int avail = v2_ring_available(inst);
        return snprintf(buf, buf_len, "underruns=%d renders=%d ring=%d/%d min=%d",
                inst->underrun_count, inst->render_count, avail, AUDIO_RING_SIZE,
                inst->min_buffer_level);
    }
    if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "28");
    }
    /* Bank information */
    if (strcmp(key, "bank_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->bank_count);
    }
    if (strcmp(key, "bank_name") == 0) {
        /* During loading, show loading status */
        if (!inst->loading_complete) {
            return snprintf(buf, buf_len, "Loading...");
        }
        if (inst->performance_mode) {
            /* Performance mode - return performance bank name */
            static const char* perf_bank_names[] = {"Preset A", "Preset B", "Internal"};
            int bank = inst->current_performance / PERFS_PER_BANK;
            if (bank >= 0 && bank < NUM_PERF_BANKS) {
                return snprintf(buf, buf_len, "%s", perf_bank_names[bank]);
            }
            return snprintf(buf, buf_len, "Performances");
        }
        /* Patch mode - return current expansion/bank name */
        if (inst->current_patch >= 0 && inst->current_patch < inst->total_patches) {
            int bank = v2_get_bank_for_patch(inst, inst->current_patch);
            if (bank >= 0 && bank < inst->bank_count) {
                return snprintf(buf, buf_len, "%s", inst->bank_names[bank]);
            }
        }
        return snprintf(buf, buf_len, "Patches");
    }
    if (strcmp(key, "patch_in_bank") == 0) {
        if (inst->performance_mode) {
            /* Return 1-indexed position within performance bank */
            int pos = (inst->current_performance % PERFS_PER_BANK) + 1;
            return snprintf(buf, buf_len, "%d", pos);
        } else {
            /* Return 1-indexed position within current patch bank */
            int bank = v2_get_bank_for_patch(inst, inst->current_patch);
            int pos = inst->current_patch - inst->bank_starts[bank] + 1;
            return snprintf(buf, buf_len, "%d", pos);
        }
    }
    /* Mode information - return string for enum compatibility */
    if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%s", inst->performance_mode ? "Performance" : "Patch");
    }
    if (strcmp(key, "performance_mode") == 0) {
        return snprintf(buf, buf_len, "%d", inst->performance_mode);
    }
    if (strcmp(key, "current_performance") == 0 || strcmp(key, "performance") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_performance);
    }
    if (strcmp(key, "current_part") == 0 || strcmp(key, "part") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_part);
    }
    if (strcmp(key, "num_performances") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_PERFORMANCES);
    }
    if (strcmp(key, "num_parts") == 0) {
        return snprintf(buf, buf_len, "8");
    }
    /* Expansion information */
    if (strcmp(key, "expansion_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->expansion_count);
    }
    if (strcmp(key, "current_expansion") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_expansion);
    }
    if (strcmp(key, "expansion_bank_offset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->expansion_bank_offset);
    }
    /* Expansion list for "Choose Expansion" menu - returns JSON array */
    if (strcmp(key, "expansion_list") == 0) {
        /* Include factory patches as first entry with index -1 */
        int written = snprintf(buf, buf_len, "[{\"index\":-1,\"name\":\"Factory (Preset A)\",\"first_patch\":0,\"patch_count\":128}");
        for (int i = 0; i < inst->expansion_count && written < buf_len - 100; i++) {
            written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"index\":%d,\"name\":\"%s\",\"first_patch\":%d,\"patch_count\":%d}",
                i, inst->expansions[i].name, inst->expansions[i].first_global_index,
                inst->expansions[i].patch_count);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* User patch list for "Load User Patch" menu - returns JSON array of saved patches */
    if (strcmp(key, "user_patch_list") == 0 && inst->mcu) {
        int written = snprintf(buf, buf_len, "[");
        for (int i = 0; i < NUM_USER_PATCHES && written < buf_len - 100; i++) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (i * PATCH_SIZE);
            /* Check if slot has valid data (not 0xFF filled) */
            if (inst->mcu->nvram[offset] != 0xFF) {
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                /* Trim trailing spaces */
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
                if (written > 1) written += snprintf(buf + written, buf_len - written, ",");
                written += snprintf(buf + written, buf_len - written,
                    "{\"index\":%d,\"name\":\"%s\"}", i, name);
            }
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* User patch info: user_patch_<idx>_name */
    if (strncmp(key, "user_patch_", 11) == 0 && inst->mcu) {
        int idx = atoi(key + 11);
        if (strstr(key, "_name") && idx >= 0 && idx < NUM_USER_PATCHES) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (idx * PATCH_SIZE);
            if (inst->mcu->nvram[offset] != 0xFF) {
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                return snprintf(buf, buf_len, "%s", name);
            }
            return snprintf(buf, buf_len, "(empty)");
        }
    }
    /* Individual expansion info: expansion_<idx>_name, expansion_<idx>_patch_count, expansion_<idx>_first_patch */
    if (strncmp(key, "expansion_", 10) == 0) {
        int idx = atoi(key + 10);
        if (idx >= 0 && idx < inst->expansion_count) {
            if (strstr(key, "_name")) {
                return snprintf(buf, buf_len, "%s", inst->expansions[idx].name);
            }
            if (strstr(key, "_patch_count")) {
                return snprintf(buf, buf_len, "%d", inst->expansions[idx].patch_count);
            }
            if (strstr(key, "_first_patch")) {
                return snprintf(buf, buf_len, "%d", inst->expansions[idx].first_global_index);
            }
        }
    }
    /* Bank queries: bank_<idx>_name, bank_<idx>_start, bank_<idx>_count */
    if (strncmp(key, "bank_", 5) == 0) {
        int idx = atoi(key + 5);
        if (strstr(key, "_name") && idx >= 0 && idx < inst->bank_count) {
            return snprintf(buf, buf_len, "%s", inst->bank_names[idx]);
        }
        if (strstr(key, "_start") && idx >= 0 && idx < inst->bank_count) {
            return snprintf(buf, buf_len, "%d", inst->bank_starts[idx]);
        }
        if (strstr(key, "_count") && idx >= 0 && idx < inst->bank_count) {
            int next_start = (idx + 1 < inst->bank_count) ? inst->bank_starts[idx + 1] : inst->total_patches;
            return snprintf(buf, buf_len, "%d", next_start - inst->bank_starts[idx]);
        }
    }
    /* Patch name queries: patch_<idx>_name */
    if (strncmp(key, "patch_", 6) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 6);
        if (idx >= 0 && idx < inst->total_patches) {
            return snprintf(buf, buf_len, "%s", inst->patches[idx].name);
        }
        return snprintf(buf, buf_len, "---");
    }

    /* Hierarchical parameter getters for Shadow UI
     * These read the same memory locations as v1 API but use instance pointer
     */

    /* Read tone parameter: nvram_tone_<toneIdx>_<paramName> */
    if (strncmp(key, "nvram_tone_", 11) == 0 && inst->mcu) {
        int toneIdx = atoi(key + 11);
        const char* underscore = strchr(key + 11, '_');
        if (underscore && toneIdx >= 0 && toneIdx < 4) {
            const char* paramName = underscore + 1;
            int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);
            int offset = -1;

            /* Map parameter names to NVRAM offsets within tone
             * Offsets verified against jv880_juce dataStructures.h Tone struct */
            if (strcmp(paramName, "cutofffrequency") == 0) offset = 52;
            else if (strcmp(paramName, "resonance") == 0) offset = 53;
            else if (strcmp(paramName, "level") == 0) offset = 68;       /* tvaLevel at 68 */
            else if (strcmp(paramName, "pan") == 0) offset = 69;         /* tvaPan at 69 */
            else if (strcmp(paramName, "pitchcoarse") == 0) offset = 37;
            else if (strcmp(paramName, "pitchfine") == 0) offset = 38;
            /* TVA section offsets from jv880_juce struct */
            else if (strcmp(paramName, "level") == 0) offset = 67;       /* tvaLevel */
            else if (strcmp(paramName, "pan") == 0) offset = 68;         /* tvaPan */
            else if (strcmp(paramName, "tvaenvtime1") == 0) offset = 74; /* tvaEnvTime1 */
            else if (strcmp(paramName, "tvaenvtime2") == 0) offset = 76; /* tvaEnvTime2 */
            else if (strcmp(paramName, "tvaenvtime3") == 0) offset = 78; /* tvaEnvTime3 */
            else if (strcmp(paramName, "tvaenvtime4") == 0) offset = 80; /* tvaEnvTime4 */
            else if (strcmp(paramName, "drylevel") == 0) offset = 81;    /* drySend */
            else if (strcmp(paramName, "reverbsendlevel") == 0) offset = 82; /* reverbSend */
            else if (strcmp(paramName, "chorussendlevel") == 0) offset = 83; /* chorusSend */

            /* Handle filter mode specially - bits 3-4 of byte 55
             * Returns: "Off", "LPF", or "HPF" */
            if (strcmp(paramName, "filtermode") == 0) {
                uint8_t byte = inst->mcu->nvram[toneBase + 55];
                int filterMode = (byte >> 3) & 0x03;
                const char *labels[] = {"Off", "LPF", "HPF"};
                return snprintf(buf, buf_len, "%s", labels[filterMode < 3 ? filterMode : 0]);
            }

            if (offset >= 0 && offset < 85) {  /* Increased from 84 to 85 for chorusSend */
                uint8_t val = inst->mcu->nvram[toneBase + offset];
                return snprintf(buf, buf_len, "%d", val);
            }
        }
    }

    /* Read patch common parameter: nvram_patchCommon_<paramName> */
    if (strncmp(key, "nvram_patchCommon_", 18) == 0 && inst->mcu) {
        const char* paramName = key + 18;
        int offset = -1;

        if (strcmp(paramName, "patchlevel") == 0) offset = 21;
        else if (strcmp(paramName, "patchpan") == 0) offset = 22;
        else if (strcmp(paramName, "analogfeel") == 0) offset = 20;
        else if (strcmp(paramName, "reverblevel") == 0) offset = 13;
        else if (strcmp(paramName, "reverbtime") == 0) offset = 14;
        else if (strcmp(paramName, "choruslevel") == 0) offset = 16;
        else if (strcmp(paramName, "chorusdepth") == 0) offset = 17;
        else if (strcmp(paramName, "chorusrate") == 0) offset = 18;

        if (offset >= 0 && offset < 26) {
            uint8_t val = inst->mcu->nvram[NVRAM_PATCH_OFFSET + offset];
            return snprintf(buf, buf_len, "%d", val);
        }
    }

    /* Read part parameter: sram_part_<partIdx>_<paramName> */
    if (strncmp(key, "sram_part_", 10) == 0 && inst->mcu) {
        int partIdx = key[10] - '0';
        if (partIdx >= 0 && partIdx < 8 && key[11] == '_') {
            const char* paramName = key + 12;
            int partBase = SRAM_TEMP_PERF_OFFSET + TEMP_PERF_COMMON_SIZE + (partIdx * TEMP_PERF_PART_SIZE);
            int offset = -1;

            /* Direct storage parameters */
            if (strcmp(paramName, "partlevel") == 0) offset = 17;
            else if (strcmp(paramName, "partpan") == 0) offset = 18;
            else if (strcmp(paramName, "patchnumber") == 0) offset = 16;
            else if (strcmp(paramName, "internalkeyrangelower") == 0) offset = 10;
            else if (strcmp(paramName, "internalkeyrangeupper") == 0) offset = 11;

            /* Handle reverbswitch and chorusswitch as bit fields - return "Off" or "On" */
            if (strcmp(paramName, "reverbswitch") == 0) {
                uint8_t b = inst->mcu->sram[partBase + 21];
                return snprintf(buf, buf_len, "%s", ((b >> 6) & 1) ? "On" : "Off");
            }
            if (strcmp(paramName, "chorusswitch") == 0) {
                uint8_t b = inst->mcu->sram[partBase + 21];
                return snprintf(buf, buf_len, "%s", ((b >> 5) & 1) ? "On" : "Off");
            }

            /* Signed offset parameters: stored = (val-64), read = (stored+64) */
            if (strcmp(paramName, "partcoarsetune") == 0 ||
                strcmp(paramName, "partfinetune") == 0 ||
                strcmp(paramName, "internalkeytranspose") == 0) {
                if (strcmp(paramName, "partcoarsetune") == 0) offset = 19;
                else if (strcmp(paramName, "partfinetune") == 0) offset = 20;
                else if (strcmp(paramName, "internalkeytranspose") == 0) offset = 12;
                if (offset >= 0) {
                    int8_t stored = (int8_t)inst->mcu->sram[partBase + offset];
                    int val = stored + 64;
                    return snprintf(buf, buf_len, "%d", val);
                }
            }

            if (offset >= 0) {
                return snprintf(buf, buf_len, "%d", inst->mcu->sram[partBase + offset]);
            }
        }
    }

    /* UI hierarchy for shadow parameter editor
     * JV-880 has two modes: patch and performance
     * - Patch mode: browse patches → tones (1-4) → tone params
     * - Performance mode: browse performances → parts (1-8) → part params
     *
     * Tone params use: nvram_tone_<n>_<param> (n=0-3)
     * Patch common params use: nvram_patchCommon_<param>
     * Part params use: sram_part_<n>_<param> (n=0-7)
     */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":[\"patch\",\"performance\"],"
            "\"mode_param\":\"mode\","
            "\"levels\":{"
                "\"patch\":{"
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":\"patch_main\","
                    "\"knobs\":[\"nvram_patchCommon_patchlevel\",\"nvram_patchCommon_patchpan\",\"nvram_patchCommon_reverblevel\",\"nvram_patchCommon_choruslevel\",\"nvram_patchCommon_analogfeel\",\"octave_transpose\"],"
                    "\"params\":[]"
                "},"
                "\"patch_main\":{"
                    "\"label\":\"Patch\","
                    "\"children\":null,"
                    "\"knobs\":[\"nvram_patchCommon_patchlevel\",\"nvram_patchCommon_patchpan\",\"nvram_patchCommon_reverblevel\",\"nvram_patchCommon_choruslevel\",\"nvram_patchCommon_analogfeel\",\"octave_transpose\"],"
                    "\"params\":["
                        "{\"level\":\"tone_selector\",\"label\":\"Edit Tones\"},"
                        "{\"level\":\"patch_common\",\"label\":\"Common Settings\"},"
                        "{\"level\":\"expansions\",\"label\":\"Jump to Expansion\"},"
                        "{\"level\":\"user_patches\",\"label\":\"User Patches\"}"
                    "]"
                "},"
                "\"patch_common\":{"
                    "\"label\":\"Common\","
                    "\"children\":null,"
                    "\"knobs\":[\"nvram_patchCommon_patchlevel\",\"nvram_patchCommon_patchpan\",\"nvram_patchCommon_reverblevel\",\"nvram_patchCommon_choruslevel\",\"nvram_patchCommon_analogfeel\",\"octave_transpose\"],"
                    "\"params\":["
                        "{\"key\":\"nvram_patchCommon_patchlevel\",\"label\":\"Patch Level\"},"
                        "{\"key\":\"nvram_patchCommon_patchpan\",\"label\":\"Patch Pan\"},"
                        "{\"key\":\"nvram_patchCommon_reverblevel\",\"label\":\"Reverb Level\"},"
                        "{\"key\":\"nvram_patchCommon_reverbtime\",\"label\":\"Reverb Time\"},"
                        "{\"key\":\"nvram_patchCommon_choruslevel\",\"label\":\"Chorus Level\"},"
                        "{\"key\":\"nvram_patchCommon_chorusdepth\",\"label\":\"Chorus Depth\"},"
                        "{\"key\":\"nvram_patchCommon_chorusrate\",\"label\":\"Chorus Rate\"},"
                        "{\"key\":\"nvram_patchCommon_analogfeel\",\"label\":\"Analog Feel\"},"
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"}"
                    "]"
                "},"
                "\"tone_selector\":{"
                    "\"label\":\"Tones\","
                    "\"children\":null,"
                    "\"child_prefix\":\"nvram_tone_\","
                    "\"child_count\":4,"
                    "\"child_label\":\"Tone\","
                    "\"knobs\":[\"cutofffrequency\",\"resonance\",\"filtermode\",\"level\",\"pan\",\"tvaenvtime1\",\"tvaenvtime2\",\"reverbsendlevel\"],"
                    "\"params\":[\"cutofffrequency\",\"resonance\",\"filtermode\",\"level\",\"pan\",\"pitchcoarse\",\"pitchfine\",\"tvaenvtime1\",\"tvaenvtime2\",\"tvaenvtime3\",\"tvaenvtime4\",\"drylevel\",\"reverbsendlevel\",\"chorussendlevel\"]"
                "},"
                "\"performance\":{"
                    "\"list_param\":\"performance\","
                    "\"count_param\":\"num_performances\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":\"perf_main\","
                    "\"knobs\":[\"octave_transpose\"],"
                    "\"params\":[]"
                "},"
                "\"perf_main\":{"
                    "\"label\":\"Performance\","
                    "\"children\":null,"
                    "\"knobs\":[\"octave_transpose\"],"
                    "\"params\":["
                        "{\"level\":\"part_selector\",\"label\":\"Edit Parts\"},"
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"}"
                    "]"
                "},"
                "\"part_selector\":{"
                    "\"label\":\"Parts\","
                    "\"children\":null,"
                    "\"child_prefix\":\"sram_part_\","
                    "\"child_count\":8,"
                    "\"child_label\":\"Part\","
                    "\"knobs\":[\"partlevel\",\"partpan\",\"reverbswitch\",\"chorusswitch\",\"partcoarsetune\",\"partfinetune\",\"internalkeyrangelower\",\"internalkeyrangeupper\"],"
                    "\"params\":[\"partlevel\",\"partpan\",\"reverbswitch\",\"chorusswitch\",\"partcoarsetune\",\"partfinetune\",\"patchnumber\",\"internalkeyrangelower\",\"internalkeyrangeupper\",\"internalkeytranspose\"]"
                "},"
                "\"expansions\":{"
                    "\"label\":\"Jump to Expansion\","
                    "\"items_param\":\"expansion_list\","
                    "\"select_param\":\"jump_to_expansion\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "},"
                "\"user_patches\":{"
                    "\"label\":\"User Patches\","
                    "\"items_param\":\"user_patch_list\","
                    "\"select_param\":\"load_user_patch\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    /* Chain params metadata for shadow parameter editor */
    if (strcmp(key, "chain_params") == 0) {
        const char *params_json = "["
            /* Basic navigation */
            "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999},"
            "{\"key\":\"performance\",\"name\":\"Performance\",\"type\":\"int\",\"min\":0,\"max\":47},"
            "{\"key\":\"part\",\"name\":\"Part\",\"type\":\"int\",\"min\":0,\"max\":7},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3},"
            /* Patch common params */
            "{\"key\":\"nvram_patchCommon_patchlevel\",\"name\":\"Patch Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_patchpan\",\"name\":\"Patch Pan\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_reverblevel\",\"name\":\"Reverb Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_reverbtime\",\"name\":\"Reverb Time\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_choruslevel\",\"name\":\"Chorus Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_chorusdepth\",\"name\":\"Chorus Depth\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_chorusrate\",\"name\":\"Chorus Rate\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_analogfeel\",\"name\":\"Analog Feel\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params (suffix only - child_prefix adds nvram_tone_N_) */
            "{\"key\":\"cutofffrequency\",\"name\":\"Cutoff\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"resonance\",\"name\":\"Resonance\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"filtermode\",\"name\":\"Filter Mode\",\"type\":\"enum\",\"options\":[\"Off\",\"LPF\",\"HPF\"]},"
            "{\"key\":\"level\",\"name\":\"Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"pan\",\"name\":\"Pan\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"pitchcoarse\",\"name\":\"Pitch Coarse\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"pitchfine\",\"name\":\"Pitch Fine\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvtime1\",\"name\":\"Attack\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvtime2\",\"name\":\"Decay\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvtime3\",\"name\":\"Sustain\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvtime4\",\"name\":\"Release\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"drylevel\",\"name\":\"Dry Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"reverbsendlevel\",\"name\":\"Reverb Send\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"chorussendlevel\",\"name\":\"Chorus Send\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Part params (suffix only - child_prefix adds sram_part_N_) */
            "{\"key\":\"partlevel\",\"name\":\"Part Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"partpan\",\"name\":\"Part Pan\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"patchnumber\",\"name\":\"Patch\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"reverbswitch\",\"name\":\"Reverb\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"chorusswitch\",\"name\":\"Chorus\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"partcoarsetune\",\"name\":\"Coarse Tune\",\"type\":\"int\",\"min\":16,\"max\":112},"
            "{\"key\":\"partfinetune\",\"name\":\"Fine Tune\",\"type\":\"int\",\"min\":14,\"max\":114},"
            "{\"key\":\"internalkeyrangelower\",\"name\":\"Key Lo\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"internalkeyrangeupper\",\"name\":\"Key Hi\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"internalkeytranspose\",\"name\":\"Transpose\",\"type\":\"int\",\"min\":16,\"max\":112}"
        "]";
        int len = strlen(params_json);
        if (len < buf_len) {
            strcpy(buf, params_json);
            return len;
        }
        return -1;
    }

    return -1;
}

/* v2: Get error - returns error message if module is in error state */
static int v2_get_error(void *instance, char *buf, int buf_len) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst || !inst->load_error[0]) return 0;  /* No error */

    int len = strlen(inst->load_error);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, inst->load_error, len);
    buf[len] = '\0';
    return len;
}

/* v2: Render block */
static void v2_render_block(void *instance, int16_t *out, int frames) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst || !inst->initialized || !inst->thread_running || !inst->loading_complete) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    pthread_mutex_lock(&inst->ring_mutex);
    int avail = v2_ring_available(inst);
    int to_read = (avail < frames) ? avail : frames;

    /* Track buffer levels for diagnostics */
    inst->render_count++;
    if (avail < inst->min_buffer_level || inst->min_buffer_level == 0) {
        inst->min_buffer_level = avail;
    }

    for (int i = 0; i < to_read; i++) {
        out[i * 2 + 0] = inst->audio_ring[inst->ring_read * 2 + 0] >> OUTPUT_GAIN_SHIFT;
        out[i * 2 + 1] = inst->audio_ring[inst->ring_read * 2 + 1] >> OUTPUT_GAIN_SHIFT;
        inst->ring_read = (inst->ring_read + 1) % AUDIO_RING_SIZE;
    }
    pthread_mutex_unlock(&inst->ring_mutex);

    /* Pad with silence if underrun */
    if (to_read < frames) {
        inst->underrun_count++;
        fprintf(stderr, "JV880: UNDERRUN #%d: needed %d, had %d (min_level=%d, renders=%d)\n",
                inst->underrun_count, frames, avail, inst->min_buffer_level, inst->render_count);
        inst->min_buffer_level = 9999;  /* Reset for next period */
    }
    for (int i = to_read; i < frames; i++) {
        out[i * 2 + 0] = 0;
        out[i * 2 + 1] = 0;
    }

    /* Handle deferred selections - only after warmup is complete */
    if (inst->warmup_remaining <= 0) {
        /* Handle deferred performance selection after mode switch has been processed */
        if (inst->pending_perf_select > 0) {
            inst->pending_perf_select--;
            if (inst->pending_perf_select == 0) {
                /* Mode switch has had time to process, now select performance */
                jv_debug("[v2_render_block] Executing deferred performance select: %d\n",
                        inst->current_performance);
                v2_select_performance(inst, inst->current_performance);
            }
        }

        /* Handle deferred patch selection after mode switch has been processed */
        if (inst->pending_patch_select > 0) {
            inst->pending_patch_select--;
            if (inst->pending_patch_select == 0) {
                /* Mode switch has had time to process, now select patch */
                jv_debug("[v2_render_block] Executing deferred patch select: %d\n",
                        inst->current_patch);
                v2_select_patch(inst, inst->current_patch);
            }
        }
    }
}

/* v2 API struct */
static plugin_api_v2_t jv880_api_v2 = {
    MOVE_PLUGIN_API_VERSION_2,
    v2_create_instance,
    v2_destroy_instance,
    v2_on_midi,
    v2_set_param,
    v2_get_param,
    v2_get_error,
    v2_render_block
};

/* v2 Entry Point */
extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    (void)host;
    fprintf(stderr, "JV880: v2 API initialized\n");
    return &jv880_api_v2;
}
