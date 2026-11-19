// SEARCH/REPLACE blocks (see patch.h).
#include "patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// memmem is not in every libc the console's toolchain has
static const char* mem_find(const char* h, int hl, const char* n, int nl) {
    if (nl <= 0) return h;
    for (int i = 0; i + nl <= hl;) {
        const char* f = memchr(h + i, n[0], hl - nl + 1 - i);
        if (!f) return NULL;
        if (!memcmp(f, n, nl)) return f;
        i = (int)(f - h) + 1;
    }
    return NULL;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int path_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/' || c == '+';
}

// "<<<<<<< BUSCAR", "<<<<<<< SEARCH", "<<<<<<<" ...  kind: '<' '=' '>'
static int marker(const char* p, int len, char kind) {
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    int n = 0;
    while (i + n < len && p[i + n] == kind) n++;
    if (n < 5) return 0;
    if (kind == '=') {   // "=====" alone
        for (int j = i + n; j < len; j++)
            if (p[j] != ' ' && p[j] != '\r') return 0;
    }
    return 1;
}

// A file name written near a block: "Archivo: `a/b.c`", "File: a/b.c", "### a/b.c", "`a/b.c`", "```c a/b.c"
static int name_from_line(const char* p, int len, char* out, int max) {
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '#' || p[i] == '*' || p[i] == '-' || p[i] == '>')) i++;
    // skip a leading word label
    static const char* const LAB[] = {"archivo", "file", "fichero", "ruta", "path", "en", "in", "modificar", "editar", "cambio", "cambios", NULL};
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; LAB[k]; k++) {
            int l = (int)strlen(LAB[k]);
            if (i + l <= len) {
                int ok = 1;
                for (int j = 0; j < l; j++) ok &= lc((unsigned char)p[i + j]) == LAB[k][j];
                if (ok && (i + l == len || p[i + l] == ':' || p[i + l] == ' ' || p[i + l] == '*')) {
                    i += l;
                    while (i < len && (p[i] == ':' || p[i] == ' ' || p[i] == '*')) i++;
                    break;
                }
            }
        }
    }
    if (i + 3 <= len && !strncmp(p + i, "```", 3)) {   // fence line: "```c chat/x.c" -> the second word
        i += 3;
        while (i < len && p[i] != ' ') i++;
        while (i < len && p[i] == ' ') i++;
    }
    while (i < len && (p[i] == '`' || p[i] == '"' || p[i] == '\'')) i++;
    int s = i;
    while (i < len && path_char((unsigned char)p[i])) i++;
    int l = i - s;
    while (l > 0 && (p[s + l - 1] == '.' || p[s + l - 1] == ':')) l--;
    if (l <= 0 || l >= max) return 0;
    int has = 0;
    for (int j = 0; j < l; j++) has |= p[s + j] == '.' || p[s + j] == '/';
    if (!has) return 0;
    // what follows must be nothing, quotes, or a closing colon: a sentence is not a name
    int j = i;
    while (j < len && (p[j] == '`' || p[j] == '"' || p[j] == '\'' || p[j] == ':' || p[j] == '*' || p[j] == ' ' || p[j] == '\r')) j++;
    if (j < len) return 0;
    memcpy(out, p + s, l);
    out[l] = 0;
    if (strstr(out, "..") || out[0] == '/') return 0;
    return 1;
}

int patch_parse(const char* text, PatchBlock* out, int max) {
    int nb = 0, state = 0;   // 0 outside, 1 in search, 2 in replace
    char cur[PATCH_PATH_MAX] = "";
    const char *s_begin = NULL, *s_end = NULL, *r_begin = NULL;
    const char* end = text + strlen(text);
    for (const char* p = text; p < end;) {
        const char* e = memchr(p, '\n', end - p);
        int len = e ? (int)(e - p) : (int)(end - p);
        const char* next = e ? e + 1 : end;
        if (state == 0) {
            if (marker(p, len, '<')) {
                state = 1;
                s_begin = next;
            } else {
                char nm[PATCH_PATH_MAX];
                if (name_from_line(p, len, nm, sizeof(nm))) snprintf(cur, sizeof(cur), "%s", nm);
            }
        } else if (state == 1) {
            if (marker(p, len, '=')) {
                s_end = p;
                r_begin = next;
                state = 2;
            }
        } else {
            if (marker(p, len, '>')) {
                if (nb < max) {
                    out[nb].search = s_begin;
                    out[nb].slen = (int)(s_end - s_begin);
                    out[nb].repl = r_begin;
                    out[nb].rlen = (int)(p - r_begin);
                    snprintf(out[nb].path, sizeof(out[nb].path), "%s", cur);
                    nb++;
                }
                state = 0;
            }
        }
        p = next;
    }
    return nb;
}

int patch_has_blocks(const char* text) {
    PatchBlock b[1];
    return patch_parse(text, b, 1) > 0;
}

// ---------------------------------------------------------------------------------------------------------------------

typedef struct {
    int start, len;   // byte range of a line (len without the line break)
} Ln;

#define MAXLN 30000
static Ln fl[MAXLN];   // lines of the file
static Ln sl[2000];    // lines of the search text

static int split_lines(const char* t, int n, Ln* out, int max) {
    int c = 0;
    for (int i = 0; i < n && c < max;) {
        const char* e = memchr(t + i, '\n', n - i);
        int l = e ? (int)(e - (t + i)) : n - i;
        out[c].start = i;
        out[c].len = l;
        c++;
        i += l + 1;
    }
    return c;
}

static int rtrim_len(const char* p, int l) {
    while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t' || p[l - 1] == '\r')) l--;
    return l;
}

static int ltrim_off(const char* p, int l) {
    int i = 0;
    while (i < l && (p[i] == ' ' || p[i] == '\t')) i++;
    return i;
}

static int indent_of(const char* p, int l) {
    int ind = 0;
    for (int i = 0; i < l && (p[i] == ' ' || p[i] == '\t'); i++) ind += p[i] == '\t' ? 4 : 1;
    return ind;
}

// mode 1: equal ignoring trailing spaces; mode 2: equal ignoring leading and trailing spaces
static int line_eq(const char* a, int al, const char* b, int bl, int mode) {
    al = rtrim_len(a, al);
    bl = rtrim_len(b, bl);
    if (mode == 2) {
        int x = ltrim_off(a, al), y = ltrim_off(b, bl);
        a += x, al -= x, b += y, bl -= y;
    }
    return al == bl && !memcmp(a, b, al);
}

// phase 10: the same line with one or two mistyped characters ("ln" for "in"): a small model copies text almost, not
// exactly. Only long lines (short ones differ by meaning: a = 1 / a = 2), ignoring the indentation.
static int near_line(const char* a, int al, const char* b, int bl) {
    al = rtrim_len(a, al), bl = rtrim_len(b, bl);
    int x = ltrim_off(a, al), y = ltrim_off(b, bl);
    a += x, al -= x, b += y, bl -= y;
    if (al < 14 || bl < 14 || al > 200 || bl > 200 || (al > bl ? al - bl : bl - al) > 2) return 0;
    int prev[202], cur[202];
    for (int j = 0; j <= bl; j++) prev[j] = j;
    for (int i = 1; i <= al; i++) {
        cur[0] = i;
        int best = cur[0];
        for (int j = 1; j <= bl; j++) {
            int c = prev[j - 1] + (a[i - 1] != b[j - 1]);
            if (prev[j] + 1 < c) c = prev[j] + 1;
            if (cur[j - 1] + 1 < c) c = cur[j - 1] + 1;
            cur[j] = c;
            if (c < best) best = c;
        }
        if (best > 2) return 0;
        memcpy(prev, cur, sizeof(int) * (bl + 1));
    }
    return prev[bl] <= 2;
}

static void hint_for(const char* buf, int nfl, const char* search, int slen, char* hint, int hint_max) {
    if (!hint || hint_max < 40) return;
    hint[0] = 0;
    // the first non-blank line of the search
    int snl = split_lines(search, slen, sl, 2000);
    int first = -1;
    for (int i = 0; i < snl; i++)
        if (rtrim_len(search + sl[i].start, sl[i].len) > ltrim_off(search + sl[i].start, sl[i].len)) {
            first = i;
            break;
        }
    if (first < 0) return;
    const char* q = search + sl[first].start;
    int ql = rtrim_len(q, sl[first].len);
    int qo = ltrim_off(q, ql);
    q += qo, ql -= qo;
    int best = -1, bscore = 0;
    for (int i = 0; i < nfl; i++) {
        const char* p = buf + fl[i].start;
        int pl = rtrim_len(p, fl[i].len);
        int po = ltrim_off(p, pl);
        p += po, pl -= po;
        int score = 0;
        if (pl == ql && !memcmp(p, q, ql)) score = 1000;
        else {
            int m = pl < ql ? pl : ql, k = 0;
            while (k < m && p[k] == q[k]) k++;
            score = k;
            if (ql >= 8 && pl >= 8 && mem_find(p, pl, q, ql < 24 ? ql : 24)) score += 12;
        }
        if (score > bscore) bscore = score, best = i;
    }
    if (best < 0 || bscore < 6) return;
    int a = best - 1 < 0 ? 0 : best - 1, b = best + 10 < nfl ? best + 10 : nfl - 1;
    int o = snprintf(hint, hint_max, "closest text in the file (lines %d-%d):\n", a + 1, b + 1);
    for (int i = a; i <= b; i++) {
        int l = fl[i].len;
        if (o + l + 2 >= hint_max) break;
        memcpy(hint + o, buf + fl[i].start, l);
        o += l;
        hint[o++] = '\n';
    }
    hint[o] = 0;
}

// Replaces bytes [from, to) of buf by rep (rl bytes), converting "\n" to "\r\n" when crlf.
static int splice(char* buf, int* len, int cap, int from, int to, const char* rep, int rl, int crlf) {
    int extra = 0;
    if (crlf)
        for (int i = 0; i < rl; i++) extra += rep[i] == '\n' && (i == 0 || rep[i - 1] != '\r');
    int nl = rl + extra;
    int newlen = *len - (to - from) + nl;
    if (newlen + 1 > cap) return -3;
    memmove(buf + from + nl, buf + to, *len - to + 1);
    int o = from;
    for (int i = 0; i < rl; i++) {
        if (crlf && rep[i] == '\n' && (i == 0 || rep[i - 1] != '\r')) buf[o++] = '\r';
        buf[o++] = rep[i];
    }
    *len = newlen;
    return 0;
}

int patch_apply(char* buf, int* len, int cap, const PatchBlock* b, int file_exists, int* line, const char** how, char* hint, int hint_max) {
    if (hint && hint_max > 0) hint[0] = 0;
    *how = "exact";
    *line = 1;
    // an empty search: create the file
    int blank = 1;
    for (int i = 0; i < b->slen; i++) blank &= b->search[i] == '\n' || b->search[i] == ' ' || b->search[i] == '\t' || b->search[i] == '\r';
    if (blank) {
        if (file_exists && *len > 0) return -4;
        if (b->rlen + 1 > cap) return -3;
        memcpy(buf, b->repl, b->rlen);
        buf[b->rlen] = 0;
        *len = b->rlen;
        return 0;
    }
    int crlf = 0;
    {
        const char* nlp = memchr(buf, '\n', *len);
        crlf = nlp && nlp > buf && nlp[-1] == '\r';
    }
    // 1. exact
    int count = 0, at = -1;
    for (const char* p = buf; p && p < buf + *len;) {
        const char* f = mem_find(p, (int)(buf + *len - p), b->search, b->slen);
        if (!f) break;
        count++;
        if (at < 0) at = (int)(f - buf);
        p = f + 1;
    }
    if (count == 1) {
        int l = 1;
        for (int i = 0; i < at; i++) l += buf[i] == '\n';
        *line = l;
        int to = at + b->slen;
        int r = splice(buf, len, cap, at, to, b->repl, b->rlen, 0);
        return r;
    }
    if (count > 1) return -2;
    // 2 and 3: line by line, ignoring trailing spaces / indentation
    int nfl = split_lines(buf, *len, fl, MAXLN);
    int snl = split_lines(b->search, b->slen, sl, 2000);
    while (snl > 0 && rtrim_len(b->search + sl[snl - 1].start, sl[snl - 1].len) == 0) snl--;   // trailing blank lines of the search
    if (snl == 0) return -1;
    for (int mode = 1; mode <= 3; mode++) {   // 3: every line equal ignoring indentation but ONE with a typo
        int hits = 0, hit_at = -1;
        for (int i = 0; i + snl <= nfl; i++) {
            int ok = 1, bad = 0;
            for (int j = 0; j < snl && ok; j++) {
                const char* fa = buf + fl[i + j].start;
                const char* sb = b->search + sl[j].start;
                if (line_eq(fa, fl[i + j].len, sb, sl[j].len, mode == 3 ? 2 : mode)) continue;
                if (mode == 3 && bad == 0 && near_line(fa, fl[i + j].len, sb, sl[j].len)) bad = 1;
                else ok = 0;
            }
            if (mode == 3 && bad == 0) ok = 0;   // (an exact match was already tried)
            if (ok) {
                hits++;
                if (hit_at < 0) hit_at = i;
            }
        }
        if (hits > 1) return -2;
        if (hits == 1) {
            int from = fl[hit_at].start;
            int to = fl[hit_at + snl - 1].start + fl[hit_at + snl - 1].len;
            if (to < *len && buf[to] == '\r') to++;
            if (to < *len && buf[to] == '\n') to++;   // the line break of the last line goes with it
            // the replacement: re-indented when the file's indentation differs
            static char rep[40000];
            int rl = b->rlen;
            if (rl > (int)sizeof(rep) - 2) return -3;
            int delta = 0;
            if (mode >= 2) {
                int fi = hit_at, si = 0;
                while (si < snl && rtrim_len(b->search + sl[si].start, sl[si].len) == ltrim_off(b->search + sl[si].start, sl[si].len)) si++, fi++;
                if (si < snl) delta = indent_of(buf + fl[fi].start, fl[fi].len) - indent_of(b->search + sl[si].start, sl[si].len);
            }
            int ro = 0;
            for (int i = 0; i < rl;) {
                const char* e = memchr(b->repl + i, '\n', rl - i);
                int l = e ? (int)(e - (b->repl + i)) : rl - i;
                const char* p = b->repl + i;
                int blank_line = rtrim_len(p, l) == ltrim_off(p, l);
                if (delta > 0 && !blank_line) {
                    if (ro + delta + l + 2 >= (int)sizeof(rep)) return -3;
                    memset(rep + ro, ' ', delta);
                    ro += delta;
                } else if (delta < 0 && !blank_line) {
                    int cut = 0;
                    while (cut < -delta && cut < l && p[cut] == ' ') cut++;
                    p += cut, l -= cut;
                }
                if (ro + l + 2 >= (int)sizeof(rep)) return -3;
                memcpy(rep + ro, p, l);
                ro += l;
                if (e) rep[ro++] = '\n';
                i += (e ? (int)(e - (b->repl + i)) : rl - i) + (e ? 1 : 0);
            }
            *line = hit_at + 1;
            *how = mode == 1 ? "spaces" : mode == 2 ? "indent" : "typo";
            return splice(buf, len, cap, from, to, rep, ro, crlf);
        }
    }
    hint_for(buf, nfl, b->search, b->slen, hint, hint_max);
    return -1;
}

// ---- phase 8: balance of brackets (see patch.h) ----------------------------------------------------------------------
static int ext_is(const char* path, const char* ext) {
    int pl = (int)strlen(path), el = (int)strlen(ext);
    return pl > el && !strcmp(path + pl - el, ext) && path[pl - el - 1] == '.';
}

int patch_balance(const char* path, const char* t, int len, int out[3]) {
    static const char* const exts[] = {"c", "h", "cc", "cpp", "hpp", "cs", "java", "js", "ts", "css", "go", "rs", "kt", "swift", "json", NULL};
    int known = 0;
    for (int i = 0; exts[i]; i++)
        if (ext_is(path, exts[i])) known = 1;
    out[0] = out[1] = out[2] = 0;
    if (!known) return 0;
    for (int i = 0; i < len; i++) {
        char c = t[i];
        if (c == '/' && i + 1 < len && t[i + 1] == '/') {
            while (i < len && t[i] != '\n') i++;
        } else if (c == '/' && i + 1 < len && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i++;
        } else if (c == '"') {
            for (i++; i < len && t[i] != '"' && t[i] != '\n'; i++)
                if (t[i] == '\\') i++;
        } else if (c == '\'') {   // a character literal only when it looks like one ('x' or '\x'): Rust lifetimes are not
            if (i + 2 < len && t[i + 1] != '\\' && t[i + 2] == '\'') i += 2;
            else if (i + 3 < len && t[i + 1] == '\\' && t[i + 3] == '\'') i += 3;
        } else if (c == '(') out[0]++;
        else if (c == ')') out[0]--;
        else if (c == '{') out[1]++;
        else if (c == '}') out[1]--;
        else if (c == '[') out[2]++;
        else if (c == ']') out[2]--;
    }
    return 1;
}
