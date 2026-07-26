///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// PreTracker Playback Plugin
//
// Implements RVPlaybackPlugin for PreTracker modules using the bundled C replayer.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef nullptr
#define nullptr ((void*)0)
#endif

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include "pretracker.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define PRETRACKER_SAMPLE_RATE 48000
#define PRETRACKER_CHANNELS 4
#define PRETRACKER_COLUMN_COUNT 4
#define PRETRACKER_SCOPE_BUFFER_SIZE 16384
#define PRETRACKER_SCOPE_BUFFER_MASK (PRETRACKER_SCOPE_BUFFER_SIZE - 1)
#define PRETRACKER_DECODE_CHUNK_SIZE 1024

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_LOG_API();

typedef struct PretrackerPluginData {
    struct PreSong* song;
    bool scope_enabled;
    float scope_buffers[PRETRACKER_CHANNELS][PRETRACKER_SCOPE_BUFFER_SIZE];
    uint64_t scope_frames_written;
} PretrackerPluginData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* pretracker_supported_extensions(void) {
    return "prt";
}

static bool pretracker_has_complete_header(const uint8_t* data, uint64_t data_size) {
    if (data == nullptr || data_size < 4 || memcmp(data, "PRT", 3) != 0) {
        return false;
    }

    // Older formats read through byte 0x41; v1.5 additionally reads byte 0x5a.
    uint64_t minimum_size = data[3] == 0x1e ? 0x5b : 0x42;
    return data_size >= minimum_size;
}

static void* pretracker_create(const RVService* service_api) {
    (void)service_api;
    return calloc(1, sizeof(PretrackerPluginData));
}

static void pretracker_close(void* user_data) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data != nullptr && data->song != nullptr) {
        pre_song_destroy(data->song);
        data->song = nullptr;
    }
    if (data != nullptr) {
        data->scope_frames_written = 0;
    }
}

static int32_t pretracker_destroy(void* user_data) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data != nullptr) {
        pretracker_close(data);
        free(data);
    }
    return 0;
}

static RVProbeResult pretracker_probe_can_play(uint8_t* data, uint64_t data_size, const char* url,
                                               uint64_t total_size) {
    (void)url;
    (void)total_size;

    if (data == nullptr || data_size < 4 || memcmp(data, "PRT", 3) != 0) {
        return RVProbeResult_Unsupported;
    }

    // Versions through 1.4 use values up to 0x1b; 1.5 uses 0x1e.
    return data[3] <= 0x1b || data[3] == 0x1e ? RVProbeResult_Supported : RVProbeResult_Unsupported;
}

static int32_t pretracker_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)service_api;
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr) {
        return -1;
    }

    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        rv_error("Failed to load %s to memory", url);
        return -1;
    }

    pretracker_close(data);

    if (read_res.data_size > UINT32_MAX || !pretracker_has_complete_header(read_res.data, read_res.data_size)) {
        rv_error("Invalid PreTracker module header: %s", url);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    data->song = pre_song_create(read_res.data, (uint32_t)read_res.data_size);
    rv_io_free_url_to_memory(read_res.data);
    if (data->song == nullptr) {
        rv_error("Failed to parse %s", url);
        return -1;
    }

    pre_song_set_subsong(data->song, (int)subsong);
    pre_song_set_sample_rate(data->song, PRETRACKER_SAMPLE_RATE);
    pre_song_start(data->song);
    return 0;
}

static void pretracker_capture_scopes(PretrackerPluginData* data, float scopes[PRETRACKER_CHANNELS][PRETRACKER_DECODE_CHUNK_SIZE],
                                      uint32_t frame_count) {
    for (uint32_t frame = 0; frame < frame_count; frame++) {
        uint32_t write_pos = (uint32_t)(data->scope_frames_written + frame) & PRETRACKER_SCOPE_BUFFER_MASK;
        for (int channel = 0; channel < PRETRACKER_CHANNELS; channel++) {
            data->scope_buffers[channel][write_pos] = scopes[channel][frame];
        }
    }
    data->scope_frames_written += frame_count;
}

static RVReadInfo pretracker_read_data(void* user_data, RVReadData dest) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    RVAudioFormat format = { RVAudioStreamFormat_F32, 2, PRETRACKER_SAMPLE_RATE };

    if (data == nullptr || data->song == nullptr || dest.channels_output == nullptr) {
        return (RVReadInfo){ format, 0, RVReadStatus_Error };
    }

    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(float) * 2);
    float* output = (float*)dest.channels_output;
    uint32_t frames_written = 0;

    if (!data->scope_enabled) {
        frames_written = (uint32_t)pre_song_decode(data->song, output, (int)max_frames);
    } else {
        float scope_storage[PRETRACKER_CHANNELS][PRETRACKER_DECODE_CHUNK_SIZE];
        float* scopes[PRETRACKER_CHANNELS];
        for (int channel = 0; channel < PRETRACKER_CHANNELS; channel++) {
            scopes[channel] = scope_storage[channel];
        }

        while (frames_written < max_frames) {
            uint32_t requested = max_frames - frames_written;
            if (requested > PRETRACKER_DECODE_CHUNK_SIZE) {
                requested = PRETRACKER_DECODE_CHUNK_SIZE;
            }

            int decoded = pre_song_decode_with_scopes(data->song, output + frames_written * 2, (int)requested, scopes,
                                                      PRETRACKER_CHANNELS);
            if (decoded <= 0) {
                break;
            }

            pretracker_capture_scopes(data, scope_storage, (uint32_t)decoded);
            frames_written += (uint32_t)decoded;
            if ((uint32_t)decoded < requested) {
                break;
            }
        }
    }

    RVReadStatus status
        = pre_song_is_finished(data->song) && frames_written == 0 ? RVReadStatus_Finished : RVReadStatus_Ok;
    return (RVReadInfo){ format, frames_written, status };
}

static int64_t pretracker_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;
    return 0;
}

static int32_t pretracker_metadata(const char* url, const RVService* service_api) {
    (void)service_api;
    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr || read_res.data_size > UINT32_MAX
        || !pretracker_has_complete_header(read_res.data, read_res.data_size)) {
        if (read_res.data != nullptr) {
            rv_io_free_url_to_memory(read_res.data);
        }
        return -1;
    }

    struct PreSong* song = pre_song_create(read_res.data, (uint32_t)read_res.data_size);
    rv_io_free_url_to_memory(read_res.data);
    if (song == nullptr) {
        return -1;
    }

    const PreSongMetadata* metadata = pre_song_get_metadata(song);
    RVMetadataId id = rv_metadata_create_url(url);
    rv_metadata_set_tag(id, RV_METADATA_SONGTYPE_TAG, "PreTracker");
    rv_metadata_set_tag(id, RV_METADATA_AUTHORINGTOOL_TAG, "PreTracker");
    rv_metadata_set_tag_f64(id, RV_METADATA_LENGTH_TAG, 0.0);

    if (metadata->song_name[0] != '\0') {
        rv_metadata_set_tag(id, RV_METADATA_TITLE_TAG, metadata->song_name);
    }
    if (metadata->author[0] != '\0') {
        rv_metadata_set_tag(id, RV_METADATA_ARTIST_TAG, metadata->author);
    }

    for (uint32_t i = 0; i < metadata->num_instruments; i++) {
        if (metadata->instrument_names[i][0] != '\0') {
            rv_metadata_add_instrument(id, metadata->instrument_names[i]);
        }
    }
    for (uint32_t i = 0; i < metadata->num_waves; i++) {
        if (metadata->wave_names[i][0] != '\0') {
            rv_metadata_add_sample(id, metadata->wave_names[i]);
        }
    }
    if (metadata->num_subsongs > 1) {
        for (uint32_t i = 0; i < metadata->num_subsongs; i++) {
            rv_metadata_add_subsong(id, i, "", 0.0f);
        }
    }

    pre_song_destroy(song);
    return 0;
}

static void pretracker_event(void* user_data, uint8_t* event_data, uint64_t len) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr || data->song == nullptr || event_data == nullptr || len < 8) {
        return;
    }

    const PrePlaybackState* state = pre_song_get_playback_state(data->song);
    event_data[7] = (uint8_t)(state->position & 0xff);
    event_data[6] = state->row;
    event_data[5] = 0;
    event_data[4] = 0;
    for (int channel = 0; channel < PRETRACKER_CHANNELS; channel++) {
        unsigned volume = (unsigned)state->channels[channel].volume * 4;
        event_data[3 - channel] = (uint8_t)(volume > 255 ? 255 : volume);
    }
}

static void pretracker_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Visualization API

static void pretracker_render_note(uint8_t note, char* out, size_t cap) {
    static const char* names[12]
        = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    if (note == 0) {
        out[0] = '\0';
    } else if (note == 0x3d) {
        snprintf(out, cap, "===");
    } else {
        int value = note - 1;
        snprintf(out, cap, "%s%d", names[value % 12], value / 12 + 1);
    }
}

static bool pretracker_get_structure(void* user_data, RVVizInfo* out) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr || data->song == nullptr || out == nullptr) {
        return false;
    }

    out->caps = RVVizCaps_PatternCells | RVVizCaps_Scope | RVVizCaps_Vu | RVVizCaps_WholeSongKnown
                | RVVizCaps_SeekablePreview | RVVizCaps_FutureKnown;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = PRETRACKER_CHANNELS;
    out->scope_channel_count = PRETRACKER_CHANNELS;
    out->column_count = PRETRACKER_COLUMN_COUNT;
    return true;
}

static uint32_t pretracker_get_columns(void* user_data, RVColumnDesc* out, uint32_t cap) {
    (void)user_data;
    static const struct {
        const char* label;
        uint8_t width;
        RVColumnKind kind;
    } columns[PRETRACKER_COLUMN_COUNT] = {
        { "Note", 3, RVColumnKind_Note },
        { "Inst", 2, RVColumnKind_Instrument },
        { "FX", 1, RVColumnKind_Effect },
        { "Prm", 2, RVColumnKind_Param },
    };

    if (out == nullptr) {
        return 0;
    }
    uint32_t count = cap < PRETRACKER_COLUMN_COUNT ? cap : PRETRACKER_COLUMN_COUNT;
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].label, 0, sizeof(out[i].label));
        strncpy((char*)out[i].label, columns[i].label, sizeof(out[i].label) - 1);
        out[i].char_width = columns[i].width;
        out[i].kind = columns[i].kind;
    }
    return count;
}

static uint32_t pretracker_fill_channels(RVChannelDesc* out, uint32_t cap) {
    if (out == nullptr) {
        return 0;
    }
    uint32_t count = cap < PRETRACKER_CHANNELS ? cap : PRETRACKER_CHANNELS;
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "Voice %u", i + 1);
        out[i].scope_width = 1;
    }
    return count;
}

static uint32_t pretracker_get_pattern_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    return data != nullptr && data->song != nullptr ? pretracker_fill_channels(out, cap) : 0;
}

static uint32_t pretracker_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    return data != nullptr && data->song != nullptr ? pretracker_fill_channels(out, cap) : 0;
}

static bool pretracker_get_position(void* user_data, RVTrackerPosition* out) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr || data->song == nullptr || out == nullptr) {
        return false;
    }

    const PreSongMetadata* metadata = pre_song_get_metadata(data->song);
    const PrePlaybackState* state = pre_song_get_playback_state(data->song);
    out->order = state->position;
    out->pattern = state->position;
    out->row = state->row;
    out->window_lo = 0;
    out->window_hi = metadata->num_steps;
    return true;
}

static uint32_t pretracker_get_channel_rows(void* user_data, uint32_t* out, uint32_t cap) {
    (void)user_data;
    (void)out;
    (void)cap;
    return 0;
}

static uint32_t pretracker_get_cells(void* user_data, int32_t channel, uint32_t row_lo, uint32_t row_hi,
                                     RVPatternCell* out, uint32_t cap) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr || data->song == nullptr || out == nullptr || channel < -1) {
        return 0;
    }

    const PreSongMetadata* metadata = pre_song_get_metadata(data->song);
    const PrePlaybackState* state = pre_song_get_playback_state(data->song);
    if (row_hi > metadata->num_steps) {
        row_hi = metadata->num_steps;
    }
    if (row_lo >= row_hi) {
        return 0;
    }

    int channel_start = channel < 0 ? 0 : channel;
    int channel_end = channel < 0 ? PRETRACKER_CHANNELS : channel + 1;
    if (channel_start >= PRETRACKER_CHANNELS) {
        return 0;
    }

    uint32_t written = 0;
    for (uint32_t row = row_lo; row < row_hi; row++) {
        for (int current_channel = channel_start; current_channel < channel_end; current_channel++) {
            uint8_t track = 0;
            int8_t pitch_shift = 0;
            PreTrackCell source = { 0 };
            (void)pitch_shift;
            bool has_cell
                = pre_song_get_position_entry(data->song, state->position, (uint8_t)current_channel, &track, &pitch_shift)
                  && pre_song_get_track_cell(data->song, track, (uint8_t)row, &source);

            for (int column = 0; column < PRETRACKER_COLUMN_COUNT; column++) {
                if (written >= cap) {
                    return written;
                }

                RVPatternCell* cell = &out[written++];
                memset(cell, 0, sizeof(*cell));
                if (!has_cell) {
                    continue;
                }

                char* text = (char*)cell->text;
                switch (column) {
                    case 0:
                        cell->raw = source.note;
                        pretracker_render_note(source.note, text, sizeof(cell->text));
                        break;
                    case 1:
                        cell->raw = source.instrument;
                        if (source.instrument != 0) {
                            snprintf(text, sizeof(cell->text), "%02X", source.instrument);
                        }
                        break;
                    case 2:
                        cell->raw = source.has_arpeggio ? 0x10 : source.effect_cmd;
                        if (source.has_arpeggio) {
                            snprintf(text, sizeof(cell->text), "A");
                        } else if (source.effect_cmd != 0 || source.effect_data != 0) {
                            snprintf(text, sizeof(cell->text), "%X", source.effect_cmd);
                        }
                        break;
                    case 3:
                        cell->raw = source.effect_data;
                        if (source.has_arpeggio || source.effect_cmd != 0 || source.effect_data != 0) {
                            snprintf(text, sizeof(cell->text), "%02X", source.effect_data);
                        }
                        break;
                }
            }
        }
    }
    return written;
}

static void pretracker_set_scope_enabled(void* user_data, bool on) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr) {
        return;
    }
    if (on && !data->scope_enabled) {
        memset(data->scope_buffers, 0, sizeof(data->scope_buffers));
        data->scope_frames_written = 0;
    }
    data->scope_enabled = on;
}

static uint32_t pretracker_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr || !data->scope_enabled || channel < 0 || channel >= PRETRACKER_CHANNELS || out == nullptr) {
        return 0;
    }

    uint64_t available = data->scope_frames_written;
    if (available > PRETRACKER_SCOPE_BUFFER_SIZE) {
        available = PRETRACKER_SCOPE_BUFFER_SIZE;
    }
    uint32_t count = cap < available ? cap : (uint32_t)available;
    uint64_t start = data->scope_frames_written - count;
    for (uint32_t i = 0; i < count; i++) {
        out[i] = data->scope_buffers[channel][(uint32_t)(start + i) & PRETRACKER_SCOPE_BUFFER_MASK];
    }
    return count;
}

static uint32_t pretracker_get_vu(void* user_data, float* out, uint32_t cap) {
    PretrackerPluginData* data = (PretrackerPluginData*)user_data;
    if (data == nullptr || data->song == nullptr || out == nullptr) {
        return 0;
    }

    const PrePlaybackState* state = pre_song_get_playback_state(data->song);
    uint32_t count = cap < PRETRACKER_CHANNELS ? cap : PRETRACKER_CHANNELS;
    for (uint32_t i = 0; i < count; i++) {
        float volume = (float)state->channels[i].volume / 64.0f;
        out[i] = volume > 1.0f ? 1.0f : volume;
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_pretracker_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "pretracker",
    "0.0.1",
    "PreTracker C Player",
    pretracker_probe_can_play,
    pretracker_supported_extensions,
    pretracker_create,
    pretracker_destroy,
    pretracker_event,
    pretracker_open,
    pretracker_close,
    pretracker_read_data,
    pretracker_seek,
    pretracker_metadata,
    pretracker_static_init,
    nullptr, // settings_updated
    nullptr, // static_destroy
    pretracker_get_structure,
    pretracker_get_columns,
    pretracker_get_pattern_channels,
    pretracker_get_scope_channels,
    pretracker_get_position,
    pretracker_get_channel_rows,
    pretracker_get_cells,
    pretracker_set_scope_enabled,
    pretracker_get_scope_samples,
    pretracker_get_vu,
};

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_pretracker_plugin;
}
