// Saving files from a reply (see outfiles.h).
#include "outfiles.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_MAX_LEN 96
#define MAX_FILES 12
#define MAX_BYTES 60000

static int path_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-' || c == '/';
}

// A relative path made of safe characters, at most 4 levels, no "..", no empty or "." component.
static int path_ok(const char* p) {
    int n = (int)strlen(p);
    if (n == 0 || n >= PATH_MAX_LEN || p[0] == '/' || p[n - 1] == '/') return 0;
    int depth = 1, clen = 0;
    for (int i = 0; i < n; i++) {
        if (!path_char((unsigned char)p[i])) return 0;
        if (p[i] == '/') {
            if (clen == 0) return 0;
            depth++, clen = 0;
        } else clen++;
    }
    if (depth > 4 || strstr(p, "..") || strstr(p, "/./") || (p[0] == '.' && p[1] == '/')) return 0;
    return strchr(p, '.') != NULL || strchr(p, '/') != NULL ? 1 : 0;
}

// The first run of path characters of s (up to len) that is a valid path and has an extension in its last part.
static int pick_path(const char* s, int len, char* out) {
    int i = 0;
    while (i < len) {
        while (i < len && !path_char((unsigned char)s[i])) i++;
        int j = i;
        while (j < len && path_char((unsigned char)s[j])) j++;
        if (j > i && j - i < PATH_MAX_LEN) {
            memcpy(out, s + i, j - i);
            out[j - i] = 0;
            const char* base = strrchr(out, '/');
            base = base ? base + 1 : out;
            if (path_ok(out) && strchr(base + (base[0] == '.'), '.')) return 1;
        }
        i = j + 1;
    }
    return 0;
}

static int starts_ci(const char* t, const char* lit) {   // case-insensitive prefix test (no strncasecmp on the console)
    for (; *lit; t++, lit++) {
        char c = *t;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != *lit) return 0;
    }
    return 1;
}

static void mkdirs(const char* rel) {
    char dir[160];
    snprintf(dir, sizeof(dir), "%s", OUT_DIR);
    mkdir("/data/garlic_files", 0777);
    mkdir(dir, 0777);
    int l = (int)strlen(dir);
    for (const char* p = rel; *p; p++) {
        if (*p == '/' && l + (p - rel) < (int)sizeof(dir) - 1) {
            char sub[160];
            snprintf(sub, sizeof(sub), "%s/%.*s", OUT_DIR, (int)(p - rel), rel);
            mkdir(sub, 0777);
        }
    }
}

static int write_file(const char* rel, const char* data, int n) {
    mkdirs(rel);
    char path[220];
    snprintf(path, sizeof(path), "%s/%s", OUT_DIR, rel);
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f < 0) return -1;
    int done = 0;
    while (done < n) {
        int w = write(f, data + done, n - done);
        if (w <= 0) {
            close(f);
            return -1;
        }
        done += w;
    }
    close(f);
    return 0;
}

int outfiles_write(const char* rel, const char* data, int n) {
    if (!path_ok(rel)) return -1;
    return write_file(rel, data, n);
}

int outfiles_path_ok(const char* rel) { return path_ok(rel); }

int outfiles_is_cmd(const char* text) {
    while (*text == ' ' || *text == '\n' || *text == '\t') text++;
    int n = (int)strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\n' || text[n - 1] == '\t')) n--;
    static const char* cmds[] = {"/save", "/guardar"};
    for (int k = 0; k < 2; k++) {
        int l = (int)strlen(cmds[k]);
        if (n != l) continue;
        int same = 1;
        for (int i = 0; i < l; i++) {
            char c = text[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != cmds[k][i]) same = 0;
        }
        if (same) return 1;
    }
    return 0;
}

static void note_add(char* note, int max, const char* fmt, const char* a, long b) {
    int l = (int)strlen(note);
    if (l >= max - 1) return;
    snprintf(note + l, max - l, fmt, a, b);
}

int outfiles_save(const char* reply, char* note, int note_max) {
    note[0] = 0;
    static char body[MAX_BYTES + 1];
    int written = 0, unnamed = 0, bad = 0, edits = 0;
    char inherit[PATH_MAX_LEN] = {0};   // a name given by a block that turned out empty (see below)
    const char* p = reply;
    const char* prev_line = NULL;   // start of the last non-empty line before the fence
    int prev_len = 0;
    while (*p) {
        const char* eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        if (len >= 3 && !strncmp(p, "```", 3)) {   // an opening fence
            char name[PATH_MAX_LEN] = {0};
            int have = pick_path(p + 3, len - 3, name);
            if (!have && inherit[0]) strcpy(name, inherit), have = 1;
            inherit[0] = 0;
            const char* b = eol ? eol + 1 : p + len;
            const char* end = NULL;   // the closing fence: a line that starts with ```
            for (const char* q = b; *q;) {
                const char* e2 = strchr(q, '\n');
                if (!strncmp(q, "```", 3)) {
                    end = q;
                    break;
                }
                if (!e2) break;
                q = e2 + 1;
            }
            if (!end) break;   // an unfinished block (the answer was cut): not saved
            int blen = (int)(end - b);
            if (blen > 0 && b[blen - 1] == '\n') blen--;
            if (!have) {   // "# file: path" or "// file: path" on the first line of the block
                const char* e1 = memchr(b, '\n', blen);
                int l1 = e1 ? (int)(e1 - b) : blen;
                const char* tag = NULL;
                for (const char* t = b; t < b + l1; t++)
                    if (starts_ci(t, "file:") || starts_ci(t, "archivo:")) {
                        tag = t;
                        break;
                    }
                int off = starts_ci(tag ? tag : "", "file:") ? 5 : 8;
                if (tag && pick_path(tag + off, (int)(b + l1 - tag - off), name)) {
                    have = 1;
                    if (e1) b = e1 + 1, blen -= l1 + 1;   // the marker line is not part of the file
                    else blen = 0;                         // (it was the only line)
                }
            }
            if (!have && prev_line) {   // `path` in backticks on the line before the block
                const char* t = prev_line;
                const char* lend = prev_line + prev_len;
                while (t < lend && !have) {
                    const char* s = memchr(t, '`', lend - t);
                    if (!s) break;
                    const char* e = memchr(s + 1, '`', lend - s - 1);
                    if (!e) break;
                    char cand[PATH_MAX_LEN] = {0};
                    if (e - s - 1 < PATH_MAX_LEN && pick_path(s + 1, (int)(e - s - 1), cand)) {
                        strcpy(name, cand);
                        have = 1;
                    }
                    t = e + 1;
                }
            }
            {   // the model sometimes writes "```yaml", the name line and then another "```yaml": the first block is only the
                // name, and the line that looked like its closing fence opens the real one
                int empty = 1;
                for (int i = 0; i < blen; i++)
                    if (b[i] != ' ' && b[i] != '\n' && b[i] != '\t' && b[i] != '\r') empty = 0;
                const char* eend = strchr(end, '\n');
                int elen = eend ? (int)(eend - end) : (int)strlen(end);
                if (have && empty && elen > 3) {
                    strcpy(inherit, name);
                    p = end;
                    prev_line = NULL;
                    continue;
                }
                if (empty) {   // nothing to write
                    const char* after0 = strchr(end, '\n');
                    p = after0 ? after0 + 1 : end + strlen(end);
                    prev_line = NULL;
                    continue;
                }
            }
            int is_edit = 0;   // SEARCH/REPLACE blocks belong to /aplicar, not to a file
            for (int i = 0; i + 5 < blen; i++)
                if ((i == 0 || b[i - 1] == '\n') && !strncmp(b + i, "<<<<<", 5)) is_edit = 1;
            if (is_edit) edits++;
            else if (!have) unnamed++;
            else if (written >= MAX_FILES || blen > MAX_BYTES) bad++;
            else {
                int n = 0;
                for (int i = 0; i < blen; i++)
                    if (b[i] != '\r') body[n++] = b[i];
                if (n > 0 && body[n - 1] != '\n') body[n++] = '\n';
                if (write_file(name, body, n) == 0) {
                    written++;
                    note_add(note, note_max, "%s (%ld B). ", name, n);
                } else bad++;
            }
            const char* after = strchr(end, '\n');
            p = after ? after + 1 : end + strlen(end);
            prev_line = NULL;
            continue;
        }
        if (len > 0) prev_line = p, prev_len = len;
        p = eol ? eol + 1 : p + len;
    }
    char tail[160];
    tail[0] = 0;
    if (written) snprintf(tail, sizeof(tail), "Saved %d in " OUT_DIR ". ", written);
    else if (edits) snprintf(tail, sizeof(tail), "Nothing saved as a file: the answer holds edit blocks, use /aplicar. ");
    else snprintf(tail, sizeof(tail), "Nothing saved: no named code block in the last answer. ");
    char all[320];
    snprintf(all, sizeof(all), "%s%s", tail, note);
    snprintf(note, note_max, "%s", all);
    int l = (int)strlen(note);
    if (unnamed && l < note_max - 1) l += snprintf(note + l, note_max - l, "%d block(s) without a name skipped. ", unnamed);
    if (edits && written && l < note_max - 1) l += snprintf(note + l, note_max - l, "%d edit block(s) left for /aplicar. ", edits);
    if (bad && l < note_max - 1) snprintf(note + l, note_max - l, "%d block(s) not saved (too big, too many or write error). ", bad);
    return written;
}
