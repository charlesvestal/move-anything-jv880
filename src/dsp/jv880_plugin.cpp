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

/* Unified patch list */
#define MAX_TOTAL_PATCHES 4096
typedef struct {
    char name[PATCH_NAME_LEN + 1];
    int expansion_index;    /* -1 for internal, 0+ for expansion */
    int local_patch_index;  /* Index within bank/expansion */
    uint32_t rom_offset;    /* Offset in ROM2 or expansion ROM */
} PatchInfo;

/* Performance mode constants */
#define NUM_PERF_BANKS 3
#define NUM_PERFORMANCES (NUM_PERF_BANKS * PERFS_PER_BANK)  /* 48 total */

/* Bank navigation */
#define MAX_BANKS 64

/* Progressive loading state machine */
enum LoadingPhase {
    PHASE_INIT = 0,
    PHASE_CHECK_CACHE,
    PHASE_SCAN_EXPANSION,
    PHASE_BUILD_PATCHES,
    PHASE_WARMUP,
    PHASE_COMPLETE
};

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

/* Parameter mapping constants */
#define MAP_SRAM_SCAN_SIZE 512  /* Bytes to scan around temp perf */

/* Audio ring buffer size */
#define AUDIO_RING_SIZE 512

/* MIDI queue sizes */
#define MIDI_QUEUE_SIZE 256
#define MIDI_MSG_MAX_LEN 256

/* Sample rates */
/* JV-880 PCM runs at 64 kHz with oversampling enabled. */
#define JV880_SAMPLE_RATE 64000
#define MOVE_SAMPLE_RATE 44100


/* Output gain (reduce to prevent clipping) */
#define OUTPUT_GAIN_SHIFT 1  /* -6dB headroom to prevent clipping on hot patches */

/* ========================================================================
 * TONE PARAMETER LOOKUP TABLE
 * Sorted alphabetically for binary search
 * ======================================================================== */

enum ToneParamType {
    TP_BYTE,        /* Simple byte read/write */
    TP_BITFIELD,    /* Bitfield within a byte */
    TP_BOOL,        /* Boolean (On/Off) as bit */
    TP_ENUM         /* Enum with string options */
};

typedef struct {
    const char *name;
    int nvram_offset;   /* Offset within tone (84 bytes) */
    int sysex_idx;      /* SysEx parameter index */
    enum ToneParamType type;
    int bit_shift;      /* For bitfields: bit position */
    int bit_mask;       /* For bitfields: mask after shift */
    int two_byte;       /* 1 if param needs 2-byte nibblized SysEx */
    int signed_param;   /* 1 if param is signed (needs +64 for SysEx, read as int8_t) */
} ToneParamEntry;

/* Sorted alphabetically for binary search
 * two_byte=1 for params needing 2-byte nibblized SysEx:
 *   wavenumber (0x01), lfo1delay (0x26), lfo2delay (0x31), pan (0x5e), tonedelaytime (0x62)
 * signed_param=1 for params needing +64 offset in SysEx and int8_t read:
 *   LFO depths, pitch coarse/fine, envelope depths/levels, velocity senses, pan
 */
static const ToneParamEntry TONE_PARAMS[] = {
    /*                  name,                     nvram, sysex, type,        shift, mask, 2byte, signed */
    {"chorussendlevel",                             83,   114, TP_BYTE,        0,    0,     0,     0},
    {"cutofffrequency",                             52,    74, TP_BYTE,        0,    0,     0,     0},
    {"cutoffkeyfollow",                             54,    77, TP_BYTE,        0,    0,     0,     0},
    {"drylevel",                                    81,   112, TP_BYTE,        0,    0,     0,     0},
    {"filtermode",                                  55,    73, TP_BITFIELD,    3, 0x03,     0,     0},
    {"fxmdepth",                                     2,     5, TP_BITFIELD,    0, 0x0F,     0,     0},  /* value-1 handled specially */
    {"fxmswitch",                                    2,     4, TP_BOOL,        7, 0x01,     0,     0},
    {"level",                                       67,    92, TP_BYTE,        0,    0,     0,     0},
    {"levelkeyfollow",                              70,    93, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"lfo1delay",                                   25,    38, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
    {"lfo1fadepolarity",                            23,    40, TP_BOOL,        7, 0x01,     0,     0},
    {"lfo1fadetime",                                26,    41, TP_BYTE,        0,    0,     0,     0},
    {"lfo1form",                                    23,    34, TP_BITFIELD,    0, 0x07,     0,     0},
    {"lfo1offset",                                  23,    35, TP_BITFIELD,    3, 0x07,     0,     0},
    {"lfo1pitchdepth",                              31,    42, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo1rate",                                    24,    37, TP_BYTE,        0,    0,     0,     0},
    {"lfo1synchro",                                 23,    36, TP_BOOL,        6, 0x01,     0,     0},
    {"lfo1tvadepth",                                33,    44, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo1tvfdepth",                                32,    43, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo2delay",                                   29,    49, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
    {"lfo2fadetime",                                30,    52, TP_BYTE,        0,    0,     0,     0},
    {"lfo2form",                                    27,    45, TP_BITFIELD,    0, 0x07,     0,     0},
    {"lfo2pitchdepth",                              34,    53, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo2rate",                                    28,    48, TP_BYTE,        0,    0,     0,     0},
    {"lfo2tvadepth",                                36,    55, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo2tvfdepth",                                35,    54, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"pan",                                         68,    94, TP_BYTE,        0,    0,     1,     2},  /* 2-byte, special: stored with +64 */
    {"panningkeyfollow",                            39,    96, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"penvdepth",                                   43,    64, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel1",                                  45,    66, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel2",                                  47,    68, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel3",                                  49,    70, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel4",                                  51,    72, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvtime1",                                   44,    65, TP_BYTE,        0,    0,     0,     0},
    {"penvtime2",                                   46,    67, TP_BYTE,        0,    0,     0,     0},
    {"penvtime3",                                   48,    69, TP_BYTE,        0,    0,     0,     0},
    {"penvtime4",                                   50,    71, TP_BYTE,        0,    0,     0,     0},
    {"penvtimekeyfollow",                           40,    63, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"penvvelocitylevelsense",                      41,    60, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvvelocityofftimesense",                    42,    62, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"penvvelocityontimesense",                     42,    61, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"pitchcoarse",                                 37,    56, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"pitchfine",                                   38,    57, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"pitchkeyfollow",                              40,    59, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"randompitchdepth",                            39,    58, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"resonance",                                   53,    75, TP_BYTE,        0,    0,     0,     0},
    {"resonancemode",                               53,    76, TP_BOOL,        7, 0x01,     0,     0},
    {"reverbsendlevel",                             82,   113, TP_BYTE,        0,    0,     0,     0},
    {"tonedelaymode",                               71,    97, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"tonedelaytime",                               69,    98, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
    {"toneswitch",                                   0,     3, TP_BOOL,        7, 0x01,     0,     0},
    {"tvaenvlevel1",                                75,   106, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvlevel2",                                77,   108, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvlevel3",                                79,   110, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime1",                                 74,   105, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime2",                                 76,   107, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime3",                                 78,   109, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime4",                                 80,   111, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtimekeyfollow",                         70,   104, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvaenvvelocitycurve",                         71,   100, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvaenvvelocitylevelsense",                    72,   101, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"tvaenvvelocityofftimesense",                  73,   103, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvaenvvelocityontimesense",                   73,   102, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"tvfenvdepth",                                 58,    83, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"tvfenvlevel1",                                60,    85, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvlevel2",                                62,    87, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvlevel3",                                64,    89, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvlevel4",                                66,    91, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime1",                                 59,    84, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime2",                                 61,    86, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime3",                                 63,    88, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime4",                                 65,    90, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtimekeyfollow",                         54,    82, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvvelocitycurve",                         55,    78, TP_BITFIELD,    0, 0x07,     0,     0},
    {"tvfenvvelocitylevelsense",                    56,    79, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"tvfenvvelocityofftimesense",                  57,    81, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvfenvvelocityontimesense",                   57,    80, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"velocityrangelower",                           3,     6, TP_BYTE,        0,    0,     0,     0},
    {"velocityrangeupper",                           4,     7, TP_BYTE,        0,    0,     0,     0},
    {"wavegroup",                                    0,     0, TP_BITFIELD,    0, 0x03,     0,     0},
    {"wavenumber",                                   1,     1, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
};
#define NUM_TONE_PARAMS (sizeof(TONE_PARAMS) / sizeof(TONE_PARAMS[0]))

/* Binary search for tone param */
static const ToneParamEntry* find_tone_param(const char *name) {
    int lo = 0, hi = NUM_TONE_PARAMS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(name, TONE_PARAMS[mid].name);
        if (cmp == 0) return &TONE_PARAMS[mid];
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

/* ========================================================================
 * SHARED HELPER FUNCTIONS
 * ======================================================================== */

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

    /* Part patch bank selection: 0=User, 1=Internal, 2=PresetA, 3=PresetB */
    int part_patchbank[8];

    /* Save/load slot browser indices */
    int save_slot_index;
    int load_slot_index;

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
    char pending_state[2048];
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

    /* Debug: show header bytes and first patch preview */
    fprintf(stderr, "JV880 v2: Scanned %s: %d patches at offset 0x%x\n",
            info->name, patch_count, patches_offset);
    fprintf(stderr, "JV880 v2: Header bytes 0x66-0x67: %02X %02X, 0x8c-0x8f: %02X %02X %02X %02X\n",
            unscrambled_data[0x66], unscrambled_data[0x67],
            unscrambled_data[0x8c], unscrambled_data[0x8d],
            unscrambled_data[0x8e], unscrambled_data[0x8f]);

    /* Check first patch at patches_offset */
    char first_patch_name[13];
    memcpy(first_patch_name, &unscrambled_data[patches_offset], 12);
    first_patch_name[12] = '\0';
    fprintf(stderr, "JV880 v2: First patch at 0x%x: name='%s', byte26=0x%02X\n",
            patches_offset, first_patch_name, unscrambled_data[patches_offset + 26]);

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

    /* Load waveform data to expansion waverom */
    memset(inst->mcu->pcm.waverom_exp, 0, EXPANSION_SIZE_8MB);
    memcpy(inst->mcu->pcm.waverom_exp, exp->unscrambled, exp->rom_size);

    /* Load patch definitions to cardram for Card patches (64-127 in Performance mode)
     * The JV-880 looks for Card patch data in cardram when a part uses patchnumber 64-127.
     * Copy up to 64 patches from the expansion's patch area. */
    memset(inst->mcu->cardram, 0, CARDRAM_SIZE);
    int patches_to_copy = (exp->patch_count > 64) ? 64 : exp->patch_count;
    int bytes_to_copy = patches_to_copy * PATCH_SIZE;
    if (bytes_to_copy > CARDRAM_SIZE) bytes_to_copy = CARDRAM_SIZE;
    memcpy(inst->mcu->cardram, &exp->unscrambled[exp->patches_offset], bytes_to_copy);

    /* Debug: show what we copied */
    jv_debug("[JV880] Expansion load: patches_offset=0x%x, patch_count=%d\n",
             exp->patches_offset, exp->patch_count);

    /* Show first patch's structure */
    uint8_t *p0 = inst->mcu->cardram;
    char name[13];
    memcpy(name, p0, 12);
    name[12] = '\0';
    jv_debug("[JV880] cardram patch 0: name='%s'\n", name);
    jv_debug("[JV880] cardram patch 0: bytes 0-15: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
             p0[0], p0[1], p0[2], p0[3], p0[4], p0[5], p0[6], p0[7],
             p0[8], p0[9], p0[10], p0[11], p0[12], p0[13], p0[14], p0[15]);
    jv_debug("[JV880] cardram patch 0: bytes 16-31: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
             p0[16], p0[17], p0[18], p0[19], p0[20], p0[21], p0[22], p0[23],
             p0[24], p0[25], p0[26], p0[27], p0[28], p0[29], p0[30], p0[31]);
    /* Tone 0 starts at offset 26. Show wavegroup and wavenumber */
    jv_debug("[JV880] cardram patch 0 tone0: wavegroup=%d wavenumber=%d,%d (at offsets 26,27,28)\n",
             p0[26], p0[27], p0[28]);

    /* Also show what's at those same offsets in the expansion ROM directly */
    uint8_t *rom_p0 = &exp->unscrambled[exp->patches_offset];
    jv_debug("[JV880] ROM patch 0: bytes 0-15: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
             rom_p0[0], rom_p0[1], rom_p0[2], rom_p0[3], rom_p0[4], rom_p0[5], rom_p0[6], rom_p0[7],
             rom_p0[8], rom_p0[9], rom_p0[10], rom_p0[11], rom_p0[12], rom_p0[13], rom_p0[14], rom_p0[15]);

    fprintf(stderr, "JV880 v2: Copied %d patches (%d bytes) to cardram\n",
            patches_to_copy, bytes_to_copy);

    inst->current_expansion = exp_index;

    /* In Performance mode, don't reset - just load the ROM data.
     * The expansion waveforms will be available for Card patches.
     * In Patch mode, reset to ensure the new expansion is properly initialized. */
    if (!inst->performance_mode) {
        inst->mcu->SC55_Reset();
        inst->warmup_remaining = 50000;  /* Warmup after expansion change */
        fprintf(stderr, "JV880 v2: Loaded expansion %s to emulator (with reset)\n", exp->name);
    } else {
        fprintf(stderr, "JV880 v2: Loaded expansion %s for Card patches (no reset)\n", exp->name);
    }
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

    /* Initialize part_patchbank to -1 (uninitialized) so we derive from SRAM on first access */
    for (int i = 0; i < 8; i++) {
        inst->part_patchbank[i] = -1;
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

/* v2: Helper to queue SysEx for tone parameter changes
 * two_byte: if true, sends nibblized 2-byte data (high nibble, low nibble)
 *           used for wavenumber, lfo delay, pan, tone delay time
 */
static void v2_queue_tone_sysex(jv880_instance_t *inst, int toneIdx, int paramIdx, int value, int two_byte) {
    if (!inst || toneIdx < 0 || toneIdx > 3) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data... checksum F7 */
    uint8_t addr[4] = { 0x00, 0x08, (uint8_t)(0x28 + toneIdx), (uint8_t)paramIdx };

    if (two_byte) {
        /* 2-byte nibblized format: high nibble then low nibble */
        uint8_t data_hi = (uint8_t)((value >> 4) & 0x0F);
        uint8_t data_lo = (uint8_t)(value & 0x0F);

        /* Calculate checksum */
        int sum = addr[0] + addr[1] + addr[2] + addr[3] + data_hi + data_lo;
        uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

        uint8_t sysex[13] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                              addr[0], addr[1], addr[2], addr[3],
                              data_hi, data_lo, chk, 0xF7 };

        /* Queue the SysEx */
        int write_idx = inst->midi_write;
        int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
        if (next == inst->midi_read) return; /* Queue full */

        memcpy(inst->midi_queue[write_idx], sysex, 13);
        inst->midi_queue_len[write_idx] = 13;
        inst->midi_write = next;
    } else {
        /* Standard 1-byte format */
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

/* v2: Helper to queue SysEx for part parameter changes
 * two_byte: if true, sends MSB/LSB format (for 0-255 range params like patchnumber)
 *           MSB = (value >> 7) & 0x7F, LSB = value & 0x7F
 */
static void v2_queue_part_sysex(jv880_instance_t *inst, int partIdx, int paramIdx, int value, int two_byte) {
    if (!inst || partIdx < 0 || partIdx > 7) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data... checksum F7 */
    /* Part address: [0x00, 0x00, 0x18 + partIdx, paramIdx] */
    uint8_t addr[4] = { 0x00, 0x00, (uint8_t)(0x18 + partIdx), (uint8_t)paramIdx };

    if (two_byte) {
        /* 2-byte nibblized format for 0-255 range (e.g., patchnumber)
         * Per Roland SysEx spec: high nibble at addr, low nibble at addr+1 */
        uint8_t data_high = (uint8_t)((value >> 4) & 0x0F);
        uint8_t data_low = (uint8_t)(value & 0x0F);

        /* Calculate checksum */
        int sum = addr[0] + addr[1] + addr[2] + addr[3] + data_high + data_low;
        uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

        uint8_t sysex[13] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                              addr[0], addr[1], addr[2], addr[3],
                              data_high, data_low, chk, 0xF7 };

        /* Queue the SysEx */
        int write_idx = inst->midi_write;
        int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
        if (next == inst->midi_read) return; /* Queue full */

        memcpy(inst->midi_queue[write_idx], sysex, 13);
        inst->midi_queue_len[write_idx] = 13;
        inst->midi_write = next;
        jv_debug("[JV880] Part%d SysEx: addr=%02X.%02X.%02X.%02X data=%02X.%02X (value=%d)\n",
                partIdx, addr[0], addr[1], addr[2], addr[3], data_high, data_low, value);
    } else {
        /* Standard 1-byte format */
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
        jv_debug("[JV880] Part%d SysEx: addr=%02X.%02X.%02X.%02X data=%02X (value=%d)\n",
                partIdx, addr[0], addr[1], addr[2], addr[3], data, value);
    }
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

        /* Restore working patch data from hex string */
        if (inst->mcu) {
            const char *patch_start = strstr(val, "\"patch\":\"");
            if (patch_start) {
                patch_start += 9;  /* Skip past "patch":" */
                const char *patch_end = strchr(patch_start, '"');
                if (patch_end && (patch_end - patch_start) == PATCH_SIZE * 2) {
                    /* Decode hex to NVRAM */
                    for (int i = 0; i < PATCH_SIZE; i++) {
                        unsigned int byte;
                        if (sscanf(patch_start + i * 2, "%2X", &byte) == 1) {
                            inst->mcu->nvram[NVRAM_PATCH_OFFSET + i] = (uint8_t)byte;
                        }
                    }
                    /* Send PC 0 to trigger emulator to reload from NVRAM */
                    uint8_t pc_msg[2] = { 0xC0, 0x00 };
                    pthread_mutex_lock(&inst->ring_mutex);
                    int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
                    if (next != inst->midi_read) {
                        memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
                        inst->midi_queue_len[inst->midi_write] = 2;
                        inst->midi_write = next;
                    }
                    pthread_mutex_unlock(&inst->ring_mutex);
                    fprintf(stderr, "JV880 v2: Restored working patch from state\n");
                }
            }
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
            /* Actually load the expansion ROM into the emulator */
            v2_load_expansion_to_emulator(inst, exp_idx);
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
        /* Optimized tone param set using binary search lookup */
        int toneIdx = atoi(key + 11);
        const char *underscore = strchr(key + 11, '_');
        if (underscore && toneIdx >= 0 && toneIdx < 4) {
            const char *paramName = underscore + 1;
            const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

            const ToneParamEntry *p = find_tone_param(paramName);
            if (p) {
                uint8_t *b = &inst->mcu->nvram[toneBase + p->nvram_offset];
                int v;
                int sysex_val;

                switch (p->type) {
                    case TP_BYTE:
                        if (p->signed_param == 1) {
                            /* Signed param: accept negative values, add 64 for SysEx */
                            v = clamp_int(atoi(val), -63, 63);
                            *b = (uint8_t)(int8_t)v;  /* Store as signed byte */
                            sysex_val = v + 64;      /* Add offset for SysEx */
                            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, sysex_val, p->two_byte);
                        } else if (p->signed_param == 2) {
                            /* Pan special case: stored with +64 offset */
                            v = clamp_int(atoi(val), -64, 63);
                            sysex_val = v + 64;      /* -64 to +63 becomes 0 to 127 */
                            *b = (uint8_t)sysex_val; /* Store with offset */
                            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, sysex_val, p->two_byte);
                        } else {
                            /* Unsigned param */
                            v = clamp_int(atoi(val), 0, 127);
                            *b = (uint8_t)v;
                            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                        }
                        return;

                    case TP_BITFIELD:
                        v = clamp_int(atoi(val), 0, p->bit_mask);
                        *b = (*b & ~(p->bit_mask << p->bit_shift)) | ((v & p->bit_mask) << p->bit_shift);
                        /* FXM depth special case: SysEx wants value-1 (1-16 becomes 0-15) */
                        if (strcmp(paramName, "fxmdepth") == 0 && v > 0) {
                            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v - 1, p->two_byte);
                        } else {
                            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                        }
                        return;

                    case TP_BOOL:
                        v = (strcmp(val, "On") == 0 || atoi(val)) ? 1 : 0;
                        if (v) *b |= (1 << p->bit_shift);
                        else *b &= ~(1 << p->bit_shift);
                        v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                        return;

                    case TP_ENUM:
                        /* Special handling for specific enums */
                        if (strcmp(paramName, "filtermode") == 0) {
                            if (strcmp(val, "Off") == 0) v = 0;
                            else if (strcmp(val, "LPF") == 0) v = 1;
                            else if (strcmp(val, "HPF") == 0) v = 2;
                            else v = clamp_int(atoi(val), 0, 2);
                            *b = (*b & ~(p->bit_mask << p->bit_shift)) | ((v & p->bit_mask) << p->bit_shift);
                            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                            return;
                        }
                        if (strcmp(paramName, "resonancemode") == 0) {
                            v = (strcmp(val, "Hard") == 0 || atoi(val)) ? 1 : 0;
                            if (v) *b |= 0x80; else *b &= ~0x80;
                            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                            return;
                        }
                        /* Default enum handling */
                        v = clamp_int(atoi(val), 0, p->bit_mask);
                        *b = (*b & ~(p->bit_mask << p->bit_shift)) | ((v & p->bit_mask) << p->bit_shift);
                        v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                        return;
                }
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

            /* Handle patchbank - selects which bank patchnumber refers to
             * Accepts: "User", "Internal", "Preset A", "Preset B" or numeric 0-3
             * Also triggers patch load at current index from new bank
             * Note: "Card" bank removed - expansion waveforms accessed via User patches */
            if (strcmp(paramName, "patchbank") == 0) {
                int bank;
                if (strcmp(val, "User") == 0) bank = 0;
                else if (strcmp(val, "Internal") == 0) bank = 1;
                else if (strcmp(val, "Preset A") == 0) bank = 2;
                else if (strcmp(val, "Preset B") == 0) bank = 3;
                else bank = clamp_int(atoi(val), 0, 3);
                inst->part_patchbank[partIdx] = bank;
                jv_debug("[JV880] Part %d patchbank set to %s (%d)\n", partIdx + 1, val, bank);

                /* Get current patch index (0-63) and load from new bank */
                int currentPatch = inst->mcu->sram[partBase + 16];
                int patchInBank = currentPatch % 64;
                int actualPatchNum;

                switch (bank) {
                    case 0:  /* User - copy user patch to temp area for playback */
                        if (inst->mcu && patchInBank < NUM_USER_PATCHES) {
                            uint32_t src = NVRAM_PATCH_INTERNAL + (patchInBank * PATCH_SIZE);
                            if (inst->mcu->nvram[src] != 0xFF) {
                                uint32_t dest = patchInBank * PATCH_SIZE;
                                if (dest + PATCH_SIZE <= CARDRAM_SIZE) {
                                    memcpy(&inst->mcu->cardram[dest],
                                           &inst->mcu->nvram[src], PATCH_SIZE);
                                }
                            }
                        }
                        actualPatchNum = 64 + patchInBank;  /* Use cardram range */
                        break;
                    case 1: actualPatchNum = patchInBank; break;         /* Internal: 0-63 */
                    case 2: actualPatchNum = 128 + patchInBank; break;   /* Preset A: 128-191 */
                    case 3: actualPatchNum = 192 + patchInBank; break;   /* Preset B: 192-255 */
                    default: actualPatchNum = patchInBank; break;
                }

                jv_debug("[JV880] Part %d loading patch %d from bank %d (actual: %d)\n",
                        partIdx + 1, patchInBank, bank, actualPatchNum);

                inst->mcu->sram[partBase + 16] = (uint8_t)actualPatchNum;
                v2_queue_part_sysex(inst, partIdx, 0x17, actualPatchNum, 1);
                return;
            }

            /* Handle patchnumber - 0-63 within selected bank
             * Combined with patchbank to determine actual patch */
            if (strcmp(paramName, "patchnumber") == 0) {
                int patchInBank = clamp_int(atoi(val), 0, 63);
                int bank = inst->part_patchbank[partIdx];
                int actualPatchNum;

                switch (bank) {
                    case 0:  /* User - copy user patch to cardram for playback */
                        if (inst->mcu && patchInBank < NUM_USER_PATCHES) {
                            uint32_t src = NVRAM_PATCH_INTERNAL + (patchInBank * PATCH_SIZE);
                            if (inst->mcu->nvram[src] != 0xFF) {
                                uint32_t dest = patchInBank * PATCH_SIZE;
                                if (dest + PATCH_SIZE <= CARDRAM_SIZE) {
                                    memcpy(&inst->mcu->cardram[dest],
                                           &inst->mcu->nvram[src], PATCH_SIZE);
                                }
                            }
                        }
                        actualPatchNum = 64 + patchInBank;  /* Use cardram range */
                        break;
                    case 1:  /* Internal: 0-63 */
                        actualPatchNum = patchInBank;
                        break;
                    case 2:  /* Preset A: 128-191 */
                        actualPatchNum = 128 + patchInBank;
                        break;
                    case 3:  /* Preset B: 192-255 */
                        actualPatchNum = 192 + patchInBank;
                        break;
                    default:
                        actualPatchNum = patchInBank;
                        break;
                }

                inst->mcu->sram[partBase + 16] = (uint8_t)actualPatchNum;
                v2_queue_part_sysex(inst, partIdx, 0x17, actualPatchNum, 1);
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
                v2_queue_part_sysex(inst, partIdx, 29, v, 0);
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
                v2_queue_part_sysex(inst, partIdx, 30, v, 0);
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
                v2_queue_part_sysex(inst, partIdx, sysexIdx, v, 0);
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
    } else if (strncmp(key, "save_to_slot_", 13) == 0 && inst->mcu) {
        /* Save current working patch to user slot 1-64 (menu items) */
        int slot = atoi(key + 13) - 1;  /* Convert 1-64 to 0-63 */
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
            }
        }
    } else if (strncmp(key, "load_from_slot_", 15) == 0 && inst->mcu) {
        /* Load user patch from slot 1-64 (menu items) */
        int slot = atoi(key + 15) - 1;  /* Convert 1-64 to 0-63 */
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            /* Check if slot has valid data (not 0xFF filled) */
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded patch '%s' from User slot %d\n", name, slot + 1);
            } else {
                fprintf(stderr, "JV880 v2: User slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "save_to_user_slot") == 0 && inst->mcu) {
        /* Save current working patch to user slot (0-63) - for UI menu */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
            }
        }
    } else if (strcmp(key, "do_save_to_slot") == 0 && inst->mcu) {
        /* Save to slot - triggered by items_param/select_param UI pattern */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
            }
        }
    } else if (strcmp(key, "do_load_from_slot") == 0 && inst->mcu) {
        /* Load from slot - triggered by items_param/select_param UI pattern */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded patch '%s' from User slot %d\n", name, slot + 1);
            } else {
                fprintf(stderr, "JV880 v2: User slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "save_slot") == 0 && inst->mcu) {
        /* Save slot browser - selecting a slot saves current patch there */
        int slot = atoi(val);
        inst->save_slot_index = slot;
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
            }
        }
    } else if (strcmp(key, "load_slot") == 0 && inst->mcu) {
        /* Load slot browser - selecting a slot loads that patch */
        int slot = atoi(val);
        inst->load_slot_index = slot;
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                /* Copy user patch to working patch area */
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded user patch '%s' from slot %d\n", name, slot + 1);

                /* Ensure patch mode in NVRAM (like v2_select_patch does) */
                inst->mcu->nvram[NVRAM_MODE_OFFSET] = 1;

                /* Trigger emulator to reload patch via PC 0 */
                uint8_t pc_msg[2] = { 0xC0, 0x00 };
                pthread_mutex_lock(&inst->ring_mutex);
                int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
                if (next != inst->midi_read) {
                    memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
                    inst->midi_queue_len[inst->midi_write] = 2;
                    inst->midi_write = next;
                }
                pthread_mutex_unlock(&inst->ring_mutex);
            } else {
                fprintf(stderr, "JV880 v2: User patch slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "load_user_patch") == 0 && inst->mcu) {
        /* Load a user patch from NVRAM into working area (legacy) */
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

/* Tone param cache - avoid hitting NVRAM on every poll */
#define TONE_CACHE_SIZE (4 * 84)  /* 4 tones × 84 bytes each */
static uint8_t tone_cache[TONE_CACHE_SIZE];
static uint64_t tone_cache_valid = 0;  /* Timestamp when cache was filled */
#define TONE_CACHE_TTL_MS 50  /* Refresh cache every 50ms */

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* v2: Get parameter */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst) return -1;

    /* Fast path for frequently-accessed tone params - uses cache to avoid NVRAM reads */
    if (strncmp(key, "nvram_tone_", 11) == 0 && inst->mcu) {
        int toneIdx = atoi(key + 11);
        const char* underscore = strchr(key + 11, '_');
        if (underscore && toneIdx >= 0 && toneIdx < 4) {
            const char* paramName = underscore + 1;

            /* Refresh cache if stale */
            uint64_t now = get_time_ms();
            if (now - tone_cache_valid > TONE_CACHE_TTL_MS) {
                int nvramBase = NVRAM_PATCH_OFFSET + 26;
                memcpy(tone_cache, &inst->mcu->nvram[nvramBase], TONE_CACHE_SIZE);
                tone_cache_valid = now;
            }

            const ToneParamEntry *p = find_tone_param(paramName);
            if (p) {
                int cacheOffset = (toneIdx * 84) + p->nvram_offset;
                uint8_t byte = tone_cache[cacheOffset];
                switch (p->type) {
                    case TP_BYTE:
                        if (p->signed_param == 1) {
                            /* Signed param: read as int8_t for proper sign extension */
                            return snprintf(buf, buf_len, "%d", (int)(int8_t)byte);
                        } else if (p->signed_param == 2) {
                            /* Pan special case: stored with +64 offset, display as -64 to +63 */
                            return snprintf(buf, buf_len, "%d", (int)byte - 64);
                        } else {
                            return snprintf(buf, buf_len, "%d", byte);
                        }
                    case TP_BITFIELD:
                        /* FXM depth special case: stored 0-15, display 1-16 */
                        if (strcmp(paramName, "fxmdepth") == 0) {
                            return snprintf(buf, buf_len, "%d", ((byte >> p->bit_shift) & p->bit_mask) + 1);
                        }
                        return snprintf(buf, buf_len, "%d", (byte >> p->bit_shift) & p->bit_mask);
                    case TP_BOOL:
                        return snprintf(buf, buf_len, "%s", (byte & (1 << p->bit_shift)) ? "On" : "Off");
                    case TP_ENUM:
                        /* Special handling for filtermode and resonancemode */
                        if (strcmp(paramName, "filtermode") == 0) {
                            int v = (byte >> p->bit_shift) & p->bit_mask;
                            const char *labels[] = {"Off", "LPF", "HPF"};
                            return snprintf(buf, buf_len, "%s", labels[v < 3 ? v : 0]);
                        }
                        if (strcmp(paramName, "resonancemode") == 0) {
                            return snprintf(buf, buf_len, "%s", (byte & 0x80) ? "Hard" : "Soft");
                        }
                        return snprintf(buf, buf_len, "%d", (byte >> p->bit_shift) & p->bit_mask);
                }
            }
        }
    }

    /* Fast path for sram_part_ params (performance mode editing) */
    if (strncmp(key, "sram_part_", 10) == 0 && inst->mcu) {
        int partIdx = key[10] - '0';
        if (partIdx >= 0 && partIdx < 8 && key[11] == '_') {
            const char* paramName = key + 12;
            int partBase = SRAM_TEMP_PERF_OFFSET + TEMP_PERF_COMMON_SIZE + (partIdx * TEMP_PERF_PART_SIZE);
            int offset = -1;

            /* Handle patchbank - return stored value, derive from SRAM only if uninitialized (-1) */
            if (strcmp(paramName, "patchbank") == 0) {
                static const char *bank_names[] = {"User", "Internal", "Preset A", "Preset B"};
                int bank = inst->part_patchbank[partIdx];

                /* If not yet initialized, derive from actual patch number */
                if (bank < 0 || bank > 3) {
                    int actualPatch = inst->mcu->sram[partBase + 16];
                    if (actualPatch < 64) bank = 1;        /* Internal */
                    else if (actualPatch < 128) bank = 0;  /* User (via cardram) */
                    else if (actualPatch < 192) bank = 2;  /* Preset A */
                    else bank = 3;                         /* Preset B */
                    inst->part_patchbank[partIdx] = bank;
                }
                return snprintf(buf, buf_len, "%s", bank_names[bank]);
            }

            /* Handle patchnumber - return 0-63 within selected bank */
            if (strcmp(paramName, "patchnumber") == 0) {
                int actualPatch = inst->mcu->sram[partBase + 16];
                int patchInBank = actualPatch % 64;  /* Extract 0-63 within bank */
                return snprintf(buf, buf_len, "%d", patchInBank);
            }

            /* Direct storage parameters */
            if (strcmp(paramName, "partlevel") == 0) offset = 17;
            else if (strcmp(paramName, "partpan") == 0) offset = 18;
            else if (strcmp(paramName, "internalkeyrangelower") == 0) offset = 10;
            else if (strcmp(paramName, "internalkeyrangeupper") == 0) offset = 11;

            /* Handle reverbswitch and chorusswitch as bit fields */
            if (strcmp(paramName, "reverbswitch") == 0) {
                uint8_t b = inst->mcu->sram[partBase + 21];
                return snprintf(buf, buf_len, "%s", ((b >> 6) & 1) ? "On" : "Off");
            }
            if (strcmp(paramName, "chorusswitch") == 0) {
                uint8_t b = inst->mcu->sram[partBase + 21];
                return snprintf(buf, buf_len, "%s", ((b >> 5) & 1) ? "On" : "Off");
            }

            /* Signed offset parameters */
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

    /* Fast path for nvram_patchCommon_ params */
    if (strncmp(key, "nvram_patchCommon_", 18) == 0 && inst->mcu) {
        const char* paramName = key + 18;
        int offset = -1;

        if (strcmp(paramName, "patchlevel") == 0) offset = 21;
        else if (strcmp(paramName, "patchpan") == 0) offset = 22;
        else if (strcmp(paramName, "reverblevel") == 0) offset = 13;
        else if (strcmp(paramName, "reverbtime") == 0) offset = 14;
        else if (strcmp(paramName, "choruslevel") == 0) offset = 16;
        else if (strcmp(paramName, "chorusdepth") == 0) offset = 17;
        else if (strcmp(paramName, "chorusrate") == 0) offset = 18;
        else if (strcmp(paramName, "analogfeel") == 0) offset = 20;

        if (offset >= 0) {
            return snprintf(buf, buf_len, "%d", inst->mcu->nvram[NVRAM_PATCH_OFFSET + offset]);
        }
    }

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
        int written = snprintf(buf, buf_len,
            "{\"mode\":%d,\"preset\":%d,\"performance\":%d,\"part\":%d,\"octave_transpose\":%d,"
            "\"expansion_index\":%d,\"expansion_bank_offset\":%d",
            inst->performance_mode ? 1 : 0,
            inst->current_patch,
            inst->current_performance,
            inst->current_part,
            inst->octave_transpose,
            inst->current_expansion,
            inst->expansion_bank_offset);

        /* Include working patch data as hex if MCU is ready */
        if (inst->mcu && written < buf_len - 800) {
            written += snprintf(buf + written, buf_len - written, ",\"patch\":\"");
            for (int i = 0; i < PATCH_SIZE && written < buf_len - 10; i++) {
                written += snprintf(buf + written, buf_len - written, "%02X",
                    inst->mcu->nvram[NVRAM_PATCH_OFFSET + i]);
            }
            written += snprintf(buf + written, buf_len - written, "\"");
        }
        written += snprintf(buf + written, buf_len - written, "}");
        return written;
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
    /* Patchbank list for part bank selection (no Card - expansion waveforms via User patches) */
    if (strcmp(key, "patchbank_list") == 0) {
        return snprintf(buf, buf_len,
            "[{\"index\":1,\"name\":\"Internal\"},"
            "{\"index\":2,\"name\":\"Preset A\"},"
            "{\"index\":3,\"name\":\"Preset B\"}]");
    }
    /* Card expansion list for Performance mode - only actual expansions, no factory */
    if (strcmp(key, "card_expansion_list") == 0) {
        if (inst->expansion_count == 0) {
            return snprintf(buf, buf_len, "[{\"index\":-1,\"name\":\"No expansions found\"}]");
        }
        int written = snprintf(buf, buf_len, "[");
        for (int i = 0; i < inst->expansion_count && written < buf_len - 100; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"index\":%d,\"name\":\"%s\",\"patch_count\":%d}",
                i, inst->expansions[i].name, inst->expansions[i].patch_count);
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
    /* Save patch slot list - shows all 64 slots for saving */
    if (strcmp(key, "save_patch_slot_list") == 0) {
        int written = snprintf(buf, buf_len, "[");
        for (int i = 0; i < 64; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");

            const char *slot_name = "(empty)";
            char name_buf[16];

            if (inst->mcu) {
                uint32_t offset = NVRAM_PATCH_INTERNAL + (i * PATCH_SIZE);
                if (inst->mcu->nvram[offset] != 0xFF) {
                    /* Copy and trim patch name */
                    memcpy(name_buf, &inst->mcu->nvram[offset], 12);
                    name_buf[12] = '\0';
                    for (int j = 11; j >= 0 && name_buf[j] == ' '; j--) {
                        name_buf[j] = '\0';
                    }
                    slot_name = name_buf;
                }
            }

            written += snprintf(buf + written, buf_len - written,
                "{\"index\":%d,\"name\":\"%02d: %s\"}", i, i + 1, slot_name);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Load patch slot list - shows only non-empty slots for loading */
    if (strcmp(key, "load_patch_slot_list") == 0) {
        if (!inst->mcu) {
            return snprintf(buf, buf_len, "[{\"index\":-1,\"name\":\"Loading...\"}]");
        }
        int written = snprintf(buf, buf_len, "[");
        int first = 1;
        for (int i = 0; i < NUM_USER_PATCHES && written < buf_len - 100; i++) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (i * PATCH_SIZE);
            /* Only show slots with valid data */
            if (inst->mcu->nvram[offset] != 0xFF) {
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
                if (!first) written += snprintf(buf + written, buf_len - written, ",");
                first = 0;
                written += snprintf(buf + written, buf_len - written,
                    "{\"index\":%d,\"name\":\"%02d: %s\"}", i, i + 1, name);
            }
        }
        /* If no patches found, show message */
        if (first) {
            written += snprintf(buf + written, buf_len - written,
                "{\"index\":-1,\"name\":\"No saved patches\"}");
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Save slot current index */
    if (strcmp(key, "save_slot") == 0) {
        return snprintf(buf, buf_len, "%d", inst->save_slot_index);
    }
    /* Save slot count for browser */
    if (strcmp(key, "save_slot_count") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_USER_PATCHES);
    }
    /* Save slot name for browser */
    if (strcmp(key, "save_slot_name") == 0 && inst->mcu) {
        int idx = inst->save_slot_index;
        if (idx >= 0 && idx < NUM_USER_PATCHES) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (idx * PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            if (inst->mcu->nvram[offset] != 0xFF) {
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
            } else {
                strcpy(name, "(empty)");
            }
            return snprintf(buf, buf_len, "%02d: %s", idx + 1, name);
        }
        return snprintf(buf, buf_len, "01: (empty)");
    }
    /* Load slot current index */
    if (strcmp(key, "load_slot") == 0) {
        return snprintf(buf, buf_len, "%d", inst->load_slot_index);
    }
    /* Load slot count for browser */
    if (strcmp(key, "load_slot_count") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_USER_PATCHES);
    }
    /* Load slot name for browser */
    if (strcmp(key, "load_slot_name") == 0 && inst->mcu) {
        int idx = inst->load_slot_index;
        if (idx >= 0 && idx < NUM_USER_PATCHES) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (idx * PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            if (inst->mcu->nvram[offset] != 0xFF) {
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
            } else {
                strcpy(name, "(empty)");
            }
            return snprintf(buf, buf_len, "%02d: %s", idx + 1, name);
        }
        return snprintf(buf, buf_len, "01: (empty)");
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
                        "{\"level\":\"expansions\",\"label\":\"Jump to Expansion\"}"
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
                    "\"knobs\":[\"level\",\"pan\",\"cutofffrequency\",\"resonance\",\"lfo1pitchdepth\",\"lfo1tvfdepth\",\"tvaenvtime1\",\"tvaenvtime2\"],"
                    "\"params\":["
                        "\"toneswitch\",\"wavegroup\",\"wavenumber\","
                        "\"level\",\"pan\",\"levelkeyfollow\",\"panningkeyfollow\","
                        "\"cutofffrequency\",\"resonance\",\"filtermode\",\"resonancemode\",\"cutoffkeyfollow\","
                        "\"pitchcoarse\",\"pitchfine\",\"randompitchdepth\",\"pitchkeyfollow\","
                        "\"lfo1form\",\"lfo1rate\",\"lfo1delay\",\"lfo1pitchdepth\",\"lfo1tvfdepth\",\"lfo1tvadepth\","
                        "\"lfo2form\",\"lfo2rate\",\"lfo2delay\",\"lfo2pitchdepth\",\"lfo2tvfdepth\",\"lfo2tvadepth\","
                        "\"penvdepth\",\"penvtime1\",\"penvlevel1\",\"penvtime2\",\"penvlevel2\",\"penvtime3\",\"penvlevel3\",\"penvtime4\",\"penvlevel4\","
                        "\"tvfenvdepth\",\"tvfenvtime1\",\"tvfenvlevel1\",\"tvfenvtime2\",\"tvfenvlevel2\",\"tvfenvtime3\",\"tvfenvlevel3\",\"tvfenvtime4\",\"tvfenvlevel4\","
                        "\"tvaenvtime1\",\"tvaenvlevel1\",\"tvaenvtime2\",\"tvaenvlevel2\",\"tvaenvtime3\",\"tvaenvlevel3\",\"tvaenvtime4\","
                        "\"velocityrangelower\",\"velocityrangeupper\","
                        "\"tonedelaymode\",\"tonedelaytime\","
                        "\"fxmswitch\",\"fxmdepth\","
                        "\"drylevel\",\"reverbsendlevel\",\"chorussendlevel\""
                    "]"
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
                        "{\"level\":\"load_expansion\",\"label\":\"Load Expansion\"},"
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"}"
                    "]"
                "},"
                "\"load_expansion\":{"
                    "\"label\":\"Load Expansion\","
                    "\"items_param\":\"expansion_list\","
                    "\"select_param\":\"load_expansion\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "},"
                "\"part_selector\":{"
                    "\"label\":\"Parts\","
                    "\"children\":null,"
                    "\"child_prefix\":\"sram_part_\","
                    "\"child_count\":8,"
                    "\"child_label\":\"Part\","
                    "\"knobs\":[\"partlevel\",\"partpan\",\"reverbswitch\",\"chorusswitch\",\"partcoarsetune\",\"partfinetune\",\"internalkeyrangelower\",\"internalkeyrangeupper\"],"
                    "\"params\":[\"patchbank\",\"patchnumber\",\"partlevel\",\"partpan\",\"reverbswitch\",\"chorusswitch\",\"partcoarsetune\",\"partfinetune\",\"internalkeyrangelower\",\"internalkeyrangeupper\",\"internalkeytranspose\"]"
                "},"
                "\"expansions\":{"
                    "\"label\":\"Jump to Expansion\","
                    "\"items_param\":\"expansion_list\","
                    "\"select_param\":\"jump_to_expansion\","
                    "\"navigate_to\":\"patch\","
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
            /* Tone params - Wave */
            "{\"key\":\"toneswitch\",\"name\":\"Tone Switch\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"wavegroup\",\"name\":\"Wave Group\",\"type\":\"int\",\"min\":0,\"max\":3},"
            "{\"key\":\"wavenumber\",\"name\":\"Wave Number\",\"type\":\"int\",\"min\":0,\"max\":255},"
            /* Tone params - Level/Pan */
            "{\"key\":\"level\",\"name\":\"Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"pan\",\"name\":\"Pan\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"levelkeyfollow\",\"name\":\"Level KF\",\"type\":\"int\",\"min\":0,\"max\":15},"
            "{\"key\":\"panningkeyfollow\",\"name\":\"Pan KF\",\"type\":\"int\",\"min\":0,\"max\":15},"
            /* Tone params - Filter */
            "{\"key\":\"cutofffrequency\",\"name\":\"Cutoff\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"resonance\",\"name\":\"Resonance\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"filtermode\",\"name\":\"Filter Mode\",\"type\":\"enum\",\"options\":[\"Off\",\"LPF\",\"HPF\"]},"
            "{\"key\":\"resonancemode\",\"name\":\"Reso Mode\",\"type\":\"enum\",\"options\":[\"Soft\",\"Hard\"]},"
            "{\"key\":\"cutoffkeyfollow\",\"name\":\"Cutoff KF\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params - Pitch */
            "{\"key\":\"pitchcoarse\",\"name\":\"Pitch Coarse\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"pitchfine\",\"name\":\"Pitch Fine\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"randompitchdepth\",\"name\":\"Random Pitch\",\"type\":\"int\",\"min\":0,\"max\":7},"
            "{\"key\":\"pitchkeyfollow\",\"name\":\"Pitch KF\",\"type\":\"int\",\"min\":0,\"max\":15},"
            /* Tone params - LFO1 */
            "{\"key\":\"lfo1form\",\"name\":\"LFO1 Wave\",\"type\":\"int\",\"min\":0,\"max\":5},"
            "{\"key\":\"lfo1rate\",\"name\":\"LFO1 Rate\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo1delay\",\"name\":\"LFO1 Delay\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo1pitchdepth\",\"name\":\"LFO1 Pitch\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo1tvfdepth\",\"name\":\"LFO1 Filter\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo1tvadepth\",\"name\":\"LFO1 Amp\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params - LFO2 */
            "{\"key\":\"lfo2form\",\"name\":\"LFO2 Wave\",\"type\":\"int\",\"min\":0,\"max\":5},"
            "{\"key\":\"lfo2rate\",\"name\":\"LFO2 Rate\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo2delay\",\"name\":\"LFO2 Delay\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo2pitchdepth\",\"name\":\"LFO2 Pitch\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo2tvfdepth\",\"name\":\"LFO2 Filter\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"lfo2tvadepth\",\"name\":\"LFO2 Amp\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params - Pitch Envelope */
            "{\"key\":\"penvdepth\",\"name\":\"P.Env Depth\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvtime1\",\"name\":\"P.Env T1\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvlevel1\",\"name\":\"P.Env L1\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvtime2\",\"name\":\"P.Env T2\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvlevel2\",\"name\":\"P.Env L2\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvtime3\",\"name\":\"P.Env T3\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvlevel3\",\"name\":\"P.Env L3\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvtime4\",\"name\":\"P.Env T4\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"penvlevel4\",\"name\":\"P.Env L4\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params - Filter Envelope */
            "{\"key\":\"tvfenvdepth\",\"name\":\"F.Env Depth\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvtime1\",\"name\":\"F.Env T1\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvlevel1\",\"name\":\"F.Env L1\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvtime2\",\"name\":\"F.Env T2\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvlevel2\",\"name\":\"F.Env L2\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvtime3\",\"name\":\"F.Env T3\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvlevel3\",\"name\":\"F.Env L3\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvtime4\",\"name\":\"F.Env T4\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvfenvlevel4\",\"name\":\"F.Env L4\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params - Amp Envelope */
            "{\"key\":\"tvaenvtime1\",\"name\":\"A.Env T1\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvlevel1\",\"name\":\"A.Env L1\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvtime2\",\"name\":\"A.Env T2\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvlevel2\",\"name\":\"A.Env L2\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvtime3\",\"name\":\"A.Env T3\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvlevel3\",\"name\":\"A.Env L3\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"tvaenvtime4\",\"name\":\"A.Env T4\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params - Velocity/Delay */
            "{\"key\":\"velocityrangelower\",\"name\":\"Vel Lo\",\"type\":\"int\",\"min\":1,\"max\":127},"
            "{\"key\":\"velocityrangeupper\",\"name\":\"Vel Hi\",\"type\":\"int\",\"min\":1,\"max\":127},"
            "{\"key\":\"tonedelaymode\",\"name\":\"Delay Mode\",\"type\":\"int\",\"min\":0,\"max\":3},"
            "{\"key\":\"tonedelaytime\",\"name\":\"Delay Time\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Tone params - FXM */
            "{\"key\":\"fxmswitch\",\"name\":\"FXM\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"fxmdepth\",\"name\":\"FXM Depth\",\"type\":\"int\",\"min\":0,\"max\":15},"
            /* Tone params - Output */
            "{\"key\":\"drylevel\",\"name\":\"Dry Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"reverbsendlevel\",\"name\":\"Reverb Send\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"chorussendlevel\",\"name\":\"Chorus Send\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Part params (suffix only - child_prefix adds sram_part_N_) */
            "{\"key\":\"patchbank\",\"name\":\"Bank\",\"type\":\"enum\",\"options\":[\"Internal\",\"Preset A\",\"Preset B\"]},"
            "{\"key\":\"partlevel\",\"name\":\"Part Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"partpan\",\"name\":\"Part Pan\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"patchnumber\",\"name\":\"Patch\",\"type\":\"int\",\"min\":0,\"max\":63},"
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
        jv_debug("[JV880] UNDERRUN #%d: needed %d, had %d (min_level=%d, renders=%d)\n",
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
    jv_debug("[JV880] v2 API initialized\n");
    return &jv880_api_v2;
}
