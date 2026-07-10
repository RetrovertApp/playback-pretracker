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
    test_sample_clamp_matches_signed_byte_rails();
    test_rejects_truncated_old_layouts();
    test_empty_layouts_remain_bounded();
    test_rejects_invalid_position_pattern();
    test_rejects_invalid_instrument_lookup_index();
    test_rejects_invalid_wave_cross_references();
    test_v15_validates_every_subsong();
    test_subsong_apply_is_bounded();
    test_subsong_rebuild_clears_stale_patterns();
    return 0;
}
