// Saved games at the level of bits (see bitscan.h).
#include "bitscan.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BITS_RAW_DIR
#define BITS_RAW_DIR "/data/garlic_files/raw"
#endif
#define KEEP_RAW 6   // cache files kept (a comparison needs two at the same time)
#define BLK 65536

typedef unsigned long long u64;

static int rd(int fd, long off, void* b, int n) {
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    int got = 0;
    while (got < n) {
        int r = read(fd, (char*)b + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

static long fsize(int fd) {
    long n = lseek(fd, 0, SEEK_END);
    return n;
}

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n--) h = (h ^ *b++) * 0x100000001b3ull;
    return h;
}

// ---- the cache of extracted entries ----------------------------------------------------------------------------

static void prune_raw(int keep) {
    DIR* d = opendir(BITS_RAW_DIR);
    if (!d) return;
    struct {
        char name[64];
        long long t;
    } L[64];
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) && n < 64) {
        size_t l = strlen(de->d_name);
        if (l < 5 || l > 40 || strcmp(de->d_name + l - 4, ".raw")) continue;
        char p[160];
        snprintf(p, sizeof(p), "%s/%.40s", BITS_RAW_DIR, de->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        snprintf(L[n].name, sizeof(L[n].name), "%.40s", de->d_name);
        L[n].t = (long long)st.st_mtime;
        n++;
    }
    closedir(d);
    while (n > keep) {
        int oldest = 0;
        for (int i = 1; i < n; i++)
            if (L[i].t < L[oldest].t) oldest = i;
        char p[160];
        snprintf(p, sizeof(p), "%s/%s", BITS_RAW_DIR, L[oldest].name);
        unlink(p);
        L[oldest] = L[--n];
    }
}

int bits_raw(const char* zip, const ZipEnt* e, char* raw, int max) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, zip, strlen(zip));
    h = fnv(h, e->name, strlen(e->name));
    uint32_t k[4] = {e->usize, e->csize, e->off, e->method};
    h = fnv(h, k, sizeof(k));
    struct stat st;
    long zs = stat(zip, &st) ? 0 : (long)st.st_size;
    h = fnv(h, &zs, sizeof(zs));
    snprintf(raw, max, "%s/%016llx.raw", BITS_RAW_DIR, (unsigned long long)h);
    if (!stat(raw, &st) && (uint32_t)st.st_size == e->usize) {   // already there: touch it (it was just used)
        int f = open(raw, O_RDWR);
        if (f >= 0) {
            unsigned char c;
            if (e->usize && rd(f, 0, &c, 1) == 1) pwrite(f, &c, 1, 0);
            close(f);
        }
        return 0;
    }
    mkdir("/data/garlic_files", 0777);
    mkdir(BITS_RAW_DIR, 0777);
    prune_raw(KEEP_RAW - 1);
    return zip_extract(zip, e, raw);
}

// ---- reading bits ----------------------------------------------------------------------------------------------

static u64 load64le(const uint8_t* p) {
    u64 x = 0;
    for (int i = 7; i >= 0; i--) x = x << 8 | p[i];
    return x;
}
static u64 load64be(const uint8_t* p) {
    u64 x = 0;
    for (int i = 0; i < 8; i++) x = x << 8 | p[i];
    return x;
}

// bit number idx (counted from the start of buf, in the order's numbering) of buf.
static int getbit(const uint8_t* buf, long idx, int msb) {
    int k = (int)(idx & 7);
    return (buf[idx >> 3] >> (msb ? 7 - k : k)) & 1;
}
// width bits starting at stream bit idx0 as a number (LSB-first: the first bit is the lowest).
static u64 getfield(const uint8_t* buf, long idx0, int width, int msb) {
    u64 v = 0;
    for (int j = 0; j < width; j++) {
        u64 b = (u64)getbit(buf, idx0 + j, msb);
        v |= msb ? b << (width - 1 - j) : b << j;
    }
    return v;
}

// ---- the report ------------------------------------------------------------------------------------------------

typedef struct {
    long from, to;   // byte range (to exclusive)
    char cls;        // Z zeros, L sparse/structured, M dense structured, H random-like
    double ent;
    int zeros;
} Zone;

static const char* const MAGICS[] = {"BNRY", "FBCHUNKS", "LTLE", "PK\x03\x04", "\x89PNG", "SQLite format 3", "RIFF", "OggS",
                                     "%PDF", "\x1f\x8b\x08", "<?xml", "GVAS", "SAVE", NULL};

static int alpha_ok(const char* s, int n) {
    int a = 0;
    for (int i = 0; i < n; i++)
        if ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == ' ') a++;
    return a * 10 >= n * 6;
}

int bits_report(const char* raw, const char* label, char* out, int max) {
    int fd = open(raw, O_RDONLY);
    if (fd < 0) return snprintf(out, max, "Cannot read %s.\n", label);
    long n = fsize(fd);
    static uint8_t buf[BLK + 32];
    Zone zones[64];
    int nz = 0;
    long first_nz = -1, last_nz = -1, zero_total = 0;
    int nblocks = 0, hblocks = 0, nzblocks = 0;
    // strings
    long nstr = 0;
    long srun_start = -1;
    int srun_len = 0;
    char sbuf[40];
    struct {
        long at;
        char t[28];
    } samples[10];
    int ns = 0;
    // signatures
    struct {
        const char* name;
        long at;
    } sigs[12];
    int nsig = 0;
    unsigned char first[256] = {0};
    for (int m = 0; MAGICS[m]; m++) first[(unsigned char)MAGICS[m][0]] = 1;
    long sig_count = 0;
    for (long base = 0; base < n; base += BLK) {
        int len = rd(fd, base, buf, (int)(n - base < BLK + 16 ? n - base : BLK + 16));
        if (len <= 0) break;
        int blen = len < BLK ? len : BLK;
        memset(buf + len, 0, 16);
        long hist[256] = {0};
        for (int i = 0; i < blen; i++) {
            uint8_t c = buf[i];
            hist[c]++;
            if (c) {
                if (first_nz < 0) first_nz = base + i;
                last_nz = base + i;
            }
            if (c >= 32 && c < 127) {
                if (srun_start < 0) srun_start = base + i, srun_len = 0;
                if (srun_len < 39) sbuf[srun_len] = (char)c;
                srun_len++;
            } else {
                if (srun_start >= 0 && srun_len >= 6) {
                    int l = srun_len < 39 ? srun_len : 39;
                    if (alpha_ok(sbuf, l)) {
                        nstr++;
                        if (ns < 10) {
                            int dup = 0;
                            for (int k = 0; k < ns; k++)
                                if (!strncmp(samples[k].t, sbuf, l < 27 ? l : 27)) dup = 1;
                            if (!dup) {
                                samples[ns].at = srun_start;
                                snprintf(samples[ns].t, sizeof(samples[ns].t), "%.*s", l < 27 ? l : 27, sbuf);
                                ns++;
                            }
                        }
                    }
                }
                srun_start = -1;
            }
            if (first[c]) {
                for (int m = 0; MAGICS[m]; m++) {
                    int ml = (int)strlen(MAGICS[m]);
                    if (c == (uint8_t)MAGICS[m][0] && i + ml <= len && !memcmp(buf + i, MAGICS[m], ml)) {
                        sig_count++;
                        if (nsig < 12) sigs[nsig].name = MAGICS[m], sigs[nsig++].at = base + i;
                    }
                }
            }
        }
        double ent = 0;
        for (int v = 0; v < 256; v++)
            if (hist[v]) {
                double p = (double)hist[v] / blen;
                ent -= p * (log(p) / log(2.0));
            }
        int zeros = (int)(hist[0] * 100 / blen);
        zero_total += hist[0];
        char cls = zeros >= 98 ? 'Z' : ent >= 7.2 ? 'H' : ent >= 5.0 ? 'M' : 'L';
        nblocks++;
        if (cls != 'Z') nzblocks++;
        if (cls == 'H') hblocks++;
        if (nz > 0 && zones[nz - 1].cls == cls && zones[nz - 1].to == base) {
            Zone* z = &zones[nz - 1];
            z->ent = (z->ent * (z->to - z->from) + ent * blen) / (z->to - z->from + blen);
            z->zeros = (int)((z->zeros * (long)(z->to - z->from) + zeros * (long)blen) / (z->to - z->from + blen));
            z->to = base + blen;
        } else if (nz < 64) {
            zones[nz].from = base, zones[nz].to = base + blen, zones[nz].cls = cls, zones[nz].ent = ent, zones[nz].zeros = zeros;
            nz++;
        }
    }
    close(fd);
    // a trailing string
    int o = snprintf(out, max, "%s: binary file, %ld bytes (0x%lX).\n", label, n, n);
    if (first_nz < 0) return o + snprintf(out + o, max - o, "It is all zeros.\n");
    o += snprintf(out + o, max - o, "Data from 0x%lX to 0x%lX; %ld%% of the bytes are zeros%s.\n", first_nz, last_nz, zero_total * 100 / n,
                  last_nz + 1 < n - 4096 ? " (everything after the data is zeros)" : "");
    o += snprintf(out + o, max - o, "Zones by 64 KB blocks (Z zeros, L sparse, M dense but structured, H random-like):\n");
    for (int i = 0; i < nz && i < 14 && o < max - 400; i++)
        o += snprintf(out + o, max - o, "  0x%lX-0x%lX %c (%.1f bits/byte, %d%% zeros)\n", zones[i].from, zones[i].to - 1, zones[i].cls, zones[i].ent, zones[i].zeros);
    if (nz > 14) o += snprintf(out + o, max - o, "  ... %d more zones\n", nz - 14);
    o += snprintf(out + o, max - o, "Plain text: %ld strings", nstr);
    for (int i = 0; i < ns && o < max - 300; i++) o += snprintf(out + o, max - o, "%s 0x%lX:\"%s\"", i ? "," : ";", samples[i].at, samples[i].t);
    o += snprintf(out + o, max - o, "\n");
    if (nsig) {
        o += snprintf(out + o, max - o, "Signatures (%ld):", sig_count);
        for (int i = 0; i < nsig && o < max - 200; i++) o += snprintf(out + o, max - o, " %s@0x%lX", sigs[i].name, sigs[i].at);
        o += snprintf(out + o, max - o, "\n");
    }
    if (nstr >= 20 && hblocks * 5 <= nzblocks)
        o += snprintf(out + o, max - o, "Verdict: NOT encrypted or compressed as a whole (it has plain text and only %d of %d data blocks look random). Numbers can be edited, but they are probably packed in bits, not whole bytes: search them with @zip:file?bits=NUMBER or compare two saves with @before.zip~after.zip?OLD/NEW.\n", hblocks, nzblocks);
    else if (hblocks * 10 >= nzblocks * 8 && nzblocks > 0)
        o += snprintf(out + o, max - o, "Verdict: almost all of it looks random: compressed or encrypted. Editing numbers by offset will not work.\n");
    else
        o += snprintf(out + o, max - o, "Verdict: mixed (%d of %d data blocks look random): part of it may be compressed or checksummed.\n", hblocks, nzblocks);
    o += snprintf(out + o, max - o, "Next: @zip:file?bits=NUMBER[+0xOFFSET] (bit-level search) | @zip:file:0x100-0x1ff (hex) | @before.zip~after.zip (what changed, read as bit fields) | /checksum OFFSET FILE.\n");
    return o;
}

// ---- search at every bit ---------------------------------------------------------------------------------------

typedef struct {
    long idx;   // stream bit index
} Hit;

int bits_find(const char* raw, const char* label, const char* spec, char* out, int max) {
    char num[48];
    long near = -1;
    const char* at = strpbrk(spec, "@+,");   // (an @token cannot hold '@': +0x3A41F is the form the attachments use)
    int nl = at ? (int)(at - spec) : (int)strlen(spec);
    snprintf(num, sizeof(num), "%.*s", nl < 47 ? nl : 47, spec);
    char* end;
    unsigned long long v = strtoull(num, &end, 0);
    if (end == num || *end || v == 0 || v > 0xFFFFFFFFull) return -2;
    if (at) {
        near = strtol(at + 1, &end, 0);
        if (end == at + 1) near = -1;
    }
    int m = 0;
    while (m < 40 && (v >> m)) m++;
    int fd = open(raw, O_RDONLY);
    if (fd < 0) return -1;
    long n = fsize(fd);
    static Hit hits[2][40];
    long count[2] = {0, 0};
    u64 mask = (m >= 64) ? ~0ull : ((1ull << m) - 1);
    static uint8_t buf[BLK + 32];
    for (long base = 0; base < n; base += BLK) {
        long lo = base, hi = base + BLK;
        if (near >= 0 && (hi < near - 70 || lo > near + 70)) continue;
        int len = rd(fd, base, buf, (int)(n - base < BLK + 16 ? n - base : BLK + 16));
        if (len <= 0) break;
        int blen = len < BLK ? len : BLK;
        memset(buf + len, 0, 16);
        for (int i = 0; i < blen; i++) {
            long byte = base + i;
            if (near >= 0 && (byte < near - 64 || byte > near + 64)) continue;
            u64 x = load64le(buf + i);
            if (x)
                for (int s = 0; s < 8; s++)
                    if (((x >> s) & mask) == v) {
                        if (count[0] < 40) hits[0][count[0]].idx = byte * 8 + s;
                        count[0]++;
                    }
            u64 y = load64be(buf + i);
            if (y)
                for (int s = 0; s < 8; s++)
                    if (((y >> (64 - s - m)) & mask) == v) {
                        if (count[1] < 40) hits[1][count[1]].idx = byte * 8 + s;
                        count[1]++;
                    }
        }
    }
    double noise = (double)n * 8.0 / (double)(1ull << m);
    int o = snprintf(out, max, "Bit-level search for %llu (needs %d bits) in %s%s:\n", v, m, label, near >= 0 ? " near the given offset" : "");
    for (int ord = 0; ord < 2; ord++) {
        int msb = ord;
        o += snprintf(out + o, max - o, "%s: %ld hit(s)", msb ? "MSB-first" : "LSB-first", count[ord]);
        if (near < 0) o += snprintf(out + o, max - o, " (a random file would give ~%.0f)", noise);
        o += snprintf(out + o, max - o, "%s\n", count[ord] > 12 ? "; only 12 shown, too common to tell which is real" : "");
        for (int k = 0; k < count[ord] && k < 12 && o < max - 220; k++) {
            long idx = hits[ord][k].idx;
            // zero bits that could be part of the same field (leading zeros): after the pattern (LSB-first) or before it (MSB-first)
            uint8_t win[16];
            long wb = idx / 8 - 8 < 0 ? 0 : idx / 8 - 8;
            memset(win, 0, sizeof(win));
            rd(fd, wb, win, 16);
            long wi = idx - wb * 8;   // the pattern's first bit inside win
            int ext = 0;
            for (int z = 0; z < 32 - m; z++) {
                long q = msb ? wi - 1 - z : wi + m + z;
                if (q < 0 || q >= 128 || getbit(win, q, msb)) break;
                ext++;
            }
            o += snprintf(out + o, max - o, "  byte 0x%lX bit %ld: %d bits, or up to %d with the zeros %s it\n", idx / 8, idx % 8, m, m + ext, msb ? "before" : "after");
        }
    }
    close(fd);
    o += snprintf(out + o, max - o, "\"bit k\" counts from the %s of the byte. A hit is only a candidate: to be sure, change the value in the game, save again and compare with @before.zip~after.zip.\n",
                  "lowest bit (LSB-first) or the highest bit (MSB-first)");
    return o;
}

// ---- compare two files -----------------------------------------------------------------------------------------

typedef struct {
    long s, e;
} Cl;
static Cl clusters[4096];

static int cl_cmp(const void* a, const void* b) {
    const Cl* x = (const Cl*)a;
    const Cl* y = (const Cl*)b;
    long sx = x->e - x->s, sy = y->e - y->s;
    if (sx != sy) return sx < sy ? -1 : 1;
    return x->s < y->s ? -1 : x->s > y->s;
}

int bits_diff(const char* rawa, const char* rawb, const char* label, const char* hint, char* out, int max) {
    int fa = open(rawa, O_RDONLY), fb = open(rawb, O_RDONLY);
    if (fa < 0 || fb < 0) {
        if (fa >= 0) close(fa);
        if (fb >= 0) close(fb);
        return -1;
    }
    long na = fsize(fa), nb = fsize(fb);
    long m = na < nb ? na : nb;
    static uint8_t A[BLK], B[BLK];
    int ncl = 0;
    long total_cl = 0, diffbytes = 0, last_nz = 0;
    long cs = -1, clast = -1;
    for (long base = 0; base < m; base += BLK) {
        int len = (int)(m - base < BLK ? m - base : BLK);
        if (rd(fa, base, A, len) != len || rd(fb, base, B, len) != len) break;
        for (int i = 0; i < len; i++) {
            if (A[i] | B[i]) last_nz = base + i;
            if (A[i] == B[i]) continue;
            long p = base + i;
            diffbytes++;
            if (cs >= 0 && p - clast <= 3) {
                clast = p;
                continue;
            }
            if (cs >= 0) {
                total_cl++;
                if (ncl < 4096) clusters[ncl].s = cs, clusters[ncl++].e = clast;
            }
            cs = p, clast = p;
        }
    }
    if (cs >= 0) {
        total_cl++;
        if (ncl < 4096) clusters[ncl].s = cs, clusters[ncl++].e = clast;
    }
    int o = snprintf(out, max, "Bit-level compare of %s: ", label);
    if (na != nb) o += snprintf(out + o, max - o, "the size changed %ld -> %ld bytes (not a fixed layout); the first %ld bytes were compared. ", na, nb, m);
    if (total_cl == 0) {
        close(fa), close(fb);
        return o + snprintf(out + o, max - o, "no byte differs.\n");
    }
    o += snprintf(out + o, max - o, "%ld changed spot(s), %ld bytes in all.\n", total_cl, diffbytes);
    qsort(clusters, ncl, sizeof(Cl), cl_cmp);
    if (hint && hint[0]) {   // the user knows the old and the new value: find the exact field
        char* gt = NULL;
        unsigned long long hv_old = strtoull(hint, &gt, 0), hv_new = 0;
        gt = strpbrk(hint, ">/");   // (an @token cannot hold '>': 5000/5100 is the form the attachments use)
        int ok = gt && gt != hint && hint[0] >= '0' && hint[0] <= '9';
        if (ok) hv_old = strtoull(hint, NULL, 0);
        char* end = NULL;
        if (ok) hv_new = strtoull(gt + 1, &end, 0), ok = end && end != gt + 1 && !*end && hv_old != hv_new && hv_old <= 0xFFFFFFFFull && hv_new <= 0xFFFFFFFFull;
        if (!ok) o += snprintf(out + o, max - o, "(\"%s\" is not OLD/NEW, e.g. 5000/5100: ignored.)\n", hint);
        else {
            int wmin = 1;
            while (wmin < 32 && ((hv_old | hv_new) >> wmin)) wmin++;
            int matches = 0;
            long seen_at[8];   // (the same field with more zeros in front is not another candidate)
            int seen_ord[8];
            for (int c = 0; c < ncl && matches < 8; c++) {
                long s = clusters[c].s, e = clusters[c].e;
                if (e - s + 1 > 8) continue;
                long w0 = s < 5 ? 0 : s - 5;   // a window of the two files around the spot
                uint8_t pa[24], pb[24];
                memset(pa, 0, sizeof(pa)), memset(pb, 0, sizeof(pb));
                int wl = (int)(e - w0 + 6 < 24 ? e - w0 + 6 : 24);
                rd(fa, w0, pa, wl), rd(fb, w0, pb, wl);
                for (int ord = 0; ord < 2 && matches < 8; ord++) {
                    long lo = -1, hi = -1;
                    for (long k = s; k <= e; k++) {
                        int x = pa[k - w0] ^ pb[k - w0];
                        for (int bit = 0; bit < 8; bit++)
                            if (x >> bit & 1) {
                                long idx = (k - w0) * 8 + (ord ? 7 - bit : bit);
                                if (lo < 0 || idx < lo) lo = idx;
                                if (idx > hi) hi = idx;
                            }
                    }
                    if (lo < 0) continue;
                    for (int w = wmin; w <= 32 && matches < 8; w++)
                        for (long f = hi - w + 1 > 0 ? hi - w + 1 : 0; f <= lo && matches < 8; f++) {
                            if (f + w > (long)wl * 8) continue;
                            if (getfield(pa, f, w, ord) == hv_old && getfield(pb, f, w, ord) == hv_new) {
                                int dup = 0;
                                for (int q = 0; q < matches; q++)
                                    if (seen_ord[q] == ord && seen_at[q] == w0 * 8 + (ord ? f : f)) dup = 1;
                                if (dup) continue;
                                seen_ord[matches] = ord, seen_at[matches] = w0 * 8 + f;
                                o += snprintf(out + o, max - o, "FIELD FOUND (%s): byte 0x%lX bit %ld, %d bits: %llu -> %llu\n", ord ? "MSB-first" : "LSB-first", w0 + f / 8, f % 8, w, hv_old, hv_new);
                                matches++;
                            }
                        }
                }
            }
            if (!matches)
                o += snprintf(out + o, max - o, "No changed spot reads as %llu -> %llu as a bit field of up to 32 bits (either bit order). The game may store a scaled number (x10, x100, a percentage), a float, or the spots below belong to another change.\n", hv_old, hv_new);
            else if (matches > 1)
                o += snprintf(out + o, max - o, "%d candidates: if the value appears in two places, one may be a display copy; change the other value or repeat with another number to tell them apart.\n", matches);
        }
    }
    int shown = 0;
    for (int c = 0; c < ncl && shown < 22 && o < max - 420; c++) {
        long s = clusters[c].s, e = clusters[c].e, len = e - s + 1;
        uint8_t pa[16], pb[16];
        memset(pa, 0, sizeof(pa)), memset(pb, 0, sizeof(pb));
        int rl = len < 12 ? (int)len : 12;
        rd(fa, s, pa, rl), rd(fb, s, pb, rl);
        o += snprintf(out + o, max - o, "  0x%lX (%ld byte%s):", s, len, len == 1 ? "" : "s");
        for (int k = 0; k < rl && k < 8; k++) o += snprintf(out + o, max - o, " %02X", pa[k]);
        o += snprintf(out + o, max - o, " ->");
        for (int k = 0; k < rl && k < 8; k++) o += snprintf(out + o, max - o, " %02X", pb[k]);
        if (len <= 8) {
            for (int ord = 0; ord < 2; ord++) {
                long lo = -1, hi = -1;
                for (int k = 0; k < len; k++) {
                    int x = pa[k] ^ pb[k];
                    for (int bit = 0; bit < 8; bit++)
                        if (x >> bit & 1) {
                            long idx = (long)k * 8 + (ord ? 7 - bit : bit);
                            if (lo < 0 || idx < lo) lo = idx;
                            if (idx > hi) hi = idx;
                        }
                }
                int w = (int)(hi - lo + 1);
                if (w <= 32) {
                    o += snprintf(out + o, max - o, "  | %s: %d bit%s at byte 0x%lX bit %ld: %llu -> %llu", ord ? "MSB-first" : "LSB-first", w, w == 1 ? "" : "s", s + lo / 8, lo % 8,
                                  getfield(pa, lo, w, ord), getfield(pb, lo, w, ord));
                }
            }
        }
        if (s < 256) o += snprintf(out + o, max - o, "  [start of the file: counter or checksum?]");
        else if (s >= last_nz - 63) o += snprintf(out + o, max - o, "  [end of the data: counter or checksum?]");
        o += snprintf(out + o, max - o, "\n");
        shown++;
    }
    if (total_cl > shown && o < max - 300)
        o += snprintf(out + o, max - o, "  ... %ld more spots%s\n", total_cl - shown, ncl < total_cl ? " (not all kept)" : "");
    if (total_cl > 200 && o < max - 300)
        o += snprintf(out + o, max - o, "Many spots changed: the game rewrites a lot every time it saves (time, progress, simulation). Make the smallest change you can in the game between the two saves, do nothing else, and save at once.\n");
    else if (o < max - 400)
        o += snprintf(out + o, max - o, "The smallest spots come first. A spot whose old and new values match what the game showed (e.g. 5000 -> 5100) is the field; the width is only the minimum: check with @zip:file?bits=OLDVALUE+0xOFFSET. Spots at the start or end of the file that change every time are checksums or counters: /checksum OFFSET FILE.\n");
    close(fa), close(fb);
    return o;
}

// ---- checksum identification -----------------------------------------------------------------------------------

static uint32_t crc_tab[256];
static void crc_init(void) {
    if (crc_tab[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}

typedef struct {
    uint32_t crc, crc_nx, adl_a, adl_b, sum8, sumw, xor8;
    uint8_t w[4];
    int wn;
} Acc;

static void acc_init(Acc* a) {
    memset(a, 0, sizeof(*a));
    a->crc = a->crc_nx = 0xFFFFFFFFu;
    a->adl_a = 1;
}
static void acc_feed(Acc* a, const uint8_t* p, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t c = p[i];
        a->crc = crc_tab[(a->crc ^ c) & 0xFF] ^ (a->crc >> 8);
        a->crc_nx = crc_tab[(a->crc_nx ^ c) & 0xFF] ^ (a->crc_nx >> 8);
        a->adl_a = (a->adl_a + c) % 65521u;
        a->adl_b = (a->adl_b + a->adl_a) % 65521u;
        a->sum8 += c;
        a->xor8 ^= c;
        a->w[a->wn++] = c;
        if (a->wn == 4) {
            a->sumw += (uint32_t)a->w[0] | (uint32_t)a->w[1] << 8 | (uint32_t)a->w[2] << 16 | (uint32_t)a->w[3] << 24;
            a->wn = 0;
        }
    }
}

int bits_checksum(const char* raw, long field, char* out, int max) {
    int fd = open(raw, O_RDONLY);
    if (fd < 0) return -1;
    long n = fsize(fd);
    uint8_t st[4];
    if (field < 0 || field + 4 > n || rd(fd, field, st, 4) != 4) {
        close(fd);
        return snprintf(out, max, "Offset 0x%lX is outside the file (%ld bytes).\n", field, n);
    }
    crc_init();
    uint32_t le = st[0] | st[1] << 8 | (uint32_t)st[2] << 16 | (uint32_t)st[3] << 24;
    uint32_t be = (uint32_t)st[0] << 24 | (uint32_t)st[1] << 16 | st[2] << 8 | st[3];
    long nz = n;
    {   // the end of the data (the rest is zeros)
        static uint8_t buf[BLK];
        nz = 0;
        for (long base = 0; base < n; base += BLK) {
            int len = (int)(n - base < BLK ? n - base : BLK);
            if (rd(fd, base, buf, len) != len) break;
            for (int i = len - 1; i >= 0; i--)
                if (buf[i]) {
                    if (base + i + 1 > nz) nz = base + i + 1;
                    break;
                }
        }
    }
    struct {
        long a, b;
        const char* name;
    } R[] = {{0, n, "the whole file (the field zeroed)"}, {0, field, "from the start up to the field"}, {field + 4, n, "after the field to the end"},
             {0, nz, "the data (up to the last nonzero byte, the field zeroed)"}, {field + 4, nz, "after the field to the end of the data"}};
    int o = snprintf(out, max, "Checksum at 0x%lX: the stored 4 bytes are %02X %02X %02X %02X (little endian %u = 0x%08X, big endian 0x%08X).\n", field, st[0], st[1], st[2], st[3], le, le, be);
    int found = 0;
    static uint8_t buf[BLK];
    for (unsigned r = 0; r < sizeof(R) / sizeof(R[0]); r++) {
        long a = R[r].a, b = R[r].b;
        if (b > n) b = n;
        if (a >= b || (r == 3 && nz == n)) continue;
        Acc ac;
        acc_init(&ac);
        for (long p = a; p < b;) {
            int len = (int)(b - p < BLK ? b - p : BLK);
            if (rd(fd, p, buf, len) != len) break;
            if (field + 4 > p && field < p + len)   // the field itself counts as zeros when it is inside the range
                for (long q = field; q < field + 4; q++)
                    if (q >= p && q < p + len && (r == 0 || r == 3)) buf[q - p] = 0;
            acc_feed(&ac, buf, len);
            p += len;
        }
        struct {
            const char* name;
            uint32_t v;
        } V[] = {{"CRC32", ~ac.crc}, {"CRC32 without the final inversion", ac.crc_nx}, {"Adler32", ac.adl_b << 16 | ac.adl_a},
                 {"sum of the bytes", ac.sum8}, {"sum of the 32-bit words", ac.sumw}, {"xor of the bytes", ac.xor8}};
        for (unsigned k = 0; k < sizeof(V) / sizeof(V[0]); k++)
            if (V[k].v == le || V[k].v == be) {
                o += snprintf(out + o, max - o, "  MATCH: %s over %s (0x%08X, stored %s endian).\n", V[k].name, R[r].name, V[k].v, V[k].v == le ? "little" : "big");
                found++;
            }
    }
    if (!found)
        o += snprintf(out + o, max - o, "No match among CRC32, Adler32 and sums over 5 ranges. The game may use another algorithm (CRC16, Murmur, MD5/SHA, a keyed hash) or this is not a checksum; the stored value is only a counter if it changes by small steps between saves.\n");
    else
        o += snprintf(out + o, max - o, "After editing, this value must be recomputed over the new bytes (not done automatically yet).\n");
    close(fd);
    return o;
}

// ---- patched copies --------------------------------------------------------------------------------------------

static int copy_file(const char* src, const char* dst) {
    int a = open(src, O_RDONLY);
    if (a < 0) return -1;
    int b = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (b < 0) {
        close(a);
        return -1;
    }
    static uint8_t buf[BLK * 4];
    int bad = 0;
    for (;;) {
        int r = read(a, buf, sizeof(buf));
        if (r < 0) bad = 1;
        if (r <= 0) break;
        int w = 0;
        while (w < r) {
            int x = write(b, buf + w, r - w);
            if (x <= 0) {
                bad = 1;
                break;
            }
            w += x;
        }
        if (bad) break;
    }
    close(a);
    if (close(b)) bad = 1;
    if (bad) unlink(dst);
    return bad ? -1 : 0;
}

int bits_patch(const char* rawin, const char* rawout, long byteoff, int bit, int width, unsigned long long value, int msb, unsigned long long* old) {
    if (width < 1 || width > 64 || bit < 0 || bit > 7) return -2;
    if (width < 64 && (value >> width)) return -2;
    int nbytes = (bit + width + 7) / 8;
    if (copy_file(rawin, rawout)) return -1;
    int fd = open(rawout, O_RDWR);
    if (fd < 0) return -1;
    long n = fsize(fd);
    if (byteoff < 0 || byteoff + nbytes > n) {
        close(fd);
        unlink(rawout);
        return -3;
    }
    uint8_t b[16];
    if (rd(fd, byteoff, b, nbytes) != nbytes) {
        close(fd);
        unlink(rawout);
        return -1;
    }
    *old = getfield(b, bit, width, msb);
    for (int j = 0; j < width; j++) {
        long idx = bit + j;
        int k = (int)(idx & 7);
        int sh = msb ? 7 - k : k;
        int vb = (int)(msb ? (value >> (width - 1 - j)) & 1 : (value >> j) & 1);
        b[idx >> 3] = (uint8_t)((b[idx >> 3] & ~(1 << sh)) | (vb << sh));
    }
    int ok = pwrite(fd, b, nbytes, byteoff) == nbytes;
    if (close(fd)) ok = 0;
    if (!ok) unlink(rawout);
    return ok ? 0 : -1;
}

int bits_patch_bytes(const char* rawin, const char* rawout, long off, const unsigned char* bytes, int n) {
    if (copy_file(rawin, rawout)) return -1;
    int fd = open(rawout, O_RDWR);
    if (fd < 0) return -1;
    long sz = fsize(fd);
    if (off < 0 || off + n > sz) {
        close(fd);
        unlink(rawout);
        return -3;
    }
    int ok = pwrite(fd, bytes, n, off) == n;
    if (close(fd)) ok = 0;
    if (!ok) unlink(rawout);
    return ok ? 0 : -1;
}

// ---- writing a zip ---------------------------------------------------------------------------------------------

static void p16(uint8_t* p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t* p, unsigned v) { p16(p, v & 0xFFFF), p16(p + 2, v >> 16); }

static int wr(int fd, const void* d, long n) {
    const uint8_t* b = (const uint8_t*)d;
    while (n > 0) {
        int w = write(fd, b, (size_t)n);
        if (w <= 0) return -1;
        b += w, n -= w;
    }
    return 0;
}

typedef struct {
    int fd, bad;
    uint32_t crc;
    long len;
} WCtx;

static int w_sink(void* c, const uint8_t* p, int n) {
    WCtx* w = (WCtx*)c;
    for (int i = 0; i < n; i++) w->crc = crc_tab[(w->crc ^ p[i]) & 0xFF] ^ (w->crc >> 8);
    w->len += n;
    if (wr(w->fd, p, n)) {
        w->bad = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    int fd, bad;
    long pos;
} CCtx;

static int c_sink(void* c, const uint8_t* p, int n) {   // compares the stream with a file
    CCtx* x = (CCtx*)c;
    static uint8_t buf[BLK];
    while (n > 0) {
        int k = n < BLK ? n : BLK;
        if (rd(x->fd, x->pos, buf, k) != k || memcmp(buf, p, (size_t)k)) {
            x->bad = 1;
            return 1;
        }
        x->pos += k, p += k, n -= k;
    }
    return 0;
}

typedef struct {
    char name[ZIP_NAME_MAX];
    uint32_t crc, size, off;
} Rec;

int bits_zip_replace(const char* src, const char* dst, const char* entry, const char* newraw, char* msg, int msgmax) {
    crc_init();
    if (!strcmp(src, dst)) return snprintf(msg, msgmax, "Refusing to overwrite the original."), -1;
    const ZipEnt* ents;
    int total;
    int n = zip_open_dir(src, &ents, &total);
    if (n < 0) return snprintf(msg, msgmax, "Cannot read %s.", src), -1;
    if (n < total) return snprintf(msg, msgmax, "The zip has too many entries (%d); not supported.", total), -1;
    ZipEnt* E = (ZipEnt*)malloc(sizeof(ZipEnt) * (n > 0 ? n : 1));
    Rec* R = (Rec*)malloc(sizeof(Rec) * (n > 0 ? n : 1));
    if (!E || !R) {
        free(E), free(R);
        return snprintf(msg, msgmax, "Out of memory."), -1;
    }
    memcpy(E, ents, sizeof(ZipEnt) * n);
    int tidx = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(E[i].name, entry)) tidx = i;
    int nfd = open(newraw, O_RDONLY);
    if (tidx < 0 || nfd < 0) {
        if (nfd >= 0) close(nfd);
        free(E), free(R);
        return snprintf(msg, msgmax, "%s is not in the zip.", entry), -1;
    }
    long nsize = fsize(nfd);
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        close(nfd), free(E), free(R);
        return snprintf(msg, msgmax, "Cannot create %s.", dst), -1;
    }
    long pos = 0;
    int fail = 0;
    char why[260] = "";
    for (int i = 0; i < n && !fail; i++) {
        if (E[i].method == 0xFFFF) {
            snprintf(why, sizeof(why), "%.100s cannot be copied (encrypted or unsupported).", E[i].name);
            fail = 1;
            break;
        }
        uint8_t h[30];
        int nl = (int)strlen(E[i].name);
        long hdr = pos;
        memset(h, 0, sizeof(h));
        p32(h, 0x04034b50), p16(h + 4, 20), p16(h + 6, 0x0800), p16(h + 8, 0), p16(h + 12, ((2026 - 1980) << 9) | (1 << 5) | 1);
        p16(h + 26, nl);
        if (wr(fd, h, 30) || wr(fd, E[i].name, nl)) {
            snprintf(why, sizeof(why), "Write error (disk full?).");
            fail = 1;
            break;
        }
        WCtx w = {fd, 0, 0xFFFFFFFFu, 0};
        if (i == tidx) {   // the new contents, from the file
            static uint8_t buf[BLK];
            for (long p = 0; p < nsize && !w.bad;) {
                int k = (int)(nsize - p < BLK ? nsize - p : BLK);
                if (rd(nfd, p, buf, k) != k) {
                    w.bad = 1;
                    break;
                }
                w_sink(&w, buf, k);
                p += k;
            }
        } else if (E[i].usize) {
            long got = zip_stream(src, &E[i], w_sink, &w);
            if (got != (long)E[i].usize || w.len != (long)E[i].usize) w.bad = 1;
        }
        if (w.bad) {
            snprintf(why, sizeof(why), "%.100s cannot be %s.", E[i].name, i == tidx ? "written" : "read (damaged or unsupported)");
            fail = 1;
            break;
        }
        uint32_t crc = ~w.crc;
        uint8_t fix[12];
        p32(fix, crc), p32(fix + 4, (uint32_t)w.len), p32(fix + 8, (uint32_t)w.len);
        if (pwrite(fd, fix, 12, hdr + 14) != 12) {
            snprintf(why, sizeof(why), "Write error (disk full?).");
            fail = 1;
            break;
        }
        snprintf(R[i].name, sizeof(R[i].name), "%s", E[i].name);
        R[i].crc = crc, R[i].size = (uint32_t)w.len, R[i].off = (uint32_t)pos;
        pos += 30 + nl + w.len;
    }
    if (!fail) {
        long cd = pos;
        for (int i = 0; i < n && !fail; i++) {
            uint8_t h[46];
            int nl = (int)strlen(R[i].name);
            memset(h, 0, sizeof(h));
            p32(h, 0x02014b50), p16(h + 4, 20), p16(h + 6, 20), p16(h + 8, 0x0800), p16(h + 14, ((2026 - 1980) << 9) | (1 << 5) | 1);
            p32(h + 16, R[i].crc), p32(h + 20, R[i].size), p32(h + 24, R[i].size), p16(h + 28, nl), p32(h + 42, R[i].off);
            if (wr(fd, h, 46) || wr(fd, R[i].name, nl)) fail = 1;
            pos += 46 + nl;
        }
        uint8_t e[22];
        memset(e, 0, sizeof(e));
        p32(e, 0x06054b50), p16(e + 8, n), p16(e + 10, n), p32(e + 12, (uint32_t)(pos - cd)), p32(e + 16, (uint32_t)cd);
        if (wr(fd, e, 22)) fail = 1;
        if (fail) snprintf(why, sizeof(why), "Write error (disk full?).");
    }
    if (close(fd)) fail = 1;
    free(R);
    if (fail) {
        close(nfd), free(E);
        unlink(dst);
        return snprintf(msg, msgmax, "%s", why[0] ? why : "Write error."), -1;
    }
    // read it back: the entry must be the new contents
    const ZipEnt* ne;
    int nt, nn = zip_open_dir(dst, &ne, &nt);
    int ok = nn == n;
    if (ok) {
        const ZipEnt* t = NULL;
        for (int i = 0; i < nn; i++)
            if (!strcmp(ne[i].name, E[tidx].name)) t = &ne[i];
        CCtx c = {nfd, 0, 0};
        ok = t && t->usize == (uint32_t)nsize && zip_stream(dst, t, c_sink, &c) == nsize && !c.bad;
    }
    close(nfd);
    free(E);
    if (!ok) return snprintf(msg, msgmax, "The new zip failed the read-back check; do not use it (%s).", dst), -1;
    msg[0] = 0;
    return 0;
}
