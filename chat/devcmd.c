#include "devcmd.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ask.h"
#include "files.h"
#include "outfiles.h"
#include "patch.h"
#include "repomap.h"
#include "zipwrite.h"
#include "progress.h"

#define FBUF (200 * 1024)
#define MAX_BLOCKS 16
#define PEND_MAX 24000

static int g_auto, g_rounds = 4, g_limit, g_round_now;
static int g_loaded;
static char g_pend[PEND_MAX];
static int g_pend_n;
static RepoSeenSet g_seen;
static char g_prev_want[420];   // phase 8: what the model asked for in the last round (a repeat is not brought again)
static char g_warn[700];        // phase 8: edits that changed the balance of () {} [] (the agent tells the model)
static int g_warn_n;

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
                                    "empaquetar", "zip", "auto", "limite", "limit", NULL};

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
            if (exists) patch_balance(rel, fbuf, len, bal0);
            int rc = patch_apply(fbuf, &len, FBUF + 32 * 1024 - 1, b, exists, &line, &how, hint, sizeof(hint));
            if (rc == 0 && outfiles_write(rel, fbuf, len) == 0) {
                snprintf(what, sizeof(what), "OK %s (line %d, %s match)", rel, line, how);
                (*ok)++;
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

// ---- commands ----
int devcmd_run(const char* text, const char* last_ai, char* note, int nm) {
    load();
    const char* args;
    int c = parse(text, &args);
    note[0] = 0;
    if (c <= 1) {   // /proyecto
        if (!args[0]) {
            snprintf(note, nm, files_project()[0] ? "Project: %s. Change it with /proyecto name.zip" : "No project. Put a repo .zip in /data/garlic_files and type /proyecto name (or /gh owner/repo zip with /red on).", files_project());
            return 0;
        }
        char name[100], sugg[300];
        if (!files_resolve_zip(args, name, sizeof(name), sugg, sizeof(sugg))) {
            snprintf(note, nm, "No such zip: %s", sugg);
            return 0;
        }
        files_set_project(name);
        save();
        snprintf(note, nm, "Project: %s. Now \"@zip\" means it: ask for the map with /mapa, or search with /grep words.", name);
    } else if (c <= 3) {   // /mapa
        if (!files_project()[0]) {
            snprintf(note, nm, "No project. Use /proyecto name.zip first.");
            return 0;
        }
        char zp[200];
        static char map[PEND_MAX - 200];
        int ns = 0;
        snprintf(zp, sizeof(zp), FILES_DIR "/%s", files_project());
        int n = repomap_build(zp, map, 12000, &ns);
        if (n < 0) snprintf(note, nm, "Cannot read %s", files_project());
        else {
            pend("[Repo map]", map, n);
            snprintf(note, nm, "Map of %s: %d source files, %d bytes. It goes with your next message.", files_project(), ns, n);
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
        int ok, bad;
        int nb = last_ai && last_ai[0] ? apply_reply(last_ai, rep, sizeof(rep), &ok, &bad) : 0;
        if (nb == 0) snprintf(note, nm, "The last answer has no edit blocks (BUSCAR / REEMPLAZAR).");
        else snprintf(note, nm, "%d applied, %d failed (files in /data/garlic_files/out, log in CAMBIOS.txt):\n%.1000s", ok, bad, rep);
    } else if (c <= 9) {   // /empaquetar
        char name[80] = "cambios", zp[200];
        if (args[0]) {
            int k = 0;
            for (; args[k] && args[k] != ' ' && k < 70; k++) name[k] = args[k] == '/' ? '_' : args[k];
            name[k] = 0;
            if (k > 4 && !strcmp(name + k - 4, ".zip")) name[k - 4] = 0;
        }
        snprintf(zp, sizeof(zp), FILES_DIR "/%s.zip", name);
        long bytes = 0;
        int n = zipwrite_dir(OUT_DIR, zp, &bytes);
        if (n == -2) snprintf(note, nm, "Nothing to pack: /data/garlic_files/out is empty.");
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
    if (g_round_now >= g_rounds) {
        if (patch_has_blocks(reply) || ask_collect(reply, (char[2]){0}, 1, (char[2]){0}, 1) > 0)
            snprintf(note, note_max, "Auto: round limit (%d) reached. Type /aplicar or /ok yourself.", g_rounds);
        return 0;
    }
    if (patch_has_blocks(reply)) {
        static char rep[3000];
        int ok, bad;
        apply_reply(reply, rep, sizeof(rep), &ok, &bad);
        g_round_now++;
        if (!bad && g_warn_n) {   // applied, but the brackets no longer add up: the model looks at it once
            snprintf(note, note_max, "Auto: %d edit(s) applied, but the brackets changed (%s). Telling the model.", ok, g_warn_n > 1 ? "several files" : "see CAMBIOS.txt");
            snprintf(next_msg, next_max,
                     "Los cambios se aplicaron, pero la comprobacion de llaves avisa (abiertos menos cerrados, frente a antes):\n%s"
                     "Un cambio normal no cambia esos numeros. Revisa tu ultimo bloque: ¿falta o sobra un }, ) o ]? "
                     "Si hay un error, reenvia SOLO el bloque corregido; si esta bien, di \"correcto\" y no pidas nada mas.",
                     g_warn);
            return 1;
        }
        if (!bad) {
            snprintf(note, note_max, "Auto: %d edit(s) applied in /data/garlic_files/out (log: CAMBIOS.txt). /empaquetar makes one zip.", ok);
            return 0;
        }
        snprintf(note, note_max, "Auto: %d applied, %d failed. Telling the model.", ok, bad);
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

const char* devcmd_system_prompt(void) {
    return
           "Eres un asistente de programación en una PS4, sin internet. Respondes en el idioma del usuario, breve y concreto. No puedes abrir archivos: la app te los da.\n"
           "\n"
           "CÓMO TRABAJAS\n"
           "1. Entiende la petición. Si no tienes el código, pídelo antes de opinar; no adivines lo que no has visto.\n"
           "2. Pide lo mínimo, máx. 3 líneas, solo la petición y nada más: por ejemplo @zip:chat/main.c#send_text, @zip:chat/files.c:100-180 o @zip?dns+tls. Un @zip solo es el mapa del repo. No pidas archivos enteros grandes ni repitas lo que ya te llegó; si llegó cortado, pide el rango que falta.\n"
           "3. Usa solo rutas que aparezcan en el mapa; los ejemplos de arriba no son archivos de este proyecto.\n"
           "4. Cambia solo lo necesario, un bloque por cambio:\n"
           "Archivo: chat/ask.c\n"
           "<<<<<<< BUSCAR\n"
           "    int a = 1;\n"
           "=======\n"
           "    int a = 2;\n"
           ">>>>>>> REEMPLAZAR\n"
           "BUSCAR se copia tal cual del código que viste (misma sangría) y debe ser único. Archivo nuevo: BUSCAR vacío. Varios cambios, varios bloques.\n"
           "5. Después de los bloques: una línea con lo que cambiaste y otra con cómo comprobarlo. Si el cambio exige tocar otro archivo (cabecera, llamadas), pídelo o cámbialo también.\n"
           "\n"
           "REGLAS\n"
           "- Ante un error: la causa primero, luego el cambio mínimo. No reescribas lo que funciona.\n"
           "- Respeta el estilo del proyecto. No inventes funciones, APIs ni versiones; si dudas, dilo y di qué comprobarías.\n"
           "- Si la app dice que un bloque falló, reenvía solo ese, copiando el texto que ella te muestra. Si avisa de que las llaves no cuadran, revisa tu último bloque.\n"
           "- Si el mensaje empieza con [REINICIO], el contexto se reinició: sigue desde TUS NOTAS sin repetir lo hecho.\n"
           "- Código nuevo sin repo: un solo bloque ``` completo y ejecutable, con el lenguaje indicado.\n"
           "- Si algo llegó cortado, avisa.\n";
}
