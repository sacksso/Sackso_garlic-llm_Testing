// Folder to zip (see zipwrite.h).
#include "zipwrite.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ZW_MAX_FILES 300
#define ZW_NAME 120
#define ZW_FILE_MAX (1 << 20)

static char names[ZW_MAX_FILES][ZW_NAME];
static int nnames;
static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_of(const unsigned char* d, int n) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void collect(const char* dir, const char* sub, int depth) {
    char path[300];
    snprintf(path, sizeof(path), "%s%s%s", dir, sub[0] ? "/" : "", sub);
    DIR* d = opendir(path);
    if (!d) return;
    struct dirent* e;
    while (nnames < ZW_MAX_FILES && (e = readdir(d)) != NULL) {
        const char* nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        char rel[ZW_NAME];
        if (snprintf(rel, sizeof(rel), "%s%s%s", sub, sub[0] ? "/" : "", nm) >= (int)sizeof(rel)) continue;
        char full[420];
        snprintf(full, sizeof(full), "%s/%s", dir, rel);
        int f = open(full, O_RDONLY);
        if (f < 0) continue;
        char c;
        int ok = read(f, &c, 1) >= 0;   // a folder opens too, but read() fails on it
        close(f);
        if (ok) strcpy(names[nnames++], rel);
        else if (depth < 4) collect(dir, rel, depth + 1);
    }
    closedir(d);
}

static void put16(unsigned char* p, unsigned v) { p[0] = v & 255, p[1] = (v >> 8) & 255; }
static void put32(unsigned char* p, uint32_t v) { p[0] = v & 255, p[1] = (v >> 8) & 255, p[2] = (v >> 16) & 255, p[3] = v >> 24; }

static int wr(int f, const void* d, int n) { return write(f, d, n) == n; }

int zipwrite_dir(const char* dir, const char* zip_path, long* bytes) {
    static unsigned char buf[ZW_FILE_MAX];
    static uint32_t crcs[ZW_MAX_FILES], sizes[ZW_MAX_FILES], offs[ZW_MAX_FILES];
    nnames = 0;
    *bytes = 0;
    collect(dir, "", 0);
    if (nnames == 0) return -2;
    crc_init();
    int z = open(zip_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (z < 0) return -1;
    uint32_t pos = 0;
    int packed = 0;
    static int idx[ZW_MAX_FILES];
    for (int i = 0; i < nnames; i++) {
        char full[420];
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
        int f = open(full, O_RDONLY);
        if (f < 0) continue;
        int n = 0, r;
        while (n < ZW_FILE_MAX && (r = read(f, buf + n, ZW_FILE_MAX - n)) > 0) n += r;
        close(f);
        if (n >= ZW_FILE_MAX) continue;   // too big for this: skipped
        unsigned char h[30];
        int nl = (int)strlen(names[i]);
        memset(h, 0, sizeof(h));
        put32(h, 0x04034b50u);
        put16(h + 4, 20);
        put16(h + 6, 0x0800);   // UTF-8 names
        put16(h + 10, 0);       // time
        put16(h + 12, 0x0021);  // 1980-01-01
        crcs[packed] = crc32_of(buf, n);
        sizes[packed] = (uint32_t)n;
        put32(h + 14, crcs[packed]);
        put32(h + 18, n);
        put32(h + 22, n);
        put16(h + 26, nl);
        offs[packed] = pos;
        if (!wr(z, h, 30) || !wr(z, names[i], nl) || !wr(z, buf, n)) {
            close(z);
            return -1;
        }
        pos += 30 + nl + n;
        idx[packed++] = i;
    }
    uint32_t cd_start = pos;
    for (int k = 0; k < packed; k++) {
        unsigned char h[46];
        int nl = (int)strlen(names[idx[k]]);
        memset(h, 0, sizeof(h));
        put32(h, 0x02014b50u);
        put16(h + 4, 20);
        put16(h + 6, 20);
        put16(h + 8, 0x0800);
        put16(h + 14, 0x0021);
        put32(h + 16, crcs[k]);
        put32(h + 20, sizes[k]);
        put32(h + 24, sizes[k]);
        put16(h + 28, nl);
        put32(h + 42, offs[k]);
        if (!wr(z, h, 46) || !wr(z, names[idx[k]], nl)) {
            close(z);
            return -1;
        }
        pos += 46 + nl;
    }
    unsigned char e[22];
    memset(e, 0, sizeof(e));
    put32(e, 0x06054b50u);
    put16(e + 8, packed);
    put16(e + 10, packed);
    put32(e + 12, pos - cd_start);
    put32(e + 16, cd_start);
    if (!wr(z, e, 22)) {
        close(z);
        return -1;
    }
    close(z);
    *bytes = pos + 22;
    return packed;
}
