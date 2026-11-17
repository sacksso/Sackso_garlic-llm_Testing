// Text files for the chat (see files.h).
#include "files.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NAME_MAX_LEN 64

static int ok_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static int cmp(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

// Regular files of FILES_DIR (names only), sorted. Returns how many fit in names.
static int list_files(char names[][NAME_MAX_LEN], int max) {
    DIR* d = opendir(FILES_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while (n < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= NAME_MAX_LEN) continue;
        char path[160];
        snprintf(path, sizeof(path), FILES_DIR "/%s", e->d_name);
        int f = open(path, O_RDONLY);   // (a folder opens too, but read() fails on it: skip it)
        if (f < 0) continue;
        char c;
        int ok = read(f, &c, 1) >= 0;
        close(f);
        if (ok) strcpy(names[n++], e->d_name);
    }
    closedir(d);
    qsort(names, n, NAME_MAX_LEN, cmp);
    return n;
}

// Reads up to max-1 bytes of a file; *size gets its full size. NULs and CRs are dropped. -1 when it cannot be read.
static int read_file(const char* name, char* out, int max, long* size) {
    char path[160];
    snprintf(path, sizeof(path), FILES_DIR "/%s", name);
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    int n = 0;
    long total = 0;
    char buf[1024];
    for (;;) {
        int r = read(f, buf, sizeof(buf));
        if (r < 0) {
            close(f);
            return -1;
        }
        if (r == 0) break;
        total += r;
        for (int i = 0; i < r && n < max - 1; i++)
            if (buf[i] != 0 && buf[i] != '\r') out[n++] = buf[i];
    }
    close(f);
    out[n] = 0;
    *size = total;
    return n;
}

// Cuts s (length n) back to a line break, or at least to a whole UTF-8 character. Returns the new length.
static int cut_clean(char* s, int n) {
    int i = n;
    while (i > n / 2 && s[i - 1] != '\n') i--;
    if (i > n / 2) n = i;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = 0;
    return n;
}

static void note_add(char* note, int note_max, const char* fmt, const char* a, long b) {
    int l = strlen(note);
    if (l >= note_max - 1) return;
    snprintf(note + l, note_max - l, fmt, a, b);
}

int files_expand(const char* text, char* ctx, int ctx_max, char* note, int note_max) {
    ctx[0] = 0;
    note[0] = 0;
    int used = 0, attached = 0, asked = 0;
    char seen[8][NAME_MAX_LEN];
    for (const char* p = text; *p; p++) {
        if (*p != '@' || (p != text && p[-1] != ' ' && p[-1] != '\n' && p[-1] != '\t')) continue;
        char name[NAME_MAX_LEN];
        int l = 0;
        while (l < NAME_MAX_LEN - 1 && ok_name_char((unsigned char)p[1 + l])) l++;
        memcpy(name, p + 1, l);
        name[l] = 0;
        if (l == 0 || strstr(name, "..")) continue;
        int dup = 0;
        for (int i = 0; i < asked && i < 8; i++)
            if (!strcmp(seen[i], name)) dup = 1;
        if (dup) continue;
        if (asked < 8) strcpy(seen[asked], name);
        asked++;
        static char body[20000];
        long size = 0;
        int n = read_file(name, body, sizeof(body), &size);
        if (n < 0) {
            note_add(note, note_max, "@%s: not found. ", name, 0);
            continue;
        }
        char head[NAME_MAX_LEN + 24];
        int hl = snprintf(head, sizeof(head), "File %s:\n```\n", name);
        const char* tail = "\n```\n";
        int room = ctx_max - used - hl - (int)strlen(tail) - 1;
        if (room < 200) {
            note_add(note, note_max, "@%s: no room left. ", name, 0);
            continue;
        }
        int cut = 0;
        if (n > room) n = cut_clean(body, room), cut = 1;
        else if (size > (long)sizeof(body) - 1) cut = 1;
        if (n > 0 && body[n - 1] == '\n') n--;
        memcpy(ctx + used, head, hl);
        used += hl;
        memcpy(ctx + used, body, n);
        used += n;
        memcpy(ctx + used, tail, strlen(tail));
        used += strlen(tail);
        ctx[used] = 0;
        attached++;
        if (cut) note_add(note, note_max, "@%s: cut to the first %ld bytes. ", name, n);
        else note_add(note, note_max, "@%s attached (%ld bytes). ", name, size);
    }
    const char* q = text;   // a message that is only "@": list the files and send nothing to the model
    while (*q == ' ' || *q == '\n') q++;
    if (*q == '@' && q[1] == 0) {
        static char names[40][NAME_MAX_LEN];
        int c = list_files(names, 40);
        if (c == 0) {
            snprintf(note, note_max, "No files in " FILES_DIR " (upload them over FTP).");
        } else {
            int l = snprintf(note, note_max, "Files (type @name): ");
            for (int i = 0; i < c && l < note_max - NAME_MAX_LEN; i++) l += snprintf(note + l, note_max - l, "%s ", names[i]);
        }
        return -1;
    }
    return attached;
}
