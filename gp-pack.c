#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t flags;
    uint32_t entry;
    uint32_t code_size;
    uint32_t data_size;
    char name[32];
} GpHeader;

int main(int argc, char** argv) {
    FILE* out;
    const char* path;
    const char* name;
    const char* entry;
    GpHeader header;
    if (argc != 4) {
        fprintf(stderr, "usage: gp-pack output.gp Name entry\n");
        return 2;
    }
    path = argv[1]; name = argv[2]; entry = argv[3];
    out = fopen(path, "wb");
    if (!out) { perror(path); return 1; }
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "GP01", 4);
    header.version = 1;
    header.flags = 1;
    header.entry = 0x1000;
    header.code_size = (uint32_t)strlen(entry);
    header.data_size = 0;
    strncpy(header.name, name, sizeof(header.name) - 1);
    fwrite(&header, sizeof(header), 1, out);
    fwrite(entry, strlen(entry), 1, out);
    fclose(out);
    return 0;
}