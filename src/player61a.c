#include "player61a.h"
#include "options.h"
#include "endianness.h"
#include "log.h"

/*
		;0 = two files (song + samples)
		;1 = sign 'P61A'
		;2 = no samples
		;3 = tempo
		;4 = icon
		;5 = delta - 8 bit delta
		;6 = sample packing - 4 bit delta

*/

/*

    0 / 4: P61A
    4 / 2: ???
    7 / 1: Number of patterns?

    4 / 2: Sample buffer offset


*/

#include <string.h>

static const char* signature = "P61A";

static void build_samples(player61a_t* output, const protracker_t* module, const char* options, uint32_t* usecode)
{
    LOG_DEBUG("Building sample table:\n");

    bool usage[PT_NUM_SAMPLES];
    size_t sample_count = protracker_get_used_samples(module, usage);

    for (size_t i = 0; i < PT_NUM_SAMPLES; ++i)
    {
        const protracker_sample_t* input = &(module->sample_headers[i]);
        p61a_sample_t* sample = &(output->sample_headers[i]);

        if (!usage[i])
        {
            continue;
        }

        uint16_t length;
        if (!input->length)
        {
            // empty

            sample->length = 1;
            sample->finetone = 0;
            sample->volume = 0;
            sample->repeat_offset = 0xffff;
        }
        else if (input->repeat_length > 1)
        {
            // looping

            length = input->repeat_offset + input->repeat_length;
            LOG_TRACE(" #%lu - %u bytes (looped)\n", (i+1), length * 2);

            if (length != input->length)
            {
                LOG_WARN("Looped sample #%lu truncated (%u -> %u bytes).\n", (i+1), input->length * 2, length * 2);
            }

            sample->length = length;
            sample->finetone = input->finetone;
            sample->volume = input->volume > 64 ? 64 : input->volume;
            sample->repeat_offset = input->repeat_offset;
        }
        else
        {
            // not looping

            length = input->length;
            LOG_TRACE(" #%lu - %u bytes\n", (i+1), input->length * 2);

            sample->length = length;
            sample->finetone = input->finetone;
            sample->volume = input->volume > 64 ? 64 : input->volume;
            sample->repeat_offset = 0xffff;
        }

        if (sample->finetone)
        {
            *usecode |= 1; // mark finetones used in usecode
        }

        // TODO: compression / delta encoding

        if (input->length > 0)
        {
            buffer_add(&(output->samples), module->sample_data[i], length * 2);
        }
        else
        {
            uint16_t temp = 0;
            buffer_add(&(output->samples), &temp, sizeof(temp));
        }
    }

    LOG_DEBUG(" %lu samples used.\n", sample_count);

    output->header.sample_count = (uint8_t)sample_count;
}

/*

* P61 Pattern Format:

* o = If set compression info follows
* n = Note (6 bits)
* i = Instrument (5 bits)
* c = Command (4 bits)
* b = Info byte (8 bits)

* onnnnnni iiiicccc bbbbbbbb	Note, instrument and command
* o110cccc bbbbbbbb		Only command
* o1110nnn nnniiiii		Note and instrument
* o1111111			Empty note

* Compression info:

* 00nnnnnn		 	n empty rows follow
* 10nnnnnn		 	n same rows follow (for faster testing)
* 01nnnnnn oooooooo		Jump o (8 bit offset) bytes back for n rows
* 11nnnnnn oooooooo oooooooo	Jump o (16 bit offset) bytes back for n rows

*/


#define CHANNEL_ALL             (0x00)
#define CHANNEL_COMMAND         (0x60)
#define CHANNEL_NOTE_INSTRUMENT (0x70)
#define CHANNEL_EMPTY           (0x7f)
#define CHANNEL_COMPRESSED      (0x80)

#define COMPRESSION_CMD_BITS    (0xc0)
#define COMPRESSION_DATA_BITS   (0x3f)

#define COMPRESSION_EMPTY_ROWS  (0x00)  // Next N rows are empty
#define COMPRESSION_REPEAT_ROWS (0x80)  // Repeat this row N times
#define COMPRESSION_JUMP        (0x40)  // 8-bit jump
#define COMPRESSION_JUMP_LONG   (0x80)  // 16-bit jump (requires COMPRESSION_JUMP)

static uint16_t periods[] = {
    856,808,762,720,678,640,604,570,538,508,480,453,    // octave 1
    428,404,381,360,339,320,302,285,269,254,240,226,    // octave 2
    214,202,190,180,170,160,151,143,135,127,120,113     // octave 3
};

static uint8_t index_from_period(uint16_t period)
{
    for (size_t i = 0; i < (sizeof(periods) / sizeof(uint16_t)); ++i)
    {
        if (periods[i] == period)
        {
            return (uint8_t)(i+1);
        }
    }
    return 0;
}

static uint16_t period_from_index(uint8_t index)
{
    if (!index || index > (sizeof(periods) / sizeof(uint16_t)))
    {
        return 0;
    }

    return periods[index-1];
}

typedef struct
{
    size_t patterns;
    size_t samples;
} p61a_offsets_t;

static size_t get_sample_offset(const player61a_t* module)
{
    size_t curr = 0;

    curr += sizeof(p61a_header_t);   // header
    curr += sizeof(p61a_sample_t) * module->header.sample_count; // sample headers
    curr += sizeof(p61a_pattern_offset_t) * module->header.pattern_count; // pattern offsets
    curr += module->song.length+1; // song positions (+0xff)

    curr += buffer_count(&(module->patterns)); // tracks

    if (curr & 1)
    {
        ++curr;
    }

    return curr;
}

static size_t get_channel_length(const p61a_channel_t* channel)
{
    if ((channel->data[0] & CHANNEL_EMPTY) == CHANNEL_EMPTY)
        return 1;

    if ((channel->data[0] & CHANNEL_NOTE_INSTRUMENT) == CHANNEL_NOTE_INSTRUMENT)
        return 2;

    if ((channel->data[0] & CHANNEL_COMMAND) == CHANNEL_COMMAND)
        return 2;

    return 3;
}

static size_t to_p61a_channel(p61a_channel_t* out, const protracker_channel_t* in, const protracker_pattern_row_t* row, size_t channel_index, uint32_t* usecode)
{
    uint8_t instrument = protracker_get_sample(in);
    uint16_t period = protracker_get_period(in);
    protracker_effect_t effect = protracker_get_effect(in);

    uint8_t note = index_from_period(period);

    bool has_command = (effect.cmd + effect.data.value) != 0;
    switch (effect.cmd)
    {
        case PT_CMD_ARPEGGIO:
        {
            if (effect.data.value)
            {
                effect.cmd = PT_CMD_8; // P61A uses 8 for arpeggio
            }
        }
        break;

        case PT_CMD_SLIDE_UP:
        case PT_CMD_SLIDE_DOWN:
        {
            has_command = effect.data.value != 0;
        }
        break;

        // TODO: these commands need processing
        case PT_CMD_CONTINUE_SLIDE:
        case PT_CMD_CONTINUE_VIBRATO:
        case PT_CMD_VOLUME_SLIDE:
        case PT_CMD_POS_JUMP:
        case PT_CMD_PATTERN_BREAK:
        {
        }
        break;

        case PT_CMD_SET_VOLUME:
        {
            effect.data.value = effect.data.value > 64 ? 64 : effect.data.value;
        }
        break;

        case PT_CMD_8:  // 8xy -> E8y
        {
            effect.cmd = PT_CMD_EXTENDED;
            effect.data.ext.cmd = PT_ECMD_E8;
        }
        break;

        case PT_CMD_EXTENDED:
        {
            switch (effect.data.ext.cmd)
            {
                case PT_ECMD_FILTER:                    // E0x
                {
                    effect.data.ext.value = (effect.data.ext.value & 1) << 1;
                }
                break;

                case PT_ECMD_CUT_SAMPLE:                // ECx
                {
                    if (effect.data.ext.value == 0)
                    {
                        effect.cmd = PT_CMD_SET_VOLUME;
                        effect.data.value = 0;
                    }
                }
                break;

                case PT_ECMD_FINESLIDE_UP:              // E1x
                case PT_ECMD_FINESLIDE_DOWN:            // E2x
                case PT_ECMD_RETRIGGER_SAMPLE:          // E9x
                case PT_ECMD_FINE_VOLUME_SLIDE_UP:      // EAx
                case PT_ECMD_FINE_VOLUME_SLIDE_DOWN:    // EBx
                case PT_ECMD_DELAY_SAMPLE:              // EDx
                case PT_ECMD_DELAY_PATTERN:             // EEx
                {
                    has_command = effect.data.ext.value != 0;
                }
                break;
            }
        }
        break;
    }

    if (has_command)
    {
        *usecode |= (effect.cmd == PT_CMD_EXTENDED) ? (1 << (effect.data.ext.cmd + 16)) : (1 << (effect.cmd));
    }
    else
    {
        effect.cmd = 0;
        effect.data.value = 0;
    }

    // empty channel
    if (!note && !instrument && !has_command)
    {
        // o1111111
        out->data[0] = CHANNEL_EMPTY;
        out->data[1] = out->data[2] = 0;
        return 1;
    }

    // note + instrument
    if (note && instrument && !has_command)
    {
        // o1110nnn nnniiiii
        out->data[0] = CHANNEL_NOTE_INSTRUMENT | ((note >> 3) & 0x7);
        out->data[1] = ((note << 5) & 0xe0) | (instrument & 0x1f);
        out->data[2] = 0;
        return 2;
    }

    // command only
    if (!note && !instrument && has_command)
    {
        // o110cccc bbbbbbbb
        out->data[0] = CHANNEL_COMMAND | (effect.cmd & 0x0f);
        out->data[1] = effect.data.value;
        out->data[2] = 0;
        return 2;
    }

    // note + instrument + command
    // onnnnnni iiiicccc bbbbbbbb

    out->data[0] = CHANNEL_ALL | ((note << 1) & 0x7e) | ((instrument >> 4) & 0x01);
    out->data[1] = ((instrument << 4) & 0xf0) | (effect.cmd & 0x0f);
    out->data[2] = effect.data.value;
    return 3;
}

static bool to_protracker_channel(protracker_channel_t* out, const p61a_channel_t* in)
{
    memset(out, 0, sizeof(protracker_channel_t));

    if ((in->data[0] & CHANNEL_EMPTY) == CHANNEL_EMPTY)
    {
        // CHANNEL_EMPTY
    }
    else if ((in->data[0] & CHANNEL_NOTE_INSTRUMENT) == CHANNEL_NOTE_INSTRUMENT)
    {
        // CHANNEL_NOTE_INSTRUMENT - o1110nnn nnniiiii
        uint8_t note = ((in->data[0] & 0x07) << 3) | ((in->data[1] & 0xe0) >> 5);
        uint8_t sample = in->data[1] & 0x1f;

        protracker_set_period(out, period_from_index(note));
        protracker_set_sample(out, sample);
    }
    else if ((in->data[0] & CHANNEL_COMMAND) == CHANNEL_COMMAND)
    {
        // CHANNEL_COMMAND - o110cccc bbbbbbbb
        protracker_effect_t effect;
        effect.cmd = in->data[0] & 0x0f;
        effect.data.value = in->data[1];

        protracker_set_effect(out, &effect);
    }
    else
    {
        // CHANNEL_ALL - onnnnnni iiiicccc bbbbbbbb
        uint8_t note = (in->data[0] & 0x7e) >> 1;
        uint8_t sample = ((in->data[0] & 0x01) << 4) | ((in->data[1] & 0xf0) >> 4);

        protracker_effect_t effect;
        effect.cmd = in->data[1] & 0x0f;
        effect.data.value = in->data[2];

        protracker_set_period(out, period_from_index(note));
        protracker_set_sample(out, sample);
        protracker_set_effect(out, &effect);
    }

    {
        protracker_effect_t effect = protracker_get_effect(out);

        switch (effect.cmd)
        {
            case PT_CMD_8:
            {
                effect.cmd = PT_CMD_ARPEGGIO;
                protracker_set_effect(out, &effect);
            }
            break;
        }
    }

    return true;
}

static size_t build_track(p61a_channel_t* channel, const protracker_pattern_t* pattern, size_t channel_index, uint32_t* usecode)
{
    for (size_t i = 0; i < PT_PATTERN_ROWS; ++i)
    {
        const protracker_pattern_row_t* row = &(pattern->rows[i]);
        const protracker_channel_t* in = &(row->channels[channel_index]);

        to_p61a_channel(&(channel[i]), in, row, channel_index, usecode);
    }
    return PT_PATTERN_ROWS;
}

static void build_patterns(player61a_t* output, const protracker_t* input, const char* options, uint32_t* usecode)
{
    LOG_DEBUG("Converting patterns...\n");

    output->header.pattern_count = input->num_patterns;

    output->song.length = input->song.length;
    memcpy(output->song.positions, input->song.positions, sizeof(uint8_t) * PT_NUM_POSITIONS);

    output->pattern_offsets = malloc(input->num_patterns * sizeof(p61a_pattern_offset_t));
    memset(output->pattern_offsets, 0, input->num_patterns * sizeof(p61a_pattern_offset_t));

    for (size_t j = 0; j < PT_NUM_CHANNELS; ++j)
    {
        for (size_t i = 0; i < input->num_patterns; ++i)
        {
            p61a_channel_t track[PT_PATTERN_ROWS];

            size_t length = build_track(track, &(input->patterns[i]), j, usecode);

            size_t offset = buffer_count(&(output->patterns));

            output->pattern_offsets[i].channels[j] = offset;

            for (size_t k = 0; k < length; ++k)
            {
                const p61a_channel_t* channel = &(track[k]);
                buffer_add(&(output->patterns), channel, get_channel_length(channel));
            }

//            LOG_TRACE("CH %lu,%lu: %lu\n", i, j, buffer_count(&(output->patterns)) - offset);
        }
    }
}

static void player61a_create(player61a_t* module)
{
    memset(module, 0, sizeof(player61a_t));

    buffer_init(&(module->patterns), 1);
    buffer_init(&(module->samples), 1);
}

static void player61a_destroy(player61a_t* module)
{
    free(module->pattern_offsets);

    buffer_release(&(module->patterns));
    buffer_release(&(module->samples));
}

#if 0

static int8_t deltas[] = {
    0,1,2,4,8,16,32,64,128,-64,-32,-16,-8,-4,-2,-1
};

#endif

static void write_song(buffer_t* buffer, const player61a_t* module, const char* options)
{
    if (has_option(options, "sign", false))
    {
        LOG_TRACE(" - Adding signature.\n");
        buffer_add(buffer, signature, strlen(signature));
    }

    // header

    {
        p61a_header_t header;

        header.sample_offset = end_htobe16(get_sample_offset(module));
        header.pattern_count = module->header.pattern_count;
        header.sample_count = module->header.sample_count;

        buffer_add(buffer, &header, sizeof(header));
    }

    // sample headers

    for (size_t i = 0; i < module->header.sample_count; ++i)
    {
        const p61a_sample_t* in = &(module->sample_headers[i]);
        p61a_sample_t sample;

        bool empty = in->length == 0;

        if (!empty)
        {
            sample.length = end_htobe16(in->length);
            sample.finetone = in->finetone;
            sample.volume = in->volume;
            sample.repeat_offset = end_htobe16(in->repeat_offset);
        }
        else
        {
            sample.length = end_htobe16(1);
            sample.finetone = 0;
            sample.volume = 0;
            sample.repeat_offset = end_htobe16(0xffff);
        }

        buffer_add(buffer, &sample, sizeof(sample));
    }

    // pattern offsets

    for (size_t i = 0; i < module->header.pattern_count; ++i)
    {
        p61a_pattern_offset_t offset;
        for (size_t j = 0; j < PT_NUM_CHANNELS; ++j)
        {
            offset.channels[j] = end_htobe16(module->pattern_offsets[i].channels[j]);
        }

        buffer_add(buffer, &offset, sizeof(offset));
    }

    // tune positions

    {
        buffer_add(buffer, module->song.positions, module->song.length);

        uint8_t temp = 0xff;
        buffer_add(buffer, &temp, sizeof(temp));
    }

    // tracks

    size_t pattern_size = buffer_count(&(module->patterns));
    if (pattern_size)
    {
        buffer_add(buffer, buffer_get(&(module->patterns), 0), pattern_size);
    }

    // align samples

    if (buffer_count(buffer) & 1)
    {
        uint8_t c = 0;
        buffer_add(buffer, &c, 1);
    }
}

static void write_samples(buffer_t* buffer, const player61a_t* module)
{
    size_t size = buffer_count(&(module->samples));
    if (size > 0)
    {
        buffer_add(buffer, buffer_get(&(module->samples), 0), size);
    }
}

bool player61a_convert(buffer_t* buffer, const protracker_t* module, const char* options)
{
    LOG_INFO("Converting to The Player 6.1A...\n");

    player61a_t temp;
    player61a_create(&temp);
    uint32_t usecode = 0;

    build_samples(&temp, module, options, &usecode);
    build_patterns(&temp, module, options, &usecode);

    LOG_TRACE("usecode: %08x\n", usecode);

    if (has_option(options, "song", true))
    {
        LOG_DEBUG(" - Writing song data...\n");
        write_song(buffer, &temp, options);
    }

    if (has_option(options, "samples", true))
    {
        LOG_DEBUG(" - Writing sample data...\n");
        write_samples(buffer, &temp);
    }

    player61a_destroy(&temp);

    return true;
}

static const uint8_t* read_sample_headers(p61a_sample_t* sample_headers, size_t sample_count, const uint8_t* curr, const uint8_t* max)
{
    LOG_TRACE("Samples:\n");
    for (size_t i = 0; i < sample_count; ++i)
    {
        if ((max - curr) < sizeof(p61a_sample_t))
        {
            LOG_ERROR("Premature end of data before sample #%lu.\n", (i+1));
            return NULL;
        }

        p61a_sample_t sample;
        memcpy(&sample, curr, sizeof(sample));
        curr += sizeof(sample);

        sample.length = end_be16toh(sample.length);
        sample.finetone = sample.finetone;
        sample.volume = sample.volume;
        sample.repeat_offset = end_be16toh(sample.repeat_offset);

        LOG_TRACE(" #%02u - length: $%04X, finetone: %u, volume: %u, repeat offset: $%04X\n",
            (i+1),
            sample.length,
            sample.finetone,
            sample.volume,
            sample.repeat_offset
        );

        sample_headers[i] = sample;
    }

    return curr;
}

static const uint8_t* read_pattern_offsets(p61a_pattern_offset_t* pattern_offsets, size_t pattern_count, const uint8_t* curr, const uint8_t* max)
{
    LOG_TRACE("Pattern Offsets:\n");
    for (size_t i = 0; i < pattern_count; ++i)
    {
        if ((max - curr) < sizeof(p61a_pattern_offset_t))
        {
            LOG_ERROR("Premature end of data before pattern offset %lu.\n", i);
            return NULL;
        }

        p61a_pattern_offset_t offset;
        memcpy(&offset, curr, sizeof(offset));
        curr += sizeof(offset);

        LOG_TRACE(" #%lu:", i);
        for (size_t j = 0; j < PT_NUM_CHANNELS; ++j)
        {
            offset.channels[j] = end_be16toh(offset.channels[j]);
            LOG_TRACE(" %04X", offset.channels[j]);
        }
        LOG_TRACE("\n");

        pattern_offsets[i] = offset;
    }
    return curr;
}

static const uint8_t* read_song_positions(p61a_song_t* song, const uint8_t* curr, const uint8_t* max)
{
    LOG_TRACE("Song Positions:\n ");
    for (size_t i = 0; i < PT_NUM_POSITIONS; ++i)
    {
        if ((max - curr) < sizeof(uint8_t))
        {
            LOG_ERROR("Premature end of data before song position %lu.\n", i);
            return NULL;
        }

        uint8_t position = *curr++;
        if (position == 0xff)
        {
            song->length = i;
            break;
        }

        LOG_TRACE(" %u", position);

        song->positions[i] = position;
    }
    LOG_TRACE("\n");

    return curr;
}

static size_t decompress_track(p61a_pattern_t* pattern, size_t channel_index, size_t offset, size_t maxrows, const uint8_t* track, bool deref, const uint8_t* base)
{
    LOG_TRACE("decompress_track(%lu, %lu%s)\n", offset, maxrows, deref ? ", deref" : "");

    while(offset < PT_PATTERN_ROWS)
    {
        uint8_t c0 = *track++, c1 = 0, c2 = 0;
        p61a_channel_t out = { 0 };

        if ((c0 & CHANNEL_EMPTY) == CHANNEL_EMPTY)
        {
            LOG_TRACE(" %02lu %04x: %02x    ", offset, ((track-1)-base) & 0xffff, c0);
            if (!(c0 & CHANNEL_COMPRESSED))
            {
                ++offset;
            }
        }
        else if ((c0 & CHANNEL_NOTE_INSTRUMENT) == CHANNEL_NOTE_INSTRUMENT)
        {
            out.data[0] = c0;
            out.data[1] = c1 = *track++;

            LOG_TRACE(" %02lu %04x: %02x%02x  ", offset, ((track-2)-base) & 0xffff, c0, c1);

            pattern->rows[offset++].channels[channel_index] = out;
        }
        else if ((c0 & CHANNEL_COMMAND) == CHANNEL_COMMAND)
        {
            out.data[0] = c0;
            out.data[1] = c1 = *track++;

            LOG_TRACE(" %02lu %04x: %02x%02x  ", offset, ((track-2)-base) & 0xffff, c0, c1);

            pattern->rows[offset++].channels[channel_index] = out;
        }
        else
        {
            out.data[0] = c0;
            out.data[1] = c1 = *track++;
            out.data[2] = c2 = *track++;

            LOG_TRACE(" %02lu %04x: %02x%02x%02x", offset, ((track-3)-base) & 0xffff, c0, c1, c2);

            pattern->rows[offset++].channels[channel_index] = out;
        }

        protracker_channel_t ptc;

        to_protracker_channel(&ptc, &out);

        char buf[32];
        protracker_channel_to_text(&ptc, buf, sizeof(buf));

        LOG_TRACE(" %s", buf);

        if (c0 & CHANNEL_COMPRESSED)
        {
            uint8_t d0 = *track++;
            LOG_TRACE(" %02x", d0);

            if (d0 & COMPRESSION_JUMP)
            {
                uint8_t rows = (d0 & COMPRESSION_DATA_BITS) + 1;
                uint16_t dist = *track++;
                LOG_TRACE("%02x", dist);
                if (d0 & COMPRESSION_JUMP_LONG)
                {
                    uint8_t d2 = *track++;
                    LOG_TRACE("%02x", d2);
                    dist = (dist << 8)|d2;
                }

                LOG_TRACE(" (%s JUMP %u %04x)\n", (d0 & COMPRESSION_JUMP_LONG) ? "LONG" : "SHORT", rows, dist);

                offset = decompress_track(pattern, channel_index, offset, rows, track - dist, true, base);
            }
            else if (d0 & COMPRESSION_REPEAT_ROWS)
            {
                uint8_t rows = (d0 & COMPRESSION_DATA_BITS);

                LOG_TRACE(" (REPEAT %d)\n", rows);

                for (size_t i = 0; i < rows; ++i)
                {
                    pattern->rows[offset++].channels[channel_index] = out;
                }
            }
            else if ((d0 & COMPRESSION_CMD_BITS) == COMPRESSION_EMPTY_ROWS)
            {
                uint8_t rows = (d0 & COMPRESSION_DATA_BITS);

                LOG_TRACE(" (EMPTY %d)\n", rows);

                offset += rows;
            }
        }
        else
        {
            LOG_TRACE("\n");
        }

        if (maxrows > 0 && ((--maxrows) == 0))
        {
            break;
        }
    }

    LOG_TRACE(" - DONE (%lu)\n", offset);

    return offset;
}

static const uint8_t* read_patterns(p61a_pattern_t* patterns, p61a_pattern_offset_t* pattern_offsets, size_t pattern_count, const uint8_t* curr, const uint8_t* max)
{
    for (size_t i = 0; i < pattern_count; ++i)
    {
        p61a_pattern_t* pattern = &(patterns[i]);
        const p61a_pattern_offset_t* offsets = &(pattern_offsets[i]);

        for (size_t j = 0; j < PT_NUM_CHANNELS; ++j)
        {
            size_t current_row = 0;
            const uint8_t* track = curr + offsets->channels[j];

            LOG_TRACE("Pattern #%lu, track #%lu:\n", i,j);

            decompress_track(pattern, j, 0, 0, curr + offsets->channels[j], false, curr + offsets->channels[j]);
        }
    }

    return curr;
}

protracker_t* player61a_load(const buffer_t* buffer)
{
    LOG_DEBUG("Loading Player 6.1A module...\n");

    p61a_pattern_t* patterns = NULL;

    protracker_t module;
    protracker_create(&module);

    size_t signature_length = strlen(signature);

    do
    {
        size_t size = buffer_count(buffer);
        if (size < sizeof(p61a_header_t) + signature_length)
        {
            LOG_ERROR("Premature end of data before header.\n");
            break;
        }

        const uint8_t* raw = buffer_get(buffer, 0);

        // check for P61A (and skip it)
        if (!memcmp(signature, raw, signature_length))
        {
            raw += signature_length;
            size -= signature_length;
        }

        const uint8_t* curr = raw;
        const uint8_t* max = curr + size;

        // header

        p61a_header_t header;
        memcpy(&header, curr, sizeof(header));
        curr += sizeof(p61a_header_t);

        header.sample_offset = end_be16toh(header.sample_offset);
        LOG_TRACE("Header:\n Sample Offset: %u\n Patterns:%u\n Sample count:%u\n", header.sample_offset, header.pattern_count, header.sample_count);

        if (!header.pattern_count)
        {
            LOG_ERROR("Invalid pattern count in header. (%u)\n", header.pattern_count);
            break;
        }

        if (header.sample_count > PT_NUM_SAMPLES)
        {
            LOG_ERROR("Invalid sample count in header. (%u > %u)\n", header.sample_count, PT_NUM_SAMPLES);
            break;
        }

        // sample headers

        p61a_sample_t sample_headers[header.sample_count];
        if (!(curr = read_sample_headers(sample_headers, header.sample_count, curr, max)))
        {
            break;
        }

        // pattern offsets

        p61a_pattern_offset_t pattern_offsets[header.pattern_count];
        if (!(curr = read_pattern_offsets(pattern_offsets, header.pattern_count, curr, max)))
        {
            break;
        }

        // song positions

        p61a_song_t song = { 0 };
        if (!(curr = read_song_positions(&song, curr, max)))
        {
            break;
        }

        // patterns

        p61a_pattern_t* patterns = malloc(sizeof(p61a_pattern_t) * header.pattern_count);
        memset(patterns, 0, sizeof(p61a_pattern_t) * header.pattern_count);
        if (!(curr = read_patterns(patterns, pattern_offsets, header.pattern_count, curr, max)))
        {
            break;
        }

        // PT: header

        for (size_t i = 0; i < header.sample_count; ++i)
        {
            const p61a_sample_t* in = &(sample_headers[i]);
            protracker_sample_t* out = &(module.sample_headers[i]);

            out->length = in->length;
            out->finetone = in->finetone & 0x0f;
            out->volume = in->volume;

            if (in->repeat_offset == 0xffff)
            {
                out->repeat_offset = 0;
                out->repeat_length = 1;
            }
            else
            {
                out->repeat_offset = in->repeat_offset;
                out->repeat_length = in->length - in->repeat_offset;
            }
        }

        // PT: Song

        module.song.length = song.length;
        module.song.restart_position = 127;
        memcpy(module.song.positions, song.positions, song.length);

        // PT: Patterns

        module.patterns = malloc(sizeof(protracker_pattern_t) * header.pattern_count);
        module.num_patterns = header.pattern_count;

        for (size_t i = 0; i < header.pattern_count; ++i)
        {
            const p61a_pattern_t* in_pattern = &(patterns[i]);
            protracker_pattern_t* out_pattern = &(module.patterns[i]);

            LOG_TRACE("Pattern #%lu:\n", i);

            for (size_t j = 0; j < PT_PATTERN_ROWS; ++j)
            {
                const p61a_pattern_row_t* in_row = &(in_pattern->rows[j]);
                protracker_pattern_row_t* out_row = &(out_pattern->rows[j]);

                LOG_TRACE("%02lu:", j);

                for (size_t k = 0; k < PT_NUM_CHANNELS; ++k)
                {
                    const p61a_channel_t* in = &(in_row->channels[k]);
                    protracker_channel_t* out = &(out_row->channels[k]);

                    to_protracker_channel(out, in);

                    char buf[32];
                    protracker_channel_to_text(out, buf, sizeof(buf));

                    LOG_TRACE(" %s", buf);
                }

                LOG_TRACE("\n");
            }
        }

        // PT: Sample Data

        const uint8_t* samples = raw + header.sample_offset;
        for (size_t i = 0; i < header.sample_count; ++i)
        {
            const p61a_sample_t* sample = &(sample_headers[i]);

            if (!sample->length)
            {
                continue;
            }

            size_t bytes = sample->length * 2;
            uint8_t* out = module.sample_data[i] = malloc(bytes);
            memcpy(out, samples, bytes);

            samples += bytes;
        }

        free(patterns);
        protracker_t* out = malloc(sizeof(protracker_t));
        *out = module;

        return out;
    }
    while (false);

    free(patterns);
    protracker_destroy(&module);

    return NULL;
}
