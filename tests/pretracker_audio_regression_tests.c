#include "pretracker_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SongState song;
    PlayerState player;
    f32* samples;
} SyntheticFixture;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                           \
            exit(EXIT_FAILURE);                                                                                        \
        }                                                                                                              \
    } while (0)

static void configure_wave(SyntheticFixture* fixture, u8 index, u32 length, u32 total_length) {
    WaveInfo* wave = &fixture->song.waveinfos[index];
    memset(wave, 0, sizeof(*wave));
    wave->osc_gain = 128;
    wave->vol_sustain = 0xFF;
    wave->sam_len = (u8)(length / HQ_MAX_PERIOD - 1);
    fixture->song.waveinfo_table[index] = wave;
    fixture->song.wavegen_order_table[index] = index;
    fixture->song.wavelength_table[index] = length;
    fixture->song.wavetotal_table[index] = total_length;
}

static void generate(SyntheticFixture* fixture, u8 wave_count) {
    u32 total = 2;
    fixture->song.num_waves = wave_count;
    fixture->song.waveinfo_ptr = fixture->song.waveinfos;
    for (u8 i = 0; i < wave_count; ++i)
        total += fixture->song.wavetotal_table[i];
    fixture->samples = calloc(total, sizeof(*fixture->samples));
    CHECK(fixture->samples != NULL);
    pretracker_player_init(&fixture->player, fixture->samples, &fixture->song);
    pretracker_wavegen_generate(&fixture->player);
}

static void destroy_fixture(SyntheticFixture* fixture) {
    free(fixture->samples);
}

static u8 quantize_signed_byte(f32 sample) {
    long value = lroundf(sample * 128.0f);
    if (value < -128)
        value = -128;
    if (value > 127)
        value = 127;
    return (u8)(i8)value;
}

static u32 golden_hash(const f32* samples, u32 count) {
    u32 hash = UINT32_C(2166136261);
    for (u32 i = 0; i < count; ++i) {
        hash ^= quantize_signed_byte(samples[i]);
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void check_quantization_margin(const f32* samples, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        f32 scaled = samples[i] * 128.0f;
        f32 half_step_distance = fabsf((scaled - floorf(scaled)) - 0.5f);
        CHECK(half_step_distance > 0.0001f);
    }
}

static void assert_golden(const char* name, const f32* samples, u32 count, u32 expected) {
    check_quantization_margin(samples, count);
    u32 actual = golden_hash(samples, count);
    if (actual != expected)
        fprintf(stderr, "%s: expected %08x, got %08x\n", name, expected, actual);
    CHECK(actual == expected);
}

static void test_higher_octaves_average_adjacent_samples(void) {
    SyntheticFixture fixture = {0};
    const u32 base_length = 2 * HQ_MAX_PERIOD;
    const u32 octave_length = base_length * 7 / 8;
    configure_wave(&fixture, 0, base_length, base_length + octave_length);
    fixture.song.waveinfos[0].flags = WI_FLAG_EXTRA_OCTAVES;
    fixture.song.waveinfos[0].osc_basenote = 1;
    generate(&fixture, 1);

    const f32* wave = fixture.player.wave_sample_table[0];
    for (u32 i = 0; i < octave_length; ++i)
        CHECK(wave[base_length + i] == (wave[i * 2] + wave[i * 2 + 1]) * 0.5f);
    assert_golden("averaged higher octaves", wave + base_length, octave_length, UINT32_C(0xccd296f8));
    destroy_fixture(&fixture);
}

static void test_layered_noise_saturates_instead_of_wrapping(void) {
    SyntheticFixture fixture = {0};
    configure_wave(&fixture, 0, HQ_MAX_PERIOD, HQ_MAX_PERIOD);
    WaveInfo* wave = &fixture.song.waveinfos[0];
    wave->flags = OSC_TYPE_NOISE;
    wave->osc_phase_min = 37;
    wave->chord_shift = 19;
    wave->chord_note1 = 1;
    wave->chord_note2 = 2;
    wave->chord_note3 = 3;
    generate(&fixture, 1);

    const f32* output = fixture.player.wave_sample_table[0];
    CHECK(output[0] == -1.0f);
    CHECK(output[1] == 127.0f / 128.0f);
    for (u32 i = 0; i < HQ_MAX_PERIOD; ++i) {
        CHECK(output[i] >= -1.0f);
        CHECK(output[i] <= 127.0f / 128.0f);
    }
    assert_golden("saturating layered noise", output, HQ_MAX_PERIOD, UINT32_C(0xfe47c06d));
    destroy_fixture(&fixture);
}

static void test_high_octave_phase_uses_full_width_product(void) {
    SyntheticFixture fixture = {0};
    configure_wave(&fixture, 0, HQ_MAX_PERIOD, HQ_MAX_PERIOD);
    WaveInfo* wave = &fixture.song.waveinfos[0];
    wave->osc_basenote = 60;
    wave->osc_phase_min = 23;
    wave->osc_phase_max = 23;
    wave->chord_shift = 11;
    wave->chord_note1 = 1;
    generate(&fixture, 1);

    const f32* output = fixture.player.wave_sample_table[0];
    CHECK(golden_hash(output, HQ_MAX_PERIOD) != UINT32_C(0x1469dfa7)); /* 68k 16-bit mulu behavior */
    assert_golden("full-width high-octave phase", output, HQ_MAX_PERIOD, UINT32_C(0xb39c08b9));
    destroy_fixture(&fixture);
}

static void test_wave_mix_uses_true_minimum_length(void) {
    SyntheticFixture fixture = {0};
    configure_wave(&fixture, 0, HQ_MAX_PERIOD, HQ_MAX_PERIOD);
    fixture.song.waveinfos[0].osc_basenote = 7;
    configure_wave(&fixture, 1, UINT16_C(0x8000), UINT16_C(0x8000));
    fixture.song.waveinfos[1].osc_basenote = 3;
    fixture.song.waveinfos[1].mix_wave = 1;
    generate(&fixture, 2);

    const f32* output = fixture.player.wave_sample_table[1];
    CHECK(golden_hash(output, UINT16_C(0x8000)) != UINT32_C(0xf835a83f)); /* 68k signed cmp behavior */
    assert_golden("true-minimum wave mixing", output, UINT16_C(0x8000), UINT32_C(0xb958cc64));
    destroy_fixture(&fixture);
}

int main(void) {
    test_higher_octaves_average_adjacent_samples();
    test_layered_noise_saturates_instead_of_wrapping();
    test_high_octave_phase_uses_full_width_product();
    test_wave_mix_uses_true_minimum_length();
    return 0;
}
