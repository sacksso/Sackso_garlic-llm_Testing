// Network commands of the chat (see webcmd.h).
#include "webcmd.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "htmltext.h"
#include "netget.h"
#include "files.h"

#define PENDING_MAX 8200

static char page[64 * 1024 + 1];   // the downloaded body
static char text[16000];           // the same as plain text
static char pending[PENDING_MAX];
static int pending_n;
static char last_urls[8][300];     // the links of the last search, for /abrir N
static char last_titles[8][90];
static int last_n;

static int enabled(void) {
    char c = '0';
    int f = open(WEB_FLAG_FILE, O_RDONLY);
    if (f >= 0) {
        if (read(f, &c, 1) != 1) c = '0';
        close(f);
    }
    return c == '1';
}

static void set_enabled(int on) {
    int f = open(WEB_FLAG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f >= 0) {
        write(f, on ? "1\n" : "0\n", 2);
        close(f);
    }
}

static int lower_eq(const char* a, int al, const char* lit) {
    if ((int)strlen(lit) != al) return 0;
    for (int i = 0; i < al; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != lit[i]) return 0;
    }
    return 1;
}

// The command word of "/word args": returns its id (0 none) and points *args after it.
enum { C_NONE, C_NET, C_GH, C_URL, C_WEB, C_CLEAR, C_HELP, C_OPEN, C_WIKI, C_STATUS, C_UNKNOWN };

static int parse_cmd(const char* t, const char** args) {
    while (*t == ' ' || *t == '\n' || *t == '\t') t++;
    if (*t != '/') return C_NONE;
    const char* w = t + 1;
    int l = 0;
    while (w[l] && w[l] != ' ' && w[l] != '\n' && w[l] != '\t') l++;
    const char* a = w + l;
    while (*a == ' ' || *a == '\t') a++;
    *args = a;
    if (lower_eq(w, l, "red") || lower_eq(w, l, "net")) return C_NET;
    if (lower_eq(w, l, "gh") || lower_eq(w, l, "g")) return C_GH;
    if (lower_eq(w, l, "url") || lower_eq(w, l, "u")) return C_URL;
    if (lower_eq(w, l, "web") || lower_eq(w, l, "buscar") || lower_eq(w, l, "search") || lower_eq(w, l, "b")) return C_WEB;
    if (lower_eq(w, l, "abrir") || lower_eq(w, l, "open") || lower_eq(w, l, "o")) return C_OPEN;
    if (lower_eq(w, l, "wiki") || lower_eq(w, l, "w")) return C_WIKI;
    if (lower_eq(w, l, "estado") || lower_eq(w, l, "status")) return C_STATUS;
    if (lower_eq(w, l, "limpiar") || lower_eq(w, l, "clear")) return C_CLEAR;
    if (lower_eq(w, l, "ayuda") || lower_eq(w, l, "help") || lower_eq(w, l, "?")) return C_HELP;
    if (l >= 2 && l <= 12) {   // "/word" made only of letters is a mistyped command, not text for the model
        int letters = 1;
        for (int i = 0; i < l; i++) {
            char c = w[i] | 32;
            if (c < 'a' || c > 'z') letters = 0;
        }
        if (letters) return C_UNKNOWN;
    }
    return C_NONE;
}

int webcmd_is_cmd(const char* text_) {
    const char* a;
    return parse_cmd(text_, &a) != C_NONE;
}

int webcmd_net_on(void) { return enabled(); }
int webcmd_net_set(int on) { set_enabled(on); return enabled(); }

int webcmd_pending_len(void) { return pending_n; }

// 1 for the commands that go to the network (the screen shows a "working" line first).
int webcmd_is_fetch(const char* text_) {
    const char* a;
    int c = parse_cmd(text_, &a);
    return (c == C_GH || c == C_URL || c == C_WEB || c == C_OPEN || c == C_WIKI) && enabled();
}

int webcmd_take_pending(char* dst, int max) {
    int n = pending_n < max ? pending_n : max;
    memcpy(dst, pending, n);
    pending_n = 0;
    return n;
}

// ---- helpers -----------------------------------------------------------------------------------------------------

static int cut_utf8(const char* s, int n) {   // the largest length <= n that ends at a line break or a whole character
    int i = n;
    while (i > n / 2 && s[i - 1] != '\n') i--;
    if (i > n / 2) n = i;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

// Queues body for the next message. Returns the bytes of body that fit.
static int add_pending(const char* title, const char* body) {
    char head[160];
    int hl = snprintf(head, sizeof(head), "Web %s:\n```\n", title);
    static const char tail[] = "\n```\n";
    int room = PENDING_MAX - 1 - pending_n - hl - (int)sizeof(tail) + 1;
    if (room < 120) return 0;
    int bl = (int)strlen(body);
    if (bl > room) bl = cut_utf8(body, room);
    memcpy(pending + pending_n, head, hl);
    pending_n += hl;
    memcpy(pending + pending_n, body, bl);
    pending_n += bl;
    memcpy(pending + pending_n, tail, sizeof(tail) - 1);
    pending_n += (int)sizeof(tail) - 1;
    return bl;
}

static void slugify(const char* in, char* out, int max) {
    int n = 0, dash = 1;
    for (; *in && n < max - 5; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = c, dash = 0;
        else if (!dash) out[n++] = '-', dash = 1;
    }
    while (n > 0 && out[n - 1] == '-') n--;
    if (n == 0) out[n++] = 'x';
    out[n] = 0;
}

static void save_copy(const char* slug, const char* ext, const char* data, int n, char* saved, int smax) {
    saved[0] = 0;
    mkdir("/data/garlic_files", 0777);
    mkdir(WEB_DIR, 0777);
    char path[200];
    snprintf(path, sizeof(path), WEB_DIR "/%s.%s", slug, ext);
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f < 0) return;
    int done = 0;
    while (done < n) {
        int w = write(f, data + done, n - done);
        if (w <= 0) break;
        done += w;
    }
    close(f);
    snprintf(saved, smax, "@web/%s.%s", slug, ext);
}

static int ok_repo_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static int ok_path(const char* p) {
    if (strstr(p, "..") || p[0] == '/') return 0;
    for (; *p; p++)
        if (!ok_repo_char((unsigned char)*p) && *p != '/' && *p != '+' && *p != '@') return 0;
    return 1;
}

static void url_encode(const char* in, char* out, int max) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *in && n < max - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out[n++] = c;
        else if (c == ' ') out[n++] = '+';
        else out[n++] = '%', out[n++] = hex[c >> 4], out[n++] = hex[c & 15];
    }
    out[n] = 0;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void url_decode(const char* in, int n, char* out, int max) {   // stops at '&'
    int o = 0;
    for (int i = 0; i < n && in[i] != '&' && o < max - 1; i++) {
        if (in[i] == '%' && i + 2 < n && hexv(in[i + 1]) >= 0 && hexv(in[i + 2]) >= 0) {
            out[o++] = (char)(hexv(in[i + 1]) * 16 + hexv(in[i + 2]));
            i += 2;
        } else out[o++] = in[i] == '+' ? ' ' : in[i];
    }
    out[o] = 0;
}

// GET into page. Returns the body length, or < 0 after writing the failure into note.
static int fetch(const char* url, char* note, int nm) {
    int status = 0;
    int n = net_get(url, page, sizeof(page), &status);
    if (n < 0) {
        snprintf(note, nm, "Network error: %s", net_last_error());
        return -1;
    }
    if (status >= 400) {
        snprintf(note, nm, "HTTP %d from %.80s", status, url);
        return -1;
    }
    return n;
}

static int looks_html(const char* s, int n) {
    int lim = n < 3000 ? n : 3000;
    for (int i = 0; i + 5 < lim; i++)
        if (s[i] == '<') {
            char a = s[i + 1] | 32, b = s[i + 2] | 32, c = s[i + 3] | 32;
            if ((a == 'h' && b == 't' && c == 'm') || (a == '!' && b == 'd' && c == 'o') || (a == 'b' && b == 'o' && c == 'd') || (a == 'h' && b == 'e' && c == 'a')) return 1;
        }
    return 0;
}

// ---- /url ----------------------------------------------------------------------------------------------------------

static void cmd_url(const char* arg, char* note, int nm) {
    char url[300];
    int l = 0;
    while (arg[l] && arg[l] != ' ' && arg[l] != '\n' && l < (int)sizeof(url) - 1) l++;
    memcpy(url, arg, l);
    url[l] = 0;
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
        snprintf(note, nm, "Usage: /url https://address");
        return;
    }
    int n = fetch(url, note, nm);
    if (n < 0) return;
    int tl;
    if (looks_html(page, n)) tl = html_to_text(page, n, text, sizeof(text), 0);
    else {
        tl = 0;
        for (int i = 0; i < n && tl < (int)sizeof(text) - 1; i++)
            if (page[i] != '\r' && page[i] != 0) text[tl++] = page[i];
        text[tl] = 0;
    }
    char slug[64], saved[96];
    slugify(url + (url[4] == 's' ? 8 : 7), slug, sizeof(slug) - 1);
    save_copy(slug, "txt", text, tl, saved, sizeof(saved));
    int added = add_pending(url, text);
    snprintf(note, nm, "%.60s: %d bytes -> %d chars of text, %d queued for your next message%s. Saved as %s", url, n, tl, added, added < tl ? " (cut)" : "", saved);
}

// ---- /gh -----------------------------------------------------------------------------------------------------------

// Lists the files of a GitHub trees API answer (JSON) as lines into out; prefix filters (may be ""). Returns the count.
static int tree_lines(const char* j, int n, const char* prefix, char* out, int max, int* shown, int* truncated_api) {
    *truncated_api = strstr(j, "\"truncated\":true") != NULL || strstr(j, "\"truncated\": true") != NULL;
    int total = 0, o = 0, pl = (int)strlen(prefix);
    *shown = 0;
    const char* end = j + n;
    for (const char* p = j; p < end;) {
        const char* a = strstr(p, "\"path\":\"");
        if (!a) break;
        a += 8;
        const char* b = a;
        while (b < end && *b != '"') b += (*b == '\\' && b + 1 < end) ? 2 : 1;
        int len = (int)(b - a);
        int is_tree = 0;
        for (const char* t = b; t < end && t < b + 80; t++)
            if (!strncmp(t, "\"type\":\"tree\"", 13) || !strncmp(t, "\"type\": \"tree\"", 14)) {
                is_tree = 1;
                break;
            } else if (!strncmp(t, "\"type\":\"blob\"", 13) || !strncmp(t, "\"path\"", 6)) break;
        p = b + 1;
        if (is_tree || len <= 0 || len > 150) continue;
        if (pl && (len < pl || memcmp(a, prefix, pl))) continue;
        total++;
        if (o + len + 2 < max) {
            memcpy(out + o, a, len);
            o += len;
            out[o++] = '\n';
            (*shown)++;
        }
    }
    out[o] = 0;
    return total;
}

static void cmd_gh(const char* arg, char* note, int nm) {
    char a1[200] = "", a2[120] = "";
    int l = 0;
    while (arg[l] && arg[l] != ' ' && l < (int)sizeof(a1) - 1) l++;
    memcpy(a1, arg, l);
    a1[l] = 0;
    const char* r = arg + l;
    while (*r == ' ') r++;
    for (l = 0; r[l] && r[l] != ' ' && r[l] != '\n' && l < (int)sizeof(a2) - 1; l++) a2[l] = r[l];
    a2[l] = 0;
    const char* s = a1;
    if (!strncmp(s, "https://github.com/", 19)) s += 19;
    else if (!strncmp(s, "github.com/", 11)) s += 11;
    char owner[64] = "", repo[96] = "", ref[64] = "HEAD", path[120] = "";
    int i = 0;
    while (s[i] && s[i] != '/' && i < 63) owner[i] = s[i], i++;
    owner[i] = 0;
    if (s[i] != '/') {
        snprintf(note, nm, "Usage: /gh owner/repo [path]   (or a github.com link)");
        return;
    }
    s += i + 1;
    i = 0;
    while (s[i] && s[i] != '/' && i < 95) repo[i] = s[i], i++;
    repo[i] = 0;
    s += i;
    int rl = (int)strlen(repo);
    if (rl > 4 && !strcmp(repo + rl - 4, ".git")) repo[rl - 4] = 0;
    if (!strncmp(s, "/blob/", 6) || !strncmp(s, "/tree/", 6)) {   // a link to a file or folder: owner/repo/blob/branch/path
        s += 6;
        i = 0;
        while (s[i] && s[i] != '/' && i < 63) ref[i] = s[i], i++;
        ref[i] = 0;
        if (s[i] == '/') snprintf(path, sizeof(path), "%s", s + i + 1);
    }
    if (a2[0]) snprintf(path, sizeof(path), "%s", a2[0] == '/' ? a2 + 1 : a2);
    if (strstr(owner, "..") || strstr(repo, "..") || strstr(ref, "..")) owner[0] = 0;
    for (const char* c = owner; *c; c++)
        if (!ok_repo_char((unsigned char)*c)) owner[0] = 0;
    for (const char* c = repo; *c; c++)
        if (!ok_repo_char((unsigned char)*c)) repo[0] = 0;
    for (const char* c = ref; *c; c++)
        if (!ok_repo_char((unsigned char)*c)) ref[0] = 0;
    if (!owner[0] || !repo[0] || !ref[0] || !ok_path(path)) {
        snprintf(note, nm, "Usage: /gh owner/repo [path]   (invalid name)");
        return;
    }
    char url[400], title[160], slug[64], saved[96];
    if (!strcmp(a2, "zip")) {   // phase 7: the whole repo as a .zip in /data/garlic_files, and it becomes the project
        static char zbuf[8 * 1024 * 1024];
        int status = 0;
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/zipball/%s", owner, repo, strcmp(ref, "HEAD") ? ref : "");
        int n = net_get(url, zbuf, sizeof(zbuf), &status);
        if (n < 0) {
            snprintf(note, nm, "Network error: %s", net_last_error());
            return;
        }
        if (status >= 400 || n < 100 || zbuf[0] != 'P' || zbuf[1] != 'K') {
            snprintf(note, nm, "Could not get the zip (HTTP %d, %d bytes%s). A repo bigger than 8 MB does not fit: use /gh owner/repo path for single files.",
                     status, n, n >= (int)sizeof(zbuf) - 1 ? ", cut at 8 MB" : "");
            return;
        }
        char zn[140], zp[260];
        snprintf(zn, sizeof(zn), "%s_%s.zip", owner, repo);
        snprintf(zp, sizeof(zp), "%s/%s", FILES_DIR, zn);
        int f = open(zp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int done = 0;
        while (f >= 0 && done < n) {
            int k = write(f, zbuf + done, n - done);
            if (k <= 0) break;
            done += k;
        }
        if (f >= 0) close(f);
        if (done != n) {
            snprintf(note, nm, "Could not save %s", zn);
            return;
        }
        files_set_project(zn);
        snprintf(note, nm, "Saved %s (%d KB) and set it as the project. Try /mapa or /grep words, or @zip:path.", zn, n / 1024);
        return;
    }
    int plen = (int)strlen(path);
    if (plen > 0 && path[plen - 1] != '/' && !strchr(path, '*')) {   // one file
        snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/%s/%s", owner, repo, ref, path);
        int n = fetch(url, note, nm);
        if (n < 0) return;
        int tl = 0;
        for (int k = 0; k < n && tl < (int)sizeof(text) - 1; k++)
            if (page[k] != '\r' && page[k] != 0) text[tl++] = page[k];
        text[tl] = 0;
        snprintf(title, sizeof(title), "%s/%s %s", owner, repo, path);
        char sl[200];
        snprintf(sl, sizeof(sl), "gh %s %s %s", owner, repo, path);
        slugify(sl, slug, sizeof(slug));
        save_copy(slug, "txt", text, tl, saved, sizeof(saved));
        int added = add_pending(title, text);
        snprintf(note, nm, "%s/%s %s: %d bytes, %d queued for your next message%s. Saved as %s", owner, repo, path, n, added, added < tl ? " (cut)" : "", saved);
        return;
    }
    // a folder or the whole repo: its file list (and the README for the whole repo)
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/git/trees/%s?recursive=1", owner, repo, ref);
    int n = fetch(url, note, nm);
    if (n < 0) return;
    static char lines[9000];
    int shown, api_cut;
    int total = tree_lines(page, n, path, lines, 4200, &shown, &api_cut);
    if (total == 0) {
        snprintf(note, nm, "%s/%s: no files found%s (is the name right? private repos are not reachable)", owner, repo, path[0] ? " under that path" : "");
        return;
    }
    char head[120];
    snprintf(head, sizeof(head), "%d files%s%s%s\n", total, path[0] ? " under " : "", path, api_cut ? " (GitHub cut the list)" : "");
    int hl = (int)strlen(head);
    memmove(lines + hl, lines, strlen(lines) + 1);
    memcpy(lines, head, hl);
    if (shown < total) strcat(lines, "...\n");
    snprintf(title, sizeof(title), "%s/%s file list", owner, repo);
    int added1 = add_pending(title, lines);
    char sl[200], saved2[96] = "";
    snprintf(sl, sizeof(sl), "gh tree %s %s", owner, repo);
    slugify(sl, slug, sizeof(slug));
    save_copy(slug, "txt", lines, (int)strlen(lines), saved, sizeof(saved));
    int added2 = 0, rn = 0;
    if (!path[0]) {   // the README of the repo
        static const char* const names[] = {"README.md", "readme.md", "README"};
        for (int k = 0; k < 3 && rn <= 0; k++) {
            snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/%s/%s", owner, repo, ref, names[k]);
            char dummy[8];
            rn = fetch(url, dummy, sizeof(dummy));
        }
        if (rn > 0) {
            int tl = 0;
            for (int k = 0; k < rn && tl < (int)sizeof(text) - 1; k++)
                if (page[k] != '\r' && page[k] != 0) text[tl++] = page[k];
            text[tl] = 0;
            snprintf(title, sizeof(title), "%s/%s README", owner, repo);
            snprintf(sl, sizeof(sl), "gh readme %s %s", owner, repo);
            slugify(sl, slug, sizeof(slug));
            save_copy(slug, "txt", text, tl, saved2, sizeof(saved2));
            added2 = add_pending(title, text);
        }
    }
    snprintf(note, nm, "%s/%s: %d files listed (%d queued)%s. Saved as %s%s%s. Ask your question now.", owner, repo, total, added1,
             !path[0] ? (rn > 0 ? ", README queued" : ", no README found") : "", saved, saved2[0] ? " and " : "", saved2);
    (void)added2;
}

// ---- /web ----------------------------------------------------------------------------------------------------------

// DuckDuckGo's HTML page: <a class="result__a" href="//duckduckgo.com/l/?uddg=ENCODED">Title</a> and
// <a class="result__snippet">...</a>. Returns the number of results written to out as "N. title / url / snippet".
static int parse_ddg(const char* h, int n, char* out, int max) {
    const char* end = h + n;
    int o = 0, count = 0;
    const char* p = h;
    out[0] = 0;
    while (count < 8 && p < end) {
        const char* a = strstr(p, "result__a");
        if (!a || a >= end) break;
        const char* tag_end = memchr(a, '>', end - a);
        if (!tag_end) break;
        const char* hr = NULL;   // href inside this tag (it can come before the class too)
        const char* tag_start = a;
        while (tag_start > h && *tag_start != '<') tag_start--;
        for (const char* q = tag_start; q + 6 < tag_end; q++)
            if (!memcmp(q, "href=\"", 6)) {
                hr = q + 6;
                break;
            }
        const char* title_end = strstr(tag_end, "</a>");
        if (!hr || !title_end) {
            p = tag_end + 1;
            continue;
        }
        const char* hq = memchr(hr, '"', end - hr);
        char link[300];
        int ll = hq ? (int)(hq - hr) : 0;
        const char* u = NULL;
        for (const char* q = hr; q + 5 < hr + ll; q++)
            if (!memcmp(q, "uddg=", 5)) {
                u = q + 5;
                break;
            }
        if (u) url_decode(u, (int)(hr + ll - u), link, sizeof(link));
        else {
            char raw_link[300];
            snprintf(raw_link, sizeof(raw_link), "%.*s", ll < 299 ? ll : 299, hr);
            if (!strncmp(raw_link, "//", 2)) snprintf(link, sizeof(link), "https:%s", raw_link);
            else snprintf(link, sizeof(link), "%s", raw_link);
        }
        char title[160];
        html_to_text(tag_end + 1, (int)(title_end - tag_end - 1), title, sizeof(title), 0);
        char snippet[260] = "";
        const char* sn = strstr(title_end, "result__snippet");
        const char* nextres = strstr(title_end, "result__a");
        if (sn && (!nextres || sn < nextres)) {
            const char* sg = memchr(sn, '>', end - sn);
            const char* se = sg ? strstr(sg, "</a>") : NULL;
            const char* se2 = sg ? strstr(sg, "</td>") : NULL;
            if (se2 && (!se || se2 < se)) se = se2;
            if (sg && se) html_to_text(sg + 1, (int)(se - sg - 1), snippet, sizeof(snippet), 0);
        }
        if (link[0] && title[0]) {
            if (count < 8) {
                snprintf(last_urls[count], sizeof(last_urls[0]), "%s", link);
                snprintf(last_titles[count], sizeof(last_titles[0]), "%s", title);
            }
            int w = snprintf(out + o, max - o, "%d. %s\n   %s\n%s%s%s", ++count, title, link, snippet[0] ? "   " : "", snippet, snippet[0] ? "\n" : "");
            if (w < 0 || o + w >= max) {
                out[o] = 0;
                count--;
                break;
            }
            o += w;
        }
        p = title_end + 4;
    }
    return count;
}

static void cmd_web(const char* arg, char* note, int nm) {
    if (!*arg) {
        snprintf(note, nm, "Usage: /web what to search   (then /abrir 2 reads result 2)");
        return;
    }
    char q[200], url[400];
    url_encode(arg, q, sizeof(q));
    snprintf(url, sizeof(url), "https://html.duckduckgo.com/html/?q=%s", q);
    last_n = 0;
    int n = fetch(url, note, nm);
    if (n < 0) return;
    static char results[6000];
    int c = parse_ddg(page, n, results, sizeof(results));
    char slug[64], saved[96], title[160];
    slugify(arg, slug, 40);
    snprintf(title, sizeof(title), "search '%.60s'", arg);
    if (c == 0) {   // the page changed or came back empty: keep the raw page to look at it, and queue its text
        char raw[96];
        save_copy("last-search-raw", "html", page, n, raw, sizeof(raw));
        if (strstr(page, "anomaly") || strstr(page, "challenge") || strstr(page, "captcha")) {
            snprintf(note, nm, "DuckDuckGo asked for a human check (too many searches?). Try again in a few minutes, or use /wiki words, or /url with a link.");
            return;
        }
        int tl = html_to_text(page, n, text, 3000, 0);
        add_pending(title, text);
        snprintf(note, nm, "Search: 0 results parsed (%d bytes came back; raw page in %s, text of it queued, %d chars)", n, raw, tl);
        return;
    }
    last_n = c < 8 ? c : 8;
    char sl[80];
    snprintf(sl, sizeof(sl), "web %s", slug);
    slugify(sl, slug, sizeof(slug));
    save_copy(slug, "txt", results, (int)strlen(results), saved, sizeof(saved));
    int added = add_pending(title, results);
    int o = snprintf(note, nm, "Search: %d results queued (%d bytes) for your next message. /abrir N reads one:", c, added);
    for (int i = 0; i < last_n && o < nm - 100; i++) o += snprintf(note + o, nm - o, "\n%d. %.80s", i + 1, last_titles[i]);
}

// ---- /abrir N: the Nth result of the last search --------------------------------------------------------------------

static void cmd_open(const char* arg, char* note, int nm) {
    int k = atoi(arg);
    if (last_n == 0) {
        snprintf(note, nm, "No search to open from. Use /web words first.");
        return;
    }
    if (k < 1 || k > last_n) {
        snprintf(note, nm, "Usage: /abrir N   (N from 1 to %d)", last_n);
        return;
    }
    cmd_url(last_urls[k - 1], note, nm);
}

// ---- /wiki ---------------------------------------------------------------------------------------------------------

// Copies the JSON string that starts right after the opening quote at p into out, undoing the escapes.
static void json_string(const char* p, const char* end, char* out, int max) {
    int o = 0;
    while (p < end && *p != '"' && o < max - 5) {
        if (*p != '\\') {
            out[o++] = *p++;
            continue;
        }
        p++;
        if (p >= end) break;
        char c = *p++;
        if (c == 'n') out[o++] = '\n';
        else if (c == 't') out[o++] = ' ';
        else if (c == 'u' && p + 4 <= end) {
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) cp = cp * 16 + (unsigned)(hexv(p[i]) < 0 ? 0 : hexv(p[i]));
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {   // a surrogate pair
                unsigned lo = 0;
                for (int i = 0; i < 4; i++) lo = lo * 16 + (unsigned)(hexv(p[2 + i]) < 0 ? 0 : hexv(p[2 + i]));
                p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) out[o++] = (char)(0xC0 | cp >> 6), out[o++] = (char)(0x80 | (cp & 63));
            else if (cp < 0x10000) out[o++] = (char)(0xE0 | cp >> 12), out[o++] = (char)(0x80 | ((cp >> 6) & 63)), out[o++] = (char)(0x80 | (cp & 63));
            else out[o++] = (char)(0xF0 | cp >> 18), out[o++] = (char)(0x80 | ((cp >> 12) & 63)), out[o++] = (char)(0x80 | ((cp >> 6) & 63)), out[o++] = (char)(0x80 | (cp & 63));
        } else out[o++] = c;   // \" \\ \/
    }
    out[o] = 0;
}

static void cmd_wiki(const char* arg, char* note, int nm) {
    if (!*arg) {
        snprintf(note, nm, "Usage: /wiki words   (Spanish Wikipedia; /wiki en:words for English)");
        return;
    }
    char lang[8] = "es";
    const char* term = arg;
    int ll = 0;
    while (ll < 4 && ((arg[ll] | 32) >= 'a' && (arg[ll] | 32) <= 'z')) ll++;
    if (ll >= 2 && ll <= 3 && arg[ll] == ':') {
        memcpy(lang, arg, ll);
        lang[ll] = 0;
        term = arg + ll + 1;
        while (*term == ' ') term++;
    }
    if (!*term) {
        snprintf(note, nm, "Usage: /wiki words");
        return;
    }
    char q[200], url[500];
    url_encode(term, q, sizeof(q));
    snprintf(url, sizeof(url), "https://%s.wikipedia.org/w/api.php?action=query&generator=search&gsrsearch=%s&gsrlimit=1&prop=extracts&exintro=1&explaintext=1&redirects=1&format=json&formatversion=2&utf8=1", lang, q);
    int n = fetch(url, note, nm);
    if (n < 0) return;
    const char* e = strstr(page, "\"extract\":\"");
    const char* t = strstr(page, "\"title\":\"");
    if (!e || !t) {
        snprintf(note, nm, "Wikipedia (%s) has nothing for '%.60s'. Try /web.", lang, term);
        return;
    }
    char wtitle[160];
    json_string(t + 9, page + n, wtitle, sizeof(wtitle));
    json_string(e + 11, page + n, text, sizeof(text));
    char head[220], slug[64], saved[96], sl[200];
    snprintf(head, sizeof(head), "Wikipedia (%s): %s", lang, wtitle);
    snprintf(sl, sizeof(sl), "wiki %s %s", lang, term);
    slugify(sl, slug, sizeof(slug));
    save_copy(slug, "txt", text, (int)strlen(text), saved, sizeof(saved));
    int added = add_pending(head, text);
    snprintf(note, nm, "%s: %d chars, %d queued for your next message. Saved as %s. For the whole article use /web or /url.", head, (int)strlen(text), added, saved);
}

// ---- dispatch ------------------------------------------------------------------------------------------------------

int webcmd_run(const char* t, char* note, int nm) {
    const char* arg = "";
    int cmd = parse_cmd(t, &arg);
    note[0] = 0;
    char argbuf[400];
    int al = 0;
    while (arg[al] && arg[al] != '\n' && al < (int)sizeof(argbuf) - 1) al++;
    while (al > 0 && arg[al - 1] == ' ') al--;
    memcpy(argbuf, arg, al);
    argbuf[al] = 0;
    switch (cmd) {
    case C_HELP:
        snprintf(note, nm,
                 "FILES\n@  list   |   @name   |   @repo.zip:path   |   @doc.docx\n/guardar  saves the files of the last answer\n"
                 "INTERNET (/red on first)\n/web words   /abrir N   /wiki words\n/url https://...   /gh owner/repo [path | zip]\n"
                 "PROJECT\n/proyecto zip   /mapa   /grep words   /aplicar   /empaquetar   /auto on|off [N]   /limite N\n"
                 "SAVED GAMES (binary files)\n@save.zip:file (report)   @save.zip:file:0x100-0x1ff (hex)   @save.zip?=12500 (find a number)   @before.zip~after.zip (what changed)   /poke OFFSET TYPE VALUE FILE [zip]\n"
                 "PROGRESS\n/progreso  the model writes its notes, saved   |   /retomar  fresh context from them\n"
                 "API (phone, PC, games)\n/api  addresses   |   /api on|off   |   /cache on|off   |   /puerto N\n"
                 "OTHER\n/estado  state   |   /limpiar  drop fetched text   |   /red on|off\n"
                 "Short forms: /b /o /w /u /g");
        break;
    case C_STATUS:
        snprintf(note, nm, "Network: %s   backend: %s\nWaiting for your next message: %d bytes of web text\nLast search: %d results%s",
                 enabled() ? "ON" : "OFF", net_backend(), pending_n, last_n, last_n ? " (/abrir N)" : "");
        break;
    case C_UNKNOWN:
        snprintf(note, nm, "Unknown command. Type /ayuda to see them.");
        break;
    case C_CLEAR:
        pending_n = 0;
        snprintf(note, nm, "Fetched web text dropped.");
        break;
    case C_NET:
        if (!strcmp(argbuf, "on") || !strcmp(argbuf, "1") || !strcmp(argbuf, "si") || !strcmp(argbuf, "sí")) set_enabled(1);
        else if (!strcmp(argbuf, "off") || !strcmp(argbuf, "0") || !strcmp(argbuf, "no")) set_enabled(0);
        snprintf(note, nm, "Network for the AI: %s (backend: %s). Only /web, /wiki, /url, /abrir and /gh use it, with what you type.", enabled() ? "ON" : "OFF", net_backend());
        break;
    case C_GH:
    case C_URL:
    case C_WEB:
    case C_OPEN:
    case C_WIKI:
        if (cmd == C_OPEN && last_n > 0 && !enabled()) {
            snprintf(note, nm, "Network is OFF. Type /red on first.");
            break;
        }
        if (!enabled()) {
            snprintf(note, nm, "Network is OFF. Type /red on to allow /web, /wiki, /url and /gh.");
            break;
        }
        if (cmd == C_GH) cmd_gh(argbuf, note, nm);
        else if (cmd == C_URL) cmd_url(argbuf, note, nm);
        else if (cmd == C_OPEN) cmd_open(argbuf, note, nm);
        else if (cmd == C_WIKI) cmd_wiki(argbuf, note, nm);
        else cmd_web(argbuf, note, nm);
        break;
    default:
        break;
    }
    return 0;
}
