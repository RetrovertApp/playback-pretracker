#include "pretracker_internal.h"

#include <assert.h>
#include <string.h>

#define OLD_SIZE 0xD2
#define V15_SIZE 0x112
#define ONE_WAVE_SIZE 0xE2
#define TWO_WAVE_SIZE 0x10C
#define ONE_WAVE_INFO_OFFSET 0xB8

static void write_be32(u8* p, u32 value) {
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static void make_old_song(u8* data) {
    memset(data, 0, OLD_SIZE);
    write_be32(data, 0x5052541B);
    write_be32(data + 0x04, 0x60);
    write_be32(data + 0x08, 0x68);
    write_be32(data + 0x0C, 0x70);
    write_be32(data + 0x10, 0x90);
    data[0x3D] = 1;
    data[0x3E] = 1;
    data[0x3F] = 1;
    data[0x41] = 1;
    data[0x60] = 1;
}

static void make_v15_song(u8* data) {
    memset(data, 0, V15_SIZE);
    write_be32(data, 0x5052541E);
    write_be32(data + 0x04, 0x60);
    write_be32(data + 0x08, 0x80);
    write_be32(data + 0x0C, 0x90);
    write_be32(data + 0x10, 0xD0);
    data[0x41] = 1;
    data[0x5A] = 2;

    data[0x61] = 1;
    data[0x62] = 1;
    data[0x63] = 1;

    data[0x69] = 1;
    data[0x6A] = 1;
    data[0x6B] = 1;
    write_be32(data + 0x6C, 3);

    data[0x70] = 1;
    data[0x78] = 1;
}

static void make_one_wave_song(u8* data) {
    memset(data, 0, ONE_WAVE_SIZE);
    make_old_song(data);
    write_be32(data + 0x10, 0xA0);
    data[0x40] = 1;
    data[0x41] = 1;
    data[0x97] = 1;
}

static void make_two_wave_song(u8* data) {
    memset(data, 0, TWO_WAVE_SIZE);
    make_old_song(data);
    write_be32(data + 0x10, 0xA0);
    data[0x40] = 1;
    data[0x41] = 2;
    data[0x43] = 1;
    data[0x97] = 1;
}

static void make_old_speed_song(u8* data, u8 speed) {
    make_old_song(data);
    data[0x3F] = 2;
    data[0x6B + 1] = PAT_CMD_SET_SPEED;
    data[0x6B + 2] = speed;
}

static void test_old_layout_and_pattern_table(void) {
    u8 data[OLD_SIZE];
    SongState song;
    make_old_song(data);

    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);
    assert(song.num_patterns == 1);
    pretracker_rebuild_pattern_table(&song);
    assert(song.pattern_table[0] == data + 0x68);
    assert(song.pattern_table[1] == NULL);
}

static void test_rejects_zero_wave_song(void) {
    u8 data[ONE_WAVE_SIZE];
    SongState song;
    make_one_wave_song(data);
    data[0x41] = 0;

    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);
    assert(pre_song_create(data, sizeof(data)) == NULL);
}

static void test_public_track_bounds(void) {
    u8 data[OLD_SIZE];
    make_old_song(data);
    struct PreSong* song = pre_song_create(data, sizeof(data));
    PreTrackCell cell;
    assert(song != NULL);
    assert(!pre_song_get_track_cell(song, 0, 0, &cell));
    assert(pre_song_get_track_cell(song, 1, 0, &cell));
    assert(!pre_song_get_track_cell(song, 2, 0, &cell));
    assert(!pre_song_get_track_cell(song, 1, 1, &cell));
    pre_song_destroy(song);
}

static void test_player_skips_missing_pattern_pointer(void) {
    SongState song;
    PlayerState player;
    WaveInfo wave;
    u8 positions[NUM_CHANNELS * 2] = {2, 0};
    u8 patterns[3] = {0};
    f32 samples[2 + HQ_MAX_PERIOD] = {0};
    memset(&song, 0, sizeof(song));
    memset(&wave, 0, sizeof(wave));

    song.num_waves = 1;
    song.num_patterns = 1;
    song.num_steps = 1;
    song.pat_pos_len = 1;
    song.pos_data_adr = positions;
    song.patterns_ptr = patterns;
    song.waveinfo_ptr = &wave;
    song.waveinfo_table[0] = &wave;
    song.wavelength_table[0] = HQ_MAX_PERIOD;
    song.wavetotal_table[0] = HQ_MAX_PERIOD;

    pretracker_player_init(&player, samples, &song);
    pretracker_player_start(&player, &song);
    assert(song.pattern_table[1] == NULL);
    pretracker_player_tick(&player);
    assert(player.channeldata[0].new_inst_num == 0);
}

static void test_extreme_tonal_note_clamps_oscillator_index(void) {
    SongState song;
    PlayerState player;
    WaveInfo wave;
    f32 samples[2 + HQ_MAX_PERIOD] = {0};
    memset(&song, 0, sizeof(song));
    memset(&wave, 0, sizeof(wave));

    wave.osc_basenote = 127;
    wave.osc_gain = 128;
    wave.vol_sustain = 0xFF;
    song.num_waves = 1;
    song.waveinfo_table[0] = &wave;
    song.wavelength_table[0] = HQ_MAX_PERIOD;
    song.wavetotal_table[0] = HQ_MAX_PERIOD;

    pretracker_player_init(&player, samples, &song);
    pretracker_wavegen_generate(&player);

    OscNoteBuffers* oscillator = &player.osc_buffers[7];
    assert(samples[2] == pretracker_clamp_sample(oscillator->saw_waves[0]));
    assert(samples[3] == oscillator->saw_waves[oscillator->wave_length - 1]);
    for (int i = 2; i < 2 + HQ_MAX_PERIOD; ++i) {
        assert(samples[i] >= -1.0f);
        assert(samples[i] <= 127.0f / 128.0f);
    }
}

static void test_fixed_point_period_interpolation(void) {
    SongState song;
    PlayerState player;
    f32 samples[2] = {0};
    memset(&song, 0, sizeof(song));

    pretracker_player_init(&player, samples, &song);

    const u16 first_semitone[] = {
        0x350, 0x34D, 0x34A, 0x347, 0x344, 0x341, 0x33E, 0x33B,
        0x338, 0x335, 0x332, 0x32F, 0x32C, 0x329, 0x326, 0x323,
    };
    assert(memcmp(player.period_table, first_semitone, sizeof(first_semitone)) == 0);
}

static void test_negative_position_transposition(void) {
    SongState song;
    PlayerState player;
    WaveInfo wave;
    u8 positions[NUM_CHANNELS * 2] = {1, 0xFE};
    u8 pattern[3] = {1, 0, 0};
    f32 samples[2 + HQ_MAX_PERIOD] = {0};
    memset(&song, 0, sizeof(song));
    memset(&wave, 0, sizeof(wave));

    song.num_waves = 1;
    song.num_patterns = 1;
    song.num_steps = 1;
    song.pat_pos_len = 1;
    song.pos_data_adr = positions;
    song.patterns_ptr = pattern;
    song.waveinfo_ptr = &wave;
    song.waveinfo_table[0] = &wave;
    song.wavelength_table[0] = HQ_MAX_PERIOD;
    song.wavetotal_table[0] = HQ_MAX_PERIOD;

    pretracker_player_init(&player, samples, &song);
    pretracker_player_start(&player, &song);
    pretracker_player_tick(&player);

    assert(player.channeldata[0].inst_curr_port_pitch == -16);
}

static void test_player_restart_resets_tempo_and_playback_state(void) {
    SongState song;
    PlayerState player;
    f32 samples[2] = {0};
    memset(&song, 0, sizeof(song));

    pretracker_player_init(&player, samples, &song);
    song.curr_pat_pos = 3;
    player.pat_curr_row = 4;
    player.next_pat_row = 2;
    player.next_pat_pos = 1;
    player.pat_speed_even = 4;
    player.pat_speed_odd = 9;
    player.pat_line_ticks = 2;
    player.pat_stopped = 0;
    player.songend_detected = 1;
    player.trigger_mask = 0xFFFF;
    player.channeldata[0].note_delay = 3;
    player.channeldata[0].note_off_delay = 2;
    player.channeldata[0].pat_2nd_inst_delay = 1;
    player.channeldata[0].track_delay_steps = 4;

    pretracker_player_start(&player, &song);

    assert(song.curr_pat_pos == 0);
    assert(player.pat_curr_row == 0);
    assert(player.next_pat_row == 0xFF);
    assert(player.next_pat_pos == 0xFF);
    assert(player.pat_speed_even == DEFAULT_PATTERN_SPEED);
    assert(player.pat_speed_odd == DEFAULT_PATTERN_SPEED);
    assert(player.pat_line_ticks == DEFAULT_PATTERN_SPEED);
    assert(player.pat_stopped == 1);
    assert(player.songend_detected == 0);
    assert(player.trigger_mask == 0);
    assert(player.channeldata[0].note_delay == 0);
    assert(player.channeldata[0].note_off_delay == 0);
    assert(player.channeldata[0].pat_2nd_inst_delay == 0);
    assert(player.channeldata[0].track_delay_steps == 0);
}

static void test_public_restart_discards_pattern_tempo_effects(void) {
    const u8 effects[] = {4, 0x38};
    const u8 changed_speeds[] = {4, 8};

    for (size_t effect = 0; effect < sizeof(effects) / sizeof(effects[0]); ++effect) {
        u8 data[OLD_SIZE];
        f32 output[7 * 2];
        make_old_speed_song(data, effects[effect]);
        struct PreSong* song = pre_song_create(data, sizeof(data));
        assert(song != NULL);
        pre_song_set_sample_rate(song, PRE_MIN_SAMPLE_RATE);

        pre_song_start(song);
        assert(pre_song_decode(song, output, 7) == 7);
        const PrePlaybackState* changed = pre_song_get_playback_state(song);
        assert(changed->row == 1);
        assert(changed->speed == changed_speeds[effect]);

        pre_song_start(song);
        assert(pre_song_decode(song, output, 1) == 1);
        const PrePlaybackState* restarted = pre_song_get_playback_state(song);
        assert(restarted->position == 0);
        assert(restarted->row == 0);
        assert(restarted->speed == DEFAULT_PATTERN_SPEED);
        assert(restarted->ticks_remaining == DEFAULT_PATTERN_SPEED - 1);
        assert(!pre_song_is_finished(song));
        pre_song_destroy(song);
    }
}

static void test_subsong_restart_preserves_layout_and_resets_tempo(void) {
    u8 data[V15_SIZE];
    f32 output[7 * 2];
    u8 track_num;
    i8 pitch_shift;
    make_v15_song(data);
    data[0x6A] = 2;
    data[0x79] = 7;
    data[0x83 + 3 + 1] = PAT_CMD_SET_SPEED;
    data[0x83 + 3 + 2] = 4;

    struct PreSong* song = pre_song_create(data, sizeof(data));
    assert(song != NULL);
    pre_song_set_subsong(song, 1);
    pre_song_set_sample_rate(song, PRE_MIN_SAMPLE_RATE);
    pre_song_start(song);
    assert(pre_song_get_metadata(song)->num_steps == 2);
    assert(pre_song_get_position_entry(song, 0, 0, &track_num, &pitch_shift));
    assert(track_num == 1);
    assert(pitch_shift == 7);

    assert(pre_song_decode(song, output, 7) == 7);
    assert(pre_song_get_playback_state(song)->speed == 4);
    pre_song_start(song);
    assert(pre_song_decode(song, output, 1) == 1);
    assert(pre_song_get_playback_state(song)->speed == DEFAULT_PATTERN_SPEED);
    assert(pre_song_get_playback_state(song)->ticks_remaining == DEFAULT_PATTERN_SPEED - 1);
    assert(pre_song_get_position_entry(song, 0, 0, &track_num, &pitch_shift));
    assert(track_num == 1);
    assert(pitch_shift == 7);
    assert(pre_song_get_metadata(song)->num_steps == 2);
    pre_song_destroy(song);
}

static void test_negative_filter_and_pitch_ramp(void) {
    SongState song;
    PlayerState player;
    WaveInfo wave;
    f32 samples[2 + HQ_MAX_PERIOD] = {0};
    memset(&song, 0, sizeof(song));
    memset(&wave, 0, sizeof(wave));

    wave.osc_gain = 128;
    wave.vol_sustain = 0xFF;
    wave.pitch_ramp = 0xFF;
    wave.flags = WI_FLAG_PITCH_LINEAR;
    wave.flt_type = FILTER_LOWPASS;
    wave.flt_start = 128;
    wave.flt_max = 255;
    wave.flt_speed = 0xFF;
    song.num_waves = 1;
    song.waveinfo_table[0] = &wave;
    song.wavelength_table[0] = HQ_MAX_PERIOD;
    song.wavetotal_table[0] = HQ_MAX_PERIOD;

    pretracker_player_init(&player, samples, &song);
    pretracker_wavegen_generate(&player);

    const f32 expected[] = {
        0.0546875f, 0.1875f, 0.375f, 0.5703125f, 0.7421875f, 0.8671875f,
        0.9375f, 0.9609375f, 0.9609375f, 0.9453125f, 0.921875f, 0.890625f,
    };
    assert(memcmp(samples + 2, expected, sizeof(expected)) == 0);
}

static void test_sample_clamp_matches_signed_byte_rails(void) {
    assert(pretracker_clamp_sample(-2.0f) == -1.0f);
    assert(pretracker_clamp_sample(-1.0f) == -1.0f);
    assert(pretracker_clamp_sample(0.5f) == 0.5f);
    assert(pretracker_clamp_sample(127.0f / 128.0f) == 127.0f / 128.0f);
    assert(pretracker_clamp_sample(1.0f) == 127.0f / 128.0f);
}

static void test_rejects_truncated_old_layouts(void) {
    u8 data[OLD_SIZE];
    SongState song;
    make_old_song(data);
    data[0x3E] = 32;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);

    make_old_song(data);
    data[0x3D] = 255;
    data[0x3F] = 255;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);
}

static void test_empty_layouts_remain_bounded(void) {
    u8 data[OLD_SIZE];
    SongState song;
    make_old_song(data);
    data[0x3E] = 0;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);

    make_old_song(data);
    data[0x3F] = 0;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);

    make_old_song(data);
    data[0x3C] = 1;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);

    u8 playable[OLD_SIZE];
    memset(playable, 0, sizeof(playable));
    make_old_song(playable);
    playable[0x3E] = 0;
    struct PreSong* public_song = pre_song_create(playable, sizeof(playable));
    f32 output[2000 * 2];
    assert(public_song != NULL);
    pre_song_start(public_song);
    assert(pre_song_decode(public_song, output, 2000) == 2000);
    assert(pre_song_get_playback_state(public_song)->channels[0].track_num == 0);
    pre_song_destroy(public_song);
}

static void test_minimum_sample_rate_preserves_decode_progress(void) {
    const u32 rates[] = {0, 1, 49, 50};
    u8 data[OLD_SIZE];
    f32 output[17 * 2];
    make_old_song(data);

    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        struct PreSong* song = pre_song_create(data, sizeof(data));
        assert(song != NULL);
        pre_song_set_sample_rate(song, rates[i]);
        pre_song_start(song);
        assert(pre_song_decode(song, output, 17) == 17);
        pre_song_destroy(song);
    }
}

static void test_non_positive_decode_counts_do_not_change_state(void) {
    const int invalid_counts[] = {0, -1};
    u8 data[OLD_SIZE];
    make_old_song(data);

    for (size_t i = 0; i < sizeof(invalid_counts) / sizeof(invalid_counts[0]); ++i) {
        struct PreSong* song = pre_song_create(data, sizeof(data));
        f32 output[4] = {123.0f, 123.0f, 123.0f, 123.0f};
        PrePlaybackState state_before;
        assert(song != NULL);
        pre_song_start(song);
        state_before = *pre_song_get_playback_state(song);

        assert(pre_song_decode(song, output, invalid_counts[i]) == 0);
        for (size_t sample = 0; sample < sizeof(output) / sizeof(output[0]); ++sample)
            assert(output[sample] == 123.0f);
        assert(memcmp(pre_song_get_playback_state(song), &state_before, sizeof(state_before)) == 0);

        assert(pre_song_decode(song, output, 2) == 2);
        for (size_t sample = 0; sample < sizeof(output) / sizeof(output[0]); ++sample)
            assert(output[sample] == 0.0f);
        pre_song_destroy(song);
    }
}

static void test_non_positive_scoped_decode_counts_do_not_change_state(void) {
    const int invalid_counts[] = {0, -1};
    u8 data[OLD_SIZE];
    make_old_song(data);

    for (size_t i = 0; i < sizeof(invalid_counts) / sizeof(invalid_counts[0]); ++i) {
        struct PreSong* song = pre_song_create(data, sizeof(data));
        f32 output[4] = {123.0f, 123.0f, 123.0f, 123.0f};
        f32 scope[2] = {456.0f, 456.0f};
        f32* scopes[] = {scope};
        PrePlaybackState state_before;
        assert(song != NULL);
        pre_song_start(song);
        state_before = *pre_song_get_playback_state(song);

        assert(pre_song_decode_with_scopes(song, output, invalid_counts[i], scopes, 1) == 0);
        for (size_t sample = 0; sample < sizeof(output) / sizeof(output[0]); ++sample)
            assert(output[sample] == 123.0f);
        for (size_t sample = 0; sample < sizeof(scope) / sizeof(scope[0]); ++sample)
            assert(scope[sample] == 456.0f);
        assert(memcmp(pre_song_get_playback_state(song), &state_before, sizeof(state_before)) == 0);

        assert(pre_song_decode_with_scopes(song, output, 2, scopes, 1) == 2);
        for (size_t sample = 0; sample < sizeof(output) / sizeof(output[0]); ++sample)
            assert(output[sample] == 0.0f);
        for (size_t sample = 0; sample < sizeof(scope) / sizeof(scope[0]); ++sample)
            assert(scope[sample] == 0.0f);
        pre_song_destroy(song);
    }
}

static void test_mixer_rejects_non_positive_counts_before_touching_buffers(void) {
    f32 output = 123.0f;
    f32 scope = 456.0f;
    f32* scopes[] = {&scope};

    assert(pretracker_mixer_render(NULL, NULL, &output, 0, scopes, 1) == 0);
    assert(output == 123.0f);
    assert(scope == 456.0f);
    assert(pretracker_mixer_render(NULL, NULL, &output, -1, scopes, 1) == 0);
    assert(output == 123.0f);
    assert(scope == 456.0f);
}

static void test_mixer_sample_rate_timing(void) {
    MixerState mixer;
    const u32 low_rates[] = {0, 1, 49, 50};

    for (size_t i = 0; i < sizeof(low_rates) / sizeof(low_rates[0]); ++i) {
        pretracker_mixer_init(&mixer, low_rates[i]);
        assert(mixer.output_rate == PRE_MIN_SAMPLE_RATE);
        assert(mixer.samples_per_tick == 1);
    }

    pretracker_mixer_init(&mixer, 44100);
    assert(mixer.output_rate == 44100);
    assert(mixer.samples_per_tick == 882);
    pretracker_mixer_init(&mixer, 48000);
    assert(mixer.output_rate == 48000);
    assert(mixer.samples_per_tick == 960);
}

static void test_rejects_invalid_position_pattern(void) {
    u8 data[OLD_SIZE];
    SongState song;
    make_old_song(data);
    data[0x60] = 2;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);
}

static void test_rejects_invalid_instrument_lookup_index(void) {
    u8 data[ONE_WAVE_SIZE];
    SongState song;
    const int lookup_fields[] = {0, 1, 2, 3, 4, 6};
    for (size_t i = 0; i < sizeof(lookup_fields) / sizeof(lookup_fields[0]); ++i) {
        memset(data, 0, sizeof(data));
        make_old_song(data);
        write_be32(data + 0x10, 0xA0);
        data[0x40] = 1;
        data[0x90 + lookup_fields[i]] = 16;
        assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);
    }

    memset(data, 0, sizeof(data));
    make_old_song(data);
    write_be32(data + 0x10, 0xA0);
    data[0x40] = 1;
    data[0x90] = 15;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);
}

static void test_rejects_invalid_wave_cross_references(void) {
    u8 data[ONE_WAVE_SIZE];

    make_one_wave_song(data);
    struct PreSong* song = pre_song_create(data, sizeof(data));
    assert(song != NULL);
    pre_song_destroy(song);

    const u8 generation_indices[] = {10, 24};
    for (size_t i = 0; i < sizeof(generation_indices); ++i) {
        make_one_wave_song(data);
        data[0x42] = generation_indices[i];
        assert(pre_song_create(data, sizeof(data)) == NULL);
    }

    make_one_wave_song(data);
    data[ONE_WAVE_INFO_OFFSET + 0x1A] = 255;
    assert(pre_song_create(data, sizeof(data)) == NULL);

    make_one_wave_song(data);
    data[0x98 + 1] = INST_CMD_SELECT_WAVE;
    data[0x98 + 2] = 2;
    assert(pre_song_create(data, sizeof(data)) == NULL);

    make_one_wave_song(data);
    data[0x98 + 1] = INST_CMD_SELECT_WAVE_NOSYNC;
    data[0x98 + 2] = 0;
    song = pre_song_create(data, sizeof(data));
    assert(song != NULL);
    pre_song_destroy(song);

    u8 two_wave_data[TWO_WAVE_SIZE];
    make_two_wave_song(two_wave_data);
    song = pre_song_create(two_wave_data, sizeof(two_wave_data));
    assert(song != NULL);
    pre_song_destroy(song);

    make_two_wave_song(two_wave_data);
    two_wave_data[0x43] = 0;
    assert(pre_song_create(two_wave_data, sizeof(two_wave_data)) == NULL);
}

static void test_v15_validates_every_subsong(void) {
    u8 data[V15_SIZE];
    SongState song;
    make_v15_song(data);
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);
    assert(song.num_patterns == 1);

    data[0x6B] = 32;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);

    make_v15_song(data);
    write_be32(data + 0x6C, 0xFFFFFFFFu);
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);

    make_v15_song(data);
    data[0x61] = 2;
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);

    make_v15_song(data);
    write_be32(data + 0x6C, 0);
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) == 0);
}

static void test_subsong_apply_is_bounded(void) {
    u8 data[V15_SIZE];
    SongState song;
    make_v15_song(data);
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);
    u8* old_patterns = song.patterns_ptr;

    assert(!pretracker_apply_subsong(&song, data, 0x85, 1));
    assert(song.patterns_ptr == old_patterns);
    assert(pretracker_apply_subsong(&song, data, sizeof(data), 1));
    assert(song.patterns_ptr == data + 0x83);
}

static void test_subsong_rebuild_clears_stale_patterns(void) {
    u8 data[V15_SIZE];
    SongState song;
    make_v15_song(data);
    data[0x61] = 2;
    data[0x70] = 2;
    write_be32(data + 0x6C, 6);
    assert(pretracker_parse_song(&song, data, sizeof(data), 0) != 0);
    pretracker_rebuild_pattern_table(&song);
    assert(song.pattern_table[1] != NULL);

    assert(pretracker_apply_subsong(&song, data, sizeof(data), 1));
    pretracker_rebuild_pattern_table(&song);
    assert(song.num_patterns == 1);
    assert(song.pattern_table[1] == NULL);
}

int main(void) {
    test_old_layout_and_pattern_table();
    test_rejects_zero_wave_song();
    test_public_track_bounds();
    test_player_skips_missing_pattern_pointer();
    test_extreme_tonal_note_clamps_oscillator_index();
    test_fixed_point_period_interpolation();
    test_negative_position_transposition();
    test_player_restart_resets_tempo_and_playback_state();
    test_public_restart_discards_pattern_tempo_effects();
    test_subsong_restart_preserves_layout_and_resets_tempo();
    test_negative_filter_and_pitch_ramp();
    test_sample_clamp_matches_signed_byte_rails();
    test_rejects_truncated_old_layouts();
    test_empty_layouts_remain_bounded();
    test_minimum_sample_rate_preserves_decode_progress();
    test_non_positive_decode_counts_do_not_change_state();
    test_non_positive_scoped_decode_counts_do_not_change_state();
    test_mixer_rejects_non_positive_counts_before_touching_buffers();
    test_mixer_sample_rate_timing();
    test_rejects_invalid_position_pattern();
    test_rejects_invalid_instrument_lookup_index();
    test_rejects_invalid_wave_cross_references();
    test_v15_validates_every_subsong();
    test_subsong_apply_is_bounded();
    test_subsong_rebuild_clears_stale_patterns();
    return 0;
}
