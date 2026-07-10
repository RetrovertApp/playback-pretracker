#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEGACY_SIZE 0xD2
#define V15_SIZE 0x112

static void write_be32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void make_legacy_seed(uint8_t* data) {
    memset(data, 0, LEGACY_SIZE);
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

static void make_v15_seed(uint8_t* data) {
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

static int write_seed(const char* directory, const char* name,
                      const uint8_t* data, size_t size) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;

    FILE* file = fopen(path, "wb");
    if (!file)
        return 0;
    int ok = fwrite(data, 1, size, file) == size;
    ok = fclose(file) == 0 && ok;
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return EXIT_FAILURE;

    uint8_t legacy[LEGACY_SIZE];
    uint8_t v15[V15_SIZE];
    make_legacy_seed(legacy);
    make_v15_seed(v15);

    if (!write_seed(argv[1], "legacy-1b.prt", legacy, sizeof(legacy)) ||
        !write_seed(argv[1], "v15-two-subsongs.prt", v15, sizeof(v15)))
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
