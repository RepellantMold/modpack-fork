#ifndef __MODPACK_PROTRACKER_H__
#define __MODPACK_PROTRACKER_H__

#include "buffer.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define PT_NUM_SAMPLES  (31)
#define PT_NUM_POSITIONS (128)
#define PT_NUM_CHANNELS (4)
#define PT_PATTERN_ROWS (64)
#define PT_PATTERN_SIZE (PT_NUM_CHANNELS * 4 * PT_PATTERN_ROWS)
#define PT_SAMPLE_HEADER_SIZE 30
#define PT_MAX_SONG_NAME_LENGTH 20
#define PT_MAX_SAMPLE_NAME_LENGTH 22
#define PT_MAGIC_STRING_LENGTH 4
#define PT_SONG_LENGTH (1+1+128)

#define PT_CMD_ARPEGGIO         (0)
#define PT_CMD_SLIDE_UP         (1)
#define PT_CMD_SLIDE_DOWN       (2)
#define PT_CMD_SLIDE_TO_NOTE    (3)
#define PT_CMD_VIBRATO          (4)
#define PT_CMD_CONTINUE_SLIDE   (5)
#define PT_CMD_CONTINUE_VIBRATO (6)
#define PT_CMD_TREMOLO          (7)
#define PT_CMD_8                (8)
#define PT_CMD_SET_SAMPLE_OFS   (9)
#define PT_CMD_VOLUME_SLIDE     (10)
#define PT_CMD_POS_JUMP         (11)
#define PT_CMD_SET_VOLUME       (12)
#define PT_CMD_PATTERN_BREAK    (13)
#define PT_CMD_EXTENDED         (14) // Extended command
#define PT_CMD_SET_SPEED        (15)

#define PT_ECMD_FILTER                  (0)
#define PT_ECMD_FINESLIDE_UP            (1)
#define PT_ECMD_FINESLIDE_DOWN          (2)
#define PT_ECMD_SET_GLISSANDO           (3)
#define PT_ECMD_SET_VIBRATO_WAVEFORM    (4)
#define PT_ECMD_SET_FINETUNE_VALUE      (5)
#define PT_ECMD_LOOP_PATTERN            (6)
#define PT_ECMD_SET_TREMOLO_WAVEFORM    (7)
#define PT_ECMD_E8                      (8)
#define PT_ECMD_RETRIGGER_SAMPLE        (9)
#define PT_ECMD_FINE_VOLUME_SLIDE_UP    (10)
#define PT_ECMD_FINE_VOLUME_SLIDE_DOWN  (11)
#define PT_ECMD_CUT_SAMPLE              (12)
#define PT_ECMD_DELAY_SAMPLE            (13)
#define PT_ECMD_DELAY_PATTERN           (14)
#define PT_ECMD_INVERT_LOOP             (15)

typedef struct __attribute__((__packed__))
{
    char name[PT_MAX_SONG_NAME_LENGTH];
} protracker_header_t;

typedef struct __attribute__((__packed__))
{
    char name[PT_MAX_SAMPLE_NAME_LENGTH];
    uint16_t length;        // sample length in words (1 word == 2 bytes)
    uint8_t finetone;       // low nibble
    uint8_t volume;         // sample volume (0..64)
    uint16_t repeat_offset;
    uint16_t repeat_length;
} protracker_sample_t;

typedef struct __attribute__((__packed__))
{
    uint8_t length;
    uint8_t restart_position;
    uint8_t positions[PT_NUM_POSITIONS];
} protracker_song_t;

typedef struct __attribute__((__packed__))
{
    uint8_t data[4];
} protracker_channel_t;

typedef struct
{
    uint8_t cmd:4;

    union {
        struct {
            uint8_t value:4;
            uint8_t cmd:4;
        } ext;
        uint8_t value;
    } data;
} protracker_effect_t;

typedef struct __attribute__((__packed__))
{
    protracker_channel_t channels[PT_NUM_CHANNELS];
} protracker_pattern_row_t;

typedef struct __attribute__((__packed__))
{
    protracker_pattern_row_t rows[PT_PATTERN_ROWS];
} protracker_pattern_t;

typedef struct __attribute__((__packed__))
{
    protracker_header_t header;

    protracker_song_t song;

    protracker_pattern_t* patterns;
    size_t num_patterns;

    protracker_sample_t sample_headers[PT_NUM_SAMPLES];
    uint8_t* sample_data[PT_NUM_SAMPLES];
} protracker_t;

void protracker_create(protracker_t* module);
void protracker_destroy(protracker_t* module);
void protracker_free(protracker_t* module);

protracker_t* protracker_load(const buffer_t* buffer);
bool protracker_convert(buffer_t* buffer, const protracker_t* module, const char* opts);

uint8_t protracker_get_sample(const protracker_channel_t* channel);
uint16_t protracker_get_period(const protracker_channel_t* channel);
protracker_effect_t protracker_get_effect(const protracker_channel_t* channel);

void protracker_set_sample(protracker_channel_t* channel, uint8_t sample);
void protracker_set_period(protracker_channel_t* channel, uint16_t period);
void protracker_set_effect(protracker_channel_t* channel, const protracker_effect_t* effect);

/**
 *
 * Get sample usage in tune
 *
 * module - ProTracker module
 * usage - Usage table (must allow for PT_NUM_SAMPLES entries)
 *
 * Returns number of active samples
 *
**/
size_t protracker_get_used_samples(const protracker_t* module, bool* usage);

/**
 *
 * module - ProTracker module
 *
 * returns number of patterns used in module
 *
**/
size_t protracker_get_pattern_count(const protracker_t* module);

/**
 *
 * Remove patterns that are not included in the module
 *
**/
void protracker_remove_unused_patterns(protracker_t* module);

/**
 *
 * Remove samples from module that are not by any pattern (sample index is preserved)
 *
**/
void protracker_remove_unused_samples(protracker_t* module);

/**
 *
 * Remove samples that are identical
 *
**/
void protracker_remove_identical_samples(protracker_t* module);

/**
 *
 * Compact sample indexes to remove empty space in sample list
 *
**/
void protracker_compact_sample_indexes(protracker_t* module);

/**
 *
 * Trim sample data, removing zero bytes from the end of non-looping samples
 *
**/
void protracker_trim_samples(protracker_t* module);

/**
 *
 * Clean effects, removing unnecessary effects and downgrading them to simpler variations
 *
**/
void protracker_clean_effects(protracker_t* module, const char* options);

/**
 *
 * Pattern iterator to simplify transforming ProTracker pattern data
 *
**/
void protracker_transform_notes(protracker_t* module, void (*transform)(protracker_channel_t* channel, uint8_t index, void* data), void* data);

/**
 *
 * Pattern iterator to simplify scanning ProTracker pattern data
 *
**/
void protracker_scan_notes(const protracker_t* module, void (*scan)(const protracker_channel_t* channel, uint8_t index, void* data), void* data);

/**
 *
 * Build a text string out of a ProTracker channel
 *
 * channel - Channel to print
 * out - Output buffer
 * buflen - Buffer size
 *
**/
void protracker_channel_to_text(const protracker_channel_t* channel, char *out, size_t buflen);

#endif
