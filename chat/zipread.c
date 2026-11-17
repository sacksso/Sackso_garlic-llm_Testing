// Reading .zip files (see zipread.h). The inflate follows the structure of Mark Adler's puff.c (public domain).
#include "zipread.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "htmltext.h"

// ---- inflate -------------------------------------------------------------------------------------------------------

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} Huff;

typedef struct {
    const uint8_t* in;
    int inlen, incnt;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t* out;
    int outmax, outcnt;
    int err;   // 1: the input ended (what was decoded so far is still good)
} St;

static int bits(St* s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) {
            s->err = 1;
            return 0;
        }
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int decode(St* s, const Huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;
}

static int construct(Huff* h, const uint8_t* length, int n) {
    for (int l = 0; l <= 15; l++) h->count[l] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l <= 15; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return left;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (uint16_t)i;
    return left;
}

static const short LENS[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DISTS[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Returns 0 at the end of the block, 1 when the output is full, < 0 on an error (or the input ended: s->err).
static int codes(St* s, const Huff* lc, const Huff* dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return sym;
        if (sym < 256) {
            if (s->outcnt >= s->outmax) return 1;
            s->out[s->outcnt++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -10;
            int len = LENS[sym] + bits(s, LEXT[sym]);
            if (s->err) return -1;
            int ds = decode(s, dc);
            if (ds < 0) return ds;
            if (ds >= 30) return -10;
            int dist = DISTS[ds] + bits(s, DEXT[ds]);
            if (s->err) return -1;
            if (dist > s->outcnt) return -11;
            while (len--) {
                if (s->outcnt >= s->outmax) return 1;
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
        }
    }
}

static int stored(St* s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) {
        s->err = 1;
        return -1;
    }
    int len = s->in[s->incnt] | (s->in[s->incnt + 1] << 8);
    int nlen = s->in[s->incnt + 2] | (s->in[s->incnt + 3] << 8);
    s->incnt += 4;
    if (len != (~nlen & 0xffff)) return -2;
    int avail = s->inlen - s->incnt;
    int room = s->outmax - s->outcnt;
    int n = len;
    int rc = 0;
    if (n > avail) n = avail, rc = -1, s->err = 1;
    if (n > room) n = room, rc = 1;
    memcpy(s->out + s->outcnt, s->in + s->incnt, n);
    s->outcnt += n;
    s->incnt += n;
    return rc;
}

static int fixed(St* s) {
    static Huff lc, dc;
    static int built;
    if (!built) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        construct(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        construct(&dc, l, 30);
        built = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(St* s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[320];
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->err) return -1;
    if (nlen > 286 || ndist > 30) return -3;
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (uint8_t)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err) return -1;
    Huff lencode;
    if (construct(&lencode, lengths, 19) != 0) return -4;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0) return sym;
        if (sym < 16) {
            lengths[index++] = (uint8_t)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                rep = 3 + bits(s, 2);
            } else if (sym == 17) rep = 3 + bits(s, 3);
            else rep = 11 + bits(s, 7);
            if (s->err) return -1;
            if (index + rep > nlen + ndist) return -6;
            while (rep--) lengths[index++] = (uint8_t)len;
        }
    }
    if (lengths[256] == 0) return -9;
    Huff lc, dc;
    int err = construct(&lc, lengths, nlen);
    if (err && (err < 0 || nlen != lc.count[0] + lc.count[1])) return -7;
    err = construct(&dc, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != dc.count[0] + dc.count[1])) return -8;
    return codes(s, &lc, &dc);
}

// Inflates in[0..inlen) into out (at most outmax bytes). Returns 0 when the stream ended, 1 when the output filled up,
// 2 when the input ended first (the output is the correct start of the data), < 0 for a damaged stream.
static int inflate_raw(const uint8_t* in, int inlen, uint8_t* out, int outmax, int* outlen) {
    St s;
    memset(&s, 0, sizeof(s));
    s.in = in, s.inlen = inlen, s.out = out, s.outmax = outmax;
    int last, rc = 0;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.err) break;
        rc = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -20;
        if (rc != 0) break;
    } while (!last);
    *outlen = s.outcnt;
    if (s.err) return 2;
    return rc;
}

// ---- zip -----------------------------------------------------------------------------------------------------------

static uint32_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static ZipEnt table[ZIP_MAX_ENTS];
static uint8_t cdbuf[512 * 1024];
static uint8_t tailbuf[66 * 1024];
static uint8_t cbuf[128 * 1024];   // compressed bytes of the entry being read

static int read_at(int fd, long off, void* buf, int n) {
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    int got = 0;
    while (got < n) {
        int r = read(fd, (char*)buf + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

int zip_open_dir(const char* path, const ZipEnt** ents, int* total) {
    *ents = table;
    *total = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long size = lseek(fd, 0, SEEK_END);
    if (size < 22) {
        close(fd);
        return -2;
    }
    int tail = size < (long)sizeof(tailbuf) ? (int)size : (int)sizeof(tailbuf);
    if (read_at(fd, size - tail, tailbuf, tail) != tail) {
        close(fd);
        return -2;
    }
    int e = -1;
    for (int i = tail - 22; i >= 0; i--)
        if (u32(tailbuf + i) == 0x06054b50) {
            e = i;
            break;
        }
    if (e < 0) {
        close(fd);
        return -2;
    }
    uint32_t count = u16(tailbuf + e + 10), cdsize = u32(tailbuf + e + 12), cdoff = u32(tailbuf + e + 16);
    if (count == 0xFFFF || cdsize == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu || cdsize > sizeof(cdbuf)) {
        close(fd);
        return -3;
    }
    if (read_at(fd, cdoff, cdbuf, (int)cdsize) != (int)cdsize) {
        close(fd);
        return -2;
    }
    close(fd);
    int n = 0, all = 0;
    uint32_t p = 0;
    for (uint32_t k = 0; k < count && p + 46 <= cdsize; k++) {
        const uint8_t* h = cdbuf + p;
        if (u32(h) != 0x02014b50) return -2;
        uint32_t nl = u16(h + 28), xl = u16(h + 30), cl = u16(h + 32);
        if (p + 46 + nl > cdsize) return -2;
        const char* name = (const char*)h + 46;
        if (nl > 0 && name[nl - 1] != '/') {   // (directories are left out)
            all++;
            if (n < ZIP_MAX_ENTS && nl < ZIP_NAME_MAX) {
                ZipEnt* z = &table[n++];
                memcpy(z->name, name, nl);
                z->name[nl] = 0;
                z->csize = u32(h + 20), z->usize = u32(h + 24), z->off = u32(h + 42);
                uint16_t method = (uint16_t)u16(h + 10);
                z->method = (u16(h + 8) & 1) || (method != 0 && method != 8) ? 0xFFFF : method;
            }
        }
        p += 46 + nl + xl + cl;
    }
    *total = all;
    return n;
}

int zip_read(const char* path, const ZipEnt* e, char* out, int max, int* cut) {
    *cut = 0;
    out[0] = 0;
    if (e->method == 0xFFFF) return -4;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t lh[30];
    if (read_at(fd, e->off, lh, 30) != 30 || u32(lh) != 0x04034b50) {
        close(fd);
        return -2;
    }
    long data = (long)e->off + 30 + u16(lh + 26) + u16(lh + 28);
    int want = e->csize < sizeof(cbuf) ? (int)e->csize : (int)sizeof(cbuf);
    int got = read_at(fd, data, cbuf, want);
    close(fd);
    if (got < 0 || (got < want && got < (int)e->csize)) return -2;
    int n = 0;
    if (e->method == 0) {
        n = got < max - 1 ? got : max - 1;
        memcpy(out, cbuf, n);
        if (n < (int)e->usize) *cut = 1;
    } else {
        int rc = inflate_raw(cbuf, got, (uint8_t*)out, max - 1, &n);
        if (rc < 0) return -3;
        if (rc != 0 || n < (int)e->usize) *cut = 1;   // output full, or the compressed start we read ran out
    }
    out[n] = 0;
    return n;
}

static int ends_ci(const char* s, const char* suf) {
    int a = (int)strlen(s), b = (int)strlen(suf);
    if (a < b) return 0;
    for (int i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != suf[i]) return 0;
    }
    return 1;
}

static const ZipEnt* find_ent(const ZipEnt* ents, int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(ents[i].name, name)) return &ents[i];
    return NULL;
}

static int slide_less(const char* a, const char* b) {   // slide2.xml before slide10.xml
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la != lb) return la < lb;
    return strcmp(a, b) < 0;
}

// ---- xlsx: shared strings + sheets ---------------------------------------------------------------------------------

static char sst_pool[48 * 1024];
static int sst_off[4000], sst_n;

static const char* find_str(const char* s, const char* end, const char* lit) {
    int l = (int)strlen(lit);
    for (; s + l <= end; s++)
        if (*s == lit[0] && !memcmp(s, lit, l)) return s;
    return NULL;
}

static void sst_load(const char* x, int n) {   // every <si>...</si> becomes one string of the pool
    sst_n = 0;
    int used = 0;
    const char* end = x + n;
    for (const char* p = x; p < end && sst_n < 4000;) {
        const char* a = find_str(p, end, "<si>");
        if (!a) a = find_str(p, end, "<si ");
        if (!a) break;
        const char* b = find_str(a, end, "</si>");
        if (!b) break;
        if (used > (int)sizeof(sst_pool) - 300) break;
        sst_off[sst_n++] = used;
        used += html_to_text(a, (int)(b - a), sst_pool + used, 200, 1) + 1;
        p = b + 5;
    }
    sst_off[sst_n < 4000 ? sst_n : 3999] = used;
}

static const char* sst_get(int i) { return i >= 0 && i < sst_n ? sst_pool + sst_off[i] : ""; }

// One sheet: cells separated by tabs, rows by line breaks.
static int xlsx_sheet(const char* x, int n, char* out, int max) {
    const char* end = x + n;
    int used = 0, first_row = 1, first_cell = 1;
    for (const char* p = x; p < end && used < max - 4;) {
        const char* lt = memchr(p, '<', end - p);
        if (!lt) break;
        p = lt;
        if (!strncmp(p, "<row", 4) && (p[4] == ' ' || p[4] == '>')) {
            if (!first_row) out[used++] = '\n';
            first_row = 0, first_cell = 1;
        } else if (p[1] == 'c' && (p[2] == ' ' || p[2] == '>')) {
            const char* te = memchr(p, '>', end - p);
            if (!te) break;
            int empty = te[-1] == '/';
            const char* cend = te + 1;
            if (!empty) {
                const char* e = find_str(te, end, "</c>");
                cend = e ? e : end;
            }
            char type = 0;   // the t="..." of the cell: s (shared), i (inline), others are plain values
            const char* t = find_str(p, te, " t=\"");
            if (t) type = t[4];
            if (!first_cell) out[used++] = '\t';
            first_cell = 0;
            if (!empty) {
                if (type == 'i') {
                    used += html_to_text(te + 1, (int)(cend - te - 1), out + used, max - used - 2, 1);
                } else {
                    const char* v = find_str(te, cend, "<v>");
                    const char* ve = v ? find_str(v, cend, "</v>") : NULL;
                    if (v && ve) {
                        if (type == 's') {
                            const char* str = sst_get(atoi(v + 3));
                            int l = (int)strlen(str);
                            if (l > max - used - 2) l = max - used - 2;
                            memcpy(out + used, str, l);
                            used += l;
                        } else used += html_to_text(v + 3, (int)(ve - v - 3), out + used, max - used - 2, 1);
                    }
                }
            }
            p = cend;
            continue;
        }
        p++;
    }
    out[used] = 0;
    return used;
}

static int xlsx_text(const char* path, const ZipEnt* ents, int n, char* out, int max, int* cut) {
    static char xml[64 * 1024 + 1];
    const ZipEnt* ss = find_ent(ents, n, "xl/sharedStrings.xml");
    sst_n = 0;
    if (ss) {
        int c;
        int xn = zip_read(path, ss, xml, sizeof(xml), &c);
        if (xn > 0) sst_load(xml, xn);
        if (c) *cut = 1;
    }
    int used = 0, any = 0;
    char name[ZIP_NAME_MAX];
    for (int k = 1; k <= 6 && used < max - 60; k++) {
        snprintf(name, sizeof(name), "xl/worksheets/sheet%d.xml", k);
        const ZipEnt* e = find_ent(ents, n, name);
        if (!e) continue;
        int c;
        int xn = zip_read(path, e, xml, sizeof(xml), &c);
        if (xn <= 0) continue;
        used += snprintf(out + used, max - used, "[sheet%d]\n", k);
        used += xlsx_sheet(xml, xn, out + used, max - used);
        if (used < max - 2) out[used++] = '\n', out[used] = 0;
        if (c) *cut = 1;
        any = 1;
    }
    if (used >= max - 60) *cut = 1;
    return any ? used : -2;
}

int zip_office_text(const char* path, char* out, int max, int* cut) {
    static char xml[64 * 1024 + 1];
    const ZipEnt* ents;
    int total;
    int n = zip_open_dir(path, &ents, &total);
    if (n < 0) return n;
    int used = 0, any = 0;
    *cut = 0;
    out[0] = 0;
    if (ends_ci(path, ".pptx")) {
        char prev[ZIP_NAME_MAX] = "";
        for (;;) {   // the next slide in order, without sorting the table
            const ZipEnt* best = NULL;
            for (int i = 0; i < n; i++) {
                const char* nm = ents[i].name;
                if (strncmp(nm, "ppt/slides/slide", 16) || !ends_ci(nm, ".xml")) continue;
                if (prev[0] && !slide_less(prev, nm)) continue;
                if (!best || slide_less(nm, best->name)) best = &ents[i];
            }
            if (!best) break;
            strcpy(prev, best->name);
            int xc;
            int xn = zip_read(path, best, xml, sizeof(xml), &xc);
            if (xn < 0) continue;
            used += snprintf(out + used, max - used, "[slide %s]\n", best->name + 16);
            used += html_to_text(xml, xn, out + used, max - used, 1);
            if (used < max - 1) out[used++] = '\n', out[used] = 0;
            any = 1;
            if (used >= max - 40) {
                *cut = 1;
                break;
            }
        }
    } else if (ends_ci(path, ".xlsx")) {
        return xlsx_text(path, ents, n, out, max, cut);
    } else {
        const char* member = ends_ci(path, ".docx") ? "word/document.xml"
                             : (ends_ci(path, ".odt") || ends_ci(path, ".ods") || ends_ci(path, ".odp")) ? "content.xml"
                                                                                                          : NULL;
        if (!member) return -5;
        const ZipEnt* e = find_ent(ents, n, member);
        if (!e) return -2;
        int xc;
        int xn = zip_read(path, e, xml, sizeof(xml), &xc);
        if (xn < 0) return -3;
        used = html_to_text(xml, xn, out, max, 1);
        if (xc || used >= max - 2) *cut = 1;
        any = 1;
    }
    return any ? used : -2;
}
