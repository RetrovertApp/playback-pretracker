#include "pretracker_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int failures;

#define CHECK(condition, ...)                  \
    do {                                       \
        if (!(condition)) {                    \
            fprintf(stderr, __VA_ARGS__);      \
            fputc('\n', stderr);               \
            failures++;                        \
            return;                            \
        }                                      \
    } while (0)

static float clamp_sample_ref(float value) {
    if (value > 1.0f)
        return 1.0f;
    if (value < -1.0f)
        return -1.0f;
    return value;
}

static float floor_lsb_ref(float value) {
    return floorf(value * 128.0f) / 128.0f;
}

static void filter_reference(float* samples, size_t length, int filter_type, int cutoff, int resonance,
                             int quantize_products) {
    float taps[4] = { 0 };
    int d2 = 255 - cutoff;
    if (!(filter_type & 1))
        d2 = 255 - d2;

    int d0 = d2 * 2;
    if (resonance != 0) {
        int divisor = (0xB6 / 2 - resonance) * 2;
        if (divisor < 0x36)
            divisor = 0x36;
        d0 = d2 + (d2 * 256) / divisor;
    }

    const float d2_coeff = (float)d2 / 256.0f;
    const float d0_coeff = (float)d0 / 256.0f;

    for (size_t i = 0; i < length; ++i) {
        const float input = samples[i];
        float increment = (taps[0] - taps[1]) * d0_coeff;
        if (quantize_products)
            increment = floor_lsb_ref(increment);
        increment = (increment - taps[0] + input) * d2_coeff;
        if (quantize_products)
            increment = floor_lsb_ref(increment);
        taps[0] += increment;

        for (int tap = 1; tap < 4; ++tap) {
            increment = (taps[tap - 1] - taps[tap]) * d2_coeff;
            if (quantize_products)
                increment = floor_lsb_ref(increment);
            taps[tap] += increment;
        }

        float output = taps[3];
        switch (filter_type) {
            case FILTER_LOWPASS:
                break;
            case FILTER_HIGHPASS:
                output -= input;
                break;
            case FILTER_BANDPASS:
                output -= taps[0] + taps[1] + taps[2];
                if (quantize_products)
                    output = floor_lsb_ref(output * 0.5f);
                else
                    output *= 0.5f;
                break;
            case FILTER_NOTCH:
                output = -(output - taps[0]);
                break;
        }
        samples[i] = clamp_sample_ref(output);
    }
}

static WaveInfo filter_settings(int type, int cutoff, int resonance) {
    WaveInfo wi = { 0 };
    wi.flt_type = (u8)type;
    wi.flt_start = (u8)cutoff;
    wi.flt_min = (u8)cutoff;
    wi.flt_max = (u8)cutoff;
    wi.flt_resonance = (u8)resonance;
    return wi;
}

enum InputKind { INPUT_IMPULSE, INPUT_STEP, INPUT_SINE, INPUT_SILENCE_TAIL, INPUT_HOT };

static void make_input(float* samples, size_t length, enum InputKind kind) {
    memset(samples, 0, length * sizeof(*samples));
    for (size_t i = 0; i < length; ++i) {
        switch (kind) {
            case INPUT_IMPULSE:
                samples[i] = i == 0 ? 0.78125f : 0.0f;
                break;
            case INPUT_STEP:
                samples[i] = 0.6015625f;
                break;
            case INPUT_SINE:
                samples[i] = 0.75f * sinf((float)i * 0.19634954084936207f);
                break;
            case INPUT_SILENCE_TAIL:
                samples[i] = i < length / 4 ? 0.75f * sinf((float)i * 0.39269908169872414f) : 0.0f;
                break;
            case INPUT_HOT:
                samples[i] = (i & 1) ? -1.0f : 1.0f;
                break;
        }
    }
}

static void test_boundary_vectors_match_reference(void) {
    static const int cutoffs[] = { 0, 1, 254, 255 };
    static const int resonances[] = { 0, 1, 63, 64, 90, 91, 92, 255 };
    enum { SAMPLE_COUNT = 512 };
    float actual[SAMPLE_COUNT];
    float expected[SAMPLE_COUNT];

    for (int type = FILTER_LOWPASS; type <= FILTER_NOTCH; ++type) {
        for (size_t c = 0; c < ARRAY_COUNT(cutoffs); ++c) {
            for (size_t r = 0; r < ARRAY_COUNT(resonances); ++r) {
                for (int input = INPUT_IMPULSE; input <= INPUT_HOT; ++input) {
                    make_input(actual, SAMPLE_COUNT, (enum InputKind)input);
                    memcpy(expected, actual, sizeof(actual));
                    WaveInfo wi = filter_settings(type, cutoffs[c], resonances[r]);
                    pretracker_test_gen_filter(actual, SAMPLE_COUNT, &wi);
                    filter_reference(expected, SAMPLE_COUNT, type, cutoffs[c], resonances[r], 1);

                    for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
                        CHECK(fabsf(actual[i] - expected[i]) <= 1.0e-7f,
                              "reference mismatch: type=%d cutoff=%d resonance=%d input=%d sample=%zu: %.9g != %.9g",
                              type, cutoffs[c], resonances[r], input, i, actual[i], expected[i]);
                    }
                }
            }
        }
    }
}

static void test_input_and_output_remain_continuous(void) {
    float samples[8] = { 0.12345f, -0.23456f, 0.34567f, -0.45678f, 0.0f, 0.0f, 0.0f, 0.0f };
    WaveInfo wi = filter_settings(FILTER_HIGHPASS, 1, 0);
    pretracker_test_gen_filter(samples, ARRAY_COUNT(samples), &wi);

    float scaled = samples[0] * 128.0f;
    CHECK(fabsf(scaled - roundf(scaled)) > 1.0e-4f,
          "filter quantized normalized input/output to the signed-byte grid");
}

static float peak_range(const float* samples, size_t begin, size_t length) {
    float peak = 0.0f;
    for (size_t i = begin; i < length; ++i) {
        float magnitude = fabsf(samples[i]);
        if (magnitude > peak)
            peak = magnitude;
    }
    return peak;
}

static void test_reference_character(void) {
    enum { ATTACK_COUNT = 4096, TAIL_COUNT = 22000 };
    float quantized[TAIL_COUNT];
    float continuous[TAIL_COUNT];

    make_input(quantized, ATTACK_COUNT, INPUT_STEP);
    memcpy(continuous, quantized, ATTACK_COUNT * sizeof(*quantized));
    WaveInfo attack_wi = filter_settings(FILTER_LOWPASS, 251, 0); // d2 = 4
    pretracker_test_gen_filter(quantized, ATTACK_COUNT, &attack_wi);
    filter_reference(continuous, ATTACK_COUNT, FILTER_LOWPASS, 251, 0, 0);
    CHECK(peak_range(quantized, 0, 24) == 0.0f, "low-cutoff compatibility attack was not delayed");
    CHECK(peak_range(continuous, 0, 24) > 0.0f, "continuous comparison did not rise immediately");
    CHECK(peak_range(quantized, ATTACK_COUNT - 256, ATTACK_COUNT) == 0.0f,
          "low-cutoff deadband did not retain its changed steady-state gain");
    CHECK(peak_range(continuous, ATTACK_COUNT - 256, ATTACK_COUNT) > 0.5f,
          "continuous low-cutoff comparison did not converge to the step level");

    memset(quantized, 0, sizeof(quantized));
    for (size_t i = 0; i < 512; ++i)
        quantized[i] = 0.75f * sinf((float)i * 0.19634954084936207f);
    memcpy(continuous, quantized, sizeof(quantized));
    WaveInfo tail_wi = filter_settings(FILTER_LOWPASS, 195, 200); // d2 = 60
    pretracker_test_gen_filter(quantized, TAIL_COUNT, &tail_wi);
    filter_reference(continuous, TAIL_COUNT, FILTER_LOWPASS, 195, 200, 0);
    CHECK(peak_range(quantized, TAIL_COUNT - 1024, TAIL_COUNT) >= 1.0f / 128.0f,
          "compatibility filter lost its retained silence-tail state");
    CHECK(peak_range(continuous, TAIL_COUNT - 1024, TAIL_COUNT) < 1.0e-5f,
          "continuous comparison did not decay to silence");
}

static void test_supported_range_is_bounded_and_stable(void) {
    enum { SAMPLE_COUNT = 256 };
    float samples[SAMPLE_COUNT];

    for (int type = FILTER_LOWPASS; type <= FILTER_NOTCH; ++type) {
        for (int cutoff = 0; cutoff <= 255; ++cutoff) {
            for (int resonance = 0; resonance <= 255; ++resonance) {
                make_input(samples, SAMPLE_COUNT, INPUT_HOT);
                WaveInfo wi = filter_settings(type, cutoff, resonance);
                pretracker_test_gen_filter(samples, SAMPLE_COUNT, &wi);
                for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
                    CHECK(isfinite(samples[i]), "non-finite output: type=%d cutoff=%d resonance=%d sample=%zu",
                          type, cutoff, resonance, i);
                    CHECK(samples[i] >= -1.0f && samples[i] <= 1.0f,
                          "unbounded output: type=%d cutoff=%d resonance=%d sample=%zu value=%g", type, cutoff,
                          resonance, i, samples[i]);
                }
            }
        }
    }
}

int main(void) {
    test_boundary_vectors_match_reference();
    test_input_and_output_remain_continuous();
    test_reference_character();
    test_supported_range_is_bounded_and_stable();

    if (failures != 0) {
        fprintf(stderr, "%d filter test(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    puts("filter tests passed");
    return EXIT_SUCCESS;
}
