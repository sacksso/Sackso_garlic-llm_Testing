// Text files for the chat (see files.h).
#include "files.h"
#include "zipread.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NAME_MAX_LEN 96

static int ok_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/' || c == ':' || c == '*';
}

static int cmp(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

// Regular files of FILES_DIR/sub ("" or a path such as "out/chat"), names with the "sub/" prefix, appended from index n.
// Folders are entered down to 4 levels (that is how the files saved by /save are laid out). Returns the new count.
static int list_dir(char names[][NAME_MAX_LEN], int n, int max, const char* sub, int depth) {
    char dir[160];
    snprintf(dir, sizeof(dir), "%s%s%s", FILES_DIR, sub[0] ? "/" : "", sub);
    DIR* d = opendir(dir);
    if (!d) return n;
    struct dirent* e;
    while (n < max && (e = readdir(d)) != NULL) {
        const char* nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..") || (nm[0] == '.' && !sub[0])) continue;   // (hidden names only inside out/)
        if (strlen(nm) + strlen(sub) + 2 >= NAME_MAX_LEN) continue;
        char rel[NAME_MAX_LEN], path[240];
        snprintf(rel, sizeof(rel), "%s%s%s", sub, sub[0] ? "/" : "", nm);
        snprintf(path, sizeof(path), FILES_DIR "/%s", rel);
        int f = open(path, O_RDONLY);
        if (f < 0) continue;
        char c;
        int ok = read(f, &c, 1) >= 0;   // (a folder opens too, but read() fails on it)
        close(f);
        if (ok) strcpy(names[n++], rel);
        else if (sub[0] && depth < 4) n = list_dir(names, n, max, rel, depth + 1);
    }
    closedir(d);
    return n;
}

// Regular files of FILES_DIR and of FILES_DIR/out (names only), sorted. Returns how many fit in names.
static int list_files(char names[][NAME_MAX_LEN], int max) {
    int n = list_dir(names, 0, max, "", 0);
    n = list_dir(names, n, max, "out", 1);
    n = list_dir(names, n, max, "web", 1);
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
    unsigned char buf[1024];
    for (;;) {
        int r = read(f, buf, sizeof(buf));
        if (r < 0) {
            close(f);
            return -1;
        }
        if (r == 0) break;
        if (total == 0) {   // a file full of NUL bytes is not text (a zip, an image, a model)
            int zeros = 0;
            for (int i = 0; i < r; i++) zeros += buf[i] < 9 || (buf[i] > 13 && buf[i] < 32);   // control characters
            if (zeros * 20 > r) {
                close(f);
                return -4;
            }
        }
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

static int starts_with(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }

// @repo.zip (the file list), @repo.zip:dir/ or @repo.zip:*.yml (a part of the list), @repo.zip:path/file (one file),
// and @doc.docx / .pptx / .xlsx / .odt / .ods / .odp (the text of the document).
// Returns the text length, or -2 when name is a plain file (not one of these), -1 not found, -3 several files match
// (out holds their names), -4 binary, -5 damaged or unsupported.
static int read_special(const char* name, char* out, int max, long* size, int* cut) {
    char base[NAME_MAX_LEN];
    const char* colon = strchr(name, ':');
    int bl = colon ? (int)(colon - name) : (int)strlen(name);
    if (bl <= 0 || bl >= NAME_MAX_LEN) return -2;
    memcpy(base, name, bl);
    base[bl] = 0;
    const char* spec = colon ? colon + 1 : NULL;
    int is_zip = ends_ci(base, ".zip");
    int is_office = ends_ci(base, ".docx") || ends_ci(base, ".pptx") || ends_ci(base, ".xlsx") || ends_ci(base, ".odt") ||
                    ends_ci(base, ".ods") || ends_ci(base, ".odp");
    if (!is_zip && !(is_office && !spec)) return -2;
    char path[200];
    snprintf(path, sizeof(path), FILES_DIR "/%s", base);
    *cut = 0;
    if (is_office) {
        int n = zip_office_text(path, out, max, cut);
        *size = n;
        return n >= 0 ? n : n == -1 ? -1 : -5;
    }
    const ZipEnt* ents;
    int total;
    int n = zip_open_dir(path, &ents, &total);
    if (n == -1) return -1;
    if (n < 0) return -5;
    int sl = spec ? (int)strlen(spec) : 0;
    if (sl == 0 || spec[0] == '*' || spec[sl - 1] == '/') {   // the list, or a part of it
        int tl = 0;   // a folder shared by every path ("repo-main/", as GitHub makes them) is left out of the lines
        const char* first = n > 0 ? strchr(ents[0].name, '/') : NULL;
        if (first) {
            tl = (int)(first - ents[0].name) + 1;
            for (int i = 0; i < n && tl; i++)
                if (strncmp(ents[i].name, ents[0].name, tl)) tl = 0;
        }
        int o = snprintf(out, max, "%s: %d files%s%.*s\n", base, total, tl ? ", paths inside " : "", tl, ents[0].name);
        int shown = 0, hidden = 0;
        int lim = sl ? max : (max < 6000 ? max : 6000);   // a whole-zip list stays short: the user narrows it with dir/
        for (int i = 0; i < n; i++) {
            const char* nm = ents[i].name;
            const char* rel = nm + tl;
            if (sl) {
                if (spec[0] == '*') {
                    if (!ends_ci(nm, spec + 1)) continue;
                } else if (!starts_with(rel, spec) && !starts_with(nm, spec)) continue;
            }
            if (spec == NULL || spec[0] != '*') {   // images, models, archives... are hidden unless asked with *.ext
                static const char* const ASSET[] = {".bin", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".idx", ".npy", ".prx", ".sprx", ".pkg", ".elf", ".oelf", ".zip", ".gz", ".xz", ".7z", ".mp3", ".mp4", ".ogg", ".wav", ".ttf", ".otf", ".woff", ".woff2", ".pdf", ".so", ".dll", ".exe", ".o", ".a", ".pyc", ".sfo", NULL};
                int asset = 0;
                for (int k = 0; ASSET[k] && !asset; k++) asset = ends_ci(nm, ASSET[k]);
                if (asset) {
                    hidden++;
                    continue;
                }
            }
            int w = snprintf(out + o, lim - o, "%u %s\n", (unsigned)ents[i].usize, rel);
            if (w < 0 || o + w >= lim) {
                o += snprintf(out + o, max - o, "... more files: narrow it with @%.60s:dir/ or @%.60s:*.ext\n", base, base);
                break;
            }
            o += w, shown++;
        }
        if (hidden && o < max - 120) o += snprintf(out + o, max - o, "(+%d binary files not listed; @%.60s:*.bin etc. lists them)\n", hidden, base);
        if (n < total) *cut = 1;
        if (sl && shown == 0 && !hidden) return -1;
        *size = o;
        return o;
    }
    const ZipEnt* hit = NULL;
    int hits = 0;
    int slen = sl;
    for (int pass = 0; pass < 3 && !hits; pass++)
        for (int i = 0; i < n; i++) {
            const char* nm = ents[i].name;
            int ok = 0;
            if (pass == 0) ok = !strcmp(nm, spec);
            else if (pass == 1) {
                const char* sub = strchr(nm, '/');
                ok = sub && !strcmp(sub + 1, spec);
            } else {
                int nl = (int)strlen(nm);
                ok = nl > slen && nm[nl - slen - 1] == '/' && !strcmp(nm + nl - slen, spec);
            }
            if (ok) hit = &ents[i], hits++;
        }
    if (hits == 0) return -1;
    if (hits > 1) {
        int o = 0;
        for (int i = 0; i < n && o < max - 130; i++) {
            int nl = (int)strlen(ents[i].name);
            if (nl > slen && ents[i].name[nl - slen - 1] == '/' && !strcmp(ents[i].name + nl - slen, spec))
                o += snprintf(out + o, max - o, "%s ", ents[i].name);
        }
        return -3;
    }
    int r = zip_read(path, hit, out, max, cut);
    if (r < 0) return hit->method == 0xFFFF ? -5 : -5;
    int zeros = 0, lim2 = r < 400 ? r : 400;
    for (int i = 0; i < lim2; i++) zeros += (unsigned char)out[i] < 9 || ((unsigned char)out[i] > 13 && (unsigned char)out[i] < 32);
    if (zeros * 20 > lim2) return -4;
    int o = 0;
    for (int i = 0; i < r; i++)
        if (out[i] != 0 && out[i] != '\r') out[o++] = out[i];
    out[o] = 0;
    *size = hit->usize;
    return o;
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
        if (l == 0 || strstr(name, "..") || strstr(name, "//") || name[0] == '/' || (name[l - 1] == '/' && !strchr(name, ':'))) continue;
        int dup = 0;
        for (int i = 0; i < asked && i < 8; i++)
            if (!strcmp(seen[i], name)) dup = 1;
        if (dup) continue;
        if (asked < 8) strcpy(seen[asked], name);
        asked++;
        static char body[20000];
        long size = 0;
        int zcut = 0;
        int n = read_special(name, body, sizeof(body), &size, &zcut);
        if (n == -2) n = read_file(name, body, sizeof(body), &size);
        if (n < 0) {
            if (n == -3) note_add(note, note_max, "@%s: several files match, say the path: ", name, 0), note_add(note, note_max, "%s. ", body, 0);
            else if (n == -4) note_add(note, note_max, "@%s: not a text file (binary). ", name, 0);
            else if (n == -5) note_add(note, note_max, "@%s: cannot be read (damaged, zip64 or unsupported). ", name, 0);
            else note_add(note, note_max, "@%s: not found. ", name, 0);
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
        else if (size > (long)sizeof(body) - 1 || zcut) cut = 1;
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
