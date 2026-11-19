// Undo (see undo.h).
#include "undo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEEP 20
#define MAXF 60

static int g_active, g_n, g_nf;   // g_n: the current batch's number; g_nf: files recorded in it
static char g_seen[MAXF][220];

static void ensure_dir(void) { mkdir(UNDO_DIR, 0777); }

static int read_int(const char* path) {
    FILE* f = fopen(path, "r");
    char b[24] = "";
    int v = 0;
    if (f) {
        if (fgets(b, sizeof(b), f)) v = atoi(b);
        fclose(f);
    }
    return v;
}

static void write_int(const char* path, int v) {
    FILE* f = fopen(path, "w");
    if (f) {
        char b[24];
        int l = snprintf(b, sizeof(b), "%d\n", v);
        fwrite(b, 1, l, f);
        fclose(f);
    }
}

static int copy_file(const char* from, const char* to) {
    FILE* a = fopen(from, "rb");
    if (!a) return -1;
    FILE* b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return -1;
    }
    char buf[4096];
    size_t r;
    int rc = 0;
    while ((r = fread(buf, 1, sizeof(buf), a)) > 0)
        if (fwrite(buf, 1, r, b) != r) {
            rc = -1;
            break;
        }
    fclose(a);
    if (fclose(b)) rc = -1;
    return rc;
}

static int exists(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void batch_path(int n, const char* what, char* out, int max) { snprintf(out, max, UNDO_DIR "/%d%s", n, what); }

// remove the files of an old batch (its journal lists them)
static void prune(int n) {
    char jp[200], line[300];
    batch_path(n, ".txt", jp, sizeof(jp));
    FILE* f = fopen(jp, "r");
    if (!f) return;
    int idx = 0;
    while (fgets(line, sizeof(line), f)) {
        char bp[200];
        snprintf(bp, sizeof(bp), UNDO_DIR "/%d_%d.bak", n, idx++);
        unlink(bp);
    }
    fclose(f);
    unlink(jp);
}

void undo_begin(void) {
    ensure_dir();
    g_n = read_int(UNDO_DIR "/counter.txt") + 1;
    write_int(UNDO_DIR "/counter.txt", g_n);
    g_nf = 0;
    g_active = 1;
    if (g_n > KEEP) prune(g_n - KEEP);
}

void undo_end(void) { g_active = 0; }
int undo_active(void) { return g_active; }

void undo_record(const char* path) {
    if (!g_active) undo_begin();   // (a write outside any batch gets its own)
    if (g_nf >= MAXF) return;
    for (int i = 0; i < g_nf; i++)
        if (!strcmp(g_seen[i], path)) return;   // (the first old content of the batch is the one to keep)
    char jp[200], bp[200];
    batch_path(g_n, ".txt", jp, sizeof(jp));
    int had = exists(path);
    snprintf(bp, sizeof(bp), UNDO_DIR "/%d_%d.bak", g_n, g_nf);
    if (had && copy_file(path, bp) != 0) return;   // could not keep the old file: nothing to promise for it
    FILE* f = fopen(jp, "a");
    if (!f) return;
    char jl[300];
    int jn = snprintf(jl, sizeof(jl), "%c %s\n", had ? 'B' : 'N', path);
    fwrite(jl, 1, jn > (int)sizeof(jl) - 1 ? (int)sizeof(jl) - 1 : jn, f);
    fclose(f);
    snprintf(g_seen[g_nf], sizeof(g_seen[0]), "%s", path);
    g_nf++;
    write_int(UNDO_DIR "/last.txt", g_n);
}

int undo_last(char* note, int note_max) {
    int n = read_int(UNDO_DIR "/last.txt");
    char jp[200];
    while (n > 0) {   // the newest batch that still has its journal
        batch_path(n, ".txt", jp, sizeof(jp));
        if (exists(jp)) break;
        n--;
    }
    if (n <= 0) {
        snprintf(note, note_max, "Nothing to undo (the AI has not changed any file yet, or the last 20 batches are already undone).");
        return 0;
    }
    FILE* f = fopen(jp, "r");
    char line[300];
    int idx = 0, done = 0, failed = 0;
    char names[500] = "";
    while (f && fgets(line, sizeof(line), f)) {
        int l = (int)strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l < 3) {
            idx++;
            continue;
        }
        const char* path = line + 2;
        char bp[200];
        snprintf(bp, sizeof(bp), UNDO_DIR "/%d_%d.bak", n, idx);
        int ok;
        if (line[0] == 'B') ok = copy_file(bp, path) == 0;
        else ok = unlink(path) == 0 || !exists(path);
        if (ok) {
            done++;
            const char* base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            int nl = (int)strlen(names);
            if (nl < (int)sizeof(names) - 60) snprintf(names + nl, sizeof(names) - nl, "%s%s%s", nl ? ", " : "", base, line[0] == 'N' ? " (removed)" : "");
        } else failed++;
        unlink(bp);
        idx++;
    }
    if (f) fclose(f);
    unlink(jp);
    write_int(UNDO_DIR "/last.txt", n - 1);
    snprintf(note, note_max, "Undone: %d file(s) back as they were before the last change%s%s. %s", done, done ? ": " : "", names, failed ? "Some could not be restored." : "(/deshacer again goes one batch further back.)");
    return done;
}
