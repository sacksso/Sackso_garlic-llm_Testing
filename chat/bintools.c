// Binary tools (see bintools.h).
#include "bintools.h"

#include "bitscan.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// ---- small helpers ---------------------------------------------------------------------------------------------

int bin_looks_binary(const unsigned char* d, int n) {
    int lim = n < 400 ? n : 400, bad = 0;
    for (int i = 0; i < lim; i++) bad += d[i] < 9 || (d[i] > 13 && d[i] < 32);
    return bad * 20 > lim;
}

int bin_parse_num(const char* s, long long* v) {
    int neg = 0;
    if (*s == '-') neg = 1, s++;
    else if (*s == '+') s++;
    if (!*s) return 0;
    unsigned long long x = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return 0;
        for (; *s; s++) {
            int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
            if (d < 0) return 0;
            x = x * 16 + d;
        }
    } else {
        for (; *s; s++) {
            if (*s < '0' || *s > '9') return 0;
            x = x * 10 + (*s - '0');
        }
    }
    *v = neg ? -(long long)x : (long long)x;
    return 1;
}

static unsigned char* load_entry(const char* zip, const ZipEnt* e, long* n, int* cut) {
    long want = e->usize;
    if (want > BIN_MAX) want = BIN_MAX;
    unsigned char* b = (unsigned char*)malloc(want + 2);
    if (!b) return NULL;
    int c = 0;
    int r = zip_read(zip, e, (char*)b, (int)want + 1, &c);
    if (r < 0) {
        free(b);
        return NULL;
    }
    *n = r;
    *cut = c || (long)e->usize > want;
    return b;
}

static const ZipEnt* find_entry(const ZipEnt* ents, int n, const char* name) {
    int nl = (int)strlen(name);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            const char* nm = ents[i].name;
            int l = (int)strlen(nm);
            if (pass == 0 ? !strcmp(nm, name) : (l > nl && nm[l - nl - 1] == '/' && !strcmp(nm + l - nl, name))) return &ents[i];
        }
    return NULL;
}

static unsigned u16le(const unsigned char* p) { return p[0] | p[1] << 8; }
static unsigned u32le(const unsigned char* p) { return p[0] | p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned long long u64le(const unsigned char* p) { return (unsigned long long)u32le(p) | (unsigned long long)u32le(p + 4) << 32; }

static int hexlines(const unsigned char* d, long n, long a, long b, char* out, int max, long base) {
    long s = a & ~15L, e = b | 15L;
    if (e >= n) e = n - 1;
    int o = 0;
    for (long row = s; row <= e && o < max - 100; row += 16) {
        o += snprintf(out + o, max - o, "%08lX ", row + base);
        for (int k = 0; k < 16; k++) {
            if (row + k < n) o += snprintf(out + o, max - o, " %02X", d[row + k]);
            else o += snprintf(out + o, max - o, "   ");
            if (k == 7) o += snprintf(out + o, max - o, " ");
        }
        o += snprintf(out + o, max - o, "  |");
        for (int k = 0; k < 16 && row + k < n; k++) out[o++] = d[row + k] >= 32 && d[row + k] < 127 ? (char)d[row + k] : '.';
        o += snprintf(out + o, max - o, "|\n");
    }
    return o;
}

// What the bytes at p can mean as numbers (the model cannot compute these).
static int meaning(const unsigned char* d, long n, long at, char* out, int max, long base) {
    int o = snprintf(out, max, "At 0x%lX: u8=%u", at + base, d[at]);
    if (at + 2 <= n) o += snprintf(out + o, max - o, "  u16le=%u", u16le(d + at));
    if (at + 4 <= n) {
        unsigned u = u32le(d + at);
        float f;
        memcpy(&f, &u, 4);
        o += snprintf(out + o, max - o, "  u32le=%u  i32le=%d  u32be=%u  f32=%g", u, (int)u, ((unsigned)d[at] << 24 | (unsigned)d[at + 1] << 16 | (unsigned)d[at + 2] << 8 | d[at + 3]), (double)f);
    }
    if (at + 8 <= n) {
        unsigned long long q = u64le(d + at);
        double dd;
        memcpy(&dd, &q, 8);
        o += snprintf(out + o, max - o, "  u64le=%llu  f64=%g", q, dd);
    }
    o += snprintf(out + o, max - o, "\n");
    return o;
}

// ---- the report and the hex dump -------------------------------------------------------------------------------

static const char* sniff(const unsigned char* d, long n) {
    if (n >= 4 && !memcmp(d, "PK\3\4", 4)) return "a zip inside (not readable here)";
    if (n >= 16 && !memcmp(d, "SQLite format 3", 15)) return "SQLite database";
    if (n >= 4 && !memcmp(d, "\0PSF", 4)) return "PS4 param.sfo (system file, not game data)";
    if (n >= 2 && d[0] == 0x1f && d[1] == 0x8b) return "gzip (compressed)";
    if (n >= 4 && !memcmp(d, "RIFF", 4)) return "RIFF (audio/video container)";
    if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return "PNG image";
    if (n >= 4 && !memcmp(d, "\x7f""ELF", 4)) return "ELF program";
    if (n >= 4 && !memcmp(d, "\x78\x9c", 2)) return "zlib stream (compressed)";
    return NULL;
}

static int distinct_bytes(const unsigned char* d, long n) {
    unsigned char seen[256];
    memset(seen, 0, sizeof(seen));
    long lim = n < 4096 ? n : 4096;
    int c = 0;
    for (long i = 0; i < lim; i++)
        if (!seen[d[i]]) seen[d[i]] = 1, c++;
    return c;
}

int bin_entry_view(const char* zip, const ZipEnt* e, long a, long b, int ranged, char* out, int max) {
    if ((long)e->usize > BITS_BIG) {   // phase 13: a big file (a saved game): from the cache file, whatever its size
        char raw[200];
        if (bits_raw(zip, e, raw, sizeof(raw)) < 0) return snprintf(out, max, "Cannot read %s (damaged or unsupported).\n", e->name);
        if (!ranged) return bits_report(raw, e->name, out, max);
        if (b < a) b = a;
        if (a >= (long)e->usize) return snprintf(out, max, "%s has %ld bytes: offset 0x%lX is past the end.\n", e->name, (long)e->usize, a);
        int clipped = b - a > 511;
        if (clipped) b = a + 511;
        long a0 = a & ~15L;
        unsigned char win[560];
        int f = open(raw, O_RDONLY), got = 0;
        if (f >= 0) {
            if (lseek(f, a0, SEEK_SET) >= 0) {
                int want = (int)(b - a0 + 16 < (long)sizeof(win) ? b - a0 + 16 : (long)sizeof(win));
                while (got < want) {
                    int r = read(f, win + got, want - got);
                    if (r <= 0) break;
                    got += r;
                }
            }
            close(f);
        }
        if (got <= 0) return snprintf(out, max, "Cannot read %s at 0x%lX.\n", e->name, a);
        int o = snprintf(out, max, "%s, bytes 0x%lX-0x%lX of %ld%s:\n", e->name, a, b < a0 + got ? b : a0 + got - 1, (long)e->usize, clipped ? " (range cut to 512 bytes)" : "");
        o += hexlines(win, got, a - a0, b - a0, out + o, max - o, a0);
        if (o < max - 260) o += meaning(win, got, a - a0, out + o, max - o, a0);
        return o;
    }
    long n = 0;
    int cut = 0;
    unsigned char* d = load_entry(zip, e, &n, &cut);
    if (!d) return snprintf(out, max, "Cannot read %s (damaged or unsupported).\n", e->name);
    int o = 0;
    if (ranged) {
        if (b < a) b = a;
        if (a >= n) {
            o = snprintf(out, max, "%s has %ld bytes: offset 0x%lX is past the end.\n", e->name, (long)e->usize, a);
            free(d);
            return o;
        }
        int clipped = 0;
        if (b - a > 511) b = a + 511, clipped = 1;
        o += snprintf(out + o, max - o, "%s, bytes 0x%lX-0x%lX of %ld%s:\n", e->name, a, b < n ? b : n - 1, (long)e->usize, clipped ? " (range cut to 512 bytes)" : "");
        o += hexlines(d, n, a, b, out + o, max - o, 0);
        if (o < max - 260) o += meaning(d, n, a, out + o, max - o, 0);
        free(d);
        return o;
    }
    o += snprintf(out + o, max - o, "%s: binary file, %ld bytes%s.\n", e->name, (long)e->usize, cut ? " (only the first part was read)" : "");
    const char* fmt = sniff(d, n);
    if (fmt) o += snprintf(out + o, max - o, "Format: %s.\n", fmt);
    int dist = distinct_bytes(d, n);
    if (n >= 2048 && dist >= 240)
        o += snprintf(out + o, max - o, "WARNING: the bytes look random (%d of 256 values in the first 4 KB): compressed or encrypted. Editing numbers by offset will not work on this file.\n", dist);
    else o += snprintf(out + o, max - o, "Structure: %d different byte values in the first 4 KB (below 240: plain data, numbers can be edited).\n", dist);
    // strings of 6+ printable characters: names of fields, items, maps...
    int ns = 0, random_like = n >= 2048 && dist >= 240;
    if (!random_like) o += snprintf(out + o, max - o, "Text inside:");
    for (long i = 0; !random_like && i < n && ns < 14 && o < max - 200;) {
        long j = i;
        while (j < n && d[j] >= 32 && d[j] < 127) j++;
        if (j - i >= 6) {
            o += snprintf(out + o, max - o, " 0x%lX:\"%.*s\"", i, (int)(j - i > 32 ? 32 : j - i), (const char*)d + i);
            ns++;
        }
        i = j + 1;
    }
    if (!random_like) o += snprintf(out + o, max - o, "%s\n", ns ? "" : " (none)");
    o += snprintf(out + o, max - o, "First bytes:\n");
    o += hexlines(d, n, 0, 159, out + o, max - o, 0);
    o += snprintf(out + o, max - o,
                  "Next: @zip:%s:0x100-0x1ff (hex of a range) | @zip?=NUMBER (where a number is stored, in every file) | @before.zip~after.zip (what changed between two saves).\n",
                  e->name);
    free(d);
    return o;
}

// ---- search for a number ---------------------------------------------------------------------------------------

typedef struct {
    unsigned char b[8];
    int len;
    const char* name;
} Needle;

static int make_needles(const char* value, Needle* nd) {
    int k = 0;
    long long v;
    double dv;
    int is_int = bin_parse_num(value, &v);
    if (is_int) dv = (double)v;
    else {
        char* end;
        dv = strtod(value, &end);
        if (end == value || *end) return -1;
    }
    if (is_int) {
        if (v >= 256 && v <= 65535) {
            nd[k].b[0] = (unsigned char)v, nd[k].b[1] = (unsigned char)(v >> 8), nd[k].len = 2, nd[k++].name = "u16le";
            nd[k].b[0] = (unsigned char)(v >> 8), nd[k].b[1] = (unsigned char)v, nd[k].len = 2, nd[k++].name = "u16be";
        }
        if ((v >= 0 && v <= 0xFFFFFFFFLL) || (v < 0 && v >= -2147483648LL)) {
            unsigned u = (unsigned)v;
            for (int i = 0; i < 4; i++) nd[k].b[i] = (unsigned char)(u >> (8 * i));
            nd[k].len = 4, nd[k++].name = v < 0 ? "i32le" : "u32le";
            if (v >= 256 || v < 0) {
                for (int i = 0; i < 4; i++) nd[k].b[i] = (unsigned char)(u >> (24 - 8 * i));
                nd[k].len = 4, nd[k++].name = "32be";
            }
        }
        if (v > 0xFFFFFFFFLL || v < -2147483648LL) {
            for (int i = 0; i < 8; i++) nd[k].b[i] = (unsigned char)((unsigned long long)v >> (8 * i));
            nd[k].len = 8, nd[k++].name = "u64le";
        }
    }
    float f = (float)dv;
    memcpy(nd[k].b, &f, 4), nd[k].len = 4, nd[k++].name = "f32";
    memcpy(nd[k].b, &dv, 8), nd[k].len = 8, nd[k++].name = "f64";
    return k;
}

static int search_one(const unsigned char* d, long n, const char* label, const Needle* nd, int nn, char* out, int max, int* total_hits) {
    int o = 0;
    for (int k = 0; k < nn; k++) {
        int hits = 0;
        for (long i = 0; i + nd[k].len <= n; i++) {
            if (d[i] != nd[k].b[0] || memcmp(d + i, nd[k].b, nd[k].len)) continue;
            hits++;
            if (hits <= 8 && o < max - 120) o += snprintf(out + o, max - o, "  %s 0x%lX %s\n", label, i, nd[k].name);
        }
        if (hits > 8 && o < max - 120) o += snprintf(out + o, max - o, "  %s: %s matches %d times (only 8 shown: the number is too common; use the exact number the game shows)\n", label, nd[k].name, hits);
        *total_hits += hits;
    }
    return o;
}

// phase 13: the same search in an entry too big to hold in memory: from the cache file, a megabyte at a time.
static int search_big(const char* zip, const ZipEnt* e, const Needle* nd, int nn, char* out, int max, int* total_hits) {
    char raw[200];
    if (bits_raw(zip, e, raw, sizeof(raw)) < 0) return 0;
    int fd = open(raw, O_RDONLY);
    if (fd < 0) return 0;
    enum { CH = 1 << 20 };
    static unsigned char buf[CH + 16];
    int hits[8] = {0}, o = 0;
    long n = (long)e->usize;
    for (long base = 0; base < n; base += CH) {
        int want = (int)(n - base < CH + 16 ? n - base : CH + 16), got = 0;
        if (lseek(fd, base, SEEK_SET) < 0) break;
        while (got < want) {
            int r = read(fd, buf + got, want - got);
            if (r <= 0) break;
            got += r;
        }
        int span = got < CH ? got : CH;   // (the positions that start in this chunk)
        for (int k = 0; k < nn && k < 8; k++)
            for (int i = 0; i < span && i + nd[k].len <= got; i++) {
                if (buf[i] != nd[k].b[0] || memcmp(buf + i, nd[k].b, nd[k].len)) continue;
                hits[k]++;
                if (hits[k] <= 8 && o < max - 120) o += snprintf(out + o, max - o, "  %s 0x%lX %s\n", e->name, base + i, nd[k].name);
            }
    }
    close(fd);
    for (int k = 0; k < nn && k < 8; k++) {
        if (hits[k] > 8 && o < max - 120) o += snprintf(out + o, max - o, "  %s: %s matches %d times (only 8 shown: the number is too common; use the exact number the game shows)\n", e->name, nd[k].name, hits[k]);
        *total_hits += hits[k];
    }
    return o;
}

int bin_zip_find(const char* zip, const char* file, const char* value, char* out, int max) {
    const ZipEnt* ents;
    int total;
    int n = zip_open_dir(zip, &ents, &total);
    if (n < 0) return -1;
    Needle nd[8];
    int nn = make_needles(value, nd);
    if (nn < 0) return -2;
    const ZipEnt* one = NULL;
    if (file && file[0]) {
        one = find_entry(ents, n, file);
        if (!one) return -3;
    }
    const char* zbase = strrchr(zip, '/') ? strrchr(zip, '/') + 1 : zip;
    int o = snprintf(out, max, "Search for %s in %s%s (as little/big endian integers and as float/double):\n", value, one ? "" : "all files of ", one ? one->name : zbase);
    int hits = 0, scanned = 0, files_hit = 0;
    for (int i = 0; i < n && o < max - 200; i++) {
        if (one && &ents[i] != one) continue;
        if (ents[i].usize == 0) continue;
        if (ents[i].usize > BITS_BIG) {
            scanned++;
            int before = hits;
            o += search_big(zip, &ents[i], nd, nn, out + o, max - o, &hits);
            if (hits > before) files_hit++;
            continue;
        }
        long len = 0;
        int cut = 0;
        unsigned char* d = load_entry(zip, &ents[i], &len, &cut);
        if (!d) continue;
        scanned++;
        int before = hits;
        o += search_one(d, len, ents[i].name, nd, nn, out + o, max - o, &hits);
        if (hits > before) files_hit++;
        free(d);
    }
    if (hits == 0) o += snprintf(out + o, max - o, "No match in %d file(s). The game may store it as a different number (e.g. money x10), inside compressed/encrypted data, or in a file not in this zip.\n", scanned);
    else o += snprintf(out + o, max - o, "%d match(es) in %d file(s) (of %d scanned). To tell which offset is the real one: change the value in the game, save again, and compare both zips with @before.zip~after.zip.\n", hits, files_hit, scanned);
    return o;
}

// ---- compare two saves -----------------------------------------------------------------------------------------

int bin_zip_compare(const char* za, const char* zb, char* out, int max) { return bin_zip_compare_hint(za, zb, NULL, out, max); }

int bin_zip_compare_hint(const char* za, const char* zb, const char* hint, char* out, int max) {
    const ZipEnt *ea, *eb;
    int ta, tb;
    int na = zip_open_dir(za, &ea, &ta);
    if (na < 0) return -1;
    // (the table of the first zip is kept: zip_open_dir reuses its buffer)
    ZipEnt* A = (ZipEnt*)malloc(sizeof(ZipEnt) * (na > 0 ? na : 1));
    if (!A) return -1;
    memcpy(A, ea, sizeof(ZipEnt) * na);
    int nb = zip_open_dir(zb, &eb, &tb);
    if (nb < 0) {
        free(A);
        return -1;
    }
    int o = snprintf(out, max, "Compare: %d files in the first zip, %d in the second.\n", na, nb);
    int same = 0, changed = 0;
    for (int i = 0; i < na && o < max - 300; i++) {
        const ZipEnt* b = NULL;
        for (int k = 0; k < nb && !b; k++)
            if (!strcmp(eb[k].name, A[i].name)) b = &eb[k];
        if (!b) {
            o += snprintf(out + o, max - o, "%s: only in the first zip.\n", A[i].name);
            continue;
        }
        if (A[i].usize > BITS_BIG || b->usize > BITS_BIG) {   // phase 13: a big file: read as bit fields, from the cache files
            char ra[200], rb[200];
            if (bits_raw(za, &A[i], ra, sizeof(ra)) < 0 || bits_raw(zb, b, rb, sizeof(rb)) < 0) {
                o += snprintf(out + o, max - o, "%s: cannot be read.\n", A[i].name);
                continue;
            }
            int o0 = o;
            o += bits_diff(ra, rb, A[i].name, hint, out + o, max - o);
            if (strstr(out + o0, "no byte differs")) o = o0, same++;
            else changed++;
            continue;
        }
        long l1 = 0, l2 = 0;
        int c1 = 0, c2 = 0;
        unsigned char* d1 = load_entry(za, &A[i], &l1, &c1);
        unsigned char* d2 = load_entry(zb, b, &l2, &c2);
        if (!d1 || !d2) {
            free(d1), free(d2);
            o += snprintf(out + o, max - o, "%s: cannot be read.\n", A[i].name);
            continue;
        }
        if (l1 == l2 && !memcmp(d1, d2, l1)) {
            same++;
            free(d1), free(d2);
            continue;
        }
        changed++;
        if (l1 != l2) o += snprintf(out + o, max - o, "%s: size changed %ld -> %ld bytes (the file is not a fixed layout, or it is compressed).\n", A[i].name, l1, l2);
        long m = l1 < l2 ? l1 : l2;
        int ranges = 0, shown = 0;
        long diffbytes = 0;
        for (long p = 0; p < m;) {
            if (d1[p] == d2[p]) {
                p++;
                continue;
            }
            long s = p, last = p;
            while (p < m && (d1[p] != d2[p] || p - last <= 3)) {
                if (d1[p] != d2[p]) last = p;
                p++;
            }
            long e = last;
            ranges++;
            diffbytes += e - s + 1;
            if (shown < 24 && o < max - 220) {
                shown++;
                long len = e - s + 1;
                o += snprintf(out + o, max - o, "  %s 0x%lX (%ld byte%s):", A[i].name, s, len, len == 1 ? "" : "s");
                for (long k = s; k <= e && k < s + 8; k++) o += snprintf(out + o, max - o, " %02X", d1[k]);
                o += snprintf(out + o, max - o, " ->");
                for (long k = s; k <= e && k < s + 8; k++) o += snprintf(out + o, max - o, " %02X", d2[k]);
                if (len <= 4 && s + 4 <= m)
                    o += snprintf(out + o, max - o, "   (u32le there: %u -> %u)", u32le(d1 + s), u32le(d2 + s));
                else if (len <= 2 && s + 2 <= m)
                    o += snprintf(out + o, max - o, "   (u16le: %u -> %u)", u16le(d1 + s), u16le(d2 + s));
                o += snprintf(out + o, max - o, "\n");
            }
        }
        if (ranges > shown && o < max - 160) o += snprintf(out + o, max - o, "  ... %d changed ranges in %s, %ld bytes in all%s\n", ranges, A[i].name, diffbytes, ranges > 200 ? " (almost everything changed: compressed or encrypted data)" : " (the rest is likely a checksum, a counter or a timestamp)");
        free(d1), free(d2);
    }
    for (int k = 0; k < nb && o < max - 200; k++) {
        int found = 0;
        for (int i = 0; i < na && !found; i++) found = !strcmp(A[i].name, eb[k].name);
        if (!found) o += snprintf(out + o, max - o, "%s: only in the second zip.\n", eb[k].name);
    }
    o += snprintf(out + o, max - o, "%d file(s) identical, %d changed. The offset you want is the one whose u32le/u16le went from the old value to the new one; small extra changes next to it are usually a checksum.\n", same, changed);
    free(A);
    return o;
}

// ---- poke: a patched copy of a zip -----------------------------------------------------------------------------

static unsigned crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static unsigned crc32_of(const unsigned char* d, long n) {
    crc_init();
    unsigned c = 0xFFFFFFFFu;
    for (long i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void p16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void p32(unsigned char* p, unsigned v) { p16(p, v & 0xFFFF), p16(p + 2, v >> 16); }

static int wr(int fd, const void* d, long n) {
    const unsigned char* p = (const unsigned char*)d;
    while (n > 0) {
        long w = write(fd, p, n > 65536 ? 65536 : n);
        if (w <= 0) return -1;
        p += w, n -= w;
    }
    return 0;
}

typedef struct {
    char name[ZIP_NAME_MAX];
    unsigned crc, size, off;
} Rec;

static int encode(const char* type, const char* value, unsigned char* b, int* len) {
    long long v = 0;
    double dv = 0;
    int is_int = bin_parse_num(value, &v);
    if (!is_int) {
        char* end;
        dv = strtod(value, &end);
        if (end == value || *end) return -1;
    } else dv = (double)v;
    int bits = 0, fl = 0, sg = 0;
    if (!strcmp(type, "u8")) bits = 8;
    else if (!strcmp(type, "u16")) bits = 16;
    else if (!strcmp(type, "u32")) bits = 32;
    else if (!strcmp(type, "u64")) bits = 64;
    else if (!strcmp(type, "i8")) bits = 8, sg = 1;
    else if (!strcmp(type, "i16")) bits = 16, sg = 1;
    else if (!strcmp(type, "i32")) bits = 32, sg = 1;
    else if (!strcmp(type, "i64")) bits = 64, sg = 1;
    else if (!strcmp(type, "f32")) bits = 32, fl = 1;
    else if (!strcmp(type, "f64")) bits = 64, fl = 1;
    else return -2;
    if (fl) {
        if (bits == 32) {
            float f = (float)dv;
            memcpy(b, &f, 4);
        } else memcpy(b, &dv, 8);
    } else {
        if (!is_int) return -1;
        if (bits < 64) {   // the value must fit the type
            long long lo = sg ? -(1LL << (bits - 1)) : 0, hi = sg ? (1LL << (bits - 1)) - 1 : (1LL << bits) - 1;
            if (v < lo || v > hi) return -3;
        } else if (!sg && v < 0) return -3;
        for (int i = 0; i < bits / 8; i++) b[i] = (unsigned char)((unsigned long long)v >> (8 * i));
    }
    *len = bits / 8;
    return 0;
}

// phase 13: a poke in an entry bigger than BIN_MAX: through the cache file, writing the new zip as a stream.
static int poke_big(const char* src, const char* dst, const ZipEnt* t, long long off, const unsigned char* nb, int nlen, char* msg, int msg_max) {
    char raw[200], patched[230], why[300];
    if (bits_raw(src, t, raw, sizeof(raw)) < 0) return snprintf(msg, msg_max, "Cannot read %s (damaged or unsupported).", t->name), -1;
    unsigned char old[8] = {0};
    int f = open(raw, O_RDONLY);
    if (f >= 0) {
        if (lseek(f, (off_t)off, SEEK_SET) >= 0 && read(f, old, nlen) != nlen) memset(old, 0, sizeof(old));
        close(f);
    }
    snprintf(patched, sizeof(patched), "%s.patched", raw);
    if (bits_patch_bytes(raw, patched, (long)off, nb, nlen)) return snprintf(msg, msg_max, "Cannot write the patched copy (disk full?)."), -1;
    int r = bits_zip_replace(src, dst, t->name, patched, why, sizeof(why));
    unlink(patched);
    if (r) return snprintf(msg, msg_max, "%s", why), -1;
    char ohex[24] = "", nhex[24] = "";
    for (int i = 0; i < nlen; i++) snprintf(ohex + i * 3, 4, "%02X ", old[i]), snprintf(nhex + i * 3, 4, "%02X ", nb[i]);
    const char* base = strrchr(dst, '/') ? strrchr(dst, '/') + 1 : dst;
    snprintf(msg, msg_max, "Done: %s at 0x%llX: %s-> %s. New zip %s (the original is untouched). Test it on the console; keep your backup.", t->name, off, ohex, nhex, base);
    return 0;
}

int bin_poke(const char* src, const char* dst, const char* entry, const char* offs, const char* type, const char* value, char* msg, int msg_max) {
    long long off;
    unsigned char nb[8];
    int nlen = 0;
    if (!bin_parse_num(offs, &off) || off < 0) return snprintf(msg, msg_max, "Bad offset \"%s\" (use 0x1A4 or 420).", offs), -1;
    int er = encode(type, value, nb, &nlen);
    if (er == -2) return snprintf(msg, msg_max, "Unknown type \"%s\" (u8 u16 u32 u64 i8 i16 i32 i64 f32 f64).", type), -1;
    if (er == -3) return snprintf(msg, msg_max, "%s does not fit in %s.", value, type), -1;
    if (er < 0) return snprintf(msg, msg_max, "Bad value \"%s\".", value), -1;
    if (!strcmp(src, dst)) return snprintf(msg, msg_max, "Refusing to overwrite the original."), -1;
    const ZipEnt* ents;
    int total;
    int n = zip_open_dir(src, &ents, &total);
    if (n < 0) return snprintf(msg, msg_max, "Cannot read %s.", src), -1;
    if (n < total) return snprintf(msg, msg_max, "The zip has too many entries (%d); not supported.", total), -1;
    ZipEnt* E = (ZipEnt*)malloc(sizeof(ZipEnt) * (n > 0 ? n : 1));
    Rec* R = (Rec*)malloc(sizeof(Rec) * (n > 0 ? n : 1));
    if (!E || !R) {
        free(E), free(R);
        return snprintf(msg, msg_max, "Out of memory."), -1;
    }
    memcpy(E, ents, sizeof(ZipEnt) * n);
    const ZipEnt* target = find_entry(E, n, entry);
    int tidx = target ? (int)(target - E) : -1;
    if (!target) {
        free(E), free(R);
        return snprintf(msg, msg_max, "%s is not in the zip.", entry), -1;
    }
    if ((long)E[tidx].usize > BIN_MAX && off + nlen <= (long long)E[tidx].usize) {
        int r = poke_big(src, dst, &E[tidx], off, nb, nlen, msg, msg_max);
        free(E), free(R);
        return r;
    }
    if (off + nlen > (long long)E[tidx].usize || (long)E[tidx].usize > BIN_MAX) {
        int r = snprintf(msg, msg_max, "Offset 0x%llX + %d bytes is outside %s (%u bytes)%s.", off, nlen, E[tidx].name, (unsigned)E[tidx].usize, (long)E[tidx].usize > BIN_MAX ? ", and files over 6 MB are not supported" : "");
        free(E), free(R);
        return r > 0 ? -1 : -1;
    }
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        free(E), free(R);
        return snprintf(msg, msg_max, "Cannot create %s.", dst), -1;
    }
    unsigned char old[8];
    long pos = 0;
    int fail = 0;
    char why[160] = "";
    for (int i = 0; i < n && !fail; i++) {
        if (E[i].method == 0xFFFF || (long)E[i].usize > BIN_MAX) {
            snprintf(why, sizeof(why), "%s cannot be copied (encrypted, unsupported or over 6 MB).", E[i].name);
            fail = 1;
            break;
        }
        long len = 0;
        int cut = 0;
        unsigned char* d = E[i].usize ? load_entry(src, &E[i], &len, &cut) : (unsigned char*)calloc(1, 2);
        if (!d || cut || len != (long)E[i].usize) {
            free(d);
            snprintf(why, sizeof(why), "%s cannot be read.", E[i].name);
            fail = 1;
            break;
        }
        if (i == tidx) {
            memcpy(old, d + off, nlen);
            memcpy(d + off, nb, nlen);
        }
        unsigned crc = crc32_of(d, len);
        unsigned char h[30];
        int nl = (int)strlen(E[i].name);
        p32(h, 0x04034b50), p16(h + 4, 20), p16(h + 6, 0x0800), p16(h + 8, 0), p16(h + 10, 0), p16(h + 12, ((2026 - 1980) << 9) | (1 << 5) | 1);
        p32(h + 14, crc), p32(h + 18, (unsigned)len), p32(h + 22, (unsigned)len), p16(h + 26, nl), p16(h + 28, 0);
        if (wr(fd, h, 30) || wr(fd, E[i].name, nl) || wr(fd, d, len)) {
            snprintf(why, sizeof(why), "Write error (disk full?).");
            fail = 1;
        }
        snprintf(R[i].name, sizeof(R[i].name), "%s", E[i].name);
        R[i].crc = crc, R[i].size = (unsigned)len, R[i].off = (unsigned)pos;
        pos += 30 + nl + len;
        free(d);
    }
    if (!fail) {
        long cd = pos;
        for (int i = 0; i < n && !fail; i++) {
            unsigned char h[46];
            int nl = (int)strlen(R[i].name);
            memset(h, 0, sizeof(h));
            p32(h, 0x02014b50), p16(h + 4, 20), p16(h + 6, 20), p16(h + 8, 0x0800), p16(h + 10, 0), p16(h + 12, 0), p16(h + 14, ((2026 - 1980) << 9) | (1 << 5) | 1);
            p32(h + 16, R[i].crc), p32(h + 20, R[i].size), p32(h + 24, R[i].size), p16(h + 28, nl);
            p32(h + 42, R[i].off);
            if (wr(fd, h, 46) || wr(fd, R[i].name, nl)) fail = 1;
            pos += 46 + nl;
        }
        unsigned char e[22];
        memset(e, 0, sizeof(e));
        p32(e, 0x06054b50), p16(e + 8, n), p16(e + 10, n), p32(e + 12, (unsigned)(pos - cd)), p32(e + 16, (unsigned)cd);
        if (wr(fd, e, 22)) fail = 1;
        if (fail) snprintf(why, sizeof(why), "Write error (disk full?).");
    }
    close(fd);
    free(R);
    if (fail) {
        unlink(dst);
        free(E);
        return snprintf(msg, msg_max, "%s", why), -1;
    }
    // read the new zip back and check the bytes
    const ZipEnt* ne;
    int nt, nn = zip_open_dir(dst, &ne, &nt);
    int ok = nn == n;
    if (ok) {
        const ZipEnt* t = find_entry(ne, nn, E[tidx].name);
        long len = 0;
        int cut = 0;
        unsigned char* d = t ? load_entry(dst, t, &len, &cut) : NULL;
        ok = d && len == (long)E[tidx].usize && !memcmp(d + off, nb, nlen);
        free(d);
    }
    char ohex[24] = "", nhex[24] = "";
    for (int i = 0; i < nlen; i++) snprintf(ohex + i * 3, 4, "%02X ", old[i]), snprintf(nhex + i * 3, 4, "%02X ", nb[i]);
    const char* base = strrchr(dst, '/') ? strrchr(dst, '/') + 1 : dst;
    int r = ok ? snprintf(msg, msg_max, "Done: %s at 0x%llX: %s-> %s(%s %s). New zip %s (%d files, the original is untouched). Test it on the console; keep your backup.", E[tidx].name, off, ohex, nhex, type, value, base, n)
               : snprintf(msg, msg_max, "The new zip failed the read-back check; it was kept as %s but do not use it.", base);
    free(E);
    return ok ? 0 : (r > 0 ? -1 : -1);
}

// ---- phase 13: bit fields, checksums and big files ----------------------------------------------------------------

// The entry `name` of a zip, extracted to the cache. 0 on success; -1 zip not readable, -3 no such file, -4 cannot extract.
static int entry_raw(const char* zip, const char* name, ZipEnt* t, char* raw, int rawmax) {
    const ZipEnt* ents;
    int total;
    int n = zip_open_dir(zip, &ents, &total);
    if (n < 0) return -1;
    const ZipEnt* f = find_entry(ents, n, name);
    if (!f) return -3;
    *t = *f;
    return bits_raw(zip, t, raw, rawmax) < 0 ? -4 : 0;
}

int bin_zip_bits(const char* zip, const char* file, const char* spec, char* out, int max) {
    ZipEnt t;
    char raw[200];
    int r = entry_raw(zip, file, &t, raw, sizeof(raw));
    if (r < 0) return r;
    return bits_find(raw, t.name, spec, out, max);
}

int bin_checksum(const char* zip, const char* file, const char* off, char* out, int max) {
    long long v;
    if (!bin_parse_num(off, &v) || v < 0) return -2;
    ZipEnt t;
    char raw[200];
    int r = entry_raw(zip, file, &t, raw, sizeof(raw));
    if (r < 0) return r;
    return bits_checksum(raw, (long)v, out, max);
}

int bin_pokebits(const char* src, const char* dst, const char* entry, const char* where, const char* width, const char* value, const char* order, char* msg, int msg_max) {
    char wh[64];
    snprintf(wh, sizeof(wh), "%s", where);
    char* colon = strchr(wh, ':');
    long long off = 0, w = 0, v = 0;
    int bit = 0;
    if (colon) {
        *colon = 0;
        long long b;
        if (!bin_parse_num(colon + 1, &b) || b < 0 || b > 7) return snprintf(msg, msg_max, "Bad bit \"%s\" (0..7, counted as the order says).", colon + 1), -1;
        bit = (int)b;
    }
    if (!bin_parse_num(wh, &off) || off < 0) return snprintf(msg, msg_max, "Bad offset \"%s\" (use 0x3A41F:5, the byte and the bit).", where), -1;
    if (!bin_parse_num(width, &w) || w < 1 || w > 64) return snprintf(msg, msg_max, "Bad width \"%s\" (1 to 64 bits).", width), -1;
    if (!bin_parse_num(value, &v) || v < 0) return snprintf(msg, msg_max, "Bad value \"%s\" (a whole number, 0 or more).", value), -1;
    int msb = !strcasecmp(order, "msb");
    if (!msb && strcasecmp(order, "lsb")) return snprintf(msg, msg_max, "Bad order \"%s\" (lsb or msb).", order), -1;
    if (!strcmp(src, dst)) return snprintf(msg, msg_max, "Refusing to overwrite the original."), -1;
    ZipEnt t;
    char raw[200], patched[230], why[300];
    int r = entry_raw(src, entry, &t, raw, sizeof(raw));
    if (r == -1) return snprintf(msg, msg_max, "Cannot read %s.", src), -1;
    if (r == -3) return snprintf(msg, msg_max, "%s is not in the zip.", entry), -1;
    if (r < 0) return snprintf(msg, msg_max, "Cannot read %s (damaged or unsupported).", entry), -1;
    snprintf(patched, sizeof(patched), "%s.patched", raw);
    unsigned long long old = 0;
    r = bits_patch(raw, patched, (long)off, bit, (int)w, (unsigned long long)v, msb, &old);
    if (r == -2) return snprintf(msg, msg_max, "%lld does not fit in %lld bits.", v, w), -1;
    if (r == -3) return snprintf(msg, msg_max, "Offset 0x%llX is outside %s (%u bytes).", off, t.name, (unsigned)t.usize), -1;
    if (r) return snprintf(msg, msg_max, "Cannot write the patched copy (disk full?)."), -1;
    r = bits_zip_replace(src, dst, t.name, patched, why, sizeof(why));
    unlink(patched);
    if (r) return snprintf(msg, msg_max, "%s", why), -1;
    const char* base = strrchr(dst, '/') ? strrchr(dst, '/') + 1 : dst;
    snprintf(msg, msg_max, "Done: %s, %lld bits (%s) at byte 0x%llX bit %d: %llu -> %lld. New zip %s (the original is untouched). Test it on the console; keep your backup. If the game rejects it, a checksum may need fixing: /checksum.", t.name, w, msb ? "MSB-first" : "LSB-first", off, bit, old, v, base);
    return 0;
}

