#include "devcmd.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ask.h"
#include "bintools.h"
#include "buildlog.h"
#include "ghinfo.h"
#include "undo.h"
#include "verify.h"
#include "files.h"
#include "outfiles.h"
#include "patch.h"
#include "repomap.h"
#include "zipwrite.h"
#include "progress.h"
#include "workspace.h"

#define FBUF (200 * 1024)
#define MAX_BLOCKS 16
#define PEND_MAX 24000

static int g_auto, g_rounds = 4, g_limit, g_round_now;
static int g_loaded;
static char g_pend[PEND_MAX];
static int g_pend_n;
static RepoSeenSet g_seen;
static char g_prev_want[420];   // phase 8: what the model asked for in the last round (a repeat is not brought again)
static char g_warn[1500];        // phase 8: edits that changed the balance of () {} [] (the agent tells the model)
static int g_warn_n;
static char g_touched[24][160];   // phase 12: files the AI wrote this session (for /verificar)
static int g_ntouched;

static void touch(const char* rel) {
    for (int i = 0; i < g_ntouched; i++)
        if (!strcmp(g_touched[i], rel)) return;
    if (g_ntouched < 24) snprintf(g_touched[g_ntouched++], sizeof(g_touched[0]), "%s", rel);
}

// Adds "rel:\n<issues>" to what the agent tells the model.
static void warn_issues(const char* rel, const char* issues) {
    int gl = (int)strlen(g_warn);
    if (gl + 60 >= (int)sizeof(g_warn)) return;
    snprintf(g_warn + gl, sizeof(g_warn) - gl, "%s:\n%s", rel, issues);
    g_warn_n++;
}

static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    FILE* f = fopen(DEV_FLAG_FILE, "r");
    if (!f) return;
    char l[200];
    while (fgets(l, sizeof(l), f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (!strncmp(l, "auto=", 5)) g_auto = atoi(l + 5) != 0;
        else if (!strncmp(l, "rounds=", 7)) g_rounds = atoi(l + 7);
        else if (!strncmp(l, "limit=", 6)) g_limit = atoi(l + 6);
        else if (!strncmp(l, "project=", 8)) files_set_project(l + 8);
    }
    fclose(f);
    if (g_rounds < 1 || g_rounds > 12) g_rounds = 4;
    if (g_limit < 0) g_limit = 0;
}

static void save(void) {
    FILE* f = fopen(DEV_FLAG_FILE, "w");
    if (!f) return;
    fprintf(f, "auto=%d\nrounds=%d\nlimit=%d\nproject=%s\n", g_auto, g_rounds, g_limit, files_project());
    fclose(f);
}

int devcmd_auto_on(void) { load(); return g_auto; }
int devcmd_token_limit(void) { load(); return g_limit; }
void devcmd_user_turn(void) { g_round_now = 0; g_seen.n = 0; g_prev_want[0] = 0; }
void devcmd_new_context(void) { g_prev_want[0] = 0; }
int devcmd_pending_len(void) { return g_pend_n; }

int devcmd_take_pending(char* dst, int max) {
    int n = g_pend_n < max ? g_pend_n : max;
    memcpy(dst, g_pend, n);
    g_pend_n = 0;
    return n;
}

// ---- command parsing ----
static int word_is(const char* t, int tl, const char* lit) {
    int l = (int)strlen(lit);
    if (tl != l) return 0;
    for (int i = 0; i < l; i++) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != lit[i]) return 0;
    }
    return 1;
}

static const char* const NAMES[] = {"proyecto", "project", "mapa", "map", "grep", "grep", "aplicar", "apply",
                                    "empaquetar", "zip", "auto", "limite", "limit", "proyectos", "projects",
                                    "instrucciones", "instructions", "poke", "parchear", "errores", "errors", "trucos", "cheats", "deshacer", "undo", "verificar", "verify", "pokebits", "checksum", NULL};

// Which command (index into NAMES) and where its arguments start. -1 when text is none of them.
static int parse(const char* text, const char** args) {
    while (*text == ' ') text++;
    if (*text != '/') return -1;
    const char* w = text + 1;
    int wl = 0;
    while (w[wl] && w[wl] != ' ' && w[wl] != '\n') wl++;
    for (int i = 0; NAMES[i]; i++)
        if (word_is(w, wl, NAMES[i])) {
            const char* a = w + wl;
            while (*a == ' ') a++;
            *args = a;
            return i;
        }
    return -1;
}

int devcmd_is_cmd(const char* text) {
    const char* a;
    return parse(text, &a) >= 0;
}

static void pend(const char* title, const char* body, int n) {
    int t = snprintf(g_pend, PEND_MAX, "%s\n", title);
    if (t >= PEND_MAX) t = PEND_MAX - 1;
    if (n > PEND_MAX - t - 1) n = PEND_MAX - t - 1;
    memcpy(g_pend + t, body, n);
    g_pend_n = t + n;
}

// ---- applying edits ----
static void log_change(const char* line) {
    char p[160];
    snprintf(p, sizeof(p), "%s/CAMBIOS.txt", OUT_DIR);
    int f = open(p, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (f < 0) return;
    if (write(f, line, strlen(line)) < 0) {}
    close(f);
}

static int read_out(const char* rel, char* buf, int max, int* len) {
    char p[300];
    snprintf(p, sizeof(p), "%s/%s", OUT_DIR, rel);
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, max - 1, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (more) return -1;
    buf[n] = 0;
    *len = n;
    return 1;
}

static char fbuf[FBUF + 32 * 1024];
static PatchBlock blocks[MAX_BLOCKS];

// Applies the edit blocks of reply. report gets one line per block (and hints for the failed ones, which the model
// can use to try again). *ok / *bad count the blocks.
static int apply_reply(const char* reply, char* report, int rmax, int* ok, int* bad) {
    int nb = patch_parse(reply, blocks, MAX_BLOCKS);
    int rl = 0;
    *ok = *bad = 0;
    report[0] = 0;
    g_warn[0] = 0;
    g_warn_n = 0;
    for (int i = 0; i < nb; i++) {
        const PatchBlock* b = &blocks[i];
        char rel[160], hint[400] = "", what[760];
        int len = 0, exists = 0, line = 0;
        const char* how = "";
        snprintf(rel, sizeof(rel), "%s", b->path);
        int r = read_out(rel, fbuf, FBUF, &len);   // an earlier edit of this session first
        if (r == 1) exists = 1;
        else if (r == 0) {
            char pr[160];
            int pr_rc = files_project_read(rel, fbuf, FBUF, &len, pr, sizeof(pr));
            if (pr_rc == 1) exists = 1, snprintf(rel, sizeof(rel), "%s", pr);
            else if (pr_rc == -1 || pr_rc == -2) r = -2;
        } else r = -2;
        if (r == -2) {
            snprintf(what, sizeof(what), "FALLO %s: file too big or not text", b->path);
            (*bad)++;
        } else if (!outfiles_path_ok(rel)) {
            snprintf(what, sizeof(what), "FALLO %s: path not allowed (letters, digits . _ - / only, at most 4 levels)", b->path);
            (*bad)++;
        } else {
            int bal0[3] = {0, 0, 0}, bal1[3];
            char vi0[400], vi1[700];
            int v0 = exists ? verify_text(rel, fbuf, len, vi0, sizeof(vi0)) : 0;
            if (exists) patch_balance(rel, fbuf, len, bal0);
            int rc = patch_apply(fbuf, &len, FBUF + 32 * 1024 - 1, b, exists, &line, &how, hint, sizeof(hint));
            if (rc == 0 && outfiles_write(rel, fbuf, len) == 0) {
                snprintf(what, sizeof(what), "OK %s (line %d, %s match)", rel, line, how);
                (*ok)++;
                touch(rel);
                int v1 = verify_text(rel, fbuf, len, vi1, sizeof(vi1));
                if (v1 > v0) {   // the edit made the file worse than it was: say what
                    int wl = (int)strlen(what);
                    snprintf(what + wl, sizeof(what) - wl, " PROBLEMS:\n%.400s", vi1);
                    warn_issues(rel, vi1);
                }
                if (patch_balance(rel, fbuf, len, bal1) && (bal1[0] != bal0[0] || bal1[1] != bal0[1] || bal1[2] != bal0[2])) {
                    char w[200];   // an edit rarely changes how many brackets are open: a block was left open or closed twice
                    snprintf(w, sizeof(w), " WARNING: now %+d in () %+d in {} %+d in [] (open minus closed, compared with before).",
                             bal1[0] - bal0[0], bal1[1] - bal0[1], bal1[2] - bal0[2]);
                    int wl = (int)strlen(what);
                    snprintf(what + wl, sizeof(what) - wl, "%s", w);
                    int gl = (int)strlen(g_warn);
                    snprintf(g_warn + gl, sizeof(g_warn) - gl, "%s: %+d en (), %+d en {}, %+d en [].\n", rel, bal1[0] - bal0[0],
                             bal1[1] - bal0[1], bal1[2] - bal0[2]);
                    g_warn_n++;
                }
            } else if (rc == 0) {
                snprintf(what, sizeof(what), "FALLO %s: could not write", rel);
                (*bad)++;
            } else {
                const char* why = rc == -1 ? "the SEARCH text was not found" : rc == -2 ? "the SEARCH text appears more than once (add lines around it)"
                                  : rc == -3 ? "no room (file too big)" : "empty SEARCH on an existing file";
                snprintf(what, sizeof(what), "FALLO %s: %s.%s%s", rel, why, hint[0] ? " Closest text in the file:\n" : "", hint);
                (*bad)++;
            }
        }
        if (what[0] == 'O') {
            char lg[780];
            snprintf(lg, sizeof(lg), "%s\n", what);
            log_change(lg);
        }
        int w = snprintf(report + rl, rmax - rl, "%s\n", what);
        if (w > rmax - rl - 1) w = rmax - rl - 1;
        rl += w;
    }
    progress_edits(*ok, *bad, report);   // phase 8: the ledger that survives a restart of the context
    return nb;
}

// ---- projects (phase 9) ----
static void name_from_zip(const char* zip, char* out, int max) {
    int n = 0;
    for (const char* p = zip; *p && n < max - 1 && n < 40; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') out[n++] = c;
        else if (n > 0 && out[n - 1] != '-') out[n++] = '-';
    }
    if (n > 4 && !strncmp(out + n - 4, ".zip", 4)) n -= 4;
    while (n > 0 && (out[n - 1] == '-' || out[n - 1] == '.')) n--;
    while (n > 0 && out[0] == '.') memmove(out, out + 1, n--);
    out[n] = 0;
}

// The ledger of the project we leave is saved, the one of the project we enter is read, and the model hears about the
// project with the next message.
static void ws_before(void) {
    if (progress_has()) progress_save();
}

static void ws_after(void) {
    static char ctx[3000];
    progress_clear();
    progress_load();
    files_set_project(ws_base_zip());
    save();
    int n = ws_context_text(ctx, sizeof(ctx));
    if (n > 0) pend("[Proyecto]", ctx, n);
}

static void cmd_project(const char* args, char* note, int nm) {
    if (!args[0]) {
        if (ws_name()[0])
            snprintf(note, nm, "Project: %s%s%s. Files: %s. /proyectos lists them, /proyecto nuevo NAME makes one, /proyecto salir leaves it.", ws_name(),
                     ws_base_zip()[0] ? " (base zip: " : "", ws_base_zip()[0] ? ws_base_zip() : "", ws_out_dir());
        else
            snprintf(note, nm, files_project()[0] ? "Repo zip: %s. No project yet: /proyecto nuevo NAME keeps what the AI creates apart."
                                                  : "No project. /proyecto nuevo NAME makes one (or put a repo .zip in /data/garlic_files and type /proyecto name.zip).",
                     files_project());
        return;
    }
    int w1 = 0;
    while (args[w1] && args[w1] != ' ' && args[w1] != '\n') w1++;
    const char* rest = args + w1;
    while (*rest == ' ') rest++;
    if (word_is(args, w1, "nuevo") || word_is(args, w1, "new") || word_is(args, w1, "crear")) {
        char name[60];
        int k = 0;
        while (rest[k] && rest[k] != ' ' && rest[k] != '\n' && k < 58) name[k] = rest[k], k++;
        name[k] = 0;
        const char* zarg = rest + k;
        while (*zarg == ' ') zarg++;
        char zip[100] = "", sugg[300];
        if (zarg[0] && !files_resolve_zip(zarg, zip, sizeof(zip), sugg, sizeof(sugg))) {
            snprintf(note, nm, "No such zip: %s", sugg);
            return;
        }
        if (!ws_valid_name(name)) {
            snprintf(note, nm, "Usage: /proyecto nuevo NAME [repo.zip]  (NAME: letters, digits . _ - , up to 40)");
            return;
        }
        ws_before();
        int rc = ws_create(name, zip);
        if (rc == -2) {
            snprintf(note, nm, "A project called %s already exists: /proyecto %s opens it.", name, name);
            return;
        }
        if (rc != 0) {
            snprintf(note, nm, "Could not create the project (is /data writable?).");
            return;
        }
        ws_after();
        snprintf(note, nm, "Project %s created and active%s%s. What the AI writes goes to %s. /instrucciones adds instructions for it.", name,
                 zip[0] ? ", base zip " : "", zip, ws_out_dir());
        return;
    }
    if (word_is(args, w1, "salir") || word_is(args, w1, "ninguno") || word_is(args, w1, "none") || word_is(args, w1, "off")) {
        ws_before();
        ws_leave();
        progress_clear();
        progress_load();
        save();
        snprintf(note, nm, "No project: files go to the shared %s again.", ws_out_dir());
        return;
    }
    char tok[60];
    snprintf(tok, sizeof(tok), "%.*s", w1 < 58 ? w1 : 58, args);
    if (ws_exists(tok)) {
        if (!strcmp(tok, ws_name())) {
            snprintf(note, nm, "Project %s is already the active one.", tok);
            return;
        }
        ws_before();
        ws_switch(tok);
        ws_after();
        snprintf(note, nm, "Project %s is now active (files in %s). Its list and instructions go with your next message; press Triangle for a new chat so the model reads them in its system prompt.", tok,
                 ws_out_dir());
        return;
    }
    char name[100], sugg[300];
    if (!files_resolve_zip(args, name, sizeof(name), sugg, sizeof(sugg))) {
        snprintf(note, nm, "No project or zip called that. %s. /proyecto nuevo NAME makes a project; /proyectos lists them.", sugg);
        return;
    }
    if (ws_name()[0]) {   // a zip while a project is active: it becomes the project's base repo
        ws_set_base_zip(name);
        files_set_project(name);
        save();
        snprintf(note, nm, "Project %s is now based on %s. \"@zip\" means it: /mapa shows it, /grep words searches it.", ws_name(), name);
        return;
    }
    char pn[60];
    name_from_zip(name, pn, sizeof(pn));
    if (!ws_valid_name(pn)) snprintf(pn, sizeof(pn), "proyecto");
    ws_before();
    if (ws_exists(pn)) ws_switch(pn);
    else ws_create(pn, name);
    ws_set_base_zip(name);
    ws_after();
    snprintf(note, nm, "Project %s (based on %s) is active. \"@zip\" means the zip: /mapa shows it, /grep words searches it. What the AI writes goes to %s.", ws_name(),
             name, ws_out_dir());
}

// ---- commands ----
// phase 10: /poke OFFSET TYPE VALUE FILE [zip] writes name-mod.zip, a copy of the saved game with those bytes changed.
// Returns 1 when it wrote the new zip.
static int do_poke(const char* args, char* note, int nm) {
    char tk[5][120];
    int nt = 0;
    const char* p = args;
    while (*p && nt < 5) {
        while (*p == ' ' || *p == '`') p++;
        if (!*p || *p == '\n') break;
        int k = 0;
        while (*p && *p != ' ' && *p != '\n' && k < 119) tk[nt][k++] = *p++;
        while (k > 0 && (tk[nt][k - 1] == '`' || tk[nt][k - 1] == '.')) k--;   // ("/poke ... file." at the end of a sentence)
        tk[nt++][k] = 0;
    }
    if (nt < 4) {
        snprintf(note, nm, "Usage: /poke OFFSET TYPE VALUE FILE [zip]   e.g. /poke 0x40 u32 999999 SAVEDATA/slot0.dat   (TYPE: u8 u16 u32 u64 i8 i16 i32 i64 f32 f64). It writes a NEW zip (name-mod.zip); the original is never changed.");
        return 0;
    }
    char zip[96], sugg[200];
    if (nt >= 5) {
        if (!files_resolve_zip(tk[4], zip, sizeof(zip), sugg, sizeof(sugg))) {
            snprintf(note, nm, "No such zip: %.60s (%s)", tk[4], sugg);
            return 0;
        }
    } else snprintf(zip, sizeof(zip), "%s", files_project());
    if (!zip[0]) {
        snprintf(note, nm, "Which zip? Name it at the end (/poke ... FILE save.zip) or open it first with @save.zip.");
        return 0;
    }
    char stem[96], src[220], dst[220];
    snprintf(stem, sizeof(stem), "%s", zip);
    int sl = (int)strlen(stem);
    if (sl > 4 && !strcasecmp(stem + sl - 4, ".zip")) stem[sl - 4] = 0;
    if (sl > 4 && strlen(stem) > 4 && !strcmp(stem + strlen(stem) - 4, "-mod")) stem[strlen(stem) - 4] = 0;   // (patching a patch keeps one -mod)
    for (char* c = stem; *c; c++)
        if (*c == '/') *c = '_';
    snprintf(src, sizeof(src), FILES_DIR "/%s", zip);
    snprintf(dst, sizeof(dst), FILES_DIR "/%s-mod.zip", stem);
    int r = bin_poke(src, dst, tk[3], tk[0], tk[1], tk[2], note, nm);
    if (r == 0) {
        char lg[300];
        snprintf(lg, sizeof(lg), "POKE %.60s %.60s %.20s %.10s %.20s -> %.80s\n", zip, tk[3], tk[0], tk[1], tk[2], dst);
        log_change(lg);
    }
    return r == 0;
}

// phase 13: /pokebits OFFSET:BIT WIDTH VALUE ORDER FILE [zip] writes name-mod.zip with that bit field changed (bitscan.h).
static int do_pokebits(const char* args, char* note, int nm) {
    char tk[6][120];
    int nt = 0;
    const char* p = args;
    while (*p && nt < 6) {
        while (*p == ' ' || *p == '`') p++;
        if (!*p || *p == '\n') break;
        int k = 0;
        while (*p && *p != ' ' && *p != '\n' && k < 119) tk[nt][k++] = *p++;
        while (k > 0 && (tk[nt][k - 1] == '`' || tk[nt][k - 1] == '.')) k--;
        tk[nt++][k] = 0;
    }
    if (nt < 5) {
        snprintf(note, nm, "Usage: /pokebits OFFSET:BIT WIDTH VALUE ORDER FILE [zip]   e.g. /pokebits 0x3A41F:5 17 100000 lsb DATA save.zip   (OFFSET:BIT = the byte and the bit inside it, counted from the lowest bit for lsb or the highest for msb, as the compare and the search say; WIDTH in bits). It writes a NEW zip (name-mod.zip); the original is never touched.");
        return 0;
    }
    char zip[96], sugg[200];
    if (nt >= 6) {
        if (!files_resolve_zip(tk[5], zip, sizeof(zip), sugg, sizeof(sugg))) {
            snprintf(note, nm, "No such zip: %.60s (%s)", tk[5], sugg);
            return 0;
        }
    } else snprintf(zip, sizeof(zip), "%s", files_project());
    if (!zip[0]) {
        snprintf(note, nm, "Which zip? Name it at the end (/pokebits ... FILE save.zip) or open it first with @save.zip.");
        return 0;
    }
    char stem[96], src[220], dst[220];
    snprintf(stem, sizeof(stem), "%s", zip);
    int sl = (int)strlen(stem);
    if (sl > 4 && !strcasecmp(stem + sl - 4, ".zip")) stem[sl - 4] = 0;
    if (strlen(stem) > 4 && !strcmp(stem + strlen(stem) - 4, "-mod")) stem[strlen(stem) - 4] = 0;
    for (char* c = stem; *c; c++)
        if (*c == '/') *c = '_';
    snprintf(src, sizeof(src), FILES_DIR "/%s", zip);
    snprintf(dst, sizeof(dst), FILES_DIR "/%s-mod.zip", stem);
    int r = bin_pokebits(src, dst, tk[4], tk[0], tk[1], tk[2], tk[3], note, nm);
    if (r == 0) {
        char lg[300];
        snprintf(lg, sizeof(lg), "POKEBITS %.60s %.60s %.20s %.8s %.20s %.4s -> %.80s\n", zip, tk[4], tk[0], tk[1], tk[2], tk[3], dst);
        log_change(lg);
    }
    return r == 0;
}

// phase 13: /checksum OFFSET FILE [zip]: which checksum is stored at that offset (the report goes with the next message).
static void do_checksum(const char* args, char* note, int nm) {
    char tk[3][120];
    int nt = 0;
    for (const char* p = args; *p && nt < 3;) {
        while (*p == ' ' || *p == '`') p++;
        int k = 0;
        while (*p && *p != ' ' && *p != '\n' && k < 119) tk[nt][k++] = *p++;
        while (k > 0 && (tk[nt][k - 1] == '`' || tk[nt][k - 1] == '.')) k--;
        if (k) tk[nt++][k] = 0;
        if (*p == '\n') break;
    }
    if (nt < 2) {
        snprintf(note, nm, "Usage: /checksum OFFSET FILE [zip]   e.g. /checksum 0x40 DATA save.zip   (OFFSET = where the 4 bytes of the checksum are: a spot that changes at the start or the end of the file every time you save, see @before.zip~after.zip).");
        return;
    }
    char zip[96], sugg[200];
    if (nt >= 3) {
        if (!files_resolve_zip(tk[2], zip, sizeof(zip), sugg, sizeof(sugg))) {
            snprintf(note, nm, "No such zip: %.60s (%s)", tk[2], sugg);
            return;
        }
    } else snprintf(zip, sizeof(zip), "%s", files_project());
    if (!zip[0]) {
        snprintf(note, nm, "Which zip? Name it at the end (/checksum OFFSET FILE save.zip) or open it first with @save.zip.");
        return;
    }
    char src[220];
    snprintf(src, sizeof(src), FILES_DIR "/%s", zip);
    static char rep[3000];
    int n = bin_checksum(src, tk[1], tk[0], rep, sizeof(rep));
    if (n == -2) snprintf(note, nm, "\"%s\" is not an offset (use 0x40 or 64).", tk[0]);
    else if (n == -3) snprintf(note, nm, "%s is not in the zip.", tk[1]);
    else if (n < 0) snprintf(note, nm, "Cannot read %s.", zip);
    else {
        pend("[Checksum]", rep, n);
        snprintf(note, nm, "%.700s\nThe report also goes with your next message.", rep);
    }
}

int devcmd_run(const char* text, const char* last_ai, char* note, int nm) {
    load();
    const char* args;
    int c = parse(text, &args);
    note[0] = 0;
    if (c <= 1) {   // /proyecto
        cmd_project(args, note, nm);
        return 0;
    } else if (c <= 3) {   // /mapa
        static char wsc[3000];
        int wn = ws_context_text(wsc, sizeof(wsc));
        if (!files_project()[0]) {
            if (wn > 0) {
                pend("[Proyecto]", wsc, wn);
                snprintf(note, nm, "Project %s: its instructions and file list (%d bytes) go with your next message.", ws_name(), wn);
            } else snprintf(note, nm, "No project. Use /proyecto nuevo NAME or /proyecto name.zip first.");
            return 0;
        }
        char zp[200];
        static char map[PEND_MAX - 200];
        int ns = 0;
        snprintf(zp, sizeof(zp), FILES_DIR "/%s", files_project());
        int n = repomap_build(zp, map, 12000, &ns);
        if (n < 0) snprintf(note, nm, "Cannot read %s", files_project());
        else {
            if (wn > 0 && n + wn + 2 < (int)sizeof(map)) n += snprintf(map + n, sizeof(map) - n, "\n%s", wsc);
            pend("[Repo map]", map, n);
            snprintf(note, nm, "Map of %s: %d source files, %d bytes%s. It goes with your next message.", files_project(), ns, n, wn > 0 ? " (with the project's own files)" : "");
        }
    } else if (c <= 5) {   // /grep
        if (!files_project()[0] || !args[0]) {
            snprintf(note, nm, "Usage: /grep word1 word2 (needs a project: /proyecto name.zip)");
            return 0;
        }
        char zp[200], kw[200];
        static char out[PEND_MAX - 200];
        snprintf(zp, sizeof(zp), FILES_DIR "/%s", files_project());
        snprintf(kw, sizeof(kw), "%s", args);
        int n = repo_search(zp, kw, out, 12000, 6, &g_seen, NULL);
        if (n < 0) snprintf(note, nm, "Cannot read %s", files_project());
        else {
            pend("[Search results]", out, (int)strlen(out));
            snprintf(note, nm, "%d file(s) match. They go with your next message.", n);
        }
    } else if (c <= 7) {   // /aplicar
        static char rep[3000];
        int ok = 0, bad = 0;
        undo_begin();   // phase 11: /deshacer puts back what this changes
        int nb = last_ai && last_ai[0] ? apply_reply(last_ai, rep, sizeof(rep), &ok, &bad) : 0;
        char fnote[400] = "";
        int nf = last_ai && last_ai[0] ? outfiles_save(last_ai, fnote, sizeof(fnote)) : 0;
        undo_end();
        if (nf > 0) {
            char lg[420];
            snprintf(lg, sizeof(lg), "GUARDADO %s\n", fnote);
            log_change(lg);
        }
        int pk = 0;
        if (last_ai && last_ai[0]) {   // a "/poke ..." line of the answer (a saved game edit)
            for (const char* l = last_ai; *l; l++) {
                if (l != last_ai && l[-1] != '\n') continue;
                const char* q = l;
                while (*q == ' ' || *q == '`') q++;
                if (!strncmp(q, "/poke ", 6)) {
                    pk = do_poke(q + 6, note, nm);
                    if (!pk && !note[0]) snprintf(note, nm, "The /poke line of the answer could not be run.");
                    return 0;
                }
                if (!strncmp(q, "/pokebits ", 10)) {   // phase 13: a bit field
                    pk = do_pokebits(q + 10, note, nm);
                    if (!pk && !note[0]) snprintf(note, nm, "The /pokebits line of the answer could not be run.");
                    return 0;
                }
            }
        }
        (void)pk;
        if (nb == 0 && nf == 0) snprintf(note, nm, "The last answer has no edit blocks (BUSCAR / REEMPLAZAR) and no named files (\"Archivo: path\" with a code block under it).");
        else snprintf(note, nm, "%d edit(s) applied, %d failed; %d file(s) saved (files in %s, log in CAMBIOS.txt):\n%.600s%s%.600s", ok, bad, nf, OUT_DIR, rep, nf ? "\n" : "", fnote);
    } else if (c <= 9) {   // /empaquetar
        char name[80] = "cambios", zp[200];
        if (ws_name()[0]) snprintf(name, sizeof(name), "%s", ws_name());
        if (args[0]) {
            int k = 0;
            for (; args[k] && args[k] != ' ' && k < 70; k++) name[k] = args[k] == '/' ? '_' : args[k];
            name[k] = 0;
            if (k > 4 && !strcmp(name + k - 4, ".zip")) name[k - 4] = 0;
        }
        snprintf(zp, sizeof(zp), FILES_DIR "/%s.zip", name);
        long bytes = 0;
        int n = zipwrite_dir(OUT_DIR, zp, &bytes);
        if (n == -2) snprintf(note, nm, "Nothing to pack: %s is empty.", OUT_DIR);
        else if (n < 0) snprintf(note, nm, "Could not create %s", zp);
        else snprintf(note, nm, "Packed %d files (%ld bytes) into %s: take it over FTP.", n, bytes, zp);
    } else if (c == 10) {   // /auto
        int v = !strncmp(args, "on", 2) || !strncmp(args, "si", 2) || !strncmp(args, "1", 1);
        int off = !strncmp(args, "off", 3) || !strncmp(args, "no", 2) || !strncmp(args, "0", 1);
        if (v || off) {
            g_auto = v;
            const char* sp = strchr(args, ' ');
            if (sp && atoi(sp + 1) >= 1 && atoi(sp + 1) <= 12) g_rounds = atoi(sp + 1);
            save();
        }
        snprintf(note, nm, "Auto: %s, up to %d rounds per message. (The agent applies edits and brings the files the answer asks for; network only if /red on.)",
                 g_auto ? "ON" : "off", g_rounds);
    } else if (c == 13 || c == 14) {   // /proyectos
        static char lst[2500];
        int n = ws_list(lst, sizeof(lst));
        if (n == 0) snprintf(note, nm, "No projects yet. /proyecto nuevo NAME makes one.");
        else snprintf(note, nm, "Projects (* = active; files in /data/garlic_files/out/NAME):\n%s/proyecto NAME opens one.", lst);
    } else if (c == 15 || c == 16) {   // /instrucciones
        if (!ws_name()[0]) {
            snprintf(note, nm, "No project. /proyecto nuevo NAME first: the instructions belong to a project.");
            return 0;
        }
        static char ins[1700];
        if (!args[0]) {
            if (ws_instructions(ins, sizeof(ins)) > 0) snprintf(note, nm, "Instructions of %s:\n%.900s\n(/instrucciones new text replaces them, /instrucciones borrar removes them.)", ws_name(), ins);
            else snprintf(note, nm, "Project %s has no instructions. /instrucciones text... sets them (e.g. language, style, what the project is). You can also edit /data/garlic_projects/%s/INSTRUCCIONES.md over FTP.", ws_name(), ws_name());
            return 0;
        }
        if (word_is(args, (int)strlen(args), "borrar") || word_is(args, (int)strlen(args), "clear")) {
            ws_set_instructions("");
            snprintf(note, nm, "Instructions of %s removed.", ws_name());
            return 0;
        }
        snprintf(ins, sizeof(ins), "%s", args);
        ws_set_instructions(ins);
        static char ctx[3000];
        int n = ws_context_text(ctx, sizeof(ctx));
        if (n > 0) pend("[Proyecto]", ctx, n);
        snprintf(note, nm, "Instructions of %s saved (%d chars). They go with your next message; a new chat (Triangle) puts them in the system prompt.", ws_name(), (int)strlen(ins));
    } else if (c == 17 || c == 18) {   // /poke
        do_poke(args, note, nm);
    } else if (c == 27) {   // /pokebits (phase 13)
        do_pokebits(args, note, nm);
    } else if (c == 28) {   // /checksum (phase 13)
        do_checksum(args, note, nm);
    } else if (c == 19 || c == 20) {   // /errores [log] [repo.zip]: the errors of a build log with the code around them
        char tk[2][120];
        int nt = 0;
        for (const char* p = args; *p && nt < 2;) {
            while (*p == ' ') p++;
            int k = 0;
            while (*p && *p != ' ' && k < 119) tk[nt][k++] = *p++;
            if (k) tk[nt++][k] = 0;
        }
        char logname[96] = "", cand[400], zipname[96] = "";
        if (nt >= 1) {
            if (!files_resolve_any(tk[0], logname, sizeof(logname))) {
                snprintf(note, nm, "No such file: %.60s. Put the build log (the build-log zip or a .txt) in /data/garlic_files.", tk[0]);
                return 0;
            }
        } else if (!buildlog_pick(logname, sizeof(logname), cand, sizeof(cand))) {
            snprintf(note, nm, "No build log found. Put the build-log zip (or a .txt) of the Actions run in /data/garlic_files, or name it: /errores NAME [repo.zip].");
            return 0;
        }
        if (nt >= 2) {
            if (!files_resolve_zip(tk[1], zipname, sizeof(zipname), cand, sizeof(cand))) {
                snprintf(note, nm, "No such repo zip: %.60s", tk[1]);
                return 0;
            }
        } else if (strcmp(files_project(), logname)) snprintf(zipname, sizeof(zipname), "%s", files_project());
        char lp[220], zp[220];
        snprintf(lp, sizeof(lp), FILES_DIR "/%s", logname);
        snprintf(zp, sizeof(zp), FILES_DIR "/%s", zipname);
        static char rep[PEND_MAX - 200];
        int n = buildlog_report(lp, zipname[0] ? zp : NULL, rep, 9000);
        if (n < 0) snprintf(note, nm, "Cannot read %s.", logname);
        else {
            pend("[Build errors]", rep, n);
            snprintf(note, nm, "%s: %d bytes of errors%s go with your next message (ask the AI to fix them; /aplicar applies its edits).", logname, n, zipname[0] ? " with the code around them" : " (no repo zip: name it as /errores log repo.zip to add the code)");
        }
    } else if (c == 21 || c == 22) {   // /trucos TITLEID: GoldHEN cheats and patches of a game
        static char gi[4000];
        int n = gh_game_info(args, gi, sizeof(gi));
        if (n < 0) snprintf(note, nm, "Usage: /trucos CUSA06548 (the game's title id). Lists its GoldHEN cheats and patches.");
        else {
            pend("[GoldHEN]", gi, n);
            snprintf(note, nm, "%.1200s(This list also goes with your next message.)", gi);
        }
    } else if (c == 25 || c == 26) {   // /verificar [file]: the check of the app on a file the AI wrote (or any file of the project)
        static char vtxt[1400];
        int total = 0, nfile = 0;
        vtxt[0] = 0;
        const char* list[25];
        int nl = 0;
        if (args[0]) list[nl++] = args;
        else
            for (int i = 0; i < g_ntouched && nl < 24; i++) list[nl++] = g_touched[i];
        for (int i = 0; i < nl; i++) {
            char vi[500], spec[160], pr[160];
            int sl = 0, k = 0;
            while (list[i][k] && list[i][k] != ' ' && list[i][k] != '\n' && k < 159) spec[k] = list[i][k], k++;
            spec[k] = 0;
            const char* shown = spec;
            int r = read_out(spec, fbuf, FBUF, &sl);
            if (r != 1 && files_project_read(spec, fbuf, FBUF, &sl, pr, sizeof(pr)) == 1) r = 1, shown = pr;
            if (r != 1) {
                int tl = (int)strlen(vtxt);
                snprintf(vtxt + tl, sizeof(vtxt) - tl, "%s: not found (or not text)\n", spec);
                continue;
            }
            nfile++;
            int n = verify_text(shown, fbuf, sl, vi, sizeof(vi));
            if (n > 0) {
                total += n;
                int tl = (int)strlen(vtxt);
                if (tl + 40 < (int)sizeof(vtxt)) snprintf(vtxt + tl, sizeof(vtxt) - tl, "%s:\n%.350s", shown, vi);
            }
        }
        if (nl == 0) snprintf(note, nm, "Nothing to check yet: the AI has written no file this session. /verificar path checks any file of the project.");
        else if (!total && nfile) snprintf(note, nm, "Checked %d file(s): no problems found (JSON, YAML/workflows, Python, shell, C and Markdown are checked).", nfile);
        else snprintf(note, nm, "%d problem(s):\n%.1200s", total, vtxt);
    } else if (c == 23 || c == 24) {   // /deshacer
        undo_last(note, nm);
    } else {   // /limite
        if (args[0]) {
            g_limit = atoi(args);
            if (g_limit < 0) g_limit = 0;
            if (g_limit && g_limit < 1000) g_limit = 1000;
            save();
        }
        if (g_limit) snprintf(note, nm, "Context per message: at most %d tokens (/limite 0 = as much as fits).", g_limit);
        else snprintf(note, nm, "Context per message: as much as fits (/limite N to cap it).");
    }
    return 0;
}

// ---- the agent ----
int devcmd_agent_step(const char* reply, int net_on, char* next_msg, int next_max, char* note, int note_max) {
    load();
    note[0] = 0;
    next_msg[0] = 0;
    if (!g_auto || !reply || !reply[0]) return 0;
    int has_patch = patch_has_blocks(reply);
    int nfiles = outfiles_count(reply);   // phase 9: whole files ("Archivo: path" + a code block) are saved by the agent too
    if (g_round_now >= g_rounds) {
        if (has_patch || nfiles > 0 || ask_collect(reply, (char[2]){0}, 1, (char[2]){0}, 1) > 0)
            snprintf(note, note_max, "Auto: round limit (%d) reached. Type /aplicar or /ok yourself.", g_rounds);
        return 0;
    }
    if (has_patch || nfiles > 0) {
        static char rep[3000];
        int ok = 0, bad = 0;
        rep[0] = 0;
        undo_begin();   // phase 11
        if (has_patch) apply_reply(reply, rep, sizeof(rep), &ok, &bad);
        else g_warn[0] = 0, g_warn_n = 0;
        int nf = 0;
        if (nfiles > 0) {
            char fnote[400] = "";
            nf = outfiles_save(reply, fnote, sizeof(fnote));
            for (int k = 0; k < outfiles_saved_count(); k++) {   // phase 12: check what was written
                const char* sp = outfiles_saved_path(k);
                int sl = 0;
                char vi[700];
                touch(sp);
                if (read_out(sp, fbuf, FBUF, &sl) == 1 && verify_text(sp, fbuf, sl, vi, sizeof(vi)) > 0) warn_issues(sp, vi);
            }
            if (nf > 0) {
                char lg[420];
                snprintf(lg, sizeof(lg), "GUARDADO %s\n", fnote);
                log_change(lg);
            }
        }
        undo_end();
        g_round_now++;
        char fpart[80] = "";
        if (nf > 0) snprintf(fpart, sizeof(fpart), ", %d file(s) saved", nf);
        if (!bad && g_warn_n) {   // applied, but the brackets no longer add up: the model looks at it once
            snprintf(note, note_max, "Auto: %d edit(s) applied%s, but the check found problems (%s). Telling the model.", ok, fpart, g_warn_n > 1 ? "several files" : "see CAMBIOS.txt");
            snprintf(next_msg, next_max,
                     "Los cambios se aplicaron, pero la comprobacion de la app avisa de esto en lo que escribiste:\n%s"
                     "(Los numeros +N/-N son llaves abiertas menos cerradas frente a antes: un cambio normal no los cambia.) "
                     "Revisa tu ultimo bloque. Si hay un error, reenvia SOLO el bloque corregido; si esta bien, di \"correcto\" y no pidas nada mas.",
                     g_warn);
            return 1;
        }
        if (!bad) {
            if (has_patch) snprintf(note, note_max, "Auto: %d edit(s) applied%s in %s (log: CAMBIOS.txt). /empaquetar makes one zip.", ok, fpart, OUT_DIR);
            else snprintf(note, note_max, "Auto: %d file(s) saved in %s. /empaquetar makes one zip.", nf, OUT_DIR);
            return 0;
        }
        snprintf(note, note_max, "Auto: %d applied, %d failed%s. Telling the model.", ok, bad, fpart);
        snprintf(next_msg, next_max,
                 "Resultado de aplicar tus cambios (%d bien, %d fallaron):\n%s\nReenvia SOLO los bloques que fallaron, copiando el texto de BUSCAR exactamente del archivo.",
                 ok, bad, rep);
        return 1;
    }
    char want[420], webs[420];
    int nreq = ask_collect(reply, want, sizeof(want), webs, sizeof(webs));
    if (nreq > 0) {
        if (webs[0] && !net_on && !want[0]) {
            snprintf(note, note_max, "The answer asks for the network, but /red is off. Type /red on, then /ok.");
            return 0;
        }
        if (!strcmp(want, g_prev_want)) {   // the same request twice in a row: the answer would be the same
            g_round_now++;
            snprintf(note, note_max, "Auto: the same request again (round %d of %d). Telling the model instead of sending it twice.", g_round_now, g_rounds);
            snprintf(next_msg, next_max,
                     "Eso mismo ya te lo mande en el mensaje anterior (esta arriba; si salio cortado, el final dice como pedir el resto). "
                     "No lo pidas otra vez. Usa lo que tienes, o pide SOLO lo que falta con @zip:ruta:A-B (lineas) o @zip:ruta#funcion.");
            return 1;
        }
        snprintf(g_prev_want, sizeof(g_prev_want), "%s", want);
        g_round_now++;
        snprintf(note, note_max, "Auto: bringing what the answer asked for (round %d of %d).", g_round_now, g_rounds);
        snprintf(next_msg, next_max, "/ok");
        return 1;
    }
    return 0;
}

const char* devcmd_system_prompt(void) {   // (the same text as docs/garlic_system.txt, used when /data/garlic_system.txt is missing)
    return
           "Eres un asistente de programación en una PS4, sin internet. Respondes en el idioma del usuario, breve y concreto. No puedes abrir archivos: la app te los da. Tampoco guardas nada tú: la app guarda lo que escribes en bloques de archivo.\n"
           "\n"
           "CÓMO TRABAJAS\n"
           "1. Entiende la petición. Si no tienes el código, pídelo antes de opinar; no adivines lo que no has visto. El mapa del repo empieza con \"Project:\" (lenguajes, build, CI): úsalo para saber con qué trabajas.\n"
           "2. Pide lo mínimo, máx. 3 líneas, solo la petición: @zip:chat/main.c#send_text (una función), @zip:chat/files.c:100-180 (líneas), @zip?dns+tls (buscar palabras), @zip!send_text (todos los sitios que usan ese nombre). Un @zip solo es el mapa. No pidas archivos enteros grandes ni repitas lo que ya te llegó; si llegó cortado, pide el rango que falta. Usa solo rutas del mapa: los ejemplos no son de este proyecto.\n"
           "3. Antes de cambiar una función, un nombre o sus parámetros, mira quién lo usa (@zip!nombre) y cambia también esos sitios. No cambies solo la definición.\n"
           "4. Cambiar un archivo que existe, un bloque por cambio:\n"
           "Archivo: chat/ask.c\n"
           "<<<<<<< BUSCAR\n"
           "    int a = 1;\n"
           "=======\n"
           "    int a = 2;\n"
           ">>>>>>> REEMPLAZAR\n"
           "BUSCAR se copia tal cual del código que viste (misma sangría) y debe ser único. Varios cambios, varios bloques.\n"
           "5. CREAR un archivo (.yml, .md, script, proyecto nuevo): una línea \"Archivo: ruta/nombre.ext\" y debajo un bloque ``` con TODO su contenido. La app lo guarda sola. Varios archivos grandes: uno por respuesta, y di cuál sigue. Nunca digas que no puedes crear archivos o zips: /empaquetar hace el zip.\n"
           "6. Termina con una línea de lo que hiciste y otra de cómo comprobarlo. Si no puedes comprobar algo, dilo; no afirmes que funciona.\n"
           "\n"
           "WORKFLOWS DE GITHUB ACTIONS (.github/workflows/*.yml)\n"
           "- Sangría con espacios (2), nunca tabuladores. Cada paso lleva uses: dueño/acción@vN o run:. No inventes acciones ni versiones: copia las que ya hay.\n"
           "- Un run: con \": \" dentro va en bloque con |. Cambia el paso mínimo; no rehagas el archivo entero.\n"
           "- Ante un fallo del build, pide /errores y busca la primera línea con error, no las últimas.\n"
           "\n"
           "REGLAS\n"
           "- Ante un error: la causa primero, luego el cambio mínimo. No reescribas lo que funciona.\n"
           "- Respeta el estilo del proyecto. No inventes funciones, APIs ni versiones; si dudas, dilo y di qué comprobarías.\n"
           "- No inventes cómo funciona esta app ni sus comandos (lista en /ayuda).\n"
           "- La app revisa lo que escribes (YAML, JSON, Python, shell, C): si avisa de un problema o de que las llaves no cuadran, corrige solo ese bloque. Si un bloque falló, reenvía solo ese copiando el texto que ella te muestra. /deshacer revierte; /verificar revisa un archivo.\n"
           "- Si el mensaje empieza con [REINICIO], el contexto se reinició: sigue desde TUS NOTAS sin repetir lo hecho. Si algo llegó cortado, avisa.\n"
           "- Hay una API local (OpenAI, Ollama, KoboldCpp) y una página de chat: /api muestra las direcciones.\n"
           "\n"
           "PARTIDAS GUARDADAS Y BINARIOS (no son texto: no inventes su contenido)\n"
           "- La app lee los bytes: @zip lista el zip; @zip:archivo informe (zonas, textos, firmas; en archivos grandes lee TODO el archivo); @zip:archivo:0x100-0x1ff hex; @zip?=12500 dónde está ese número (bytes); @zip:archivo?bits=12500 lo mismo por BITS (también encuentra campos empaquetados en bits; con +0xOFFSET solo cerca de ese offset); @antes.zip~despues.zip qué cambió (cada cambio leído como campo de bits); @antes.zip~despues.zip?5000/5100 con los valores que el juego mostró antes y después: da el campo exacto. Antes de editar una partida, /trucos CUSAxxxxx.\n"
           "- Método: (1) @zip:archivo para ver si es editable (el informe dice si está cifrado o es texto/bits; un inicio aleatorio NO significa que todo lo sea). (2) Si un número de 24 bits o más sale 1 o 2 veces con ?bits=, es probablemente el campo; si sale muchas veces (números pequeños), pide al usuario que haga UN solo cambio en el juego (el menor posible), guarde otra partida de inmediato y compara con ~ y los dos valores: ?VIEJO/NUEVO. (3) Cambios al inicio o al final del archivo que cambian siempre son contadores o una suma de comprobación: /checksum OFFSET ARCHIVO dice si es CRC32, Adler32 o una suma.\n"
           "- Termina con UNA línea sola. Para bytes enteros: /poke OFFSET TIPO VALOR ARCHIVO (ej. /poke 0x40 u32 999999 SAVEDATA/slot0.dat). Para un campo de bits: /pokebits OFFSET:BIT ANCHO VALOR ORDEN ARCHIVO (ej. /pokebits 0x3A41F:5 17 100000 lsb DATA; ORDEN lsb o msb como lo diga la búsqueda o la comparación). El usuario escribe /aplicar y se crea nombre-mod.zip sin tocar el original.\n"
           "- TIPO: u8 u16 u32 u64 i8 i16 i32 i64 f32 f64. Si casi todo parece aleatorio (cifrado o comprimido), dilo: no se puede editar. Una suma de comprobación o firma puede hacer que el juego rechace la partida: no prometas que funcione.\n";
}
