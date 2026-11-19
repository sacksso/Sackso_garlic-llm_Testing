// Text files for the chat (see files.h).
#include "files.h"
#include "zipread.h"
#include "bintools.h"
#include "repomap.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define NAME_MAX_LEN 96

static int ok_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/' || c == ':' || c == '*' || c == '#' || c == '?' || c == '+' || c == '=' || c == '~' || c == '!';
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
        else if (sub[0] && depth < 6) n = list_dir(names, n, max, rel, depth + 1);
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

// ---- friendlier names (phase 6) ----------------------------------------------------------------------------------
static char last_zip[NAME_MAX_LEN];   // the zip used last: "@zip", "@repo" or "@:path" mean it
static int g_att_a, g_att_b;           // lines of the last part read (0 = the whole file)
static char g_last_rel[100];          // path inside the zip of the last file read (without the repo-main/ folder)
static FilesAttached g_att[FILES_ATT_MAX];
static int g_natt;

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int contains_ci(const char* s, const char* sub) {
    int sl = (int)strlen(s), bl = (int)strlen(sub);
    for (int i = 0; i + bl <= sl; i++) {
        int j = 0;
        while (j < bl && lc(s[i + j]) == lc(sub[j])) j++;
        if (j == bl) return 1;
    }
    return 0;
}

static int prefix_ci(const char* s, const char* p) {
    while (*p && lc(*s) == lc(*p)) s++, p++;
    return *p == 0;
}

// Edit distance between two short names, case-insensitive (capped: strings longer than 95 are not compared).
static int edit_dist(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 95 || lb > 95) return 99;
    int prev[97], cur[97];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int c = prev[j - 1] + (lc(a[i - 1]) != lc(b[j - 1]));
            if (prev[j] + 1 < c) c = prev[j] + 1;
            if (cur[j - 1] + 1 < c) c = cur[j - 1] + 1;
            cur[j] = c;
        }
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

static int file_exists(const char* name) {
    char path[200];
    snprintf(path, sizeof(path), FILES_DIR "/%s", name);
    int f = open(path, O_RDONLY);
    if (f < 0) return 0;
    close(f);
    return 1;
}

// Turns what the user typed before the ":" into a real file name of FILES_DIR. Returns 0 as typed, 1 changed
// (how: 1 alias for the last/only zip, 2 unique prefix or substring, 3 one typo away), -1 nothing found.
// sugg gets up to three close names for the "did you mean" note.
static int resolve_base(char* base, char* sugg, int sugg_max) {
    static char names[60][NAME_MAX_LEN];
    sugg[0] = 0;
    int n = list_files(names, 60);
    int is_alias = base[0] == 0 || !strcasecmp(base, "zip") || !strcasecmp(base, "repo");
    if (is_alias) {
        if (last_zip[0] && file_exists(last_zip)) {
            strcpy(base, last_zip);
            return 1;
        }
        int hit = -1, cnt = 0;   // the only zip there is
        for (int i = 0; i < n; i++)
            if (strlen(names[i]) > 4 && !strcasecmp(names[i] + strlen(names[i]) - 4, ".zip") && !strchr(names[i], '/')) hit = i, cnt++;
        if (cnt == 1) {
            strcpy(base, names[hit]);
            return 1;
        }
        snprintf(sugg, sugg_max, cnt ? "several zips, say which one" : "no zip in " FILES_DIR);
        return -1;
    }
    if (file_exists(base)) return 0;
    int hit = -1, cnt = 0;
    for (int i = 0; i < n; i++)
        if (prefix_ci(names[i], base)) hit = i, cnt++;
    if (cnt == 0)
        for (int i = 0; i < n; i++)
            if (contains_ci(names[i], base)) hit = i, cnt++;
    if (cnt == 1) {
        strcpy(base, names[hit]);
        return 2;
    }
    int best[3] = {-1, -1, -1}, bd[3] = {99, 99, 99};
    int bl = (int)strlen(base);
    for (int i = 0; i < n; i++) {
        int d = edit_dist(base, names[i]);
        for (int k = 0; k < 3; k++)
            if (d < bd[k]) {
                for (int m = 2; m > k; m--) bd[m] = bd[m - 1], best[m] = best[m - 1];
                bd[k] = d, best[k] = i;
                break;
            }
    }
    if (best[0] >= 0 && bd[0] <= 2 && bd[0] * 4 <= bl && bd[1] > bd[0]) {   // one clear near miss: take it
        strcpy(base, names[best[0]]);
        return 3;
    }
    int o = 0;
    for (int k = 0; k < 3; k++)
        if (best[k] >= 0 && bd[k] <= bl / 2 + 2 && o < sugg_max - NAME_MAX_LEN) o += snprintf(sugg + o, sugg_max - o, "%s ", names[best[k]]);
    return -1;
}

// "Key files" that follow the list of a whole zip, so the model sees the project's purpose and its build without
// the user asking for each one: README (top level) and the first workflows. Appends to out (at o), returns the new o.
static int zip_key_files(const char* path, const ZipEnt* ents, int n, int tl, char* out, int o, int max, int readme_budget, int max_flows, int flow_budget) {
    static char tmp[3200];
    int done_readme = 0, flows = 0;
    for (int i = 0; i < n && o < max - 600; i++) {
        const char* rel = ents[i].name + tl;
        int is_readme = !done_readme && !strchr(rel, '/') && prefix_ci(rel, "readme") && !strcasecmp(rel, "readme.md");
        int is_flow = flows < max_flows && prefix_ci(rel, ".github/workflows/") && (ends_ci(rel, ".yml") || ends_ci(rel, ".yaml"));
        if (!is_readme && !is_flow) continue;
        int budget = is_readme ? readme_budget : flow_budget;
        int cut = 0;
        int r = zip_read(path, &ents[i], tmp, budget < (int)sizeof(tmp) ? budget : (int)sizeof(tmp) - 1, &cut);
        if (r <= 0) continue;
        int w = 0;
        for (int k = 0; k < r; k++)
            if (tmp[k] != 0 && tmp[k] != '\r') tmp[w++] = tmp[k];
        tmp[w] = 0;
        if (cut) w = cut_clean(tmp, w);
        int need = w + (int)strlen(rel) + 40;
        if (o + need >= max) break;
        o += snprintf(out + o, max - o, "\n--- %s%s ---\n%s\n", rel, cut ? " (first part)" : "", tmp);
        if (is_readme) done_readme = 1;
        else flows++;
    }
    return o;
}

// A number of a range: decimal or 0x hex (offsets of binary files are usually written in hex).
static const char* num_at(const char* q, long* v) {
    long long x = 0;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X') && ((q[2] >= '0' && q[2] <= '9') || (q[2] >= 'a' && q[2] <= 'f') || (q[2] >= 'A' && q[2] <= 'F'))) {
        q += 2;
        for (;; q++) {
            int d = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 : *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
            if (d < 0 || x > (1LL << 40)) break;
            x = x * 16 + d;
        }
    } else
        while (*q >= '0' && *q <= '9' && x < (1LL << 40)) x = x * 10 + (*q++ - '0');
    *v = (long)x;
    return q;
}

// @repo.zip (the file list), @repo.zip:dir/ or @repo.zip:*.yml (a part of the list), @repo.zip:path/file (one file),
// and @doc.docx / .pptx / .xlsx / .odt / .ods / .odp (the text of the document).
// Returns the text length, or -2 when name is a plain file (not one of these), -1 not found, -3 several files match
// (out holds their names), -4 binary, -5 damaged or unsupported.
static int read_special(const char* name, char* out, int max, long* size, int* cut) {
    {   // phase 10: @before.zip~after.zip = which bytes changed between two saves
        const char* tl = strchr(name, '~');
        if (tl && !strchr(name, ':') && !strchr(name, '?') && !strchr(name, '!')) {
            char pa[200], pb[200];
            snprintf(pa, sizeof(pa), FILES_DIR "/%.*s", (int)(tl - name), name);
            snprintf(pb, sizeof(pb), FILES_DIR "/%s", tl + 1);
            *cut = 0;
            int r = bin_zip_compare(pa, pb, out, max);
            if (r < 0) return -5;
            *size = r;
            return r;
        }
    }
    char base[NAME_MAX_LEN];
    const char* colon = strpbrk(name, ":?!");
    int bl = colon ? (int)(colon - name) : (int)strlen(name);
    if (bl <= 0 || bl >= NAME_MAX_LEN) return -2;
    memcpy(base, name, bl);
    base[bl] = 0;
    const char* spec = colon ? colon + 1 : NULL;
    int is_query = colon && *colon == '?';
    int is_bang = colon && *colon == '!';   // phase 12: @repo.zip!name = every place that uses name
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
    g_last_rel[0] = 0;
    if (is_bang) {
        int th = 0;
        int r = repo_refs(path, spec, out, max < 9000 ? max : 9000, &th);
        if (r < 0) return -5;
        *size = (long)strlen(out);
        return (int)*size;
    }
    if (is_query && spec[0] == '=') {   // phase 10: @save.zip?=12500 = where that number is stored, in every file
        int r = bin_zip_find(path, NULL, spec + 1, out, max);
        if (r == -2) r = snprintf(out, max, "\"%s\" is not a number (use 12500, -3, 0x1F4 or 12.5).\n", spec + 1);
        if (r < 0) return -5;
        *size = r;
        return r;
    }
    if (!is_query && spec && strstr(spec, "?=")) {   // @save.zip:file?=12500: the same, in one file
        char fl[ZIP_NAME_MAX + 8];
        const char* qe = strstr(spec, "?=");
        snprintf(fl, sizeof(fl), "%.*s", (int)(qe - spec), spec);
        int r = bin_zip_find(path, fl, qe + 2, out, max);
        if (r == -2) r = snprintf(out, max, "\"%s\" is not a number (use 12500, -3, 0x1F4 or 12.5).\n", qe + 2);
        else if (r == -3) r = snprintf(out, max, "%s is not in the zip (see the list with @%.60s).\n", fl, base);
        if (r < 0) return -5;
        *size = r;
        return r;
    }
    if (is_query) {   // @repo.zip?word1+word2: the parts of the repo that mention them (phase 7)
        int r = repo_search(path, spec, out, max < 9000 ? max : 9000, 3, NULL, NULL);
        if (r < 0) return -5;
        *size = (long)strlen(out);
        return (int)*size;
    }
    if (sl == 0) {   // the whole zip: a map of the repo (functions with their lines) + README + the first workflow
        int nsrc = 0;
        int mapmax = max < 9500 ? max * 6 / 10 : 5700 + (max - 9500) / 4;
        if (mapmax > 9000) mapmax = 9000;
        int m = repomap_build(path, out, mapmax, &nsrc);
        if (m > 0 && nsrc > 0) {
            int tl2 = repo_root_len(ents, n);
            int o = zip_key_files(path, ents, n, tl2, out, m, max, 1500, 1, 1800);
            {   // phase 10: a small zip whose map leaves binary files out (a saved game): add every file with its size
                const char* bn = strstr(out, "binary or assets not listed");
                int has_bin = bn && bn - out > 3 && !(bn[-2] == '0' && !(bn[-3] >= '0' && bn[-3] <= '9'));
                if (has_bin && n <= 80 && o < max - 300) {
                    o += snprintf(out + o, max - o, "\nAll %d files (size, path; the ones without text are binary: @%.60s:path gives a report, @%.60s?=NUMBER finds a number in all of them):\n", n, base, base);
                    for (int i = 0; i < n && o < max - 140; i++) o += snprintf(out + o, max - o, "%u %s\n", (unsigned)ents[i].usize, ents[i].name + tl2);
                }
            }
            *size = o;
            if (n < total) *cut = 1;
            return o;
        }
    }
    if (sl == 0 || spec[0] == '*' || spec[sl - 1] == '/') {   // the list, or a part of it
        int tl = 0;   // a folder shared by every path ("repo-main/", as GitHub makes them) is left out of the lines
        const char* first = n > 0 ? strchr(ents[0].name, '/') : NULL;
        if (first) {
            tl = (int)(first - ents[0].name) + 1;
            for (int i = 0; i < n && tl; i++)
                if (strncmp(ents[i].name, ents[0].name, tl)) tl = 0;
        }
        int o = snprintf(out, max, "%s: %d files%s%.*s\n", base, total, tl ? ", paths inside " : "", tl, ents[0].name);
        int shown = 0, hidden = 0, show_all = 0;
        int lim = sl ? max : (max < 4500 ? max : 4500);   // a whole-zip list stays short: the user narrows it with dir/
        int o0 = o;
    relist:
        o = o0, shown = 0, hidden = 0;
        for (int i = 0; i < n; i++) {
            const char* nm = ents[i].name;
            const char* rel = nm + tl;
            if (sl) {
                if (spec[0] == '*') {
                    if (!ends_ci(nm, spec + 1)) continue;
                } else if (!starts_with(rel, spec) && !starts_with(nm, spec)) continue;
            }
            if (!show_all && (spec == NULL || spec[0] != '*')) {   // images, models, archives... are hidden unless asked with *.ext
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
        if (!show_all && sl == 0 && hidden > 0 && (shown == 0 || n <= 60)) {   // phase 10: a saved game is ALL binary: list it all
            show_all = 1;
            goto relist;
        }
        if (show_all && o < max - 260) o += snprintf(out + o, max - o, "(binary files are in this list: @%.60s:path gives a report of one, @%.60s:path:0x0-0xff its bytes in hex, @%.60s?=NUMBER finds a number in all of them.)\n", base, base, base);
        if (hidden && o < max - 120) o += snprintf(out + o, max - o, "(+%d binary files not listed; @%.60s:*.bin etc. lists them)\n", hidden, base);
        if (n < total) *cut = 1;
        if (sl && shown == 0 && !hidden) return -1;
        if (sl == 0) o = zip_key_files(path, ents, n, tl, out, o, max, 2000, 2, 2000);   // README and workflows come along
        *size = o;
        return o;
    }
    char fspec[ZIP_NAME_MAX + 8], sym[64] = "";
    int ra = 0, rb = 0, ranged = 0;
    snprintf(fspec, sizeof(fspec), "%s", spec);
    {
        char* hash = strchr(fspec, '#');
        if (hash) {   // @zip:path#function
            *hash = 0;
            snprintf(sym, sizeof(sym), "%s", hash + 1);
        } else {      // @zip:path:A-B  or  @zip:path:A
            char* c2 = strrchr(fspec, ':');
            if (c2 && c2[1] >= '0' && c2[1] <= '9') {
                long a1 = 0, b1 = 0;
                const char* q = num_at(c2 + 1, &a1);
                b1 = a1;
                if (*q == '-') {
                    q++;
                    if (*q) q = num_at(q, &b1);
                    else b1 = 0;
                    if (b1 == 0) b1 = 1 << 29;
                }
                if (*q == 0) {
                    *c2 = 0;
                    ra = (int)a1, rb = (int)b1, ranged = 1;
                }
            }
        }
        if (!fspec[0]) return -1;
        spec = fspec;
        sl = (int)strlen(fspec);
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
    {   // remember where it is (for the repo search and the edits), without the repo-main/ folder
        int tl3 = repo_root_len(ents, n);
        snprintf(g_last_rel, sizeof(g_last_rel), "%s", hit->name + tl3);
    }
    {   // phase 10: a binary file is not shown as text: a report, or the hex of a byte range (@zip:file:0x100-0x1ff)
        unsigned char probe[512];
        int pc = 0, pr = zip_read(path, hit, (char*)probe, sizeof(probe) - 1, &pc);
        if (pr > 0 && bin_looks_binary(probe, pr)) {
            int bo = bin_entry_view(path, hit, ra, rb, ranged, out, max);
            *size = hit->usize;
            *cut = 0;
            g_att_a = g_att_b = 0;
            return bo;
        }
    }
    if (ra > 0 || sym[0]) {   // part of a file: read it whole (up to 200 KB) and cut the lines out
        static char big[200001];
        int bc = 0;
        int br = zip_read(path, hit, big, sizeof(big) - 1, &bc);
        if (br < 0) return -5;
        int w = 0;
        for (int i = 0; i < br; i++)
            if (big[i] != 0 && big[i] != '\r') big[w++] = big[i];
        big[w] = 0;
        int tot = 0, a2 = ra, b2 = rb;
        if (sym[0]) {
            if (!repo_symbol_range(g_last_rel, big, w, sym, &a2, &b2)) {
                // the model guessed the wrong file (it happens a lot): look for the name in the whole repo and bring
                // what mentions it, saying so, instead of an error that makes it ask again for the same thing
                static char found[9001];
                int fr = repo_search(path, sym, found, max < 6000 ? max - 200 : 6000, 2, NULL, NULL);
                if (fr > 0 && found[0]) {
                    int o2 = snprintf(out, max, "(There is no %s in %s; these are the places of the repo that mention it:)\n%s", sym,
                                      g_last_rel, found);
                    if (o2 > 0 && o2 < max) {
                        *size = (long)o2;
                        *cut = 0;
                        g_att_a = g_att_b = 0;
                        return o2;
                    }
                }
                snprintf(out, max, "%s", sym);
                return -6;
            }
        }
        int o = repo_extract_lines(big, w, a2, b2, out, max, &tot);
        if (o <= 0) {
            snprintf(out, max, "%d", tot);
            return -7;
        }
        *size = hit->usize;
        *cut = 0;
        g_att_a = a2, g_att_b = b2 > tot ? tot : b2;
        return o;
    }
    int r = zip_read(path, hit, out, max, cut);
    if (r < 0) return -5;
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
    g_natt = 0;
    char seen[8][NAME_MAX_LEN];
    for (const char* p = text; *p; p++) {
        if (*p != '@' || (p != text && p[-1] != ' ' && p[-1] != '\n' && p[-1] != '\t')) continue;
        char name[NAME_MAX_LEN];
        int l = 0;
        while (l < NAME_MAX_LEN - 1 && ok_name_char((unsigned char)p[1 + l])) l++;
        memcpy(name, p + 1, l);
        name[l] = 0;
        while (l > 0 && (name[l - 1] == '?' || name[l - 1] == '+' || name[l - 1] == '#' || name[l - 1] == '!')) name[--l] = 0;   // "@zip?" at the end of a sentence
        if (l == 0 || strstr(name, "..") || strstr(name, "//") || name[0] == '/' || (name[l - 1] == '/' && !strchr(name, ':'))) continue;
        int dup = 0;
        for (int i = 0; i < asked && i < 8; i++)
            if (!strcmp(seen[i], name)) dup = 1;
        if (dup) continue;
        if (asked < 8) strcpy(seen[asked], name);
        asked++;
        static char body[48000];
        long size = 0;
        char shown[NAME_MAX_LEN + 8];   // what the note and the header call it
        strcpy(shown, name);
        if (strchr(name, '~') && !strchr(name, ':') && !strchr(name, '?')) {   // phase 10: @before.zip~after.zip
            char za[NAME_MAX_LEN], zb[NAME_MAX_LEN], sg[300];
            char* tl = strchr(name, '~');
            snprintf(za, sizeof(za), "%.*s", (int)(tl - name), name);
            snprintf(zb, sizeof(zb), "%s", tl + 1);
            int ha = resolve_base(za, sg, sizeof(sg)), hb = resolve_base(zb, sg, sizeof(sg));
            if (ha < 0 || hb < 0 || !strcasecmp(za, zb)) {
                note_add(note, note_max, "@%s: needs two different zips that exist (the one from before and the one from after). ", name, 0);
                continue;
            }
            snprintf(name, sizeof(name), "%.90s~%.90s", za, zb);
            snprintf(shown, sizeof(shown), "%s", name);
            strcpy(last_zip, zb);
        } else
        {   // phase 6: "@zip", "@:path", a name start, or a typo of a file name
            char* colon = strpbrk(name, ":?!");
            char base[NAME_MAX_LEN], rest[NAME_MAX_LEN];
            int bl2 = colon ? (int)(colon - name) : (int)strlen(name);
            memcpy(base, name, bl2);
            base[bl2] = 0;
            snprintf(rest, sizeof(rest), "%s", colon ? colon : "");
            char sugg[300];
            int how = resolve_base(base, sugg, sizeof(sugg));
            if (how < 0) {
                if (sugg[0]) {
                    char m[360];
                    snprintf(m, sizeof(m), "@%.80s: not found. Did you mean: %s. ", name, sugg);
                    int ml = (int)strlen(note);
                    snprintf(note + ml, note_max - ml > 0 ? note_max - ml : 0, "%s", m);
                } else note_add(note, note_max, "@%s: not found. ", name, 0);
                continue;
            }
            if (how > 0) {
                if (how >= 2) {   // tell the user what it was taken for
                    char m[240];
                    snprintf(m, sizeof(m), "@%.70s = %.100s. ", name, base);
                    int ml = (int)strlen(note);
                    if (ml < note_max - 1) snprintf(note + ml, note_max - ml, "%s", m);
                }
                snprintf(name, sizeof(name), "%.*s%s", (int)sizeof(name) - 1 - (int)strlen(rest), base, rest);
                snprintf(shown, sizeof(shown), "%s", name);
            }
            if (ends_ci(base, ".zip")) strcpy(last_zip, base);
        }
        int zcut = 0;
        g_att_a = g_att_b = 0;
        int n = read_special(name, body, sizeof(body), &size, &zcut);
        if (n == -2) n = read_file(name, body, sizeof(body), &size);
        if (n < 0) {
            if (n == -3) note_add(note, note_max, "@%s: several files match, say the path: ", name, 0), note_add(note, note_max, "%s. ", body, 0);
            else if (n == -4) note_add(note, note_max, "@%s: not a text file (binary). ", name, 0);
            else if (n == -5) note_add(note, note_max, "@%s: cannot be read (damaged, zip64 or unsupported). ", name, 0);
            else if (n == -6) note_add(note, note_max, "@%s: no such function or heading. ", name, 0), note_add(note, note_max, "Try @zip?%s. ", body, 0);
            else if (n == -7) note_add(note, note_max, "@%s: that line range is empty (the file has ", name, 0), note_add(note, note_max, "%s lines). ", body, 0);
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
        if (!g_att_a && strchr(name, ':') && strncmp(name, "web/", 4) && n > FILES_WHOLE_CAP && room > FILES_WHOLE_CAP) {
            // phase 8: a whole file of the project is only its first part (the prefill of ~65 tokens/s makes the rest cost
            // minutes); the end of the answer tells how to ask for a function or a line range
            n = cut_clean(body, FILES_WHOLE_CAP);
            cut = 1;
        } else
        if (n > room) n = cut_clean(body, room), cut = 1;
        else if (!g_att_a && (size > (long)sizeof(body) - 1 || zcut)) cut = 1;   // (a line range / function is whole: size is the file's)
        if (n > 0 && body[n - 1] == '\n') n--;
        char marker[220] = "";
        if (cut && !g_att_a && strchr(name, ':') && strncmp(name, "web/", 4)) {   // tell the model the file goes on
            snprintf(marker, sizeof(marker), "(cut: %d of %ld bytes shown; the rest with @%.80s:A-B, a line range, or @%.80s#function)\n", n, size, name, name);
            if ((int)strlen(marker) > room - n) marker[0] = 0;
        }
        memcpy(ctx + used, head, hl);
        used += hl;
        memcpy(ctx + used, body, n);
        used += n;
        memcpy(ctx + used, tail, strlen(tail));
        used += strlen(tail);
        if (marker[0]) {
            int ml = (int)strlen(marker);
            memcpy(ctx + used, marker, ml);
            used += ml;
        }
        ctx[used] = 0;
        attached++;
        if (g_last_rel[0] && g_natt < FILES_ATT_MAX) {
            snprintf(g_att[g_natt].path, sizeof(g_att[0].path), "%s", g_last_rel);
            g_att[g_natt].a = g_att_a ? g_att_a : 1;
            g_att[g_natt].b = g_att_a ? g_att_b : 1 << 29;
            g_natt++;
        }
        if (cut) note_add(note, note_max, "@%s: cut to the first %ld bytes. ", name, n);
        else if (g_att_a) {
            char lr[200];
            snprintf(lr, sizeof(lr), "@%.80s attached (lines %d-%d). ", name, g_att_a, g_att_b);
            int nl = (int)strlen(note);
            if (nl < note_max - 1) snprintf(note + nl, note_max - nl, "%s", lr);
        } else note_add(note, note_max, "@%s attached (%ld bytes). ", name, size);
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

// ---- phase 7: what the dev commands (devcmd.c) need from here ---------------------------------------------------

int files_attached(FilesAttached* out, int max) {
    int n = g_natt < max ? g_natt : max;
    memcpy(out, g_att, sizeof(FilesAttached) * n);
    return n;
}

void files_set_project(const char* zip_name) {
    snprintf(last_zip, sizeof(last_zip), "%s", zip_name ? zip_name : "");
}

const char* files_project(void) { return last_zip; }

int files_resolve_zip(const char* typed, char* name_out, int max, char* sugg, int sugg_max) {
    char base[NAME_MAX_LEN];
    snprintf(base, sizeof(base), "%s", typed ? typed : "");
    int how = resolve_base(base, sugg, sugg_max);
    if (how < 0) return 0;
    if (!ends_ci(base, ".zip")) {
        snprintf(sugg, sugg_max, "%.60s is not a .zip", base);
        return 0;
    }
    snprintf(name_out, max, "%s", base);
    return 1;
}

// The entry of the project zip for "path", "folder/path" without the repo-main/ part, or a unique file name.
// Returns it (0 when none or several); ents/n are the table of zip_open_dir, zpath the zip's path.
static const ZipEnt* find_project_entry(const char* spec, const ZipEnt** ents, int* n, char* zpath, int zmax) {
    if (!last_zip[0] || !file_exists(last_zip) || !spec[0]) return NULL;
    snprintf(zpath, zmax, FILES_DIR "/%s", last_zip);
    int total;
    *n = zip_open_dir(zpath, ents, &total);
    if (*n < 0) return NULL;
    int slen = (int)strlen(spec);
    const ZipEnt* hit = NULL;
    int hits = 0;
    for (int pass = 0; pass < 3 && !hits; pass++)
        for (int i = 0; i < *n; i++) {
            const char* nm = (*ents)[i].name;
            int ok = 0;
            if (pass == 0) ok = !strcmp(nm, spec);
            else if (pass == 1) {
                const char* sub = strchr(nm, '/');
                ok = sub && !strcmp(sub + 1, spec);
            } else {
                int nl = (int)strlen(nm);
                ok = nl > slen && nm[nl - slen - 1] == '/' && !strcmp(nm + nl - slen, spec);
            }
            if (ok) hit = &(*ents)[i], hits++;
        }
    return hits == 1 ? hit : NULL;
}

// Path inside the zip (without the repo-main/ folder) of what spec names. 1 found, 0 not found, -3 no project.
int files_project_resolve(const char* spec, char* rel, int rel_max) {
    if (!last_zip[0] || !file_exists(last_zip)) return -3;
    const ZipEnt* ents;
    int n;
    char zp[200];
    const ZipEnt* hit = find_project_entry(spec, &ents, &n, zp, sizeof(zp));
    if (!hit) return 0;
    snprintf(rel, rel_max, "%s", hit->name + repo_root_len(ents, n));
    return 1;
}

// Returns the whole file into out (raw: CRs kept), *len its size, rel its path inside the zip.
// 1 = ok, 0 = not found, -1 = bigger than max, -2 = binary or damaged, -3 = no project.
int files_project_read(const char* spec, char* out, int max, int* len, char* rel, int rel_max) {
    if (!last_zip[0] || !file_exists(last_zip)) return -3;
    const ZipEnt* ents;
    int n;
    char path[200];
    const ZipEnt* hit = find_project_entry(spec, &ents, &n, path, sizeof(path));
    if (!hit) return 0;
    if (hit->usize >= (uint32_t)max) return -1;
    int cut = 0;
    int r = zip_read(path, hit, out, max - 1, &cut);
    if (r < 0 || cut) return -2;
    for (int i = 0; i < r && i < 400; i++)
        if ((unsigned char)out[i] == 0) return -2;
    *len = r;
    out[r] = 0;
    snprintf(rel, rel_max, "%s", hit->name + repo_root_len(ents, n));
    return 1;
}

// phase 11: any file of FILES_DIR as typed (a start of the name, a typo) -> its real name. 1 = found.
int files_resolve_any(const char* typed, char* name_out, int max) {
    char base[NAME_MAX_LEN], sg[300];
    snprintf(base, sizeof(base), "%s", typed ? typed : "");
    if (resolve_base(base, sg, sizeof(sg)) < 0) return 0;
    snprintf(name_out, max, "%s", base);
    return 1;
}
