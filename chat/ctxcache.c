// Context cache on disk (see ctxcache.h). The caches live in GPU-visible memory the CPU can read and write (like the
// weights, which are read() straight into it), so a snapshot is written and read in chunks from/to those addresses.
#include "ctxcache.h"

#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CTXCACHE_DIR
#define CTXCACHE_DIR "/data/garlic_cache"
#endif
#ifndef CTXCACHE_OFF_FILE
#define CTXCACHE_OFF_FILE "/data/garlic_cache.txt"
#endif

#ifdef CTXCACHE_HOST
#include <time.h>
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
#else
#include <orbis/libkernel.h>
static double now_s(void) { return (double)sceKernelGetProcessTime() / 1e6; }
#endif

void say(const char* fmt, ...);   // the log (main.c)

#define KEEP 3
#define VERSION 1
#define CHUNK (1u << 20)
#define MIN_READ_MBPS 15.0   // below this the CPU reads GPU memory too slowly to be worth a snapshot

static const char MAGIC[8] = {'G', 'R', 'L', 'C', 'C', 'H', '0', '1'};
static const char END_MAGIC[8] = {'G', 'R', 'L', 'C', 'E', 'N', 'D', '1'};

typedef struct {
    char magic[8];
    uint32_t version, n_layers, n_kv, head_dim, hidden, max_seq, pre, nblocks, full_mask, pad0;
    uint64_t conv_n, state_bytes, kv_block, tq_split, weights_size, key, created;
    uint64_t body_bytes;
} Hdr;

static int g_on = -1;
static int g_slow;   // this session: reading GPU memory is too slow, no more saves

int ctxcache_enabled(void) {
    if (g_on < 0) {
        char c = '1';
        int f = open(CTXCACHE_OFF_FILE, O_RDONLY);
        if (f >= 0) {
            if (read(f, &c, 1) != 1) c = '1';
            close(f);
        }
        g_on = c != '0';
    }
    return g_on;
}

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n--) h = (h ^ *b++) * 0x100000001b3ull;
    return h;
}

// Where the KV blocks are: one block [base, base + block) holding every full-attention layer's k and v for positions
// 0..8191 (engine.c, TurboQuant), the next 8192 at tq_split bytes further. 0 when the engine is not laid out that way.
static int layout(const Engine* e, uint64_t* base, uint64_t* block) {
    if (!e->qwen35 || !e->tq || !e->kc || !e->vc) return 0;
    uint32_t nf = 0;
    int first = -1;
    for (uint32_t i = 0; i < e->n_layers; i++)
        if (e->full_mask >> i & 1) {
            if (first < 0) first = (int)i;
            nf++;
        }
    if (!nf) return 0;
    uint64_t kb = (uint64_t)e->n_kv * 8192 * (e->head_dim / 2 + 4);
    uint64_t b0 = e->kc[first];
    uint32_t j = 0;
    for (uint32_t i = 0; i < e->n_layers; i++)
        if (e->full_mask >> i & 1) {
            if (e->kc[i] != b0 + j * 2 * kb || e->vc[i] != e->kc[i] + kb) return 0;
            j++;
        }
    *base = b0;
    *block = (uint64_t)nf * 2 * kb;
    return 1;
}

static uint64_t states_bytes(const Engine* e) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < e->n_layers; i++)
        if (!(e->full_mask >> i & 1)) n += e->conv_n * 12 + e->state_bytes;
    return n;
}

static int want_header(const Engine* e, const uint32_t* ids, int pre, Hdr* h) {
    uint64_t base, block;
    if (pre < 1 || !layout(e, &base, &block)) return 0;
    int nb = pre > 8192 ? 2 : 1;
    if (nb == 2 && (!e->tq16 || !e->tq_split || pre > (int)e->ctx_limit)) return 0;
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, MAGIC, 8);
    h->version = VERSION;
    h->n_layers = e->n_layers, h->n_kv = e->n_kv, h->head_dim = e->head_dim, h->hidden = e->hidden;
    h->max_seq = e->max_seq, h->pre = (uint32_t)pre, h->nblocks = (uint32_t)nb, h->full_mask = e->full_mask;
    h->conv_n = e->conv_n, h->state_bytes = e->state_bytes, h->kv_block = block, h->tq_split = e->tq_split;
    h->weights_size = e->weights_size;
    h->body_bytes = states_bytes(e) + (uint64_t)nb * block;
    uint64_t k = 0xcbf29ce484222325ull;
    k = fnv(k, h, offsetof(Hdr, key));   // everything above (magic, version, shape, sizes)
    k = fnv(k, ids, (size_t)pre * 4);
    h->key = k;
    return 1;
}

static void path_of(char* out, int max, uint64_t key, int tmp) {
    snprintf(out, max, "%s/%016llx.%s", CTXCACHE_DIR, (unsigned long long)key, tmp ? "tmp" : "kv");
}

static int write_all(int f, const void* p, uint64_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        uint64_t c = n < CHUNK ? n : CHUNK;
        ssize_t w = write(f, b, (size_t)c);
        if (w <= 0) return -1;
        b += w;
        n -= (uint64_t)w;
    }
    return 0;
}

static int read_all(int f, void* p, uint64_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n) {
        uint64_t c = n < CHUNK ? n : CHUNK;
        ssize_t r = read(f, b, (size_t)c);
        if (r <= 0) return -1;
        b += r;
        n -= (uint64_t)r;
    }
    return 0;
}

// Keeps at most `keep` snapshots (the least recently used go first: a load rewrites a byte, which updates the mtime)
// and removes the leftovers of an interrupted save.
static void prune(int keep) {
    DIR* d = opendir(CTXCACHE_DIR);
    if (!d) return;
    struct {
        char name[64];
        long long t;
    } L[64];
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) && n < 64) {
        size_t l = strlen(de->d_name);
        if (l < 4 || l > 40) continue;
        char p[160];
        snprintf(p, sizeof(p), "%s/%.40s", CTXCACHE_DIR, de->d_name);
        if (!strcmp(de->d_name + l - 4, ".tmp")) {
            unlink(p);
            continue;
        }
        if (strcmp(de->d_name + l - 3, ".kv")) continue;
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
        snprintf(p, sizeof(p), "%s/%s", CTXCACHE_DIR, L[oldest].name);
        unlink(p);
        L[oldest] = L[--n];
    }
}

int ctxcache_save(Engine* e, const uint32_t* ids, int pre) {
    if (!ctxcache_enabled() || g_slow) return 0;
    Hdr h;
    uint64_t base, block;
    if (!want_header(e, ids, pre, &h) || !layout(e, &base, &block)) return 0;
    // Is the CPU's read of this memory fast enough? (Garlic memory can be uncached for the CPU.) 256 KiB first.
    {
        static uint8_t probe[256 * 1024];
        double t0 = now_s();
        memcpy(probe, (const void*)(uintptr_t)base, sizeof(probe));
        double dt = now_s() - t0;
        double mbps = dt > 0 ? sizeof(probe) / 1048576.0 / dt : 1e9;
        if (mbps < MIN_READ_MBPS) {
            g_slow = 1;
            say("ctx cache: not saved, the CPU reads GPU memory at %.1f MiB/s (too slow for a %u MiB snapshot)",
                mbps, (unsigned)(h.body_bytes >> 20));
            return 0;
        }
    }
    mkdir(CTXCACHE_DIR, 0777);
    char path[160], tmp[160];
    path_of(path, sizeof(path), h.key, 0);
    path_of(tmp, sizeof(tmp), h.key, 1);
    prune(KEEP - 1);
    unlink(path);
    double t0 = now_s();
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f < 0) {
        say("ctx cache: cannot create %s", tmp);
        return 0;
    }
    int bad = write_all(f, &h, sizeof(h));
    for (uint32_t i = 0; !bad && i < e->n_layers; i++)
        if (!(e->full_mask >> i & 1))
            bad = write_all(f, (const void*)(uintptr_t)e->kc[i], e->conv_n * 12) ||
                  write_all(f, (const void*)(uintptr_t)e->vc[i], e->state_bytes);
    if (!bad) bad = write_all(f, (const void*)(uintptr_t)base, block);
    if (!bad && h.nblocks == 2) bad = write_all(f, (const void*)(uintptr_t)(base + e->tq_split), block);
    if (!bad) bad = write_all(f, END_MAGIC, 8);
    if (close(f)) bad = 1;
    if (bad || rename(tmp, path)) {
        unlink(tmp);
        say("ctx cache: the snapshot could not be written (disk full?)");
        return 0;
    }
    double dt = now_s() - t0;
    say("ctx cache: saved %d tokens (%u MiB) in %.2f s: %s", pre, (unsigned)(h.body_bytes >> 20), dt, path);
    return 1;
}

int ctxcache_load(Engine* e, const uint32_t* ids, int pre) {
    if (!ctxcache_enabled()) return 0;
    Hdr want, got;
    uint64_t base, block;
    if (!want_header(e, ids, pre, &want) || !layout(e, &base, &block)) return 0;
    char path[160];
    path_of(path, sizeof(path), want.key, 0);
    int f = open(path, O_RDWR);
    if (f < 0) return 0;
    struct stat st;
    uint64_t need = sizeof(Hdr) + want.body_bytes + 8;
    char endm[8];
    if (fstat(f, &st) || (uint64_t)st.st_size != need || read_all(f, &got, sizeof(got)) ||
        memcmp(&got, &want, offsetof(Hdr, created)) || got.key != want.key || got.body_bytes != want.body_bytes ||
        lseek(f, (off_t)(need - 8), SEEK_SET) < 0 || read_all(f, endm, 8) || memcmp(endm, END_MAGIC, 8) ||
        lseek(f, (off_t)sizeof(Hdr), SEEK_SET) < 0) {
        close(f);
        say("ctx cache: %s does not match this prefix or is damaged, it is read again", path);
        return 0;
    }
    // (from here on the engine's memory is being overwritten)
    double t0 = now_s();
    int bad = 0;
    for (uint32_t i = 0; !bad && i < e->n_layers; i++)
        if (!(e->full_mask >> i & 1))
            bad = read_all(f, (void*)(uintptr_t)e->kc[i], e->conv_n * 12) ||
                  read_all(f, (void*)(uintptr_t)e->vc[i], e->state_bytes);
    if (!bad) bad = read_all(f, (void*)(uintptr_t)base, block);
    if (!bad && want.nblocks == 2) bad = read_all(f, (void*)(uintptr_t)(base + e->tq_split), block);
    __asm__ volatile("sfence" ::: "memory");
    if (bad) {
        close(f);
        say("ctx cache: reading %s failed halfway, starting from an empty context", path);
        return -1;
    }
    pwrite(f, &got.created, sizeof(got.created), (off_t)offsetof(Hdr, created));   // (touches the mtime: recently used)
    close(f);
    double dt = now_s() - t0;
    say("ctx cache: restored %d tokens (%u MiB) in %.2f s (reading them again would take ~%.0f s)", pre,
        (unsigned)(want.body_bytes >> 20), dt, pre / 65.0);
    return 1;
}
