// Build log reader (see buildlog.h).
#include "buildlog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "files.h"
#include "zipread.h"

#define LOG_MAX (600 * 1024)
#define MAX_ERR 6

static int ends_ci(const char* s, const char* suf) {
    int a = (int)strlen(s), b = (int)strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

// ---- finding the log -------------------------------------------------------------------------------------------

static long num_in(const char* s) {   // the biggest number in a name
    long best = -1;
    for (const char* p = s; *p; p++)
        if (*p >= '0' && *p <= '9') {
            long v = 0;
            while (*p >= '0' && *p <= '9' && v < 100000000) v = v * 10 + (*p++ - '0');
            if (v > best) best = v;
            p--;
        }
    return best;
}

int buildlog_pick(char* name, int name_max, char* list, int list_max) {
    DIR* d = opendir(FILES_DIR);
    if (!d) return 0;
    struct dirent* de;
    long best = -2;
    int found = 0, lo = 0;
    name[0] = 0;
    list[0] = 0;
    while ((de = readdir(d)) != NULL) {
        const char* nm = de->d_name;
        if (nm[0] == '.') continue;
        int ext = ends_ci(nm, ".zip") || ends_ci(nm, ".txt") || ends_ci(nm, ".log");
        int islog = 0;
        for (const char* p = nm; *p && !islog; p++) islog = !strncasecmp(p, "log", 3) || !strncasecmp(p, "build", 5);
        if (!ext || !islog) continue;
        if (lo < list_max - 100) lo += snprintf(list + lo, list_max - lo, "%s ", nm);
        long v = num_in(nm);
        if (v > best || (v == best && strcmp(nm, name) > 0)) {
            best = v;
            snprintf(name, name_max, "%s", nm);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

// ---- reading the log (cleaned: no timestamps, no colors, no CRs) -------------------------------------------------

static char* read_text(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char* b = (char*)malloc(LOG_MAX + 2);
    if (!b) {
        fclose(f);
        return NULL;
    }
    long r = (long)fread(b, 1, LOG_MAX, f);
    fclose(f);
    b[r] = 0;
    *n = r;
    return b;
}

static char* read_zip_logs(const char* path, long* n) {
    const ZipEnt* ents;
    int total;
    int ne = zip_open_dir(path, &ents, &total);
    if (ne < 0) return NULL;
    char* b = (char*)malloc(LOG_MAX + 2);
    if (!b) return NULL;
    long o = 0;
    for (int i = 0; i < ne && o < LOG_MAX - 4096; i++) {
        const char* nm = ents[i].name;
        const char* dot = strrchr(nm, '.');
        if (dot && strcasecmp(dot, ".txt") && strcasecmp(dot, ".log")) continue;
        if (ents[i].usize == 0) continue;
        o += snprintf(b + o, LOG_MAX - o, "\n===== %s =====\n", nm);
        int cut = 0;
        int room = LOG_MAX - (int)o - 1;
        if (room > 300 * 1024) room = 300 * 1024;
        int r = zip_read(path, &ents[i], b + o, room, &cut);
        if (r > 0) o += r;
    }
    b[o] = 0;
    *n = o;
    return b;
}

// Cleans the text in place into lines: *starts gets the offset of each line. Returns the number of lines.
static int clean_lines(char* t, long n, int** starts) {
    int cap = 4096, nl = 0;
    int* st = (int*)malloc(sizeof(int) * cap);
    if (!st) return 0;
    char* w = t;
    char* r = t;
    char* end = t + n;
    while (r < end) {
        char* line_start = w;
        if (r + 24 < end && r[0] >= '0' && r[0] <= '9' && r[4] == '-' && r[10] == 'T') {   // 2026-11-18T20:41:09.1234567Z
            char* z = memchr(r, 'Z', 34 < end - r ? 34 : end - r);
            if (z && z[1] == ' ') r = z + 2;
        }
        while (r < end && *r != '\n') {
            unsigned char c = (unsigned char)*r;
            if (c == 0x1b && r[1] == '[') {   // color
                r += 2;
                while (r < end && *r != 'm' && *r != '\n') r++;
                if (r < end && *r == 'm') r++;
                continue;
            }
            if (c == '\r') {
                r++;
                continue;
            }
            if (c == 0xEF && r + 2 < end && (unsigned char)r[1] == 0xBB && (unsigned char)r[2] == 0xBF) {
                r += 3;
                continue;
            }
            *w++ = *r++;
        }
        *w++ = 0;
        if (r < end) r++;
        if (nl >= cap) {
            if (cap >= 200000) break;
            cap *= 2;
            int* ns = (int*)realloc(st, sizeof(int) * cap);
            if (!ns) break;
            st = ns;
        }
        st[nl++] = (int)(line_start - t);
    }
    *starts = st;
    return nl;
}

// ---- errors ----------------------------------------------------------------------------------------------------

typedef struct {
    char path[200];
    int line;
    char msg[240];
    int log_line;   // where in the log (its extra lines follow)
    int kind;       // 0 compiler, 1 linker, 2 python, 3 other
} Err;

// "<path>:<line>:<col>: error: msg" or "<path>:<line>: error: msg" in one log line.
static int all_digits(const char* a, const char* b) {
    if (a >= b) return 0;
    for (; a < b; a++)
        if (*a < '0' || *a > '9') return 0;
    return 1;
}

static int parse_compiler(const char* ln, Err* e) {
    const char* p = strstr(ln, ": error:");
    int skip = 8;
    if (!p) {
        p = strstr(ln, ": fatal error:");
        skip = 14;
    }
    if (!p || p == ln) return 0;
    const char* x = p;   // last ':' before the message
    while (x > ln && x[-1] != ':') x--;
    if (x == ln) return 0;
    const char* c1 = x - 1;               // the ':' before the first number
    const char* n1 = c1 + 1;              // number1 = [n1, p)
    if (!all_digits(n1, p)) return 0;
    int num1 = atoi(n1);
    const char* path_end = c1;
    int line = num1;
    const char* y = c1;                   // is there another number before (line:col)?
    while (y > ln && y[-1] != ':') y--;
    if (y > ln && all_digits(y, c1)) {
        line = atoi(y);
        path_end = y - 1;
    }
    int pl = (int)(path_end - ln);
    if (pl <= 0 || pl >= (int)sizeof(e->path)) return 0;
    memcpy(e->path, ln, pl);
    e->path[pl] = 0;
    e->line = line;
    snprintf(e->msg, sizeof(e->msg), "error:%s", p + skip);
    e->kind = 0;
    return 1;
}

static int has_err(const Err* es, int n, const char* path, int line, const char* msg) {
    for (int i = 0; i < n; i++)
        if (es[i].line == line && !strcmp(es[i].path, path) && !strncmp(es[i].msg, msg, 40)) return 1;
    return 0;
}

static int collect(char* t, const int* st, int nl, Err* es) {
    int n = 0;
    for (int i = 0; i < nl && n < MAX_ERR; i++) {
        const char* ln = t + st[i];
        Err e;
        memset(&e, 0, sizeof(e));
        e.log_line = i;
        if (parse_compiler(ln, &e)) {
            if (!has_err(es, n, e.path, e.line, e.msg)) es[n++] = e;
            continue;
        }
        const char* u = strstr(ln, "undefined symbol:");
        if (!u) u = strstr(ln, "undefined reference to");
        if (u) {   // linker: the place that uses it is in the next lines (">>> referenced by file.c:123")
            snprintf(e.msg, sizeof(e.msg), "%.230s", u);
            e.kind = 1;
            for (int k = i + 1; k < nl && k < i + 6; k++) {
                const char* r = strstr(t + st[k], "referenced by ");
                if (!r) continue;
                r += 14;
                const char* c = strrchr(r, ':');
                const char* sp = r;
                while (*sp && *sp != ' ') sp++;
                const char* colon = NULL;
                for (const char* x = r; x < sp; x++)
                    if (*x == ':') colon = x;
                if (colon && colon - r < (int)sizeof(e.path) - 1) {
                    memcpy(e.path, r, colon - r);
                    e.path[colon - r] = 0;
                    e.line = atoi(colon + 1);
                }
                (void)c;
                break;
            }
            if (!has_err(es, n, e.path, e.line, e.msg)) es[n++] = e;
            continue;
        }
        if (!strncmp(ln, "Traceback (most recent call last):", 34)) {   // Python: the last "File" line and the error line
            int k = i + 1;
            const char* lastfile = NULL;
            while (k < nl && (t[st[k]] == ' ' || t[st[k]] == '\t')) {
                if (strstr(t + st[k], "File \"")) lastfile = t + st[k];
                k++;
            }
            if (lastfile) {
                const char* a = strstr(lastfile, "File \"") + 6;
                const char* b = strchr(a, '"');
                const char* ln2 = b ? strstr(b, "line ") : NULL;
                if (b && ln2 && b - a < (int)sizeof(e.path) - 1) {
                    memcpy(e.path, a, b - a);
                    e.path[b - a] = 0;
                    e.line = atoi(ln2 + 5);
                }
            }
            snprintf(e.msg, sizeof(e.msg), "%.230s", k < nl ? t + st[k] : "Python error");
            e.kind = 2;
            e.log_line = k < nl ? k : i;
            if (!has_err(es, n, e.path, e.line, e.msg)) es[n++] = e;
            i = k;
            continue;
        }
        if (!strncmp(ln, "##[error]", 9) && n < MAX_ERR) {
            snprintf(e.msg, sizeof(e.msg), "%.230s", ln + 9);
            e.kind = 3;
            if (!has_err(es, n, "", 0, e.msg)) es[n++] = e;
        }
    }
    int other = 0;   // the generic "Process completed with exit code 1" adds nothing when a real error was found
    for (int i = 0; i < n; i++) other += es[i].kind != 3;
    if (other)
        for (int i = n - 1; i >= 0; i--)
            if (es[i].kind == 3) {
                for (int k = i; k + 1 < n; k++) es[k] = es[k + 1];
                n--;
            }
    return n;
}

// ---- the code around an error ----------------------------------------------------------------------------------

static const char* repo_rel(const char* path) {   // "/home/runner/work/repo/repo/chat/files.c" -> "chat/files.c"
    const char* w = strstr(path, "/work/");
    if (w) {
        const char* p = w + 6;
        for (int k = 0; k < 2; k++) {
            const char* s = strchr(p, '/');
            if (!s) return path;
            p = s + 1;
        }
        return p;
    }
    while (path[0] == '.' && path[1] == '/') path += 2;
    while (*path == '/') path++;
    return path;
}

static int code_around(const char* zip, const char* path, int line, char* out, int max) {
    if (!zip || !zip[0] || line <= 0) return 0;
    const ZipEnt* ents;
    int total;
    int ne = zip_open_dir(zip, &ents, &total);
    if (ne < 0) return 0;
    const char* rel = repo_rel(path);
    int rl = (int)strlen(rel), hit = -1, hits = 0;
    for (int pass = 0; pass < 2 && hit < 0; pass++)
        for (int i = 0; i < ne; i++) {
            const char* nm = ents[i].name;
            int nl = (int)strlen(nm);
            int ok = pass == 0 ? (nl >= rl && !strcmp(nm + nl - rl, rel) && (nl == rl || nm[nl - rl - 1] == '/'))
                               : (strrchr(rel, '/') ? !strcmp(nm + nl - strlen(strrchr(rel, '/') + 1), strrchr(rel, '/') + 1) : 0);
            if (ok) {
                hit = i;
                hits++;
            }
        }
    if (hit < 0) return 0;
    long sz = ents[hit].usize;
    if (sz > 1024 * 1024) return 0;
    char* buf = (char*)malloc(sz + 2);
    if (!buf) return 0;
    int cut = 0;
    int r = zip_read(zip, &ents[hit], buf, (int)sz + 1, &cut);
    if (r <= 0) {
        free(buf);
        return 0;
    }
    int o = snprintf(out, max, "   Code of %s:\n", ents[hit].name);
    int from = line - 5 < 1 ? 1 : line - 5, to = line + 5;
    int cur = 1;
    const char* p = buf;
    const char* e = buf + r;
    while (p < e && cur <= to && o < max - 220) {
        const char* nlp = memchr(p, '\n', e - p);
        int l = nlp ? (int)(nlp - p) : (int)(e - p);
        if (l > 0 && p[l - 1] == '\r') l--;
        if (cur >= from) o += snprintf(out + o, max - o, "   %c%4d| %.*s\n", cur == line ? '>' : ' ', cur, l > 150 ? 150 : l, p);
        cur++;
        p = nlp ? nlp + 1 : e;
    }
    free(buf);
    return o;
}

// ---- the report ------------------------------------------------------------------------------------------------

int buildlog_report(const char* log_path, const char* repo_zip, char* out, int max) {
    long n = 0;
    char* t = ends_ci(log_path, ".zip") ? read_zip_logs(log_path, &n) : read_text(log_path, &n);
    if (!t) return -1;
    int* st = NULL;
    int nl = clean_lines(t, n, &st);
    const char* base = strrchr(log_path, '/') ? strrchr(log_path, '/') + 1 : log_path;
    static Err es[MAX_ERR];
    int ne = st ? collect(t, st, nl, es) : 0;
    int o = 0;
    if (ne == 0) {
        int warn = 0, failed = 0;
        for (int i = 0; i < nl; i++) {
            warn += strstr(t + st[i], "warning:") != NULL;
            failed += strstr(t + st[i], "exit code 1") != NULL || strstr(t + st[i], "Process completed with exit code") != NULL;
        }
        o += snprintf(out, max, "[Build log %s] %d lines: no compiler, linker or Python errors found%s. %d warning line(s).\nLast lines of the log:\n", base, nl,
                      failed ? " (but a step exited with an error: read the end)" : " (the build looks fine)", warn);
        for (int i = nl > 14 ? nl - 14 : 0; i < nl && o < max - 260; i++) o += snprintf(out + o, max - o, "%.200s\n", t + st[i]);
    } else {
        o += snprintf(out, max, "[Build log %s] %d error(s). Fix the first one first (the others are often caused by it):\n", base, ne);
        for (int i = 0; i < ne && o < max - 400; i++) {
            const Err* e = &es[i];
            if (e->path[0]) o += snprintf(out + o, max - o, "%d) %s:%d: %s\n", i + 1, repo_rel(e->path), e->line, e->msg);
            else o += snprintf(out + o, max - o, "%d) %s\n", i + 1, e->msg);
            int lim = e->kind == 0 ? 4 : 0;   // clang prints the source line, a caret and notes after the error
            for (int k = e->log_line + 1; k < nl && k <= e->log_line + lim && o < max - 300; k++) {
                const char* l = t + st[k];
                if (!l[0] || strstr(l, ": error:") || strstr(l, "##[")) break;
                o += snprintf(out + o, max - o, "   | %.180s\n", l);
            }
            if (e->path[0] && e->line > 0 && o < max - 700) {
                int c = code_around(repo_zip, e->path, e->line, out + o, max - o);
                o += c;
            }
        }
    }
    free(st);
    free(t);
    return o;
}
