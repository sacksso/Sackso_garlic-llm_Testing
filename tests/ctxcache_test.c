// Host test of the context cache on disk (chat/ctxcache.c) with a fake engine in malloc'd memory.
// gcc -DCTXCACHE_HOST -DCTXCACHE_DIR='"..."' -DCTXCACHE_OFF_FILE='"..."' tests/ctxcache_test.c chat/ctxcache.c
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ctxcache.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static Engine E;
static uint8_t* kvmem;
static uint64_t kb, block;

static void fill(unsigned seed) {   // every byte the cache holds, from a pattern
    unsigned x = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < E.n_layers; i++)
        if (!(E.full_mask >> i & 1)) {
            for (uint64_t k = 0; k < E.conv_n * 12; k++) ((uint8_t*)(uintptr_t)E.kc[i])[k] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16);
            for (uint64_t k = 0; k < E.state_bytes; k++) ((uint8_t*)(uintptr_t)E.vc[i])[k] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16);
        }
    for (uint64_t k = 0; k < 2 * block; k++) kvmem[k] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16);
}
static unsigned long long checksum(int with_block2) {
    unsigned long long h = 1469598103934665603ull;
    for (uint32_t i = 0; i < E.n_layers; i++)
        if (!(E.full_mask >> i & 1)) {
            for (uint64_t k = 0; k < E.conv_n * 12; k++) h = (h ^ ((uint8_t*)(uintptr_t)E.kc[i])[k]) * 1099511628211ull;
            for (uint64_t k = 0; k < E.state_bytes; k++) h = (h ^ ((uint8_t*)(uintptr_t)E.vc[i])[k]) * 1099511628211ull;
        }
    for (uint64_t k = 0; k < block * (with_block2 ? 2 : 1); k++) h = (h ^ kvmem[k]) * 1099511628211ull;
    return h;
}
static void wipe(void) {
    for (uint32_t i = 0; i < E.n_layers; i++)
        if (!(E.full_mask >> i & 1)) {
            memset((void*)(uintptr_t)E.kc[i], 0, E.conv_n * 12);
            memset((void*)(uintptr_t)E.vc[i], 0, E.state_bytes);
        }
    memset(kvmem, 0, 2 * block);
}
static int count_kv(void) {
    DIR* d = opendir(CTXCACHE_DIR);
    int n = 0;
    struct dirent* de;
    while (d && (de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 3 && !strcmp(de->d_name + l - 3, ".kv")) n++;
    }
    if (d) closedir(d);
    return n;
}

int main(void) {
    system("rm -rf /tmp/cctest; mkdir -p /tmp/cctest");
    E.n_layers = 8, E.full_mask = 0x88, E.n_kv = 2, E.head_dim = 32, E.hidden = 64, E.max_seq = 16384, E.ctx_limit = 16384;
    E.qwen35 = 1, E.tq = 1, E.tq16 = 1, E.conv_n = 100, E.state_bytes = 4096, E.weights_size = 12345;
    kb = (uint64_t)E.n_kv * 8192 * (E.head_dim / 2 + 4);
    block = 2 * 2 * kb;   // two full layers, k and v
    kvmem = calloc(1, 2 * block);
    E.tq_split = block;
    E.kc = calloc(8, 8), E.vc = calloc(8, 8);
    for (int i = 0; i < 8; i++)
        if (!(E.full_mask >> i & 1)) {
            E.kc[i] = (uint64_t)(uintptr_t)calloc(1, E.conv_n * 12);
            E.vc[i] = (uint64_t)(uintptr_t)calloc(1, E.state_bytes);
        }
    E.kc[3] = (uint64_t)(uintptr_t)kvmem, E.vc[3] = E.kc[3] + kb;
    E.kc[7] = E.kc[3] + 2 * kb, E.vc[7] = E.kc[7] + kb;

    static uint32_t ids[12000], ids2[12000];
    for (int i = 0; i < 12000; i++) ids[i] = ids2[i] = (uint32_t)(i * 7 + 1);
    ids2[1500] ^= 1;   // one token different

    // 1. a prefix of 2000 tokens: save, wipe, load
    fill(1);
    unsigned long long a = checksum(0);
    CHECK(ctxcache_load(&E, ids, 2000) == 0);       // nothing yet
    CHECK(ctxcache_save(&E, ids, 2000) == 1);
    CHECK(count_kv() == 1);
    wipe();
    CHECK(ctxcache_load(&E, ids, 2000) == 1);
    CHECK(checksum(0) == a);
    // 2. another prefix (one token differs), another length, another model file: no snapshot, engine untouched
    fill(2);
    unsigned long long b = checksum(1);
    CHECK(ctxcache_load(&E, ids2, 2000) == 0);
    CHECK(ctxcache_load(&E, ids, 2001) == 0);
    E.weights_size = 999;
    CHECK(ctxcache_load(&E, ids, 2000) == 0);
    E.weights_size = 12345;
    CHECK(checksum(1) == b);
    // 3. longer than 8192 tokens: both blocks
    fill(3);
    unsigned long long c = checksum(1);
    CHECK(ctxcache_save(&E, ids, 9000) == 1);
    wipe();
    CHECK(ctxcache_load(&E, ids, 9000) == 1);
    CHECK(checksum(1) == c);
    // 4. second block lent (ctx_limit 8192): a 9000-token prefix is not cached
    E.ctx_limit = 8192;
    CHECK(ctxcache_save(&E, ids, 9000) == 0 && ctxcache_load(&E, ids, 9000) == 0);
    E.ctx_limit = 16384;
    // 5. truncated and corrupted files are refused without touching the engine
    // 5. truncated files are refused without touching the engine
    char p[300];
    DIR* d = opendir(CTXCACHE_DIR);
    struct dirent* de;
    while ((de = readdir(d)))
        if (strstr(de->d_name, ".kv")) {
            struct stat st;
            snprintf(p, sizeof(p), "%s/%.60s", CTXCACHE_DIR, de->d_name);
            stat(p, &st);
            CHECK(truncate(p, st.st_size - 5) == 0);
        }
    closedir(d);
    fill(4);
    unsigned long long dd = checksum(1);
    CHECK(ctxcache_load(&E, ids, 2000) == 0 && ctxcache_load(&E, ids, 9000) == 0);
    CHECK(checksum(1) == dd);
    // 6. at most 3 snapshots
    for (int k = 0; k < 6; k++) {
        ids2[10] = 1000 + k;
        fill(10 + k);
        CHECK(ctxcache_save(&E, ids2, 1500 + k) == 1);
    }
    CHECK(count_kv() == 3);
    // 7. off switch
    FILE* f = fopen(CTXCACHE_OFF_FILE, "w");
    fputs("0\n", f);
    fclose(f);
    printf(bad ? "ctxcache_test: FAILED\n" : "ctxcache_test: ok\n");
    return bad;
}
