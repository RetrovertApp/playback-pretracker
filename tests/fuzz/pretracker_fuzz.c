#include "pretracker_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_INPUT_SIZE (1024u * 1024u)
#define FUZZ_MAX_SAMPLE_COUNT (1024u * 1024u)
#define FUZZ_MAX_SUBSONGS 8
#define FUZZ_DECODE_FRAMES 128

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > FUZZ_MAX_INPUT_SIZE || size > UINT32_MAX)
        return 0;

    uint8_t* mutable_data = (uint8_t*)malloc(size);
    if (!mutable_data)
        return 0;
    memcpy(mutable_data, data, size);

    SongState parsed;
    uint32_t sample_count = pretracker_parse_song(&parsed, mutable_data, (uint32_t)size, 0);
    if (sample_count == 0 || sample_count > FUZZ_MAX_SAMPLE_COUNT) {
        free(mutable_data);
        return 0;
    }
    free(mutable_data);

    PreSong* song = pre_song_create(data, (uint32_t)size);
    if (!song)
        return 0;

    const PreSongMetadata* metadata = pre_song_get_metadata(song);
    unsigned int subsongs = metadata->num_subsongs;
    if (subsongs > FUZZ_MAX_SUBSONGS)
        subsongs = FUZZ_MAX_SUBSONGS;

    float output[FUZZ_DECODE_FRAMES * 2];
    pre_song_set_sample_rate(song, PRE_MIN_SAMPLE_RATE);
    for (unsigned int subsong = 0; subsong < subsongs; ++subsong) {
        pre_song_set_subsong(song, (int)subsong);
        pre_song_start(song);
        (void)pre_song_decode(song, output, FUZZ_DECODE_FRAMES);
    }

    pre_song_destroy(song);
    return 0;
}
