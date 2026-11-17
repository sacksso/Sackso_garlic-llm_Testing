// HTML / XML to plain text (see htmltext.h).
#include "htmltext.h"

#include <string.h>

typedef struct {
    char* o;
    int n, max;
    int sp, nl, pre;
} Out;

static void raw(Out* w, char c) {
    if (w->n < w->max - 1) w->o[w->n++] = c;
}

static void flush_pending(Out* w) {
    if (w->n > 0 && w->nl) {
        raw(w, '\n');
        if (w->nl > 1) raw(w, '\n');
    } else if (w->n > 0 && w->sp && w->o[w->n - 1] != '\n') {
        raw(w, ' ');
    }
    w->nl = 0, w->sp = 0;
}

static void put(Out* w, unsigned char c) {
    if (c == '\r') return;
    if (c == '\n') {
        if (w->pre) {
            if (w->nl < 1) w->nl = 1;
            else if (w->nl < 2) w->nl = 2;   // (a blank line inside a <pre>)
        } else w->sp = 1;
        return;
    }
    if (c == ' ' || c == '\t') {
        if (w->pre) {
            flush_pending(w);
            raw(w, c);
        } else w->sp = 1;
        return;
    }
    flush_pending(w);
    raw(w, (char)c);
}

static void brk(Out* w, int k) {
    if (k > w->nl) w->nl = k;
}

static void put_utf8(Out* w, unsigned cp) {
    if (cp < 0x80) put(w, (unsigned char)cp);
    else if (cp < 0x800) {
        put(w, (unsigned char)(0xC0 | (cp >> 6)));
        put(w, (unsigned char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put(w, (unsigned char)(0xE0 | (cp >> 12)));
        put(w, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put(w, (unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        put(w, (unsigned char)(0xF0 | (cp >> 18)));
        put(w, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
        put(w, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put(w, (unsigned char)(0x80 | (cp & 0x3F)));
    }
}

// An entity at in[i] == '&'. Returns how many bytes it took (0: not an entity, '&' is plain text).
static int entity(Out* w, const char* in, int i, int n) {
    int j = i + 1;
    while (j < n && j - i < 10 && in[j] != ';' && in[j] != ' ' && in[j] != '&' && in[j] != '<') j++;
    if (j >= n || in[j] != ';') return 0;
    int l = j - i - 1;
    const char* e = in + i + 1;
    if (l == 3 && !memcmp(e, "amp", 3)) put(w, '&');
    else if (l == 2 && !memcmp(e, "lt", 2)) put(w, '<');
    else if (l == 2 && !memcmp(e, "gt", 2)) put(w, '>');
    else if (l == 4 && !memcmp(e, "quot", 4)) put(w, '"');
    else if (l == 4 && !memcmp(e, "apos", 4)) put(w, '\'');
    else if (l == 4 && !memcmp(e, "nbsp", 4)) put(w, ' ');
    else if (l >= 2 && e[0] == '#') {
        unsigned cp = 0;
        int ok = 1;
        if (e[1] == 'x' || e[1] == 'X') {
            for (int k = 2; k < l; k++) {
                char c = e[k];
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) ok = 0;
                else cp = cp * 16 + d;
            }
        } else {
            for (int k = 1; k < l; k++) {
                if (e[k] < '0' || e[k] > '9') ok = 0;
                else cp = cp * 10 + (e[k] - '0');
            }
        }
        if (!ok || cp == 0 || cp > 0x10FFFF) return 0;
        put_utf8(w, cp);
    } else return 0;
    return j - i + 1;
}

static int starts_ci(const char* t, int avail, const char* lit) {
    int l = (int)strlen(lit);
    if (avail < l) return 0;
    for (int i = 0; i < l; i++) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != lit[i]) return 0;
    }
    return 1;
}

static int in_set(const char* name, const char* const* set) {
    for (; *set; set++)
        if (!strcmp(name, *set)) return 1;
    return 0;
}

int html_to_text(const char* in, int n, char* out, int max, int xml) {
    static const char* const BLOCK[] = {"p", "div", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "section", "article", "ul", "ol",
                                        "table", "blockquote", "pre", "header", "footer", "main", "li", "dt", "dd", "title", NULL};
    static const char* const XMLP[] = {"p", "w:p", "a:p", "text:p", "text:h", "si", "row", "w:tr", NULL};
    Out w = {out, 0, max, 0, 0, 0};
    int i = 0;
    while (i < n && w.n < max - 1) {
        char c = in[i];
        if (c == '<') {
            if (starts_ci(in + i, n - i, "<!--")) {
                int j = i + 4;
                while (j + 2 < n && memcmp(in + j, "-->", 3)) j++;
                i = j + 3 < n ? j + 3 : n;
                continue;
            }
            int j = i + 1;
            int closing = j < n && in[j] == '/';
            if (closing) j++;
            char name[24];
            int l = 0;
            while (j < n && l < 23 && in[j] != '>' && in[j] != ' ' && in[j] != '/' && in[j] != '\n' && in[j] != '\t') {
                char ch = in[j++];
                name[l++] = ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
            }
            name[l] = 0;
            char q = 0;   // the end of the tag, with quoted attributes skipped
            int selfclose = 0;
            while (j < n && !(in[j] == '>' && !q)) {
                if (q) {
                    if (in[j] == q) q = 0;
                } else if (in[j] == '"' || in[j] == '\'') q = in[j];
                selfclose = in[j] == '/';
                j++;
            }
            int next = j < n ? j + 1 : n;
            if (!xml && !closing && (!strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "noscript") || !strcmp(name, "svg"))) {
                const char* endtag = !strcmp(name, "script") ? "</script" : !strcmp(name, "style") ? "</style" : !strcmp(name, "svg") ? "</svg" : "</noscript";
                int k = next;
                while (k < n && !starts_ci(in + k, n - k, endtag)) k++;
                while (k < n && in[k] != '>') k++;
                i = k < n ? k + 1 : n;
                continue;
            }
            if (xml) {
                if (closing && in_set(name, XMLP)) brk(&w, 1);
                else if (!closing && (!strcmp(name, "w:br") || !strcmp(name, "text:line-break"))) brk(&w, 1);
                else if (!closing && (!strcmp(name, "w:tab") || !strcmp(name, "text:tab"))) w.sp = 1;
            } else {
                if (!strcmp(name, "br") || !strcmp(name, "hr")) brk(&w, 1);
                else if (in_set(name, BLOCK)) {
                    brk(&w, !strcmp(name, "li") || !strcmp(name, "tr") ? 1 : 2);
                    if (!closing && !strcmp(name, "li")) {
                        put(&w, '-');
                        w.sp = 1;
                    }
                    if (!strcmp(name, "pre")) w.pre = !closing;
                }
            }
            (void)selfclose;
            i = next;
            continue;
        }
        if (c == '&') {
            int k = entity(&w, in, i, n);
            if (k) {
                i += k;
                continue;
            }
        }
        put(&w, (unsigned char)c);
        i++;
    }
    while (w.n > 0 && (out[w.n - 1] == '\n' || out[w.n - 1] == ' ')) w.n--;
    out[w.n] = 0;
    return w.n;
}
