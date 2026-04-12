// PreTracker C player — C port of the Raspberry Casket Amiga music replayer
// Copyright (c) 2022 Chris 'platon42' Hodges <chrisly@platon42.de>
// Copyright (c) 2026 Daniel Collin <daniel@collin.com>
// SPDX-License-Identifier: MIT

#include "pretracker_internal.h"

#include <math.h>
#include <string.h>

_Static_assert(sizeof(WaveInfo) == 42, "WaveInfo must be 42 bytes to match assembly layout");

// Big-endian read helpers (PRT files are Amiga/68k big-endian)
static u16 read_be16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}

static u32 read_be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

// Clamp to [-1.0, +1.0] — matches assembly CLIPTO8BIT / CLIPTO8BITAFTERADD saturation
static inline f32 clamp_sample(f32 x) {
    if (x > 1.0f)
        return 1.0f;
    if (x < -1.0f)
        return -1.0f;
    return x;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Lookup tables from raspberry_casket.asm (lines 4115-4198)

// clang-format off
// Amiga resolution: { 128, 121, 114, 107, 102, 96, 90, 85, 80, 76, 72, 67 }
// HQ resolution:    { 222, 210, 198, 186, 177, 166, 156, 147, 139, 132, 125, 116 }
static const u16 s_log12_table[NOTES_IN_OCTAVE] = { 128, 121, 114, 107, 102, 96, 90, 85, 80, 76, 72, 67 };

static const u8 s_vib_speed_table[16] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 20, 40, 80 };

static const u8 s_vib_depth_table[16] = { 0, 8, 9, 10, 11, 12, 13, 14, 18, 20, 28, 40, 50, 70, 160, 255 };

static const u8 s_vib_delay_table[16] = { 0, 4, 8, 10, 12, 14, 16, 18, 20, 24, 32, 40, 56, 96, 150, 255 };

static const u8 s_ramp_up_16[16] = { 0, 1, 3, 6, 7, 9, 10, 11, 12, 13, 14, 16, 19, 35, 55, 143 };

static const i16 s_fast_roll_off_16[16] = { 
    0x400, 0x200, 0x80, 0x64, 0x50, 0x40, 0x30, 0x20, 0x10, 14, 12, 10, 8, 4, 2, 1 
};

static const i16 s_roll_off_table[] = { 
    0x400, 0x200, 0x180, 0x140, 0x100, 0xC0, 0xA0, 0x80, 0x78, 0x74, 0x6E, 0x69, 0x64, 0x5A, 0x46, 0x40, 0x38, 0x30,
    0x28,  0x20,  0x1F,  0x1E,  0x1D,  0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    15,    14,    13,    13,    12,    12,   11,   11,   10,   10,   9,    9,    8,    8,    8,    8,    7,    7,
    7,     7,     6,     6,     6,     6,    5,    5,    5,    5,    4,    4,    4,    4,    4,    4,    4,    4,
    4,     4,     3,     4,     4,     3,    4,    4,    3,    4,    3,    4,    3,    4,    3,    4,    3,    3,
    3,     3,     3,     3,     3,     3,    3,    2,    3,    3,    3,    2,    3,    3,    2,    3,    3,    2,
    3,     3,     2,     3,     2,     3,    2,    3,    2,    3,    2,    3,    2,    2,    2,    2,    2,    2,
    2,     2,     1,     2,     1,     2,    1,    2,    1,    2,    1,    1,    2,    1,    1,    1,    2,    1 
};

static const i16 s_ramp_up_down_32[32] = { 
    0 * 32,  1 * 32,  2 * 32,  3 * 32,  4 * 32,  5 * 32,  6 * 32,  7 * 32,  8 * 32,  9 * 32,  10 * 32,
    11 * 32, 12 * 32, 13 * 32, 14 * 32, 15 * 32, 15 * 32, 14 * 32, 13 * 32, 12 * 32, 11 * 32, 10 * 32,
    9 * 32,  8 * 32,  7 * 32,  6 * 32,  5 * 32,  4 * 32,  3 * 32,  2 * 32,  1 * 32,  0 * 32 
};

static const u16 s_modulator_ramp_8[8] = { 0x4D, 0x125, 0x21B, 0x437, 0x539, 0x755, 0x96D, 0xBD7 };

static const u16 s_period_table[3 * NOTES_IN_OCTAVE + 1] = { 
    0x350, 0x320, 0x2F2, 0x2C8, 0x2A0, 0x279, 0x256, 0x236, 0x216, 0x1F8, 0x1DC, 0x1C0, 0x1A8,
    0x190, 0x179, 0x164, 0x151, 0x13E, 0x12C, 0x11B, 0x10B, 0x0FC, 0x0EE, 0x0E0, 0x0D4, 0x0C8,
    0x0BD, 0x0B2, 0x0A8, 0x09F, 0x096, 0x08D, 0x086, 0x07E, 0x078, 0x071, 0x071 
};
// clang-format on

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Forward declarations for pipeline stages

static void gen_filter(MyPlayer* player, const WaveInfo* wi);
static void gen_modulator(MyPlayer* player, const WaveInfo* wi);
static void gen_volume_envelope(MyPlayer* player, const WaveInfo* wi);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pre_song_init: Parse PRT file data into MySong structure
// Matches pre_SongInit at raspberry_casket.asm:724-915

u32 pre_song_init(MySong* song, u8* prt_data, u32 prt_size, int subsong) {
    (void)prt_size;

    memset(song, 0, sizeof(MySong));

    u32 header = read_be32(prt_data);
    u8 version = (u8)(header & 0xFF);
    u32 magic = header & 0xFFFFFF00;
    if (magic != 0x50525400) {
        return 0;
    }

    u8 max_inst_names = MAX_INSTRUMENTS;
    u32 posd_offset = read_be32(prt_data + 0x04);
    u32 patt_offset = read_be32(prt_data + 0x08);

    if (version == 0x1E) {
        // V1.5 subsong support
        u8 num_subsongs = prt_data[0x5A];
        song->num_subsongs = num_subsongs > 0 ? num_subsongs : 1;

        int sel = (subsong >= 0 && subsong < song->num_subsongs) ? subsong : 0;

        // Subsong headers are contiguous at the start of POSD, 8 bytes each:
        //   byte 0: restart, byte 1: num_patterns, byte 2: num_steps, byte 3: song_length
        //   bytes 4-7: BE32 pattern data offset relative to PATT section
        u8* subsong_hdr = prt_data + posd_offset + (u32)sel * 8;

        song->pat_restart_pos = subsong_hdr[0];
        song->pat_pos_len = subsong_hdr[3];
        song->num_steps = subsong_hdr[2];

        // Position data starts after all subsong headers, each subsong's entries are contiguous
        u32 pos_data_base = posd_offset + (u32)song->num_subsongs * 8;
        u32 pos_entries_before = 0;
        for (int i = 0; i < sel; i++) {
            u8* hdr = prt_data + posd_offset + (u32)i * 8;
            pos_entries_before += hdr[3]; // song_length field
        }
        song->pos_data_adr = prt_data + pos_data_base + pos_entries_before * 8;

        // Adjust PATT base by subsong's relative pattern offset
        u32 pat_rel = read_be32(subsong_hdr + 4);
        song->patterns_ptr = prt_data + patt_offset + pat_rel;

        max_inst_names = 2 * MAX_INSTRUMENTS;
    } else if (version > 0x1B) {
        return 0;
    } else {
        song->num_subsongs = 1;
        song->pat_restart_pos = prt_data[0x3C];
        song->pat_pos_len = prt_data[0x3E];
        song->num_steps = prt_data[0x3F];
        song->pos_data_adr = prt_data + posd_offset;
        song->patterns_ptr = prt_data + patt_offset;
    }

    song->num_waves = prt_data[0x41];

    // Skip instrument names
    u32 inst_offset = read_be32(prt_data + 0x0C);
    u8* ptr = prt_data + inst_offset;
    for (int i = 0; i < max_inst_names; i++) {
        int remaining = 23 - 1;
        while (*ptr != 0 && remaining > 0) {
            ptr++;
            remaining--;
        }
        ptr++;
    }

    // Parse instrument infos
    u8 num_instruments = prt_data[0x40];
    u8 actual_instruments = num_instruments;
    if (version == 0x1E && actual_instruments > MAX_INSTRUMENTS) {
        actual_instruments = MAX_INSTRUMENTS;
    }

    u8* inst_info_base = ptr;
    // d0 in asm = ptr to after all inst infos = inst_info_base + num_instruments * 8
    u8* inst_pattern_ptr = inst_info_base + (u32)num_instruments * 8;

    for (int i = 0; i < actual_instruments; i++) {
        u8* ii = inst_info_base + i * 8;
        UnpackedInstrumentInfo* uii = &song->inst_infos[i];

        u8 vd = ii[0]; // vibrato_delay
        uii->vibrato_delay = (i16)(s_vib_delay_table[vd] + 1);

        u8 vdp = ii[1]; // vibrato_depth
        uii->vibrato_depth = (i16)s_vib_depth_table[vdp];

        u8 vs = ii[2]; // vibrato_speed
        i16 speed_val = (i16)s_vib_speed_table[vs];
        // muls uii_vibrato_depth(a4),d1; asr.w #4,d1
        uii->vibrato_speed = (i16)((speed_val * uii->vibrato_depth) >> 4);

        u8 atk = ii[3]; // adsr_attack
        uii->adsr_attack = s_fast_roll_off_16[atk];

        u8 dec = ii[4]; // adsr_decay
        uii->adsr_decay = (i16)s_ramp_up_16[dec];

        u8 sus = ii[5]; // adsr_sustain
        if (sus == 15) {
            sus = 16;
        }
        uii->adsr_sustain = (i16)((u16)sus << 6);

        u8 rel = ii[6]; // adsr_release
        uii->adsr_release = s_ramp_up_16[rel];

        u8 steps = ii[7]; // pattern_steps
        uii->pattern_steps = steps;

        // Store instrument pattern pointer, advance past pattern data
        song->inst_patterns_table[i] = inst_pattern_ptr;
        inst_pattern_ptr += (u32)steps * 3;
    }

    // Skip wave names
    u32 wave_offset = read_be32(prt_data + 0x10);
    ptr = prt_data + wave_offset;
    for (int i = 0; i < MAX_WAVES; i++) {
        int remaining = 23 - 1;
        while (*ptr != 0 && remaining > 0) {
            ptr++;
            remaining--;
        }
        ptr++;
    }

    // Align to even address
    if ((uintptr_t)ptr & 1) {
        ptr++;
    }
    song->waveinfo_ptr = (WaveInfo*)ptr;

    // Wave generation ordering
    if (version > 0x19) {
        memcpy(song->wavegen_order_table, prt_data + 0x42, MAX_WAVES);
    } else {
        for (int i = 0; i < MAX_WAVES; i++) {
            song->wavegen_order_table[i] = (u8)i;
        }
    }

    // Calculate sample sizes
    u32 total_chip_mem = 2;
    if (song->num_waves > 0) {
        WaveInfo* wi = song->waveinfo_ptr;
        for (int i = 0; i < song->num_waves; i++) {
            song->waveinfo_table[i] = wi;
            u32 std_len = ((u32)wi->sam_len + 1) * HQ_MAX_PERIOD;
            song->wavelength_table[i] = std_len;

            u32 total_len = std_len;
            if (wi->flags & WI_FLAG_EXTRA_OCTAVES) {
                total_len = (std_len * 15) / 8;
            }
            song->wavetotal_table[i] = total_len;
            total_chip_mem += total_len;
            wi++;
        }
    }

    return total_chip_mem;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Oscillator buffer generation
// Matches raspberry_casket.asm:1040-1098

static void gen_osc_buffers(MyPlayer* player) {
    for (int note = 0; note < NOTES_IN_OCTAVE; note++) {
        OscNoteBuffers* nb = &player->osc_buffers[note];
        u16 period = s_log12_table[note];
        nb->wave_length = period;

        u16 frac_inc = 0xFF00 / period;
        u16 half_period = period >> 1;
        u16 quarter_period = half_period >> 1;

        u16 acc = 0;

        // a2 in asm points into tri_waves, starting at offset (period - quarter_period)
        // and wrapping back by period when pos == quarter_period
        int tri_pos = (int)period - (int)quarter_period;

        for (u16 pos = 0; pos < period; pos++) {
            u8 frac = (u8)(acc >> 8);

            // Sawtooth: map 0..255 -> +1.0..-1.0
            nb->saw_waves[pos] = 1.0f - 2.0f * (f32)frac / 255.0f;

            // Doubled frac for triangle/square
            u8 doubled = (u8)(frac << 1); // add.b d2,d2 wraps at 8 bits

            // Wrap tri_pos when pos == quarter_period
            if (pos == quarter_period) {
                tri_pos -= period;
            }

            // cmpa.w d0,a3; ble.s .otherhalf — 68k sign-extends d0.w to 32-bit for comparison
            // When acc > 0x7FFF it becomes negative, so pos (always positive) is greater
            if ((i16)pos > (i16)acc) {
                // First half: triangle ramp up, square low
                nb->tri_waves[tri_pos] = 1.0f - 2.0f * (f32)doubled / 255.0f;
                nb->sqr_waves[pos] = -1.0f;
            } else {
                // Second half: triangle ramp down, square high (or low at midpoint)
                nb->tri_waves[tri_pos] = -1.0f + 2.0f * (f32)doubled / 255.0f;
                nb->sqr_waves[pos] = (pos == half_period) ? -1.0f : 1.0f;
            }

            tri_pos++;
            acc += frac_inc;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Noise generator
// Matches raspberry_casket.asm:1314-1437

static void gen_noise(MyPlayer* player, const WaveInfo* wi, i16 octave, i16 basenote, i32 d2_pitch_ramp,
                      bool pitch_linear) {
    i32 base_speed = 0x8000; // d5 — constant base speed, never modified after init

    if (octave < 0) {
        // Negative octave noise speed calculation
        i16 neg_note = -basenote;
        u16 abs_note = (u16)neg_note;

        // Calculate shift amount from division
        u16 div_result = abs_note / NOTES_IN_OCTAVE;
        div_result++;
        u16 shifted = (u16)(0x8000 >> div_result);
        u16 step_per_note = shifted / NOTES_IN_OCTAVE;

        // Cheap mod12: subtract 12 until negative, then negate
        i16 mod_val = (i16)abs_note;
        do {
            mod_val -= NOTES_IN_OCTAVE;
        } while (mod_val >= 0);
        mod_val = -mod_val;

        base_speed = (i32)shifted + (i32)((u16)mod_val * step_per_note);
    }

    u16 noise_seed = (u16)(wi->osc_phase_min + wi->chord_shift + 1);
    f32 noise_gain_f = (f32)wi->osc_gain / 128.0f;

    f32* out = player->wg_curr_sample_ptr;
    i32 noise_speed = base_speed; // a1 — current speed, updated by ramp
    i32 noise_acc = 0x8000;       // a5 — single variable for symmetry AND accumulation
    i32 noise_ramp_acc = 0;       // d6

    for (;;) {
        // PRNG (xorshift)
        u16 ns = noise_seed;
        ns ^= (ns << 13);
        ns ^= (ns >> 9);
        ns ^= (ns << 7);
        noise_seed = ns;

        // Symmetry oscillation on noise_acc (a5)
        if (noise_acc != 0x8000) {
            i32 d4 = noise_acc + (i32)0xFFFF8000;
            i32 d1 = noise_acc + (i32)0xFFFF7FFF;
            d1 &= ~0x7FFF; // andi.w #$8000 — preserves high word, clears bits 0-14
            noise_acc = d4 - d1;
        }

        // Apply gain to noise sample
        f32 noise_sample = (f32)(i8)(ns & 0xFF) / 128.0f;
        f32 gained = noise_gain_f * noise_sample;
        f32 out_val = clamp_sample(*out + gained);

        // Inner loop
        for (;;) {
            *out++ = out_val;
            noise_acc += noise_speed; // adda.l a1,a5

            if (d2_pitch_ramp != 0) {
                noise_ramp_acc += d2_pitch_ramp;
                i32 ramp_adj = noise_ramp_acc >> 10;
                i32 new_speed = base_speed + ramp_adj; // d5 + ramp_adj (constant base)

                if (pitch_linear) {
                    i32 decay = d2_pitch_ramp >> 7;
                    d2_pitch_ramp -= decay;
                }

                noise_speed = new_speed;

                if (new_speed <= 0x1FF) { // cmpa.w #$1FF,a1; bgt skips clamp
                    d2_pitch_ramp = 0;
                    noise_acc = 0;
                    noise_speed = 0x200;
                }
            }

            if (out >= player->wg_curr_samend_ptr) {
                return;
            }
            if ((i32)noise_acc > 0x7FFF) { // cmpa.w #$7FFF,a5; ble continues
                break;
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tonal oscillator generator (saw/tri/sqr with chord and pitch ramp)
// Matches raspberry_casket.asm:1439-1525

static void gen_tonal(MyPlayer* player, const WaveInfo* wi, f32* osc_buf, i16 octave, i32 d6_step, i32 d7_period,
                      i32 phase_min, i32 phase_max, i32 a5_limit, i32 phase_speed, i32 d2_pitch_ramp,
                      bool pitch_linear) {

    // Unisono speed adjustment
    i32 osc_speed = d6_step;
    if (player->wg_unisono_run) {
        u8 unisono_bits = (wi->mod_density >> MOD_UNISONO_SHIFT) & MOD_UNISONO_MASK;
        i32 detune_shift = 9 - unisono_bits;
        osc_speed = d6_step + (d6_step >> detune_shift);
    }

    f32* out = player->wg_curr_sample_ptr;
    i32 a1_phase = phase_min; // current phase modulation value
    i32 a2_ramp_acc = 0;      // pitch ramp accumulator
    i32 osc_pos = 0;          // will be set from chord position calc

    // Calculate initial position (done by caller, stored in d0_pos)
    // The caller passes d2_pitch_ramp for the pitch ramp increment
    u32 chord_shift_val = (u32)wi->chord_shift;
    u16 chord_factor = player->wg_chord_flag + player->wg_chord_note_num;
    chord_shift_val *= chord_factor;
    u32 phase_offset = (u32)wi->osc_phase_min + chord_shift_val;
    u32 raw_pos = ((u32)d6_step >> 4) * (phase_offset << 4);

    // Wrap within period
    u32 d7u = (u32)d7_period;
    while (raw_pos > d7u) {
        raw_pos -= d7u;
    }
    osc_pos = (i32)raw_pos;

    f32 gain_f = (f32)wi->osc_gain / 128.0f;
    i32 local_speed = osc_speed;
    i32 local_phase_speed = phase_speed;
    i32 local_ramp_inc = d2_pitch_ramp;

    for (;;) {
        // Fetch oscillator sample
        i32 sample_idx = osc_pos - a1_phase;
        if (sample_idx < 0) {
            sample_idx = 0;
        }
        sample_idx >>= 15; // asr.l #8 + asr.l #7
        f32 osc_sample = osc_buf[sample_idx];

        // Apply gain and mix with existing sample
        *out = clamp_sample(*out + gain_f * osc_sample);
        out++;

        // Advance position
        osc_pos += local_speed;
        if (osc_pos >= d7_period) {
            osc_pos -= d7_period;

            // Phase oscillation
            a1_phase += local_phase_speed;
            if (a1_phase >= a5_limit) {
                local_phase_speed = -local_phase_speed;
                a1_phase = a5_limit;
            }
            if (a1_phase <= player->wg_osc_speed) {
                local_phase_speed = -local_phase_speed;
                a1_phase = player->wg_osc_speed;
            }
        }

        // Pitch ramp
        if (local_ramp_inc != 0) {
            a2_ramp_acc += local_ramp_inc;
            i32 ramp_adj = a2_ramp_acc >> 10;
            local_speed = d6_step + ramp_adj;

            if (pitch_linear) {
                i32 decay = local_ramp_inc >> 7;
                local_ramp_inc -= decay;
            }

            if ((u32)local_speed >= (u32)d7_period) {
                local_ramp_inc = 0;
                local_speed = 0;
            }
        }

        if (out >= player->wg_curr_samend_ptr) {
            break;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Filter coefficient calculation per 64-byte chunk
// Handles boundary clamping, direction reversal, and normal interpolation.
// Matches the coefficient logic in raspberry_casket.asm:1542-1640

static f32 calc_filter_coeff(i32 flt_pos, i32* flt_speed, i32* next_pos, i32 flt_min, i32 flt_max, const WaveInfo* wi) {
    if (*flt_speed > 0) {
        // Boundary clamp: position past max AND past absolute ceiling
        if (flt_pos > flt_max && flt_pos > 0xFF00 && *next_pos > 0xFEFF) {
            *flt_speed = -*flt_speed;
            *next_pos = 0xFF00;
            return 0.0f;
        }
        // Reached max: position hasn't passed max yet, but next_pos crosses it
        if (flt_pos <= flt_max && *next_pos >= flt_max) {
            if (flt_min == flt_max) {
                *next_pos = flt_min;
            } else {
                *flt_speed = -*flt_speed;
                *next_pos = flt_max;
            }
            return (f32)(u8)(~(u8)wi->flt_max) / 256.0f;
        }
    } else {
        // Boundary clamp: position below min AND next_pos crosses zero
        if (flt_pos < flt_min && flt_pos >= 0 && *next_pos <= 0) {
            *flt_speed = -*flt_speed;
            *next_pos = 0;
            return 255.0f / 256.0f;
        }
        // Reached min: position hasn't passed min yet, but next_pos crosses it
        if (flt_pos >= flt_min && *next_pos <= flt_min) {
            *next_pos = flt_min;
            if (flt_min != flt_max) {
                *flt_speed = -*flt_speed;
            }
            return (f32)(u8)(~(u8)wi->flt_min) / 256.0f;
        }
    }

    // Normal case: coefficient from interpolated position
    return (f32)(u8)(~(u8)(*next_pos >> 8)) / 256.0f;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Filter processing
// Matches raspberry_casket.asm:1542-1767

static void gen_filter(MyPlayer* player, const WaveInfo* wi) {
    u8 flt_type = wi->flt_type;
    if (flt_type == 0 || player->wg_curr_sample_len == 0) {
        return;
    }

    f32 taps[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    i32 flt_pos = (i32)wi->flt_start << 8;
    i32 flt_min = (i32)wi->flt_min << 8;
    i32 flt_max = (i32)wi->flt_max << 8;
    i32 flt_speed = (i32)(i8)wi->flt_speed << 7;

    f32* out = player->wg_curr_sample_ptr;

    while (out < player->wg_curr_samend_ptr) {
        i32 next_pos = flt_pos + flt_speed;
        f32 d2_coeff = calc_filter_coeff(flt_pos, &flt_speed, &next_pos, flt_min, flt_max, wi);

        // Adjust for highpass/notch (types 2 and 4)
        if (!(flt_type & 1)) {
            d2_coeff = 1.0f - d2_coeff;
        }
        f32 d0_coeff = d2_coeff * 2.0f;

        // Resonance
        i16 d7_resonance = wi->flt_resonance;
        if (d7_resonance != 0) {
            i16 res_divisor = (0xB6 / 2 - d7_resonance) * 2;
            if (res_divisor < 0x36) {
                res_divisor = 0x36;
            }
            f32 res_adj = d2_coeff * 256.0f / (f32)res_divisor;
            d0_coeff = d2_coeff + res_adj;
        }

        // Process chunk (64 samples at Amiga rate, scaled for HQ)
        f32* chunk_end = out + (64 * HQ_MAX_PERIOD / AMIGA_MAX_PERIOD);
        if (chunk_end > player->wg_curr_samend_ptr) {
            chunk_end = player->wg_curr_samend_ptr;
        }

        while (out < chunk_end) {
            f32 input = *out;

            f32 d7 = taps[0] - taps[1];
            d7 = d7 * d0_coeff;
            d7 -= taps[0];
            d7 += input;
            d7 = d7 * d2_coeff;
            taps[0] += d7;

            d7 = taps[0] - taps[1];
            d7 = d7 * d2_coeff;
            taps[1] += d7;

            d7 = taps[1] - taps[2];
            d7 = d7 * d2_coeff;
            taps[2] += d7;

            d7 = taps[2] - taps[3];
            d7 = d7 * d2_coeff;
            taps[3] += d7;

            d7 = taps[3];

            // Apply filter type
            switch (flt_type) {
                case FILTER_LOWPASS: // d7 is already taps[3]
                    break;
                case FILTER_HIGHPASS:
                    d7 -= input;
                    break;
                case FILTER_BANDPASS:
                    d7 -= taps[0];
                    d7 -= taps[1];
                    d7 -= taps[2];
                    d7 *= 0.5f;
                    break;
                case FILTER_NOTCH:
                    d7 -= taps[0];
                    d7 = -d7;
                    break;
            }

            *out++ = clamp_sample(d7);
        }

        flt_pos = next_pos;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Modulator (chorus/delay effect)
// Matches pre_Modulator at raspberry_casket.asm:2204-2288

static void gen_modulator(MyPlayer* player, const WaveInfo* wi) {
    if (wi->mod_wetness == 0) {
        return;
    }
    u8 density = wi->mod_density & MOD_DENSITY_MASK;
    if (density == 0) {
        return;
    }

    f32* buf = player->wg_curr_sample_ptr;
    u16 sample_len = player->wg_curr_sample_len;
    f32 wetness_f = (f32)wi->mod_wetness / 256.0f;
    bool is_post = (wi->mod_density & MOD_POST_FLAG) != 0;

    for (u16 run = 0; run < density; run++) {
        // Calculate delay length from modulator ramp table, scaled for HQ rate
        u32 delay_len = (u32)wi->mod_length * s_modulator_ramp_8[run];

        u32 predelay = wi->mod_predelay;
        if (is_post) {
            predelay <<= 8;
        } else {
            delay_len >>= 2;
            predelay <<= 6;
        }
        // Scale delay and predelay by HQ_MAX_PERIOD/AMIGA_MAX_PERIOD for higher sample rate
        delay_len = delay_len * HQ_MAX_PERIOD / AMIGA_MAX_PERIOD;
        predelay = predelay * HQ_MAX_PERIOD / AMIGA_MAX_PERIOD;
        delay_len += predelay;

        u16 d3_acc = 0;

        for (u16 pos = 0; pos < sample_len; pos++) {
            // Scale sweep rate down for HQ: more samples per unit time = less increment per sample
            d3_acc += (u16)((u32)(8 + run) * AMIGA_MAX_PERIOD / HQ_MAX_PERIOD);

            u16 table_idx = (d3_acc >> 11) & 0x1F;
            u32 d1 = s_ramp_up_down_32[table_idx];
            d1 += delay_len;
            d1 >>= 6;

            i16 offset = (i16)pos - (i16)d1;
            if (offset < 0) {
                continue;
            }

            f32 delayed = buf[offset];
            if (run & 1) {
                delayed = -delayed;
            }

            buf[pos] = clamp_sample(buf[pos] + wetness_f * delayed);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Volume envelope - attack phase
// Returns true if processing should continue to the delay phase, false if done.
// Matches raspberry_casket.asm:1786-1870

static bool gen_vol_attack(f32** out_ptr, i16* remaining, const WaveInfo* wi, bool boost, bool vol_fast) {
    u8 attack_val = wi->vol_attack;
    if (attack_val == 0) {
        return true;
    }

    i32 vol = 0;
    i32 vol_inc;
    if (attack_val == 1) {
        vol_inc = 0x00020000;
    } else if (attack_val == 2) {
        vol_inc = 0x00010000;
    } else {
        vol_inc = 0x20000 / attack_val;
    }

    if (vol_fast) {
        vol_inc <<= 4;
    }

    // Scale vol_inc for HQ rate so attack duration in musical time stays the same
    vol_inc = (i32)((int64_t)vol_inc * AMIGA_MAX_PERIOD / HQ_MAX_PERIOD);

    vol += vol_inc;
    if (vol > 0xFFFFFF) {
        return true;
    }

    f32* out = *out_ptr;

    // Attack loop
    while (*remaining >= 0) {
        f32 vol_factor = (f32)(vol >> 16) / (boost ? 64.0f : 256.0f);
        *out = clamp_sample(*out * vol_factor);
        out++;

        (*remaining)--;
        if (*remaining < 0) {
            *out_ptr = out;
            return false;
        }
        vol += vol_inc;
        if (vol > 0xFFFFFF) {
            break;
        }
    }

    *out_ptr = out;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Volume envelope - delay phase
// Returns true if processing should continue to the decay phase, false if done.
// Matches raspberry_casket.asm:1871-1944

static bool gen_vol_delay(f32** out_ptr, i16* remaining, const WaveInfo* wi, bool boost) {
    u16 delay_len = (u16)(((u32)wi->vol_delay << 4) * HQ_MAX_PERIOD / AMIGA_MAX_PERIOD);
    f32* out = *out_ptr;

    if (boost) {
        // Boosted delay: multiply by 4.0 (was two saturating double operations)
        f32* delay_end = out + delay_len + 2;
        for (;;) {
            if (*remaining < 0) {
                *out_ptr = out;
                return false;
            }
            *out = clamp_sample(*out * 4.0f);
            out++;
            if (out >= delay_end) {
                break;
            }
            (*remaining)--;
            if (*remaining < 0) {
                *out_ptr = out;
                return false;
            }
        }
    } else {
        // Normal delay: skip samples (pass through unchanged)
        i16 skip = (i16)(delay_len + 1);
        *remaining -= skip;
        if (*remaining < 0) {
            *out_ptr = out;
            return false;
        }
        out += skip + 1;
    }

    (*remaining)--;

    *out_ptr = out;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Volume envelope - decay phase
// Returns true if processing should continue to the sustain phase, false if done.
// Matches raspberry_casket.asm:1945-2038

static bool gen_vol_decay(f32** out_ptr, i16* remaining, const WaveInfo* wi, bool boost, bool vol_fast) {
    u8 decay_val = wi->vol_decay;
    if (decay_val == 0) {
        return true;
    }

    i32 d7_inc = (i32)decay_val;
    d7_inc = (d7_inc * d7_inc) / 4 + decay_val; // (d3^2)/4 + d3
    // Scale for HQ rate: more samples per unit time = less position advance per sample
    d7_inc = d7_inc * AMIGA_MAX_PERIOD / HQ_MAX_PERIOD;

    i32 d3_pos;
    if (vol_fast) {
        d3_pos = 0;
    } else {
        d3_pos = (i32)decay_val << 12; // lsl.w #8; lsl.l #4
    }

    u16 table_idx = (u16)(d3_pos >> 16);
    const i16* upper_bound = &s_roll_off_table[table_idx];
    i16 lower_bound = *upper_bound++;

    i32 vol_dec = 0;
    u16 volume = 0xFFFF;

    f32* out = *out_ptr;

    while (*remaining >= 0) {
        d3_pos += d7_inc;
        u16 new_idx = (u16)(d3_pos >> 16);

        if (new_idx <= 0x8E) {
            if (new_idx > table_idx) {
                lower_bound = s_roll_off_table[new_idx];
                upper_bound = &s_roll_off_table[1 + new_idx];
                table_idx = new_idx;
            }

            i16 ub_val = *upper_bound;
            i16 delta = ub_val - lower_bound;
            vol_dec = (i32)lower_bound;
            if (delta != 0) {
                u16 frac = (u16)(d3_pos & 0xFFFF) >> 8;
                vol_dec = (i32)(((i32)delta * frac) >> 8) + lower_bound;
            }
        }

        if (volume <= (u16)vol_dec) {
            break;
        }
        volume -= (u16)vol_dec;

        u8 vol8 = (u8)(volume >> 8);
        if (vol8 <= wi->vol_sustain) {
            break;
        }

        f32 vol_factor = (f32)vol8 / (boost ? 64.0f : 256.0f);
        *out = clamp_sample(*out * vol_factor);
        out++;
        (*remaining)--;
    }

    *out_ptr = out;
    return *remaining >= 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Volume envelope - sustain phase
// Matches raspberry_casket.asm:2039-2086

static void gen_vol_sustain(f32* out, i16 remaining, const WaveInfo* wi, bool boost) {
    u8 sustain = wi->vol_sustain;
    if (sustain == 0) {
        while (remaining >= 0) {
            *out++ = 0.0f;
            remaining--;
        }
        return;
    }

    f32 sustain_f = (f32)sustain / (boost ? 64.0f : 256.0f);

    // Skip if sustain is effectively 1.0 (no scaling needed)
    if (!boost && sustain == 0xFF) {
        return;
    }

    while (remaining >= 0) {
        *out = clamp_sample(*out * sustain_f);
        out++;
        remaining--;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Volume envelope
// Matches raspberry_casket.asm:1786-2086

static void gen_volume_envelope(MyPlayer* player, const WaveInfo* wi) {
    f32* out = player->wg_curr_sample_ptr;
    i16 remaining = (i16)player->wg_curr_sample_len;
    if (remaining == 0) {
        return;
    }
    remaining--;

    bool boost = (wi->flags & WI_FLAG_BOOST) != 0;
    bool vol_fast = (wi->flags & WI_FLAG_VOL_FAST) != 0;

    if (wi->vol_attack == 0 && wi->vol_sustain == 0xFF) {
        return; // no envelope needed
    }

    if (!gen_vol_attack(&out, &remaining, wi, boost, vol_fast))
        return;

    if (!gen_vol_delay(&out, &remaining, wi, boost))
        return;

    if (!gen_vol_decay(&out, &remaining, wi, boost, vol_fast))
        return;

    gen_vol_sustain(out, remaining, wi, boost);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Generate a single chord tone: compute octave, select oscillator, set up pitch ramp and phase, then render.
// Matches the per-note body of the chord loop in raspberry_casket.asm:1115-1195

static void gen_chord_tone(MyPlayer* player, const WaveInfo* wi, i16 note, bool* was_tonal) {
    // Split into octave and note-within-octave
    i16 adjusted = note + NOTES_IN_OCTAVE * NOTES_IN_OCTAVE;
    i16 octave = adjusted / NOTES_IN_OCTAVE - NOTES_IN_OCTAVE;
    i16 note_in_oct = adjusted % NOTES_IN_OCTAVE;

    i16 saved_note = note;

    OscNoteBuffers* onb = &player->osc_buffers[note_in_oct];
    u8 osc_type = wi->flags & WI_FLAG_OSC_TYPE_MASK;
    f32* osc_buf = NULL;
    if (osc_type == OSC_TYPE_SAWTOOTH) {
        osc_buf = onb->saw_waves;
    } else if (osc_type == OSC_TYPE_TRIANGLE) {
        osc_buf = onb->tri_waves;
    } else if (osc_type == OSC_TYPE_SQUARE) {
        osc_buf = onb->sqr_waves;
    }

    // d6 = 0x8000 shifted by octave
    i32 d6 = 0x8000;
    if (octave > 0) {
        d6 <<= octave;
    } else if (octave < 0) {
        d6 >>= (-octave);
    }

    // Pitch ramp
    i32 pitch_ramp_val = (i32)(i8)wi->pitch_ramp;
    bool pitch_linear = (wi->flags & WI_FLAG_PITCH_LINEAR) != 0;

    if (!pitch_linear) {
        if (pitch_ramp_val > 0) {
            pitch_ramp_val = pitch_ramp_val * pitch_ramp_val;
        }
    } else {
        if (pitch_ramp_val <= 0) {
            pitch_ramp_val <<= octave;
            pitch_ramp_val += pitch_ramp_val;
        } else {
            pitch_ramp_val = pitch_ramp_val * pitch_ramp_val;
        }
    }
    i32 d2_ramp = pitch_ramp_val << 10;

    i32 d7_period = (i32)onb->wave_length << 15;
    i16 phase_scale = (15 - octave) * 8;

    i32 phase_min = (i32)wi->osc_phase_min * phase_scale;
    phase_min <<= 6;
    i32 phase_max = (i32)wi->osc_phase_max * phase_scale;
    phase_max <<= 6;

    i32 phase_speed = (i32)wi->osc_phase_spd << 11;
    i32 a5_limit = phase_max;

    if (phase_max < phase_min) {
        phase_speed = -phase_speed;
        a5_limit = phase_min;
    }

    i32 osc_start = (phase_max >= phase_min) ? phase_min : phase_max;
    player->wg_osc_speed = osc_start;

    if (osc_buf == NULL) {
        gen_noise(player, wi, octave, saved_note, d2_ramp, pitch_linear);
    } else {
        *was_tonal = true;
        gen_tonal(player, wi, osc_buf, octave, d6, d7_period, phase_min, phase_max, a5_limit, phase_speed, d2_ramp,
                  pitch_linear);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pre_player_init: Initialize player and calculate samples
// Matches pre_PlayerInit at raspberry_casket.asm:927-2197

void pre_player_init(MyPlayer* player, f32* sample_buffer, MySong* song) {
    memset(player, 0, sizeof(MyPlayer));

    player->my_song = song;
    player->sample_buffer_ptr = sample_buffer;

    // Empty sample (2 floats of silence)
    sample_buffer[0] = 0.0f;
    sample_buffer[1] = 0.0f;
    f32* ptr = sample_buffer + 2;

    // Set up wave sample start pointers
    for (int i = 0; i < song->num_waves; i++) {
        player->wave_sample_table[i] = ptr;
        ptr += song->wavetotal_table[i];
    }

    // Generate interpolated period table (16 fine steps between each note)
    {
        u16* dst = player->period_table;
        for (int i = 0; i < 3 * NOTES_IN_OCTAVE; i++) {
            i32 d0 = (i32)s_period_table[i] << 16;
            i32 d1 = ((i32)s_period_table[i + 1] - (i32)s_period_table[i]) << 16;
            d1 >>= 4;
            for (int j = 0; j < 16; j++) {
                *dst++ = (u16)((u32)d0 >> 16);
                d0 += d1;
            }
        }
    }

    // Build pattern table
    {
        u32 step_size = (u32)song->num_steps * 3;
        u8* pat = song->patterns_ptr;
        for (int i = 0; i < 255; i++) {
            song->pattern_table[i] = pat;
            pat += step_size;
        }
    }

    // Initialize player state
    player->pat_curr_row = 0x00;
    player->next_pat_row = 0xFF;
    player->next_pat_pos = 0xFF;
    player->pat_speed_even = 0x06;
    player->pat_speed_odd = 0x06;
    player->pat_line_ticks = 0x06;
    player->pat_stopped = 0x01;
    player->songend_detected = 0x00;

    // Initialize channels
    WaveInfo* first_wave = (song->num_waves > 0) ? &song->waveinfo_ptr[0] : NULL;
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        PerChannelData* pcd = &player->channeldata[ch];
        pcd->pat_vol = MAX_VOLUME;
        pcd->track_delay_offset = 0xFF;
        pcd->waveinfo_ptr = first_wave;
        pcd->adsr_phase = ADSR_PHASE_RELEASE;
        pcd->out.sam_ptr_offset = 0;
        pcd->out.length = 2;
        pcd->out.period = 0x7B;
        pcd->channel_num = (u8)ch;
        pcd->channel_mask = (u8)(1 << ch);
    }

    // Generate oscillator waveform buffers
    gen_osc_buffers(player);

    // Wave generation loop
    if (song->num_waves == 0) {
        return;
    }

    for (player->wg_wave_ord_num = 0; player->wg_wave_ord_num < song->num_waves; player->wg_wave_ord_num++) {

        u8 wave_idx = song->wavegen_order_table[player->wg_wave_ord_num];
        f32* wave_buf = player->wave_sample_table[wave_idx];
        player->wg_curr_sample_ptr = wave_buf;

        u32 std_len = song->wavelength_table[wave_idx];
        player->wg_curr_sample_len = (u16)std_len;

        WaveInfo* wi = song->waveinfo_table[wave_idx];

        memset(wave_buf, 0, std_len * sizeof(f32));
        player->wg_curr_samend_ptr = wave_buf + std_len;

        // Read chord information
        u8 cn1 = wi->chord_note1;
        u8 cn2 = wi->chord_note2;
        u8 cn3 = wi->chord_note3;

        // ASM: seq d4; neg.b d4 — gives 1 when NO chords, 0 when chords exist
        u8 has_chord = (cn1 | cn2 | cn3) ? 0 : 1;
        player->wg_chord_flag = has_chord;

        u8 basenote = wi->osc_basenote;
        player->wg_chord_pitches[0] = basenote;
        player->wg_chord_pitches[1] = basenote + cn1;
        player->wg_chord_pitches[2] = basenote + cn2;
        player->wg_chord_pitches[3] = basenote + cn3;

        player->wg_chord_note_num = 0;
        player->wg_unisono_run = 0;

        bool was_tonal = false;

        for (;;) {     // Outer loop: runs chord loop, then restarts once for unisono
            for (;;) { // Chord loop
                u8 chord_idx = player->wg_chord_note_num;
                i16 note = (i16)(i8)player->wg_chord_pitches[chord_idx];

                if (chord_idx == 0 || (u8)note != basenote) {
                    gen_chord_tone(player, wi, note, &was_tonal);
                }

                player->wg_chord_note_num++;
                if (player->wg_chord_note_num >= 4) {
                    break;
                }
            }

            // Unisono check
            u8 unisono = (wi->mod_density >> MOD_UNISONO_SHIFT) & MOD_UNISONO_MASK;
            if (unisono != 0 && was_tonal && !player->wg_unisono_run) {
                // ASM: move.w #$0001,pv_wg_chord_note_num_b(a4) — word write sets
                // chord_note_num=0 (high byte) and unisono_run=1 (low byte) simultaneously
                player->wg_chord_note_num = 0;
                player->wg_unisono_run = 1;
                continue; // Restart chord loop for unisono pass
            }
            break;
        }

        // Filter
        gen_filter(player, wi);

        // Pre-modulator
        if (!(wi->mod_density & MOD_POST_FLAG)) {
            gen_modulator(player, wi);
        }

        // Volume envelope
        gen_volume_envelope(player, wi);

        // Post-modulator
        if (wi->mod_density & MOD_POST_FLAG) {
            gen_modulator(player, wi);
        }

        // Wave mixing
        MySong* sv = player->my_song;
        if (wi->mix_wave != 0) {
            u8 mix_idx = wi->mix_wave - 1;
            f32* mix_src = player->wave_sample_table[mix_idx];
            u32 mix_len = sv->wavelength_table[mix_idx];
            u16 curr_len = player->wg_curr_sample_len;
            u16 min_len = (curr_len < (u16)mix_len) ? curr_len : (u16)mix_len;

            f32* dst = player->wg_curr_sample_ptr;
            for (u16 j = 0; j < min_len; j++) {
                dst[j] = clamp_sample(dst[j] + mix_src[j]);
            }
        }

        // Higher octaves (2:1 downsample with averaging)
        if (wi->flags & WI_FLAG_EXTRA_OCTAVES) {
            u32 base_len = ((u32)wi->sam_len + 1) * HQ_MAX_PERIOD;
            f32* src = player->wg_curr_sample_ptr;
            f32* oct_dst = src + base_len;
            u32 oct_len = (base_len * 7) / 8;
            for (u32 j = 0; j < oct_len; j++) {
                oct_dst[j] = (src[j * 2] + src[j * 2 + 1]) * 0.5f;
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Octave select tables (asm:4181-4211)

static const u8 s_octave_note_offset_table[] = {
    1 * NOTES_IN_OCTAVE * 4, 1 * NOTES_IN_OCTAVE * 4, 1 * NOTES_IN_OCTAVE * 4, 2 * NOTES_IN_OCTAVE * 4,
    2 * NOTES_IN_OCTAVE * 4, 2 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4,
    3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4,
    3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4,
    3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4, 3 * NOTES_IN_OCTAVE * 4,
};

static const u8 s_octave_select_table[] = {
    1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
};

_Static_assert(sizeof(s_octave_note_offset_table) >= sizeof(s_octave_select_table),
               "octave_note_offset_table must cover same range as octave_select_table");

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Amiga clock constant: PAL system clock / 2
// Period to frequency: freq = AMIGA_CLOCK / period

#define AMIGA_CLOCK 3546895.0

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Scale an Amiga-rate sample offset/length to HQ rate

static inline u16 scale_offset(u16 amiga_val) {
    return (u16)((u32)amiga_val * HQ_MAX_PERIOD / AMIGA_MAX_PERIOD);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Clear per-channel instrument state when triggering a new or 2nd instrument.
// When preserve_port_pitch is true, inst_curr_port_pitch is kept (2nd instrument trigger).

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Trigger ADSR release phase: compute vol64, set phase speed from release parameter.

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process pattern effect commands (asm:2790-2925)

static void process_pattern_effects(PerChannelData* pcd, u8 effect_cmd, u8 effect_data, MyPlayer* player) {
    switch (effect_cmd) {
        case PAT_CMD_SLIDE_UP:
            pcd->pat_pitch_slide = (i16)effect_data;
            break;
        case PAT_CMD_SLIDE_DOWN:
            pcd->pat_pitch_slide = -(i16)effect_data;
            break;
        case PAT_CMD_SET_VIBRATO: { // asm:2836-2849
            pcd->vibrato_pos = 0;
            pcd->vibrato_delay = 1;
            u8 depth_idx = effect_data & 0x0F;
            u8 speed_idx = effect_data >> 4;
            pcd->vibrato_depth = s_vib_depth_table[depth_idx];
            i16 spd = (i16)s_vib_speed_table[speed_idx];
            spd = (i16)((spd * (i16)pcd->vibrato_depth) >> 4);
            pcd->vibrato_speed = (u16)spd;
            break;
        }
        case PAT_CMD_TRACK_DELAY: { // asm:2852-2895
            if (pcd->channel_num >= NUM_CHANNELS - 1) {
                break;
            }
            // Clear next channel's delay buffer volumes
            PerChannelData* next_pcd = &player->channeldata[pcd->channel_num + 1];
            for (int i = 0; i < MAX_TRACK_DELAY; i++) {
                next_pcd->track_delay_buffer[i].volume = 0;
            }
            if (effect_data == 0) {
                pcd->track_delay_steps = 0xFF; // clear signal
            } else {
                u8 steps = (effect_data & 0x0F) * 2;
                pcd->track_delay_steps = steps;
                pcd->track_delay_vol16 = effect_data >> 4;
            }
            break;
        }
        case PAT_CMD_SET_WAVE_OFFSET:
            pcd->wave_offset = effect_data;
            break;
        case PAT_CMD_VOLUME_RAMP: { // asm:2869-2884
            if (effect_data == 0) {
                break;
            }
            u8 down = effect_data & 0x0F;
            if (down != 0) {
                pcd->pat_vol_ramp_speed = -(i8)down;
            } else {
                pcd->pat_vol_ramp_speed = (i8)(effect_data >> 4);
            }
            break;
        }
        case PAT_CMD_POSITION_JUMP:
            player->next_pat_pos = effect_data;
            break;
        case PAT_CMD_SET_VOLUME: { // asm:2920-2925
            u8 vol = effect_data;
            if (vol > MAX_VOLUME) {
                vol = MAX_VOLUME;
            }
            pcd->pat_vol = vol;
            break;
        }
        case PAT_CMD_PATTERN_BREAK:
            player->next_pat_row = effect_data;
            break;
        case PAT_CMD_SET_SPEED: { // asm:2810-2833
            if (effect_data < MAX_SPEED) {
                player->pat_speed_even = effect_data;
                player->pat_speed_odd = effect_data;
                player->pat_line_ticks = effect_data;
                player->pat_stopped = (effect_data != 0) ? 1 : 0;
                if (effect_data == 0) {
                    player->songend_detected = 1;
                }
            } else {
                // Shuffle speed
                u8 even_spd = effect_data >> 4;
                u8 odd_spd = effect_data & 0x0F;
                player->pat_speed_even = even_spd;
                player->pat_speed_odd = odd_spd;
                u8 spd = (player->pat_curr_row & 1) ? odd_spd : even_spd;
                player->pat_line_ticks = spd;
            }
            break;
        }
        default:
            break;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Trigger ADSR release phase: compute vol64, set phase speed from release parameter.

static void trigger_adsr_release(PerChannelData* pcd) {
    i16 vol64 = (i16)pcd->adsr_volume >> 6;
    pcd->adsr_vol64 = (u16)vol64;
    pcd->adsr_pos = 16;
    i16 phase_speed = 16 - vol64;
    phase_speed >>= 1;
    phase_speed += pcd->adsr_release;
    pcd->adsr_phase_speed = (u8)phase_speed;
    pcd->adsr_phase = ADSR_PHASE_RELEASE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Clear per-channel instrument state when triggering a new or 2nd instrument.
// When preserve_port_pitch is true, inst_curr_port_pitch is kept (2nd instrument trigger).

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process instrument pattern steps (asm:3077-3401)
// Reads instrument pattern commands (wave select, slides, jumps, etc.) and advances step position.
// Handles stitching (chaining steps within a single tick) via a loop.

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process subloop / ping-pong and wave offset handling (asm:3596-3721)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process pitch: vibrato, octave selection, trigger detection, period lookup (asm:3723-3874)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process track delay handling (asm:3878-3977)
// Copies current channel output into delay buffer, loads delayed data to shadow channel.
// Advances *ch to skip the shadow channel when active.
// Returns true if the outer for-loop should break (delay on last channel pair).

static bool process_track_delay(PerChannelData* pcd, MyPlayer* player, int* ch) {
    if (pcd->track_delay_steps == 0) {
        return false; // no delay active
    }
    if (pcd->channel_num >= NUM_CHANNELS - 1) {
        return true; // last channel: break out of loop
    }

    u8 delayed_offset = MAX_TRACK_DELAY - 1;  // asm:3905 — default read from last buffer slot

    if (pcd->track_delay_steps == 0xFF) {
        // Clear track delay (asm:3949-3954)
        pcd->track_delay_steps = 0;
        PerChannelData* next_ch = &player->channeldata[*ch + 1];
        next_ch->pat_vol = 0;
        next_ch->track_delay_steps = 0;
        next_ch->track_delay_offset = 0xFF;
    } else {
        // Normal track delay (asm:3891-3947)
        PerChannelData* next_ch = &player->channeldata[*ch + 1];
        u8 offset = next_ch->track_delay_offset;
        offset = (offset + 1) & (MAX_TRACK_DELAY - 1);
        next_ch->track_delay_offset = offset;

        // Copy current output to delay buffer
        OutputChannelData* dst_buf = &next_ch->track_delay_buffer[offset];
        dst_buf->sam_ptr_offset = pcd->out.sam_ptr_offset;
        dst_buf->length = pcd->out.length;
        dst_buf->loop_offset = pcd->out.loop_offset;
        dst_buf->period = pcd->out.period;
        dst_buf->volume = pcd->out.volume;
        dst_buf->trigger = pcd->out.trigger;

        // Adjust trigger for next channel (asm:3931-3938)
        // ASM: when trigger is set (d2!=0 after shift), always OR into trigger_mask.
        // The next_ch->track_delay_steps check only applies to the d2==0 path (no-op).
        u8 trg = pcd->out.trigger << 1;
        if (trg != 0) {
            player->trigger_mask |= trg;
        }

        // Apply track delay volume (asm:3926-3933)
        u16 vol = (u16)dst_buf->volume * (u16)pcd->track_delay_vol16;
        vol >>= 4;
        dst_buf->volume = (u8)vol;

        next_ch->track_delay_steps = pcd->track_delay_steps;

        // Read delayed data (asm:3937-3963)
        delayed_offset = (offset - pcd->track_delay_steps) & (MAX_TRACK_DELAY - 1);
    }

    // Load track data from delay buffer into shadow channel
    (*ch)++;
    PerChannelData* target = &player->channeldata[*ch];
    OutputChannelData* src_buf = &target->track_delay_buffer[delayed_offset];
    target->out.sam_ptr_offset = src_buf->sam_ptr_offset;
    target->out.length = src_buf->length;
    target->out.loop_offset = src_buf->loop_offset;
    target->out.period = src_buf->period;
    target->out.volume = src_buf->volume;
    target->out.trigger = src_buf->trigger;

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process pitch: vibrato, octave selection, trigger detection, period lookup (asm:3723-3874)

static void process_pitch(PerChannelData* pcd, MyPlayer* player) {
    WaveInfo* wi = pcd->waveinfo_ptr;

    i16 d0_pitch = pcd->inst_pitch - 0x10;
    if (!pcd->inst_pitch_pinned) {
        d0_pitch += pcd->inst_sel_arp_note;
        d0_pitch += pcd->inst_curr_port_pitch;
        d0_pitch -= 0x10;
    }
    d0_pitch += pcd->inst_note_pitch;

    // Vibrato (asm:3738-3764)
    u8 vib_delay_lo = (u8)(pcd->vibrato_delay & 0xFF);
    if (vib_delay_lo == 0) {
        // Vibrato active
        i16 vib_speed = (i16)pcd->vibrato_speed;
        if (vib_speed != 0) {
            i16 vib_depth = (i16)pcd->vibrato_depth;
            i16 vib_pos = (i16)pcd->vibrato_pos + vib_speed;
            if (vib_pos > vib_depth || vib_pos < -vib_depth) {
                vib_speed = -vib_speed;
                pcd->vibrato_speed = (u16)vib_speed;
                if (vib_pos > vib_depth) {
                    vib_pos = vib_depth;
                } else {
                    vib_pos = -vib_depth;
                }
            }
            pcd->vibrato_pos = (u16)vib_pos;
            d0_pitch += vib_pos >> 3;
        }
    } else {
        vib_delay_lo--;
        pcd->vibrato_delay = (pcd->vibrato_delay & 0xFF00) | vib_delay_lo;
    }

    // Octave selection for high pitches (asm:3770-3847)
    u16 d3_len = pcd->out.length;
    i16 d6_clamp_pitch = d0_pitch;

    if (d0_pitch > 0x219) {
        d6_clamp_pitch = 0x231;
        if (wi->flags & WI_FLAG_EXTRA_OCTAVES) {
            u16 chipram = scale_offset(read_be16((const u8*)&wi->chipram));
            u16 d5_idx = (u16)(d0_pitch - 0x219) >> 6;
            if (d5_idx >= sizeof(s_octave_select_table)) {
                d5_idx = sizeof(s_octave_select_table) - 1;
            }
            u8 shift = s_octave_select_table[d5_idx];

            u16 lof = pcd->out.loop_offset;
            if (lof != 0xFFFF) {
                // Has loop offset - shift it (asm:3789-3793)
                lof >>= shift;
                pcd->out.loop_offset = lof;
                d3_len >>= shift;
                pcd->out.length = d3_len;
            } else {
                // No loop - retrigger handling (asm:3797-3825)
                if (pcd->out.trigger && pcd->inst_wave_num != 0xFFFF) {
                    u16 wave_num = pcd->inst_wave_num;
                    f32* wave_base = player->wave_sample_table[wave_num >> 2];
                    u32 curr_off = pcd->out.sam_ptr_offset - (u32)(wave_base - player->sample_buffer_ptr);
                    u16 d6_total = d3_len + (u16)curr_off;
                    u16 d7_remain = chipram - d6_total;
                    if (d3_len <= d7_remain) {
                        d3_len = d3_len + d6_total - chipram;
                        d3_len >>= shift;
                    } else {
                        d3_len = 2;
                    }
                    pcd->out.length = d3_len;
                    curr_off >>= shift;
                    pcd->out.sam_ptr_offset = (u32)(wave_base - player->sample_buffer_ptr) + curr_off;
                }
            }

            // Add octave offset to sample pointer (asm:3828-3836)
            // Non-triggered one-shots jump to .no_retrigger_new (asm:3798), skipping octave offset
            if (shift >= 1 && (lof != 0xFFFF || pcd->out.trigger)) {
                u32 oct_offset = 0;
                u32 sam_size = chipram;
                for (u8 i = 0; i < shift; i++) {
                    oct_offset += sam_size;
                    sam_size >>= 1;
                }
                pcd->out.sam_ptr_offset += oct_offset;
            }

            // Subtract octave note offset (asm:3840-3844)
            u8 note_off = s_octave_note_offset_table[d5_idx];
            d0_pitch -= (i16)((u16)note_off << 2);

            if (d0_pitch > 0x231) {
                d0_pitch = 0x231;
            }

            if (d0_pitch < 0) {
                d0_pitch = 0;
            }
            d6_clamp_pitch = d0_pitch;
        }
        // else: no extra octaves, d6_clamp_pitch stays 0x231
    } else {
        if (d0_pitch < 0) {
            d0_pitch = 0;
        }
        d6_clamp_pitch = d0_pitch;
    }

    // Trigger handling (asm:3855-3871)
    if (pcd->out.trigger) {
        u16 lof = pcd->out.loop_offset;
        if (lof != 0xFFFF) {
            pcd->out.sam_ptr_offset += lof;
            pcd->out.loop_offset = 0;
        }
    }
    if (d3_len != pcd->last_trigger_pos) {
        pcd->last_trigger_pos = d3_len;
        pcd->out.trigger = pcd->channel_mask;
        player->trigger_mask |= pcd->channel_mask;
    }

    // Period table lookup (asm:3873-3874)
    u16 pitch_idx = (u16)d6_clamp_pitch;
    if (pitch_idx >= 16 * NOTES_IN_OCTAVE * 3) {
        pitch_idx = 16 * NOTES_IN_OCTAVE * 3 - 1;
    }
    pcd->out.period = player->period_table[pitch_idx];
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process subloop / ping-pong and wave offset handling (asm:3596-3721)

static void process_subloop(PerChannelData* pcd, MyPlayer* player) {
    WaveInfo* wi = pcd->waveinfo_ptr;
    u16 subloop_len = scale_offset(read_be16((const u8*)&wi->subloop_len));

    if (subloop_len != 0) {
        pcd->out.length = subloop_len;
        u16 subloop_step = scale_offset(read_be16((const u8*)&wi->subloop_step));
        u16 d1_offset;

        if (pcd->wave_offset != 0 && wi->allow_9xx) {
            // Wave offset with subloop (asm:3607-3619)
            d1_offset = scale_offset((u16)pcd->wave_offset << 7);
            pcd->wave_offset = 0;
            if ((i8)pcd->inst_ping_pong_dir >= 0) {
                d1_offset -= subloop_step;
            } else {
                d1_offset += subloop_step;
            }
            // Reset wait
            pcd->inst_subloop_wait = (u16)wi->subloop_wait;
        } else {
            // Auto subloop movement (asm:3621-3677)
            pcd->inst_subloop_wait--;
            if ((i16)pcd->inst_subloop_wait > 0) {
                // Still waiting - set loop offset only
                pcd->out.loop_offset = pcd->inst_loop_offset;
            } else {
                d1_offset = pcd->inst_loop_offset;
                pcd->inst_subloop_wait = (u16)wi->subloop_wait;

                if ((i8)pcd->inst_ping_pong_dir < 0) {
                    // Moving forward
                    d1_offset += subloop_step;
                    u16 next_end = d1_offset + subloop_len;
                    u16 loop_end = scale_offset(read_be16((const u8*)&wi->loop_end));
                    u16 chipram = scale_offset(read_be16((const u8*)&wi->chipram));
                    u16 boundary = (d1_offset <= loop_end) ? loop_end : chipram;
                    i16 space = (i16)boundary - (i16)next_end;
                    if (space <= 0) {
                        d1_offset += (u16)space;
                        pcd->inst_ping_pong_dir = 0; // going backwards
                        if (space == 0) {
                            pcd->inst_subloop_wait--;
                        }
                    }
                } else {
                    // Moving backward
                    d1_offset -= subloop_step;
                    u16 loop_start = scale_offset(read_be16((const u8*)&wi->loop_start));
                    i16 diff = (i16)loop_start - (i16)d1_offset;
                    if (diff >= 0) {
                        d1_offset = loop_start;
                        pcd->inst_ping_pong_dir = 0xFF; // going forward
                        if (diff == 0) {
                            pcd->inst_subloop_wait--;
                        }
                    }
                }

                pcd->inst_loop_offset = d1_offset;
                pcd->out.loop_offset = d1_offset;
            }
        }

        // Set sample pointer (asm:3684-3685)
        u16 wave_num = pcd->inst_wave_num;
        if (wave_num != 0xFFFF) {
            pcd->out.sam_ptr_offset = (u32)(player->wave_sample_table[wave_num >> 2] - player->sample_buffer_ptr);
        }
    } else {
        // No subloop - wave offset handling (asm:3688-3721)
        if (pcd->wave_offset != 0 && wi->allow_9xx) {
            u16 d1_off = scale_offset(((u16)pcd->wave_offset << 8) >> 1);
            pcd->wave_offset = 0;

            pcd->out.trigger = pcd->channel_mask;
            player->trigger_mask |= pcd->channel_mask;

            u16 chipram = scale_offset(read_be16((const u8*)&wi->chipram));
            i16 remaining = (i16)chipram - (i16)d1_off;
            if (remaining <= 0) {
                remaining = 2;
                d1_off = 0;
            }
            pcd->out.length = (u16)remaining;

            u16 wave_num = pcd->inst_wave_num;
            if (wave_num != 0xFFFF) {
                f32* base = player->wave_sample_table[wave_num >> 2];
                pcd->out.sam_ptr_offset = (u32)((base + d1_off) - player->sample_buffer_ptr);
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process instrument pattern steps (asm:3077-3401)
// Reads instrument pattern commands (wave select, slides, jumps, etc.) and advances step position.
// Handles stitching (chaining steps within a single tick) via a loop.

static void process_inst_pattern_steps(PerChannelData* pcd, MySong* song, MyPlayer* player) {
    pcd->inst_pitch_slide = 0;
    pcd->inst_vol_slide = 0;

    u8 step_pos = pcd->inst_step_pos;
    u8 d2_stitched;

    // Past end check (asm:3086-3090)
    if (step_pos >= pcd->inst_pattern_steps) {
        d2_stitched = 0xFF;
        pcd->inst_line_ticks = (u8)(d2_stitched + pcd->inst_speed_stop);
        pcd->inst_step_pos = step_pos;
        return;
    }

    u8 d7_jumped = 0;
    u8 d3_stitched = 0;
    u8* a0 = song->inst_patterns_table[(pcd->inst_num4 >> 2) - 1] + (u32)step_pos * 3;

    for (;;) {
        u8 pitch_byte = *a0++;
        u8 d6_is_stitched_raw = pitch_byte;
        d2_stitched = (pitch_byte & 0x80) ? 1 : 0;

        // Load pitch (asm:3131-3142)
        if (!d3_stitched) {
            u8 note = pitch_byte & 0x3F;
            if (note != 0) {
                note--;
                pcd->inst_note_pitch = (i16)((u16)note << 4);
                pcd->inst_pitch_pinned = (d6_is_stitched_raw & 0x40) ? 0xFF : 0;
            }
        }

        // Read command (asm:3144-3150)
        u8 cmd = *a0++ & 0x0F;
        u8 cmd_data = *a0++;
        bool do_fetch_next = false;

        // Instrument command jump table (asm:3152-3168)
        switch (cmd) {
            case INST_CMD_SELECT_WAVE:
            case INST_CMD_SELECT_WAVE_NOSYNC: {
                // inst_select_wave (asm:3175-3256)
                u16 wave_idx = (u16)(cmd_data - 1);
                if (wave_idx >= MAX_WAVES) {
                    break;
                }
                u16 wave_num4 = wave_idx << 2;
                if (wave_num4 == pcd->inst_wave_num) {
                    break;
                }
                pcd->inst_wave_num = wave_num4;
                WaveInfo* wi = song->waveinfo_table[wave_idx];
                pcd->waveinfo_ptr = wi;

                pcd->out.trigger = pcd->channel_mask;
                player->trigger_mask |= pcd->channel_mask;

                f32* wave_ptr = player->wave_sample_table[wave_idx];
                // In the asm, d6 is overwritten with (cmd_number * 2) by lines 3144-3146
                // before reaching .inst_select_wave. tst.w d6 at line 3192 checks
                // cmd 0x00 (d6=0, sync) vs cmd 0x04 (d6=8, nosync).
                bool nosync = (cmd != INST_CMD_SELECT_WAVE);

                if (!nosync) {
                    // Sync path (asm:3195-3222)
                    u16 loop_off = scale_offset(read_be16((const u8*)&wi->loop_offset));
                    u16 subloop = scale_offset(read_be16((const u8*)&wi->subloop_len));
                    if (subloop == 0) {
                        wave_ptr += loop_off;
                        u16 chipram = scale_offset(read_be16((const u8*)&wi->chipram));
                        u16 len = chipram - loop_off;
                        if (len <= 1) {
                            len = 2;
                        }
                        pcd->out.length = len;
                        pcd->out.loop_offset = 0xFFFF;
                    } else {
                        pcd->out.loop_offset = 0;
                    }
                    pcd->out.sam_ptr_offset = (u32)(wave_ptr - player->sample_buffer_ptr);

                    pcd->inst_ping_pong_dir = 0xFF; // st = set to $FF
                    pcd->inst_subloop_wait = (u16)wi->subloop_wait + 1;
                    pcd->inst_loop_offset = scale_offset(read_be16((const u8*)&wi->loop_offset));
                } else {
                    // No-sync path (asm:3225-3256)
                    u16 chipram = scale_offset(read_be16((const u8*)&wi->chipram));
                    u16 subloop = scale_offset(read_be16((const u8*)&wi->subloop_len));
                    if (subloop == 0) {
                        u16 loop_off = scale_offset(read_be16((const u8*)&wi->loop_offset));
                        wave_ptr += loop_off;
                        u16 len = chipram - loop_off;
                        if (len <= 1) {
                            len = 2;
                        }
                        pcd->out.length = len;
                        pcd->out.loop_offset = 0xFFFF;
                    } else {
                        pcd->out.loop_offset = 0;
                    }
                    pcd->out.sam_ptr_offset = (u32)(wave_ptr - player->sample_buffer_ptr);

                    u16 prev_loop = pcd->inst_loop_offset;
                    if (chipram < prev_loop) {
                        pcd->inst_ping_pong_dir = 0xFF;
                    }
                    pcd->inst_subloop_wait = 0;
                    u16 step = scale_offset(read_be16((const u8*)&wi->subloop_step));
                    pcd->inst_loop_offset = prev_loop - step;
                }
                break;
            }
            case INST_CMD_SLIDE_UP: // asm:3261-3262
                pcd->inst_pitch_slide = (i16)cmd_data;
                break;
            case INST_CMD_SLIDE_DOWN: // asm:3259
                pcd->inst_pitch_slide = -(i16)cmd_data;
                break;
            case INST_CMD_ADSR: { // asm:3266-3287
                if (cmd_data == ADSR_CMD_RELEASE) {
                    trigger_adsr_release(pcd);
                } else if (cmd_data == ADSR_CMD_RESTART) {
                    pcd->adsr_phase = ADSR_PHASE_ATTACK;
                    pcd->adsr_volume = 0;
                }
                break;
            }
            case INST_CMD_VOLUME_SLIDE: { // asm:3290-3304
                u8 down = cmd_data & 0x0F;
                if (down != 0) {
                    pcd->inst_vol_slide = -(i8)down;
                } else {
                    pcd->inst_vol_slide = (i8)(cmd_data >> 4);
                }
                break;
            }
            case INST_CMD_JUMP_TO_STEP: { // asm:3307-3335
                if (cmd_data >= step_pos) {
                    break; // only backward jumps
                }
                step_pos = cmd_data;
                if (!d7_jumped) {
                    do_fetch_next = true;
                    break;
                }
                // Already jumped once (stitching) - exit immediately
                pcd->inst_line_ticks = pcd->inst_speed_stop;
                pcd->inst_step_pos = step_pos;
                return;
            }
            case INST_CMD_SET_VOLUME: { // asm:3338-3344
                i16 vol = (i16)cmd_data;
                if (vol > MAX_VOLUME) {
                    vol = MAX_VOLUME;
                }
                pcd->inst_vol = (u16)vol;
                break;
            }
            case INST_CMD_USE_PAT_ARP: { // asm:3347-3372
                u8 arp_idx = cmd_data & 3;
                if (arp_idx == 0) {
                    pcd->inst_sel_arp_note = 0;
                    break;
                }
                u8 hi = cmd_data >> 4;
                if (hi == 0) {
                    // Skip empty arp note
                    u8 arp_val = pcd->arp_notes[arp_idx - 1];
                    if (arp_val != 0) {
                        pcd->inst_sel_arp_note = (i16)((u16)arp_val << 4);
                    } else {
                        step_pos++;
                        do_fetch_next = true;
                    }
                    break;
                }
                if (hi != 1) {
                    break; // illegal
                }
                u8 arp_val = pcd->arp_notes[arp_idx - 1];
                pcd->inst_sel_arp_note = (i16)((u16)arp_val << 4);
                break;
            }
            case INST_CMD_SET_SPEED: { // asm:3375-3380
                u8 spd = (u8)cmd_data;
                if (spd == 0) {
                    spd = 0xFF;
                }
                pcd->inst_speed_stop = spd;
                break;
            }
            default:
                break;
        }

        // Normal advance (asm:3384-3393)
        if (!do_fetch_next) {
            step_pos++;
            if (!d2_stitched) {
                // Non-stitched exit (inst_pat_loop_exit2)
                pcd->inst_line_ticks = (u8)(d2_stitched + pcd->inst_speed_stop);
                pcd->inst_step_pos = step_pos;
                return;
            }
        }

        // inst_fetch_next: continue stitching
        d7_jumped = 1;
        d3_stitched = d2_stitched;
        if (step_pos < pcd->inst_pattern_steps) {
            a0 = song->inst_patterns_table[(pcd->inst_num4 >> 2) - 1] + (u32)step_pos * 3;
            continue;
        }
        // Past end (inst_pat_loop_exit)
        d2_stitched = 0xFF;
        pcd->inst_line_ticks = (u8)(d2_stitched + pcd->inst_speed_stop);
        pcd->inst_step_pos = step_pos;
        return;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Clear per-channel instrument state when triggering a new or 2nd instrument.
// When preserve_port_pitch is true, inst_curr_port_pitch is kept (2nd instrument trigger).

static void clear_inst_state(PerChannelData* pcd, bool preserve_port_pitch) {
    pcd->pat_portamento_dest = 0;
    pcd->pat_pitch_slide = 0;
    pcd->pat_vol_ramp_speed = 0;
    pcd->pat_2nd_inst_num4 = 0;
    pcd->pat_2nd_inst_delay = 0;
    pcd->wave_offset = 0;
    pcd->inst_pitch_slide = 0;
    pcd->inst_sel_arp_note = 0;
    pcd->inst_note_pitch = 0;
    if (!preserve_port_pitch) {
        pcd->inst_curr_port_pitch = 0;
    }
    pcd->inst_line_ticks = 0;
    pcd->inst_pitch_pinned = 0;
    pcd->inst_vol_slide = 0;
    pcd->inst_step_pos = 0;
    pcd->inst_wave_num = 0xFFFF;
    pcd->track_delay_offset = 0xFF;
    pcd->inst_speed_stop = 1;
    pcd->inst_pitch = 0x10;
    pcd->inst_vol = MAX_VOLUME;
    pcd->loaded_inst_vol = MAX_VOLUME;
    pcd->pat_vol = MAX_VOLUME;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process pattern data for a single channel (asm:2373-2925)
// Handles ADSR release delay, 2nd instrument trigger, portamento, volume ramp,
// delayed notes, pattern reading, ARP, instrument triggers, and effect commands.

static void process_pattern_channel(PerChannelData* pcd, MySong* song, MyPlayer* player) {
    // Handle ADSR release delay (PARANOIA_MODE, asm:2376-2397)
    if (pcd->pat_adsr_rel_delay > 0) {
        pcd->pat_adsr_rel_delay--;
        if (pcd->pat_adsr_rel_delay == 0) {
            trigger_adsr_release(pcd);
        }
    }

    // Handle 2nd instrument trigger (asm:2400-2437)
    bool triggered_2nd_inst = false;
    if (pcd->pat_2nd_inst_num4 != 0) {
        if (pcd->pat_2nd_inst_delay == 0) {
            // Trigger 2nd instrument
            u8 inst_num4 = pcd->pat_2nd_inst_num4;
            pcd->new_inst_num = inst_num4;
            pcd->inst_num4 = inst_num4;
            UnpackedInstrumentInfo* uii = &song->inst_infos[(inst_num4 >> 2) - 1];
            pcd->inst_info_ptr = uii;
            pcd->inst_pattern_steps = uii->pattern_steps;

            // Clear channel state (asm:2421-2435)
            // Note: inst_curr_port_pitch is PRESERVED (asm:2427 addq.w #2,a1 skips it)
            clear_inst_state(pcd, true);
            triggered_2nd_inst = true;
        } else {
            pcd->pat_2nd_inst_delay--;
        }
    }

    // Handle portamento and volume ramping (skipped if 2nd inst just triggered)
    if (!triggered_2nd_inst) {
        // Handle portamento (asm:2443-2466)
        if (pcd->pat_portamento_dest != 0) {
            i16 curr = pcd->inst_curr_port_pitch;
            i16 dest = pcd->pat_portamento_dest;
            i16 speed = (i16)(u16)pcd->pat_portamento_speed;
            if (curr < dest) {
                curr += speed;
                if (curr > dest) {
                    pcd->pat_portamento_dest = 0;
                    curr = dest;
                }
            } else {
                curr -= speed;
                if (curr < dest) {
                    pcd->pat_portamento_dest = 0;
                    curr = dest;
                }
            }
            pcd->inst_curr_port_pitch = curr;
        }

        // Handle volume ramping (asm:2470-2481)
        if (pcd->pat_vol_ramp_speed != 0) {
            i16 vol = (i16)(i8)pcd->pat_vol_ramp_speed + (i16)pcd->pat_vol;
            if (vol < 0) {
                vol = 0;
            }
            if (vol > MAX_VOLUME) {
                vol = MAX_VOLUME;
            }
            pcd->pat_vol = (u8)vol;
        }
    }

    // Handle delayed note (asm:2487-2503)
    i16 note_delay_val = (i16)(i8)pcd->note_delay;
    if (note_delay_val < 0) {
        return;
    }
    if (note_delay_val > 0) {
        note_delay_val--;
        if (note_delay_val == 0) {
            pcd->note_delay = 0xFF; // release note delay
        } else {
            pcd->note_delay = (u8)note_delay_val;
            return;
        }
    }

    // Read pattern data (asm:2505-2527)
    u16 pos_offset = song->curr_pat_pos * 4 + pcd->channel_num;
    pos_offset *= 2;
    u8* pos_ptr = song->pos_data_adr + pos_offset;

    u8 curr_row = player->pat_curr_row;
    if (curr_row >= song->num_steps) {
        return;
    }

    u8 pat_num = pos_ptr[0]; // ppd_pat_num
    if (pat_num == 0) {
        return;
    }

    u8* pat_data = song->pattern_table[pat_num - 1];
    i16 d0_shift = (i16)(i8)pos_ptr[1]; // ppd_pat_shift (signed)

    // Offset into pattern: row * 3 bytes per row
    pat_data += (u32)curr_row * 3;

    u8 pitch_ctrl = pat_data[0];  // pdb_pitch_ctrl
    u8 inst_effect = pat_data[1]; // pdb_inst_effect
    u8 effect_data = pat_data[2]; // pdb_effect_data

    u8 effect_cmd = inst_effect & 0x0F;
    u8 inst_num = inst_effect >> 4;
    if (pitch_ctrl & PITCH_CTRL_INST_HI) {
        inst_num += 16; // 5-bit instrument number
    }
    u8 pitch = pitch_ctrl & PITCH_CTRL_NOTE_MASK;
    u16 inst_num4 = (u16)inst_num << 2;

    // Handle Exy commands (asm:2548-2578)
    if (effect_cmd == PAT_CMD_EXTENDED) {
        if ((i8)pcd->note_delay >= 0) { // not already running
            u8 sub_val = effect_data & 0x0F;
            u8 sub_cmd = effect_data >> 4;
            if (sub_cmd == PAT_EXT_NOTE_DELAY) {
                pcd->note_delay = sub_val;
                return;
            } else if (sub_cmd == PAT_EXT_NOTE_OFF_DELAY) {
                pcd->note_off_delay = sub_val;
            }
        }
    }
    pcd->note_delay = 0xFF; // mark as processed

    // ARP processing (asm:2583-2668)
    u8 arp_flag = 0;
    u8 alt_inst = 0;          // d3: alternate instrument for 2nd inst trigger
    bool do_arp_port = false; // whether to do ARP/portamento resolution before effects

    if (pitch_ctrl & PITCH_CTRL_HAS_ARP) {
        // Has ARP note
        u8 d3 = effect_cmd | effect_data;
        if (d3 != 0) {
            pcd->arp_notes[0] = effect_cmd;
            pcd->arp_notes[1] = effect_data >> 4;
            pcd->arp_notes[2] = effect_data & 0x0F;
        }
        arp_flag = 1;
        // Assembly clears d2 (effect_cmd) and d3 (alt_inst) at asm:2667-2668
        // to prevent stale effect commands from being processed later
        effect_cmd = 0;
    } else if (effect_cmd == PAT_CMD_PLAY_2ND_INST && effect_data != 0) {
        // 0xx: play second instrument (asm:2612-2668)
        u8 second_inst = effect_data & 0x0F;
        u16 second_inst4 = (u16)second_inst << 2;

        if (pitch == 0) {
            // No pitch - trigger 2nd inst without pitch
            d0_shift += 1;
            d0_shift <<= 4;

            if (second_inst == 0) {
                memset(pcd->arp_notes, 0, 4);
                // d7=pitch=0, d4=inst_num, d3=0 -> clear_portamento path
                pcd->inst_pitch = 0x10;
                pcd->inst_curr_port_pitch = d0_shift;
                pcd->pat_portamento_dest = 0;
                // Skip directly to effect handling (no ARP/portamento resolution)
                pcd->pat_vol_ramp_speed = 0;
                pcd->pat_pitch_slide = 0;
                process_pattern_effects(pcd, effect_cmd, effect_data, player);
                return;
            }

            // Trigger with swapped instruments
            alt_inst = (u8)inst_num4;
            inst_num4 = second_inst4;
            // Trigger new instrument and do ARP/portamento resolution
            pcd->new_inst_num = (u8)inst_num4;
            pcd->inst_num4 = inst_num4;
            {
                u16 idx = inst_num4;
                UnpackedInstrumentInfo* uii = (UnpackedInstrumentInfo*)((u8*)song->inst_infos + idx * 4 - 16);
                pcd->inst_info_ptr = uii;
                pcd->inst_pattern_steps = uii->pattern_steps;
            }
            clear_inst_state(pcd, false);
            do_arp_port = true;
        } else {
            // Has pitch - 2nd inst triggers immediately, original becomes delayed
            // (asm:2638-2641) d3=original inst (for delayed trigger), d4=2nd inst (for immediate trigger)
            alt_inst = (u8)inst_num4;
            inst_num4 = second_inst4;
            // Fall through to no_new_note path below
        }
    } else if (effect_cmd == PAT_CMD_PLAY_2ND_INST) {
        // No effect (cmd=0 with data=0)
        effect_cmd = PAT_CMD_PLAY_2ND_INST;
        alt_inst = 0;
    }

    if (!do_arp_port) {
        // Check for instrument-only change (no pitch) (asm:2592-2605)
        if (inst_num4 != 0 && pitch == 0) {
            // Only change of instrument, not pitch
            pcd->pat_vol = pcd->loaded_inst_vol;
            if (inst_num4 == pcd->inst_num4) {
                // Same instrument - attack!
                pcd->adsr_phase = ADSR_PHASE_ATTACK;
                pcd->adsr_volume = 0;
            }
        }

        // Check for note off (asm:2674-2690)
        if (pitch == NOTE_OFF_PITCH) {
            // Release note
            trigger_adsr_release(pcd);
            // Skip to effect handling
        } else if (pitch != 0) {
            // Has note (asm:2728-2735)
            if (inst_num4 == 0) {
                // Pitch but no instrument
                d0_shift += pitch;
                d0_shift <<= 4;
            } else {
                // Has instrument + pitch (asm:2693-2695)
                d0_shift += pitch;
                d0_shift <<= 4;

                if (effect_cmd != PAT_CMD_TONE_PORTAMENTO) {
                    // Trigger new instrument (asm:2699-2726)
                    pcd->new_inst_num = (u8)inst_num4;
                    pcd->inst_num4 = inst_num4;
                    {
                        u16 idx = inst_num4;
                        UnpackedInstrumentInfo* uii = (UnpackedInstrumentInfo*)((u8*)song->inst_infos + idx * 4 - 16);
                        pcd->inst_info_ptr = uii;
                        pcd->inst_pattern_steps = uii->pattern_steps;
                    }
                    clear_inst_state(pcd, false);
                }
            }
            do_arp_port = true;
        }
        // else: pitch == 0, no note → skip to effects
    }

    // ARP/portamento resolution (asm:2737-2772)
    if (do_arp_port) {
        if (!arp_flag) {
            memset(pcd->arp_notes, 0, 4);
        }
        if (effect_cmd == PAT_CMD_TONE_PORTAMENTO) {
            // Set portamento (asm:2763-2772)
            d0_shift += 0x10;
            pcd->pat_portamento_dest = d0_shift;
            pcd->inst_curr_port_pitch += pcd->inst_pitch;
            pcd->inst_pitch = 0;
            if (effect_data != 0) {
                pcd->pat_portamento_speed = effect_data;
            }
        } else {
            // Clear portamento (asm:2753-2757)
            pcd->inst_pitch = 0x10;
            pcd->inst_curr_port_pitch = d0_shift;
            pcd->pat_portamento_dest = 0;
        }
    }

    // Effect handling (asm:2774-2925)
    // Set 2nd instrument (asm:2774-2779)
    if (alt_inst != 0) {
        pcd->pat_2nd_inst_num4 = alt_inst;
        pcd->pat_2nd_inst_delay = effect_data >> 4;
    }

    // Clear per-row effect state (asm:2782-2783)
    pcd->pat_vol_ramp_speed = 0;
    pcd->pat_pitch_slide = 0;

    if (!arp_flag) {
        process_pattern_effects(pcd, effect_cmd, effect_data, player);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pre_player_tick: Full port of pre_PlayerTick from raspberry_casket.asm:2343
// Called once per 50 Hz VBL tick. Processes pattern data, instrument patterns, ADSR, vibrato,
// and writes per-channel output parameters.

void pre_player_tick(MyPlayer* player) {
    MySong* song = player->my_song;

    if (player->pat_stopped) {
        // ---- Phase 1: Pattern processing for all 4 channels (asm:2373) ----
        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
            PerChannelData* pcd = &player->channeldata[ch];
            process_pattern_channel(pcd, song, player);

            // Track delay channel skip logic (asm:2931-2943)
            if (pcd->track_delay_steps != 0) {
                if (pcd->channel_num >= NUM_CHANNELS - 2) {
                    break; // end pattern processing loop
                }
                ch++; // skip next channel (shadow channel)
            }
        }

        // ---- Phase 2: Pattern advancing (asm:2949-3024) ----
        player->pat_line_ticks--;
        if (player->pat_line_ticks == 0) {
            // Clear note delay for all channels
            for (int i = 0; i < NUM_CHANNELS; i++) {
                player->channeldata[i].note_delay = 0;
            }

            u8 num_steps = song->num_steps;
            u8 curr_row = player->pat_curr_row + 1;
            u16 pat_pos = song->curr_pat_pos;

            // Pattern break handling (asm:2966-2977)
            if ((i8)player->next_pat_row >= 0) {
                curr_row = player->next_pat_row;
                player->next_pat_row = 0xFF;
                if (curr_row >= num_steps) {
                    curr_row = num_steps - 1;
                }
                pat_pos++;
            } else if (curr_row >= num_steps) {
                curr_row = 0;
                pat_pos++;
            }

            // Position jump handling (asm:2991-3006)
            if ((i8)player->next_pat_pos >= 0) {
                u8 new_pos = player->next_pat_pos;
                player->next_pat_pos = 0xFF;
                if (new_pos <= pat_pos) {
                    player->songend_detected = 1;
                }
                pat_pos = new_pos;
                curr_row = 0;
            }

            // Song loop (asm:3009-3015)
            if (pat_pos >= song->pat_pos_len) {
                pat_pos = song->pat_restart_pos;
                player->songend_detected = 1;
            }

            player->pat_curr_row = curr_row;
            song->curr_pat_pos = pat_pos;

            // Speed/shuffle (asm:3019-3024)
            u8 speed = player->pat_speed_even;
            if (curr_row & 1) {
                speed = player->pat_speed_odd;
            }
            player->pat_line_ticks = speed;
        }
    }

    // ---- Phase 3: Instrument pattern processing for all channels (asm:3042-3977) ----
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        PerChannelData* pcd = &player->channeldata[ch];
        UnpackedInstrumentInfo* uii = pcd->inst_info_ptr;

        if (uii != NULL) {
            // Pitch slide accumulation (asm:3052-3062)
            i16 pitch_delta = pcd->inst_pitch_slide + pcd->pat_pitch_slide;
            if (pitch_delta != 0) {
                i16 p = pcd->inst_pitch + pitch_delta;
                if (p > (3 * NOTES_IN_OCTAVE) << 4) {
                    p = (3 * NOTES_IN_OCTAVE) << 4;
                }
                pcd->inst_pitch = p;
            }

            // Volume slide (asm:3064-3074)
            if (pcd->inst_vol_slide != 0) {
                i16 v = (i16)(i8)(pcd->inst_vol & 0xFF) + (i16)pcd->inst_vol_slide;
                if (v < 0) {
                    v = 0;
                }
                if (v > MAX_VOLUME) {
                    v = MAX_VOLUME;
                }
                pcd->inst_vol = (pcd->inst_vol & 0xFF00) | (u16)(u8)v;
            }

            // Instrument pattern step processing (asm:3077-3401)
            if (pcd->inst_line_ticks == 0) {
                process_inst_pattern_steps(pcd, song, player);
            }

            // Wave not yet selected? Default to wave 0 (asm:3404-3451)
            if (pcd->inst_wave_num == 0xFFFF || (pcd->inst_wave_num & 0xFF) >= 0x80) {
                pcd->inst_wave_num = 0;
                WaveInfo* wi = song->waveinfo_ptr;
                pcd->waveinfo_ptr = wi;

                pcd->out.trigger = pcd->channel_mask;
                player->trigger_mask |= pcd->channel_mask;

                u16 subloop = scale_offset(read_be16((const u8*)&wi->subloop_len));
                if (subloop == 0) {
                    u16 loop_off = scale_offset(read_be16((const u8*)&wi->loop_offset));
                    f32* wave_ptr = player->wave_sample_table[0] + loop_off;
                    pcd->out.sam_ptr_offset = (u32)(wave_ptr - player->sample_buffer_ptr);

                    u16 chipram = scale_offset(read_be16((const u8*)&wi->chipram));
                    u16 len;
                    // ASM: subq.w #1,d6; cmp.w d5,d6; ble → signed comparison
                    if ((i16)(chipram - 1) > (i16)loop_off) {
                        len = chipram - loop_off;
                    } else {
                        len = 2;
                    }
                    pcd->out.length = len;
                    pcd->out.loop_offset = 0xFFFF;
                } else {
                    pcd->out.sam_ptr_offset = (u32)(player->wave_sample_table[0] - player->sample_buffer_ptr);
                    pcd->out.loop_offset = 0;
                }

                u16 loop_off_val = scale_offset(read_be16((const u8*)&wi->loop_offset));
                pcd->inst_subloop_wait = (u16)wi->subloop_wait + 1;
                pcd->inst_loop_offset = loop_off_val;
                pcd->inst_ping_pong_dir = 0xFF;
            }

            // Tick down instrument line ticks (asm:3453-3457)
            if (pcd->inst_line_ticks != 0xFF) {
                pcd->inst_line_ticks--;
            }
        }

        // ---- ADSR state machine (asm:3462-3566) ----
        i16 d2_adsr_vol = (i16)pcd->adsr_volume;

        if (pcd->new_inst_num != 0) {
            // Load instrument (asm:3525-3540)
            pcd->loaded_inst_vol = (u8)(i16)pcd->inst_vol;
            pcd->vibrato_delay = uii->vibrato_delay;
            pcd->vibrato_depth = (u16)uii->vibrato_depth;
            pcd->vibrato_speed = (u16)uii->vibrato_speed;
            pcd->adsr_release = uii->adsr_release;

            d2_adsr_vol = 0;
            pcd->adsr_phase = ADSR_PHASE_ATTACK;
            pcd->adsr_volume = 0;
            pcd->new_inst_num = 0;
            pcd->vibrato_pos = 0;
            // Fall through to attack
        }

        switch (pcd->adsr_phase) {
            case ADSR_PHASE_ATTACK: { // asm:3542-3549
                d2_adsr_vol += uii->adsr_attack;
                if (d2_adsr_vol >= MAX_VOLUME << 4) {
                    d2_adsr_vol = MAX_VOLUME << 4;
                    pcd->adsr_phase = ADSR_PHASE_DECAY;
                    pcd->adsr_phase_speed = (u8)(uii->adsr_decay & 0xFF);
                }
                break;
            }
            case ADSR_PHASE_DECAY:     // asm:3489, then shared code at 3489-3523
            case ADSR_PHASE_RELEASE: { // asm:3480-3513
                if (pcd->adsr_phase == ADSR_PHASE_RELEASE) {
                    // Release accumulator (asm:3481-3486)
                    u16 pos = pcd->adsr_pos + pcd->adsr_vol64;
                    pcd->adsr_pos = pos;
                    if (pos < 16) {
                        break; // not ready to decay yet
                    }
                    pcd->adsr_pos = pos - 16;
                }

                // Shared decay/release code (asm:3489-3523)
                u8 phase_spd = pcd->adsr_phase_speed;
                i16 decay_val;
                if (phase_spd < 0x8F) {
                    decay_val = s_roll_off_table[phase_spd];
                    pcd->adsr_phase_speed = phase_spd + 1;
                } else {
                    decay_val = 2;
                    pcd->adsr_phase_speed = 0x8F;
                }

                if (pcd->adsr_phase == ADSR_PHASE_RELEASE) {
                    // Release: clamp to 0
                    d2_adsr_vol -= decay_val;
                    if (d2_adsr_vol < 0) {
                        d2_adsr_vol = 0;
                    }
                } else {
                    // Decay: check sustain
                    d2_adsr_vol -= decay_val;
                    if (d2_adsr_vol <= uii->adsr_sustain) {
                        pcd->adsr_phase = ADSR_PHASE_SUSTAIN;
                        d2_adsr_vol = uii->adsr_sustain;
                    }
                }
                break;
            }
            case ADSR_PHASE_SUSTAIN: // no change
                break;
        }

        pcd->adsr_volume = (u16)d2_adsr_vol;

        // Note off delay (EAx) (asm:3557-3566)
        // Note: assembly only writes to struct, does NOT modify d2 (current tick volume)
        // so the release takes effect on the NEXT tick, not the current one
        if (pcd->note_off_delay != 0) {
            pcd->note_off_delay--;
            if (pcd->note_off_delay == 0) {
                pcd->adsr_volume = 0;
                pcd->adsr_phase = ADSR_PHASE_RELEASE;
            }
        }

        // ---- Volume calculation (asm:3570-3591) ----
        {
            i16 adsr_scaled = d2_adsr_vol >> 4;
            i16 vol = (adsr_scaled * (i16)(u8)(pcd->inst_vol & 0xFF)) >> 6;
            vol = (vol * (i16)pcd->pat_vol) >> 6;
            pcd->out.volume = (u8)vol;
        }

        // ---- Subloop / ping-pong handling (asm:3596-3721) ----
        process_subloop(pcd, player);

        // ---- Pitch handling (asm:3723-3874) ----
        process_pitch(pcd, player);

        // ---- Track delay handling (asm:3878-3977) ----
        if (process_track_delay(pcd, player, &ch)) {
            break;
        }
    }

    // Clear trigger mask for next tick (asm:3999)
    player->trigger_mask = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// BLEP (Band-Limited stEP) anti-aliasing — coefficients from PreTracker.exe at 0x460e2c
// 8 taps x 5 phases, linear interpolation between adjacent phases for sub-sample precision.

static const f32 s_blep_table[8][5] = {
    { 0.996602f, 0.991206f, 0.979689f, 0.957750f, 0.919731f },
    { 0.859311f, 0.770949f, 0.651934f, 0.504528f, 0.337462f },
    { 0.165926f, 0.009528f, -0.111687f, -0.182304f, -0.196163f },
    { -0.158865f, -0.087007f, -0.003989f, 0.066445f, 0.106865f },
    { 0.110733f, 0.083313f, 0.038687f, -0.005893f, -0.036136f },
    { -0.045075f, -0.034260f, -0.011718f, 0.012108f, 0.028606f },
    { 0.033769f, 0.028673f, 0.017904f, 0.006982f, 0.000000f },
    { 0.000000f, 0.000000f, 0.000000f, 0.000000f, 0.000000f },
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// blep_insert: Insert a BLEP correction into a circular buffer

static void blep_insert(f32* buf, i32 read_idx, i32* count, f32 delta, f32 phase) {
    // phase is 0..1, map to table position 0..5
    f32 table_pos = phase * 5.0f;
    i32 idx = (i32)table_pos;
    if (idx >= 4) {
        idx = 4;
        table_pos = 4.0f;
    }
    f32 frac = table_pos - (f32)idx;

    // Always insert starting from current read position so tap[0] aligns with the next drain
    i32 wi = read_idx;
    for (i32 tap = 0; tap < 8; tap++) {
        f32 coef = s_blep_table[tap][idx] + (s_blep_table[tap][idx + 1] - s_blep_table[tap][idx]) * frac;
        buf[wi] += coef * delta;
        wi = (wi + 1) & 7;
    }
    *count = 8;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// blep_drain: Drain one tap from a BLEP circular buffer

static f32 blep_drain(f32* buf, i32* read_idx, i32* count) {
    if (*count <= 0)
        return 0.0f;
    i32 ri = *read_idx;
    f32 val = buf[ri];
    buf[ri] = 0.0f;
    *read_idx = (ri + 1) & 7;
    (*count)--;
    return val;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Windowed sinc interpolation — 8-point Lanczos (a=4), 128 sub-sample phases.
// Produces cleaner output than nearest-neighbor + BLEP when source buffers are high-quality floats.

#define SINC_TAPS 8
#define SINC_PHASES 128

static const f32 s_sinc_table[SINC_PHASES][SINC_TAPS] = {
    {  0.000000000f, -0.000000000f,  0.000000000f,  1.000000000f,  0.000000000f, -0.000000000f,  0.000000000f, -0.000000000f },
    { -0.000772606f,  0.002467182f, -0.006966743f,  0.999891471f,  0.007100224f, -0.002506035f,  0.000790336f, -0.000003829f },
    { -0.001527170f,  0.004894410f, -0.013798614f,  0.999565993f,  0.014332463f, -0.005049778f,  0.001598069f, -0.000015372f },
    { -0.002263400f,  0.007280624f, -0.020494302f,  0.999023800f,  0.021695182f, -0.007630045f,  0.002422846f, -0.000034704f },
    { -0.002981027f,  0.009624809f, -0.027052569f,  0.998265223f,  0.029186772f, -0.010245608f,  0.003264293f, -0.000061893f },
    { -0.003679802f,  0.011925993f, -0.033472260f,  0.997290690f,  0.036805556f, -0.012895201f,  0.004122018f, -0.000096994f },
    { -0.004359494f,  0.014183249f, -0.039752294f,  0.996100722f,  0.044549786f, -0.015577519f,  0.004995606f, -0.000140057f },
    { -0.005019896f,  0.016395694f, -0.045891670f,  0.994695937f,  0.052417647f, -0.018291217f,  0.005884624f, -0.000191119f },
    { -0.005660821f,  0.018562486f, -0.051889464f,  0.993077046f,  0.060407254f, -0.021034909f,  0.006788617f, -0.000250208f },
    { -0.006282102f,  0.020682832f, -0.057744832f,  0.991244851f,  0.068516657f, -0.023807174f,  0.007707113f, -0.000317344f },
    { -0.006883591f,  0.022755979f, -0.063457006f,  0.989200248f,  0.076743837f, -0.026606551f,  0.008639618f, -0.000392534f },
    { -0.007465163f,  0.024781219f, -0.069025296f,  0.986944225f,  0.085086712f, -0.029431542f,  0.009585620f, -0.000475776f },
    { -0.008026709f,  0.026757890f, -0.074449089f,  0.984477860f,  0.093543136f, -0.032280614f,  0.010544587f, -0.000567059f },
    { -0.008568144f,  0.028685370f, -0.079727851f,  0.981802318f,  0.102110896f, -0.035152197f,  0.011515968f, -0.000666361f },
    { -0.009089399f,  0.030563084f, -0.084861123f,  0.978918856f,  0.110787721f, -0.038044684f,  0.012499194f, -0.000773649f },
    { -0.009590427f,  0.032390500f, -0.089848523f,  0.975828814f,  0.119571276f, -0.040956435f,  0.013493676f, -0.000888880f },
    { -0.010071199f,  0.034167127f, -0.094689744f,  0.972533622f,  0.128459165f, -0.043885776f,  0.014498808f, -0.001012002f },
    { -0.010531703f,  0.035892518f, -0.099384557f,  0.969034791f,  0.137448934f, -0.046830999f,  0.015513964f, -0.001142949f },
    { -0.010971948f,  0.037566271f, -0.103932805f,  0.965333919f,  0.146538071f, -0.049790363f,  0.016538503f, -0.001281648f },
    { -0.011391961f,  0.039188023f, -0.108334407f,  0.961432685f,  0.155724007f, -0.052762097f,  0.017571764f, -0.001428013f },
    { -0.011791785f,  0.040757455f, -0.112589355f,  0.957332847f,  0.165004115f, -0.055744396f,  0.018613069f, -0.001581950f },
    { -0.012171483f,  0.042274289f, -0.116697714f,  0.953036246f,  0.174375715f, -0.058735426f,  0.019661726f, -0.001743352f },
    { -0.012531135f,  0.043738289f, -0.120659620f,  0.948544797f,  0.183836073f, -0.061733325f,  0.020717022f, -0.001912102f },
    { -0.012870835f,  0.045149259f, -0.124475283f,  0.943860497f,  0.193382402f, -0.064736199f,  0.021778232f, -0.002088073f },
    { -0.013190698f,  0.046507044f, -0.128144982f,  0.938985414f,  0.203011866f, -0.067742128f,  0.022844612f, -0.002271128f },
    { -0.013490852f,  0.047811529f, -0.131669065f,  0.933921692f,  0.212721575f, -0.070749164f,  0.023915404f, -0.002461119f },
    { -0.013771442f,  0.049062638f, -0.135047950f,  0.928671546f,  0.222508593f, -0.073755335f,  0.024989835f, -0.002657886f },
    { -0.014032629f,  0.050260334f, -0.138282122f,  0.923237264f,  0.232369938f, -0.076758641f,  0.026067117f, -0.002861260f },
    { -0.014274589f,  0.051404619f, -0.141372134f,  0.917621200f,  0.242302577f, -0.079757058f,  0.027146448f, -0.003071063f },
    { -0.014497511f,  0.052495531f, -0.144318604f,  0.911825780f,  0.252303437f, -0.082748540f,  0.028227013f, -0.003287105f },
    { -0.014701602f,  0.053533148f, -0.147122214f,  0.905853491f,  0.262369398f, -0.085731016f,  0.029307981f, -0.003509186f },
    { -0.014887078f,  0.054517583f, -0.149783713f,  0.899706889f,  0.272497300f, -0.088702396f,  0.030388512f, -0.003737096f },
    { -0.015054174f,  0.055448985f, -0.152303909f,  0.893388591f,  0.282683940f, -0.091660566f,  0.031467750f, -0.003970616f },
    { -0.015203134f,  0.056327538f, -0.154683673f,  0.886901275f,  0.292926077f, -0.094603396f,  0.032544829f, -0.004209516f },
    { -0.015334217f,  0.057153461f, -0.156923937f,  0.880247680f,  0.303220431f, -0.097528734f,  0.033618872f, -0.004453556f },
    { -0.015447692f,  0.057927009f, -0.159025692f,  0.873430601f,  0.313563684f, -0.100434411f,  0.034688990f, -0.004702488f },
    { -0.015543843f,  0.058648468f, -0.160989987f,  0.866452891f,  0.323952485f, -0.103318243f,  0.035754283f, -0.004956054f },
    { -0.015622962f,  0.059318157f, -0.162817927f,  0.859317457f,  0.334383447f, -0.106178028f,  0.036813843f, -0.005213986f },
    { -0.015685354f,  0.059936426f, -0.164510674f,  0.852027259f,  0.344853148f, -0.109011551f,  0.037866753f, -0.005476007f },
    { -0.015731334f,  0.060503660f, -0.166069444f,  0.844585308f,  0.355358139f, -0.111816582f,  0.038912083f, -0.005741832f },
    { -0.015761227f,  0.061020271f, -0.167495505f,  0.836994666f,  0.365894938f, -0.114590878f,  0.039948901f, -0.006011165f },
    { -0.015775369f,  0.061486700f, -0.168790179f,  0.829258442f,  0.376460035f, -0.117332187f,  0.040976262f, -0.006283705f },
    { -0.015774102f,  0.061903421f, -0.169954836f,  0.821379791f,  0.387049892f, -0.120038243f,  0.041993218f, -0.006559140f },
    { -0.015757780f,  0.062270932f, -0.170990898f,  0.813361912f,  0.397660945f, -0.122706774f,  0.042998812f, -0.006837149f },
    { -0.015726764f,  0.062589762f, -0.171899832f,  0.805208049f,  0.408289606f, -0.125335497f,  0.043992082f, -0.007117406f },
    { -0.015681421f,  0.062860463f, -0.172683155f,  0.796921486f,  0.418932265f, -0.127922122f,  0.044972060f, -0.007399575f },
    { -0.015622128f,  0.063083616f, -0.173342427f,  0.788505546f,  0.429585287f, -0.130464356f,  0.045937776f, -0.007683314f },
    { -0.015549268f,  0.063259825f, -0.173879252f,  0.779963591f,  0.440245020f, -0.132959898f,  0.046888254f, -0.007968272f },
    { -0.015463230f,  0.063389720f, -0.174295277f,  0.771299018f,  0.450907791f, -0.135406444f,  0.047822514f, -0.008254092f },
    { -0.015364410f,  0.063473953f, -0.174592192f,  0.762515261f,  0.461569910f, -0.137801687f,  0.048739575f, -0.008540410f },
    { -0.015253207f,  0.063513199f, -0.174771724f,  0.753615784f,  0.472227671f, -0.140143319f,  0.049638453f, -0.008826856f },
    { -0.015130029f,  0.063508155f, -0.174835641f,  0.744604083f,  0.482877354f, -0.142429033f,  0.050518165f, -0.009113053f },
    { -0.014995286f,  0.063459540f, -0.174785746f,  0.735483686f,  0.493515224f, -0.144656521f,  0.051377723f, -0.009398620f },
    { -0.014849393f,  0.063368091f, -0.174623880f,  0.726258146f,  0.504137536f, -0.146823477f,  0.052216144f, -0.009683166f },
    { -0.014692767f,  0.063234566f, -0.174351917f,  0.716931043f,  0.514740533f, -0.148927600f,  0.053032442f, -0.009966300f },
    { -0.014525831f,  0.063059742f, -0.173971766f,  0.707505983f,  0.525320452f, -0.150966592f,  0.053825634f, -0.010247622f },
    { -0.014349010f,  0.062844412f, -0.173485365f,  0.697986594f,  0.535873519f, -0.152938161f,  0.054594740f, -0.010526729f },
    { -0.014162731f,  0.062589388f, -0.172894686f,  0.688376527f,  0.546395956f, -0.154840023f,  0.055338782f, -0.010803214f },
    { -0.013967422f,  0.062295497f, -0.172201726f,  0.678679450f,  0.556883981f, -0.156669900f,  0.056056785f, -0.011076664f },
    { -0.013763516f,  0.061963580f, -0.171408513f,  0.668899053f,  0.567333807f, -0.158425527f,  0.056747780f, -0.011346664f },
    { -0.013551444f,  0.061594497f, -0.170517100f,  0.659039039f,  0.577741646f, -0.160104646f,  0.057410802f, -0.011612795f },
    { -0.013331638f,  0.061189116f, -0.169529566f,  0.649103130f,  0.588103712f, -0.161705013f,  0.058044893f, -0.011874636f },
    { -0.013104532f,  0.060748323f, -0.168448011f,  0.639095059f,  0.598416219f, -0.163224396f,  0.058649099f, -0.012131760f },
    { -0.012870559f,  0.060273012f, -0.167274562f,  0.629018571f,  0.608675382f, -0.164660580f,  0.059222478f, -0.012383742f },
    { -0.012630152f,  0.059764091f, -0.166011363f,  0.618877424f,  0.618877424f, -0.166011363f,  0.059764091f, -0.012630152f },
    { -0.012383742f,  0.059222478f, -0.164660580f,  0.608675382f,  0.629018571f, -0.167274562f,  0.060273012f, -0.012870559f },
    { -0.012131760f,  0.058649099f, -0.163224396f,  0.598416219f,  0.639095059f, -0.168448011f,  0.060748323f, -0.013104532f },
    { -0.011874636f,  0.058044893f, -0.161705013f,  0.588103712f,  0.649103130f, -0.169529566f,  0.061189116f, -0.013331638f },
    { -0.011612795f,  0.057410802f, -0.160104646f,  0.577741646f,  0.659039039f, -0.170517100f,  0.061594497f, -0.013551444f },
    { -0.011346664f,  0.056747780f, -0.158425527f,  0.567333807f,  0.668899053f, -0.171408513f,  0.061963580f, -0.013763516f },
    { -0.011076664f,  0.056056785f, -0.156669900f,  0.556883981f,  0.678679450f, -0.172201726f,  0.062295497f, -0.013967422f },
    { -0.010803214f,  0.055338782f, -0.154840023f,  0.546395956f,  0.688376527f, -0.172894686f,  0.062589388f, -0.014162731f },
    { -0.010526729f,  0.054594740f, -0.152938161f,  0.535873519f,  0.697986594f, -0.173485365f,  0.062844412f, -0.014349010f },
    { -0.010247622f,  0.053825634f, -0.150966592f,  0.525320452f,  0.707505983f, -0.173971766f,  0.063059742f, -0.014525831f },
    { -0.009966300f,  0.053032442f, -0.148927600f,  0.514740533f,  0.716931043f, -0.174351917f,  0.063234566f, -0.014692767f },
    { -0.009683166f,  0.052216144f, -0.146823477f,  0.504137536f,  0.726258146f, -0.174623880f,  0.063368091f, -0.014849393f },
    { -0.009398620f,  0.051377723f, -0.144656521f,  0.493515224f,  0.735483686f, -0.174785746f,  0.063459540f, -0.014995286f },
    { -0.009113053f,  0.050518165f, -0.142429033f,  0.482877354f,  0.744604083f, -0.174835641f,  0.063508155f, -0.015130029f },
    { -0.008826856f,  0.049638453f, -0.140143319f,  0.472227671f,  0.753615784f, -0.174771724f,  0.063513199f, -0.015253207f },
    { -0.008540410f,  0.048739575f, -0.137801687f,  0.461569910f,  0.762515261f, -0.174592192f,  0.063473953f, -0.015364410f },
    { -0.008254092f,  0.047822514f, -0.135406444f,  0.450907791f,  0.771299018f, -0.174295277f,  0.063389720f, -0.015463230f },
    { -0.007968272f,  0.046888254f, -0.132959898f,  0.440245020f,  0.779963591f, -0.173879252f,  0.063259825f, -0.015549268f },
    { -0.007683314f,  0.045937776f, -0.130464356f,  0.429585287f,  0.788505546f, -0.173342427f,  0.063083616f, -0.015622128f },
    { -0.007399575f,  0.044972060f, -0.127922122f,  0.418932265f,  0.796921486f, -0.172683155f,  0.062860463f, -0.015681421f },
    { -0.007117406f,  0.043992082f, -0.125335497f,  0.408289606f,  0.805208049f, -0.171899832f,  0.062589762f, -0.015726764f },
    { -0.006837149f,  0.042998812f, -0.122706774f,  0.397660945f,  0.813361912f, -0.170990898f,  0.062270932f, -0.015757780f },
    { -0.006559140f,  0.041993218f, -0.120038243f,  0.387049892f,  0.821379791f, -0.169954836f,  0.061903421f, -0.015774102f },
    { -0.006283705f,  0.040976262f, -0.117332187f,  0.376460035f,  0.829258442f, -0.168790179f,  0.061486700f, -0.015775369f },
    { -0.006011165f,  0.039948901f, -0.114590878f,  0.365894938f,  0.836994666f, -0.167495505f,  0.061020271f, -0.015761227f },
    { -0.005741832f,  0.038912083f, -0.111816582f,  0.355358139f,  0.844585308f, -0.166069444f,  0.060503660f, -0.015731334f },
    { -0.005476007f,  0.037866753f, -0.109011551f,  0.344853148f,  0.852027259f, -0.164510674f,  0.059936426f, -0.015685354f },
    { -0.005213986f,  0.036813843f, -0.106178028f,  0.334383447f,  0.859317457f, -0.162817927f,  0.059318157f, -0.015622962f },
    { -0.004956054f,  0.035754283f, -0.103318243f,  0.323952485f,  0.866452891f, -0.160989987f,  0.058648468f, -0.015543843f },
    { -0.004702488f,  0.034688990f, -0.100434411f,  0.313563684f,  0.873430601f, -0.159025692f,  0.057927009f, -0.015447692f },
    { -0.004453556f,  0.033618872f, -0.097528734f,  0.303220431f,  0.880247680f, -0.156923937f,  0.057153461f, -0.015334217f },
    { -0.004209516f,  0.032544829f, -0.094603396f,  0.292926077f,  0.886901275f, -0.154683673f,  0.056327538f, -0.015203134f },
    { -0.003970616f,  0.031467750f, -0.091660566f,  0.282683940f,  0.893388591f, -0.152303909f,  0.055448985f, -0.015054174f },
    { -0.003737096f,  0.030388512f, -0.088702396f,  0.272497300f,  0.899706889f, -0.149783713f,  0.054517583f, -0.014887078f },
    { -0.003509186f,  0.029307981f, -0.085731016f,  0.262369398f,  0.905853491f, -0.147122214f,  0.053533148f, -0.014701602f },
    { -0.003287105f,  0.028227013f, -0.082748540f,  0.252303437f,  0.911825780f, -0.144318604f,  0.052495531f, -0.014497511f },
    { -0.003071063f,  0.027146448f, -0.079757058f,  0.242302577f,  0.917621200f, -0.141372134f,  0.051404619f, -0.014274589f },
    { -0.002861260f,  0.026067117f, -0.076758641f,  0.232369938f,  0.923237264f, -0.138282122f,  0.050260334f, -0.014032629f },
    { -0.002657886f,  0.024989835f, -0.073755335f,  0.222508593f,  0.928671546f, -0.135047950f,  0.049062638f, -0.013771442f },
    { -0.002461119f,  0.023915404f, -0.070749164f,  0.212721575f,  0.933921692f, -0.131669065f,  0.047811529f, -0.013490852f },
    { -0.002271128f,  0.022844612f, -0.067742128f,  0.203011866f,  0.938985414f, -0.128144982f,  0.046507044f, -0.013190698f },
    { -0.002088073f,  0.021778232f, -0.064736199f,  0.193382402f,  0.943860497f, -0.124475283f,  0.045149259f, -0.012870835f },
    { -0.001912102f,  0.020717022f, -0.061733325f,  0.183836073f,  0.948544797f, -0.120659620f,  0.043738289f, -0.012531135f },
    { -0.001743352f,  0.019661726f, -0.058735426f,  0.174375715f,  0.953036246f, -0.116697714f,  0.042274289f, -0.012171483f },
    { -0.001581950f,  0.018613069f, -0.055744396f,  0.165004115f,  0.957332847f, -0.112589355f,  0.040757455f, -0.011791785f },
    { -0.001428013f,  0.017571764f, -0.052762097f,  0.155724007f,  0.961432685f, -0.108334407f,  0.039188023f, -0.011391961f },
    { -0.001281648f,  0.016538503f, -0.049790363f,  0.146538071f,  0.965333919f, -0.103932805f,  0.037566271f, -0.010971948f },
    { -0.001142949f,  0.015513964f, -0.046830999f,  0.137448934f,  0.969034791f, -0.099384557f,  0.035892518f, -0.010531703f },
    { -0.001012002f,  0.014498808f, -0.043885776f,  0.128459165f,  0.972533622f, -0.094689744f,  0.034167127f, -0.010071199f },
    { -0.000888880f,  0.013493676f, -0.040956435f,  0.119571276f,  0.975828814f, -0.089848523f,  0.032390500f, -0.009590427f },
    { -0.000773649f,  0.012499194f, -0.038044684f,  0.110787721f,  0.978918856f, -0.084861123f,  0.030563084f, -0.009089399f },
    { -0.000666361f,  0.011515968f, -0.035152197f,  0.102110896f,  0.981802318f, -0.079727851f,  0.028685370f, -0.008568144f },
    { -0.000567059f,  0.010544587f, -0.032280614f,  0.093543136f,  0.984477860f, -0.074449089f,  0.026757890f, -0.008026709f },
    { -0.000475776f,  0.009585620f, -0.029431542f,  0.085086712f,  0.986944225f, -0.069025296f,  0.024781219f, -0.007465163f },
    { -0.000392534f,  0.008639618f, -0.026606551f,  0.076743837f,  0.989200248f, -0.063457006f,  0.022755979f, -0.006883591f },
    { -0.000317344f,  0.007707113f, -0.023807174f,  0.068516657f,  0.991244851f, -0.057744832f,  0.020682832f, -0.006282102f },
    { -0.000250208f,  0.006788617f, -0.021034909f,  0.060407254f,  0.993077046f, -0.051889464f,  0.018562486f, -0.005660821f },
    { -0.000191119f,  0.005884624f, -0.018291217f,  0.052417647f,  0.994695937f, -0.045891670f,  0.016395694f, -0.005019896f },
    { -0.000140057f,  0.004995606f, -0.015577519f,  0.044549786f,  0.996100722f, -0.039752294f,  0.014183249f, -0.004359494f },
    { -0.000096994f,  0.004122018f, -0.012895201f,  0.036805556f,  0.997290690f, -0.033472260f,  0.011925993f, -0.003679802f },
    { -0.000061893f,  0.003264293f, -0.010245608f,  0.029186772f,  0.998265223f, -0.027052569f,  0.009624809f, -0.002981027f },
    { -0.000034704f,  0.002422846f, -0.007630045f,  0.021695182f,  0.999023800f, -0.020494302f,  0.007280624f, -0.002263400f },
    { -0.000015372f,  0.001598069f, -0.005049778f,  0.014332463f,  0.999565993f, -0.013798614f,  0.004894410f, -0.001527170f },
    { -0.000003829f,  0.000790336f, -0.002506035f,  0.007100224f,  0.999891471f, -0.006966743f,  0.002467182f, -0.000772606f },
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// sinc_interpolate: 8-point Lanczos-windowed sinc with 128 sub-sample phases

static inline f32 sinc_interpolate(const f32* data, u32 data_len, f64 frac_pos) {
    i32 ipos = (i32)frac_pos;
    f32 frac = (f32)(frac_pos - (f64)ipos);
    i32 phase = (i32)(frac * SINC_PHASES);
    if (phase >= SINC_PHASES)
        phase = SINC_PHASES - 1;
    const f32* kernel = s_sinc_table[phase];
    f32 sum = 0.0f;
    for (i32 t = 0; t < SINC_TAPS; t++) {
        i32 idx = ipos - SINC_TAPS / 2 + 1 + t;
        if (idx < 0)
            idx = 0;
        else if ((u32)idx >= data_len)
            idx = (i32)data_len - 1;
        sum += data[idx] * kernel[t];
    }
    return sum;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pre_play_init: Initialize mixer state

void pre_play_init(MixerState* mixer, u32 output_rate) {
    memset(mixer, 0, sizeof(MixerState));
    mixer->output_rate = output_rate;
    mixer->samples_per_tick = output_rate / 50;
    mixer->samples_until_tick = 0;
    mixer->solo_channel = -1;
    mixer->stereo_mix = 0.0f;

    for (int i = 0; i < NUM_CHANNELS; i++) {
        mixer->channels[i].active = false;
        mixer->channels[i].volume = 0.0f;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pre_play_start: Start playback from beginning

void pre_play_start(MyPlayer* player, MySong* song, MixerState* mixer) {
    // Amiga hard-panning: channels 0,3 left; 1,2 right (matches PreTracker.exe)
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (i == 0 || i == 3) {
            mixer->channels[i].pan_left = 1.0f;
            mixer->channels[i].pan_right = 0.0f;
        } else {
            mixer->channels[i].pan_left = 0.0f;
            mixer->channels[i].pan_right = 1.0f;
        }
    }

    // Wire up sample buffer info for bounds checking in mixer_sync_channels.
    // Compute total buffer size from wavetotal_table (matches pre_song_init return value).
    u32 total_size = 2; // 2 bytes of silence at start
    for (int i = 0; i < song->num_waves; i++) {
        total_size += song->wavetotal_table[i];
    }
    mixer->sample_buffer_ptr = player->sample_buffer_ptr;
    mixer->sample_buffer_size = total_size;

    // Reset song position
    song->curr_pat_pos = 0;
    player->pat_curr_row = 0;
    player->next_pat_row = 0xFF;
    player->next_pat_pos = 0xFF;
    player->pat_line_ticks = player->pat_speed_even;
    player->pat_stopped = 1;
    player->songend_detected = 0;
    player->trigger_mask = 0;

    // Reset all channels
    WaveInfo* first_wave = (song->num_waves > 0) ? &song->waveinfo_ptr[0] : ((void*)0);
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        PerChannelData* pcd = &player->channeldata[ch];
        pcd->pat_vol = MAX_VOLUME;
        pcd->track_delay_offset = 0xFF;
        pcd->waveinfo_ptr = first_wave;
        pcd->adsr_phase = ADSR_PHASE_RELEASE;
        pcd->out.sam_ptr_offset = 0;
        pcd->out.length = 2;
        pcd->out.loop_offset = 0xFFFF;
        pcd->out.period = 0x7B;
        pcd->out.volume = 0;
        pcd->out.trigger = 0;
        pcd->inst_info_ptr = ((void*)0);
        pcd->new_inst_num = 0;
        pcd->inst_num4 = 0;
        pcd->inst_wave_num = 0xFFFF;
        pcd->note_delay = 0;
        pcd->note_off_delay = 0;
        pcd->pat_portamento_dest = 0;
        pcd->pat_pitch_slide = 0;
        pcd->pat_vol_ramp_speed = 0;
        pcd->pat_2nd_inst_num4 = 0;
        pcd->inst_pitch = 0x10;
        pcd->inst_vol = MAX_VOLUME;
        pcd->inst_curr_port_pitch = 0;
        pcd->inst_sel_arp_note = 0;
        pcd->inst_note_pitch = 0;
        pcd->inst_pitch_slide = 0;
        pcd->inst_line_ticks = 0;
        pcd->inst_pitch_pinned = 0;
        pcd->inst_vol_slide = 0;
        pcd->inst_step_pos = 0;
        pcd->inst_speed_stop = 1;
        pcd->track_delay_steps = 0;
        pcd->adsr_volume = 0;
        pcd->vibrato_pos = 0;
        pcd->vibrato_delay = 0;
        pcd->vibrato_depth = 0;
        pcd->vibrato_speed = 0;
        pcd->adsr_release = 0;
        pcd->adsr_phase_speed = 0;
        pcd->adsr_pos = 0;
        pcd->adsr_vol64 = 0;
        pcd->loaded_inst_vol = MAX_VOLUME;
        pcd->last_trigger_pos = 0;
        pcd->wave_offset = 0;
        pcd->inst_loop_offset = 0;
        pcd->inst_subloop_wait = 0;
        pcd->inst_ping_pong_dir = 0xFF;
        pcd->pat_portamento_speed = 0;
        pcd->pat_adsr_rel_delay = 0;
        pcd->inst_pattern_steps = 0;
        pcd->track_delay_vol16 = 0;
        pcd->track_init_delay = 0;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// mixer_sync_channels: Transfer tick engine output to mixer state

static void mixer_sync_channels(MyPlayer* player, MixerState* mixer) {
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        PerChannelData* pcd = &player->channeldata[ch];
        MixerChannel* mc = &mixer->channels[ch];

        // Convert period to playback speed
        u16 period = pcd->out.period;
        if (period > 0) {
            f64 freq = AMIGA_CLOCK / (f64)period;
            mc->speed = freq / (f64)mixer->output_rate * ((f64)HQ_MAX_PERIOD / (f64)AMIGA_MAX_PERIOD);
        } else {
            mc->speed = 0.0;
        }

        // Volume: 0-64 -> 0.0-1.0
        mc->volume = (f32)pcd->out.volume / 64.0f;

        // Resolve sample pointer and handle trigger/loop offset
        // Mirrors the Amiga DMA output code (asm:4067-4097):
        // - Triggered channels: DMA writes ac_ptr, ac_len, starts new sample
        // - Non-triggered looping: DMA writes ac_ptr only (length unchanged)
        // - Non-triggered one-shot: DMA unchanged (keep playing current buffer)
        if (pcd->out.trigger) {
            // Triggered: write both pointer and length (asm:4079-4082)
            mc->sample_length = pcd->out.length;
            u32 off = pcd->out.sam_ptr_offset;
            if (off + mc->sample_length <= mixer->sample_buffer_size) {
                mc->sample_data = player->sample_buffer_ptr + off;
            } else {
                mc->sample_data = player->sample_buffer_ptr;
            }
            mc->loop_offset = pcd->out.loop_offset;
            mc->frac_pos = 0.0;
            mc->active = true;
            pcd->out.trigger = 0;

            // Post-trigger loop setup (asm:4136-4156): after the one-shot buffer
            // finishes, Amiga DMA reloads from sam_ptr+loop_offset. Pre-compute
            // the loop pointer so the render loop can switch to it on first wrap.
            mc->loop_data = nullptr;
            if (pcd->out.loop_offset != 0xFFFF) {
                u32 loop_off = pcd->out.sam_ptr_offset + pcd->out.loop_offset;
                if (loop_off + mc->sample_length <= mixer->sample_buffer_size) {
                    mc->loop_data = player->sample_buffer_ptr + loop_off;
                }
            }
        } else if (pcd->out.loop_offset != 0xFFFF) {
            // Non-triggered looping: set pending loop pointer (asm:4086-4091)
            // Amiga DMA finishes current buffer then reloads from new ac_ptr on wrap.
            u32 off = pcd->out.sam_ptr_offset + pcd->out.loop_offset;
            if (off + mc->sample_length <= mixer->sample_buffer_size) {
                mc->loop_data = player->sample_buffer_ptr + off;
            }
            mc->loop_offset = pcd->out.loop_offset;
        }
        // Non-triggered one-shot: keep current sample_data/length unchanged (asm:4088-4089)

        if (mc->sample_length <= 1 || mc->speed <= 0.0) {
            mc->active = false;
        }

        // Solo channel: silence non-selected channels
        if (mixer->solo_channel >= 0 && ch != mixer->solo_channel) {
            mc->active = false;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pre_play_render: Render N stereo float frames, interleaving tick calls with mixing
// Optional scopes: per-channel mono output (sample * volume, pre-pan). NULL to skip.

int pre_play_render(MyPlayer* player, MixerState* mixer, f32* buffer, int num_frames, f32** scopes,
                    int num_scopes) {
    if (num_scopes > NUM_CHANNELS)
        num_scopes = NUM_CHANNELS;

    // Zero scope buffers upfront so inactive channels read as 0.0f
    if (scopes) {
        for (int ch = 0; ch < num_scopes; ch++) {
            if (scopes[ch])
                memset(scopes[ch], 0, (size_t)num_frames * sizeof(f32));
        }
    }

    int frames_written = 0;

    while (frames_written < num_frames) {
        // Time to tick?
        if (mixer->samples_until_tick == 0) {
            pre_player_tick(player);
            mixer_sync_channels(player, mixer);
            mixer->samples_until_tick = mixer->samples_per_tick;
        }

        // How many samples until next tick or end of buffer
        int chunk = num_frames - frames_written;
        if ((u32)chunk > mixer->samples_until_tick) {
            chunk = (int)mixer->samples_until_tick;
        }

        f32* out = buffer + frames_written * 2;

        // Mix chunk
        for (int i = 0; i < chunk; i++) {
            f32 left = 0.0f;
            f32 right = 0.0f;

            for (int ch = 0; ch < NUM_CHANNELS; ch++) {
                MixerChannel* mc = &mixer->channels[ch];
                if (!mc->active || mc->sample_length <= 1)
                    continue;

                f32 sample_f;
                f32 volume_f = mc->volume;

                if (mixer->interp_mode == PRE_INTERP_SINC) {
                    // Sinc: reconstruct the continuous signal between samples
                    sample_f = sinc_interpolate(mc->sample_data, mc->sample_length, mc->frac_pos) * 0.5f;
                } else {
                    // BLEP: nearest-neighbor + anti-aliasing correction (matches PreTracker.exe)
                    u32 pos = (u32)mc->frac_pos;
                    sample_f = mc->sample_data[pos] * 0.5f;

                    // Update sub-sample accumulator for BLEP phase tracking
                    f32 step_f = (f32)mc->speed;
                    mc->sub_accum += step_f;
                    if (mc->sub_accum >= 1.0f) {
                        mc->sub_accum -= 1.0f;
                        mc->blep_step = step_f;
                        mc->blep_wrap = mc->sub_accum;
                    }

                    // BLEP on sample value change
                    if (sample_f != mc->prev_sample) {
                        f32 delta = mc->prev_sample - sample_f;
                        if (mc->blep_step > 0.0f && mc->blep_step > mc->blep_wrap) {
                            f32 phase = mc->blep_wrap / mc->blep_step;
                            if (phase >= 0.0f && phase <= 1.0f) {
                                blep_insert(mc->blep_buf, mc->blep_read_idx, &mc->blep_count, delta, phase);
                            }
                        }
                        mc->prev_sample = sample_f;
                    }

                    // Drain sample BLEP
                    sample_f += blep_drain(mc->blep_buf, &mc->blep_read_idx, &mc->blep_count);
                }

                // BLEP on volume change (both modes — volume changes are instant step functions)
                if (volume_f != mc->prev_volume) {
                    f32 delta = mc->prev_volume - volume_f;
                    blep_insert(mc->vol_blep_buf, mc->vol_blep_read_idx, &mc->vol_blep_count, delta, 0.0f);
                    mc->prev_volume = volume_f;
                }

                // Drain volume BLEP
                volume_f += blep_drain(mc->vol_blep_buf, &mc->vol_blep_read_idx, &mc->vol_blep_count);

                // Mix to stereo
                f32 mixed = sample_f * volume_f;
                if (scopes && ch < num_scopes && scopes[ch])
                    scopes[ch][frames_written + i] = mixed;

                left += mixed * mc->pan_left;
                right += mixed * mc->pan_right;

                // Advance position
                mc->frac_pos += mc->speed;

                // Handle looping / end (Amiga DMA reload behavior)
                if ((u32)mc->frac_pos >= mc->sample_length) {
                    if (mc->loop_offset != 0xFFFF) {
                        // On first wrap after trigger: switch from one-shot to loop buffer
                        // (asm:4141-4151 post-trigger sets ac_ptr = sam_ptr + loop_offset)
                        if (mc->loop_data) {
                            mc->sample_data = mc->loop_data;
                            mc->loop_data = nullptr;
                        }
                        while ((u32)mc->frac_pos >= mc->sample_length)
                            mc->frac_pos -= (f64)mc->sample_length;
                    } else {
                        mc->active = false;
                    }
                }
            }

            out[i * 2] = left;
            out[i * 2 + 1] = right;
        }

        frames_written += chunk;
        mixer->samples_until_tick -= (u32)chunk;
    }

    // Stereo separation post-pass (matches PreTracker.exe)
    // stereo_mix: 0.0 = full separation (no cross-feed), 1.0 = mono
    f32 sep = 1.0f - mixer->stereo_mix;
    if (sep < 1.0f) {
        f32 blend = 1.0f - sep;
        f32 inv_divisor = 1.0f / (2.0f - sep);
        for (int i = 0; i < frames_written; i++) {
            f32 l = buffer[i * 2];
            f32 r = buffer[i * 2 + 1];
            buffer[i * 2] = (r * blend + l) * inv_divisor;
            buffer[i * 2 + 1] = (l * blend + r) * inv_divisor;
        }
    }

    // Haas effect stereo widening post-pass
    if (mixer->haas_delay_samples > 0) {
        f32 blend = mixer->haas_blend;
        f32 norm = 1.0f / (1.0f + blend);
        u32 delay = mixer->haas_delay_samples;
        for (int i = 0; i < frames_written; i++) {
            f32 l = buffer[i * 2];
            f32 r = buffer[i * 2 + 1];
            u32 read_idx = (mixer->haas_write_idx - delay) & 63;
            f32 delayed_l = mixer->haas_buf_l[read_idx];
            f32 delayed_r = mixer->haas_buf_r[read_idx];
            buffer[i * 2] = (l + blend * delayed_r) * norm;
            buffer[i * 2 + 1] = (r + blend * delayed_l) * norm;
            mixer->haas_buf_l[mixer->haas_write_idx & 63] = l;
            mixer->haas_buf_r[mixer->haas_write_idx & 63] = r;
            mixer->haas_write_idx++;
        }
    }

    return frames_written;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pre_play_is_finished: Check if song has reached its end

bool pre_play_is_finished(const MyPlayer* player) {
    return player->songend_detected != 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Public API: consolidated opaque handle

typedef struct PreSong {
    MySong song;
    MyPlayer player;
    MixerState mixer;
    PreSongMetadata metadata;
    PrePlaybackState playback_state;
    f32* sample_buffer;
    u8* prt_data;           // owned copy (WaveInfo pointers reference into it)
    u32 prt_data_size;
    u32 sample_rate;        // default 48000
    i32 solo_channel;       // default -1 (all)
    f32 stereo_mix;         // default 0.0 (full stereo)
    f32 stereo_width_ms;    // Haas delay in ms (0.0 = disabled)
    u8 interp_mode;         // default 0 (BLEP)
    int subsong;            // default 0
    int last_parsed_subsong; // tracks which subsong was last parsed
} PreSong;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Parse a null-terminated name (max max_chars characters) from PRT data, advance pointer past it.

static void parse_name(const u8** ptr, char* out, int max_chars) {
    const u8* p = *ptr;
    int i = 0;
    while (*p != 0 && i < max_chars) {
        out[i++] = (char)*p++;
    }
    out[i] = '\0';
    if (*p == 0)
        p++;
    *ptr = p;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void fill_metadata(PreSongMetadata* meta, const u8* prt_data, const MySong* song) {
    // Song name (20 bytes at offset 0x14) and author (20 bytes at offset 0x28)
    memcpy(meta->song_name, prt_data + 0x14, 20);
    meta->song_name[20] = '\0';
    memcpy(meta->author, prt_data + 0x28, 20);
    meta->author[20] = '\0';

    meta->num_waves = song->num_waves;
    meta->num_steps = song->num_steps;
    meta->num_positions = song->pat_pos_len;
    meta->num_subsongs = song->num_subsongs;

    u8 version = prt_data[3];
    u8 num_inst_names = (version == 0x1E) ? 2 * PRE_MAX_INSTRUMENTS : PRE_MAX_INSTRUMENTS;
    meta->num_instruments = prt_data[0x40];
    if (meta->num_instruments > PRE_MAX_INSTRUMENTS)
        meta->num_instruments = PRE_MAX_INSTRUMENTS;

    // Parse instrument names
    u32 inst_offset = read_be32(prt_data + 0x0C);
    const u8* ptr = prt_data + inst_offset;
    for (int i = 0; i < num_inst_names; i++) {
        if (i < PRE_MAX_INSTRUMENTS)
            parse_name(&ptr, meta->instrument_names[i], 23);
        else
            parse_name(&ptr, (char[24]){0}, 23); // skip extra V1.5 slots
    }

    // Parse wave names
    u32 wave_offset = read_be32(prt_data + 0x10);
    ptr = prt_data + wave_offset;
    for (int i = 0; i < PRE_MAX_WAVES; i++) {
        parse_name(&ptr, meta->wave_names[i], 23);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

PreSong* pre_song_create(const u8* data, u32 size) {
    PreSong* ps = (PreSong*)calloc(1, sizeof(PreSong));
    if (!ps) {
        return nullptr;
    }

    // Own a copy of PRT data (WaveInfo pointers reference into it)
    ps->prt_data = (u8*)malloc(size);
    if (!ps->prt_data) {
        free(ps);
        return nullptr;
    }
    memcpy(ps->prt_data, data, size);
    ps->prt_data_size = size;

    // Defaults
    ps->sample_rate = 48000;
    ps->solo_channel = -1;
    ps->stereo_mix = 0.0f;
    ps->subsong = 0;
    ps->last_parsed_subsong = 0;

    // Parse PRT file
    u32 chip_size = pre_song_init(&ps->song, ps->prt_data, size, 0);
    if (chip_size == 0) {
        free(ps->prt_data);
        free(ps);
        return nullptr;
    }

    fill_metadata(&ps->metadata, ps->prt_data, &ps->song);

    // Allocate sample buffer and generate all samples
    ps->sample_buffer = (f32*)calloc(chip_size, sizeof(f32));
    if (!ps->sample_buffer) {
        free(ps->prt_data);
        free(ps);
        return nullptr;
    }

    pre_player_init(&ps->player, ps->sample_buffer, &ps->song);
    return ps;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void pre_song_destroy(PreSong* song) {
    if (!song) {
        return;
    }
    free(song->sample_buffer);
    free(song->prt_data);
    free(song);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Public API: configuration setters

void pre_song_set_subsong(PreSong* song, int subsong) {
    song->subsong = subsong;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void pre_song_set_sample_rate(PreSong* song, u32 rate) {
    song->sample_rate = rate;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void pre_song_set_solo_channel(PreSong* song, i32 channel) {
    song->solo_channel = channel;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void pre_song_set_stereo_mix(PreSong* song, f32 mix) {
    song->stereo_mix = mix;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void pre_song_set_stereo_width(PreSong* song, f32 delay_ms) {
    song->stereo_width_ms = delay_ms;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void pre_song_set_interp_mode(PreSong* song, PreInterpMode mode) {
    song->interp_mode = (u8)mode;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Internal helpers for subsong switching

static void apply_subsong(MySong* song, u8* prt_data, int subsong) {
    u32 posd_offset = read_be32(prt_data + 0x04);
    u32 patt_offset = read_be32(prt_data + 0x08);

    int sel = (subsong >= 0 && subsong < song->num_subsongs) ? subsong : 0;

    u8* subsong_hdr = prt_data + posd_offset + (u32)sel * 8;
    song->pat_restart_pos = subsong_hdr[0];
    song->pat_pos_len = subsong_hdr[3];
    song->num_steps = subsong_hdr[2];

    u32 pos_data_base = posd_offset + (u32)song->num_subsongs * 8;
    u32 pos_entries_before = 0;
    for (int i = 0; i < sel; i++) {
        u8* hdr = prt_data + posd_offset + (u32)i * 8;
        pos_entries_before += hdr[3];
    }
    song->pos_data_adr = prt_data + pos_data_base + pos_entries_before * 8;

    u32 pat_rel = read_be32(subsong_hdr + 4);
    song->patterns_ptr = prt_data + patt_offset + pat_rel;
}

static void rebuild_pattern_table(MySong* song) {
    u32 step_size = (u32)song->num_steps * 3;
    u8* pat = song->patterns_ptr;
    for (int i = 0; i < 255; i++) {
        song->pattern_table[i] = pat;
        pat += step_size;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void update_playback_state(PreSong* ps) {
    PrePlaybackState* state = &ps->playback_state;
    MySong* song = &ps->song;
    MyPlayer* player = &ps->player;

    state->position = song->curr_pat_pos;
    state->row = player->pat_curr_row;
    state->speed = (player->pat_curr_row & 1) ? player->pat_speed_odd : player->pat_speed_even;
    state->ticks_remaining = player->pat_line_ticks;

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        PerChannelData* pcd = &player->channeldata[ch];

        // Look up position table entry for this channel
        u16 pos_offset = song->curr_pat_pos * 4 + (u16)ch;
        pos_offset *= 2;
        u8* pos_ptr = song->pos_data_adr + pos_offset;
        state->channels[ch].track_num = pos_ptr[0];
        state->channels[ch].pitch_shift = (i8)pos_ptr[1];

        state->channels[ch].instrument = (u8)(pcd->inst_num4 >> 2);
        state->channels[ch].volume = pcd->pat_vol;
        state->channels[ch].wave = (u8)(pcd->inst_wave_num >> 2);
        state->channels[ch].adsr_phase = (u8)pcd->adsr_phase;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Public API: playback

void pre_song_start(PreSong* song) {
    // Re-apply subsong if changed (only relevant for V1.5 multi-subsong files)
    if (song->subsong != song->last_parsed_subsong && song->song.num_subsongs > 1) {
        apply_subsong(&song->song, song->prt_data, song->subsong);
        rebuild_pattern_table(&song->song);
        song->last_parsed_subsong = song->subsong;
    }

    pre_play_init(&song->mixer, song->sample_rate);
    song->mixer.solo_channel = song->solo_channel;
    song->mixer.stereo_mix = song->stereo_mix;
    song->mixer.interp_mode = song->interp_mode;
    if (song->stereo_width_ms > 0.0f) {
        u32 delay = (u32)(song->stereo_width_ms * 0.001f * (f32)song->sample_rate + 0.5f);
        if (delay > 63)
            delay = 63;
        song->mixer.haas_delay_samples = delay;
        song->mixer.haas_blend = 0.3f;
    } else {
        song->mixer.haas_delay_samples = 0;
        song->mixer.haas_blend = 0.0f;
    }
    pre_play_start(&song->player, &song->song, &song->mixer);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int pre_song_decode(PreSong* song, f32* buffer, int num_frames) {
    int result = pre_play_render(&song->player, &song->mixer, buffer, num_frames, nullptr, 0);
    update_playback_state(song);
    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int pre_song_decode_with_scopes(PreSong* song, f32* buffer, int num_frames, f32** scopes, int num_scopes) {
    int result = pre_play_render(&song->player, &song->mixer, buffer, num_frames, scopes, num_scopes);
    update_playback_state(song);
    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool pre_song_is_finished(const PreSong* song) {
    return pre_play_is_finished(&song->player);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const PreSongMetadata* pre_song_get_metadata(const PreSong* ps) {
    return &ps->metadata;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool pre_song_get_position_entry(const PreSong* ps, u16 position, u8 channel, u8* track_num, i8* pitch_shift) {
    const MySong* song = &ps->song;
    if (position >= song->pat_pos_len || channel >= NUM_CHANNELS) {
        return false;
    }
    u16 pos_offset = position * 4 + channel;
    pos_offset *= 2;
    const u8* pos_ptr = song->pos_data_adr + pos_offset;
    *track_num = pos_ptr[0];
    *pitch_shift = (i8)pos_ptr[1];
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool pre_song_get_track_cell(const PreSong* ps, u8 track, u8 row, PreTrackCell* cell) {
    const MySong* song = &ps->song;
    if (track == 0 || row >= song->num_steps) {
        return false;
    }
    const u8* pat_data = song->pattern_table[track - 1] + (u32)row * 3;
    u8 pitch_ctrl = pat_data[0];
    u8 inst_effect = pat_data[1];

    cell->note = pitch_ctrl & PITCH_CTRL_NOTE_MASK;
    cell->has_arpeggio = (pitch_ctrl & PITCH_CTRL_HAS_ARP) != 0;
    cell->instrument = inst_effect >> 4;
    if (pitch_ctrl & PITCH_CTRL_INST_HI) {
        cell->instrument += 16;
    }
    cell->effect_cmd = cell->has_arpeggio ? 0 : (inst_effect & 0x0F);
    cell->effect_data = pat_data[2];
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const PrePlaybackState* pre_song_get_playback_state(const PreSong* ps) {
    return &ps->playback_state;
}
