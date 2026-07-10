#include "pretracker_internal.h"

#include <string.h>

bool pretracker_read_name_record(const u8** cursor, const u8* end, char* output, size_t output_size) {
    const u8* start = *cursor;
    const u8* p = start;

    while (p < end && (size_t)(p - start) <= 23) {
        if (*p == 0) {
            size_t length = (size_t)(p - start);
            if (output != NULL) {
                if (output_size == 0 || length >= output_size)
                    return false;
                memcpy(output, start, length);
                output[length] = '\0';
            }
            *cursor = p + 1;
            return true;
        }
        ++p;
    }
    return false;
}

u32 pretracker_parse_song(SongState* song, u8* prt_data, u32 prt_size, int subsong) {
    memset(song, 0, sizeof(SongState));

    if (prt_size < 0x5A)
        return 0;

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
        if (prt_size < 0x5B)
            return 0;
        // V1.5 subsong support
        u8 num_subsongs = prt_data[0x5A];
        song->num_subsongs = num_subsongs > 0 ? num_subsongs : 1;

        int sel = (subsong >= 0 && subsong < song->num_subsongs) ? subsong : 0;
        if (posd_offset > prt_size || (u32)song->num_subsongs * 8 > prt_size - posd_offset)
            return 0;

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
    if (song->num_waves > MAX_WAVES || posd_offset >= prt_size || patt_offset >= prt_size)
        return 0;

    // Skip instrument names
    u32 inst_offset = read_be32(prt_data + 0x0C);
    if (inst_offset >= prt_size)
        return 0;
    const u8* end = prt_data + prt_size;
    const u8* name_ptr = prt_data + inst_offset;
    for (int i = 0; i < max_inst_names; i++) {
        if (!pretracker_read_name_record(&name_ptr, end, NULL, 0))
            return 0;
    }
    u8* ptr = (u8*)name_ptr;

    // Parse instrument infos
    u8 num_instruments = prt_data[0x40];
    u8 actual_instruments = num_instruments;
    if (actual_instruments > MAX_INSTRUMENTS) {
        actual_instruments = MAX_INSTRUMENTS;
    }

    u8* inst_info_base = ptr;
    if ((size_t)(end - inst_info_base) < (size_t)num_instruments * sizeof(InstrumentInfo))
        return 0;
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
        if ((size_t)(end - inst_pattern_ptr) < (size_t)steps * 3)
            return 0;
        song->inst_patterns_table[i] = inst_pattern_ptr;
        inst_pattern_ptr += (u32)steps * 3;
    }

    // Skip wave names
    u32 wave_offset = read_be32(prt_data + 0x10);
    if (wave_offset >= prt_size)
        return 0;
    name_ptr = prt_data + wave_offset;
    for (int i = 0; i < MAX_WAVES; i++) {
        if (!pretracker_read_name_record(&name_ptr, end, NULL, 0))
            return 0;
    }
    ptr = (u8*)name_ptr;

    // Align to even address
    if ((uintptr_t)ptr & 1) {
        if (ptr == end)
            return 0;
        ptr++;
    }
    if ((size_t)(end - ptr) < (size_t)song->num_waves * sizeof(WaveInfo))
        return 0;
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

void pretracker_rebuild_pattern_table(SongState* song) {
    u32 step_size = (u32)song->num_steps * 3;
    u8* pattern = song->patterns_ptr;
    for (int index = 0; index < 255; index++) {
        song->pattern_table[index] = pattern;
        pattern += step_size;
    }
}
