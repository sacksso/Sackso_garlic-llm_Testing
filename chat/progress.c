// Progress of a piece of work (see progress.h).
#include "progress.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char g_goal[640];    // the task: the first real message
static char g_last[320];    // the latest real message, when it is not the task
static char g_edits[1000];  // "- path (line N, exact match)" per applied block, the oldest dropped first
static char g_fail[200];    // the last edit that failed (cleared by a round without failures)
static char g_asked[460];   // the @files of the model's last request
static char g_notes[1600];  // what the model wrote at the checkpoint

static void copy_clean(char* dst, int cap, const char* src, int drop_at_words) {
    // Copies src into dst (at most cap-1 bytes, never half a UTF-8 character). Lines are trimmed. With drop_at_words
    // the @words are left out; otherwise an @ becomes "(@)" so it can never bring a file in.
    int n = 0;
    while (*src == ' ' || *src == '\n' || *src == '\t' || *src == '\r') src++;
    while (*src && n < cap - 4) {
        if (*src == '@') {
            if (drop_at_words) {
                while (*src && !isspace((unsigned char)*src)) src++;
                continue;
            }
            dst[n++] = '(', dst[n++] = '@', dst[n++] = ')';
            src++;
            continue;
        }
        if (*src == '\r') {
            src++;
            continue;
        }
        dst[n++] = *src++;
    }
    while (n > 0 && ((unsigned char)dst[n] & 0xC0) == 0x80 && *src) n--;   // (cut inside a character: back up)
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\n' || dst[n - 1] == '\t')) n--;
    dst[n] = 0;
}

static int starts_ci(const char* s, const char* p) {
    for (; *p; s++, p++)
        if (tolower((unsigned char)*s) != *p) return 0;
    return 1;
}

int progress_is_cmd(const char* text) {
    while (*text == ' ') text++;
    int n = (int)strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\n')) n--;
    char t[24];
    if (n <= 0 || n >= (int)sizeof(t)) return 0;
    memcpy(t, text, n);
    t[n] = 0;
    if (starts_ci(t, "/progreso") && t[9] == 0) return 1;
    if (starts_ci(t, "/checkpoint") && t[11] == 0) return 1;
    if (starts_ci(t, "/retomar") && t[8] == 0) return 2;
    if (starts_ci(t, "/resume") && t[7] == 0) return 2;
    return 0;
}

void progress_goal(const char* t) {
    while (*t == ' ' || *t == '\n' || *t == '\t') t++;
    if (*t == '/') return;
    int real = 0;   // characters outside the @words: "ok", "si" or a bare list of files is not a request
    for (const char* p = t; *p;) {
        if (*p == '@') {
            while (*p && !isspace((unsigned char)*p)) p++;
            continue;
        }
        if (!isspace((unsigned char)*p)) real++;
        p++;
    }
    if (real < 6) return;
    if (!g_goal[0]) {
        copy_clean(g_goal, sizeof(g_goal), t, 1);
        return;
    }
    char tmp[sizeof(g_last)];
    copy_clean(tmp, sizeof(tmp), t, 1);
    if (tmp[0] && strcmp(tmp, g_goal)) strcpy(g_last, tmp);
}

void progress_edits(int ok, int bad, const char* report) {
    (void)ok;
    if (!report) return;
    char fail[sizeof(g_fail)] = "";
    for (const char* l = report; *l;) {
        const char* e = strchr(l, '\n');
        int len = e ? (int)(e - l) : (int)strlen(l);
        if (len > 4 && !strncmp(l, "OK ", 3)) {
            char one[120];
            int c = len - 3 < (int)sizeof(one) - 4 ? len - 3 : (int)sizeof(one) - 4;
            memcpy(one, l + 3, c);
            one[c] = 0;
            char* par = strchr(one, '(');   // "path (line N, exact match)" -> "path (line N)"
            if (par) {
                char* comma = strchr(par, ',');
                if (comma) strcpy(comma, ")");
            }
            int el = (int)strlen(g_edits), add = (int)strlen(one) + 3;
            while (el + add >= (int)sizeof(g_edits) && el > 0) {   // drop the oldest line
                char* nl = strchr(g_edits, '\n');
                if (!nl) {
                    g_edits[0] = 0;
                    el = 0;
                    break;
                }
                memmove(g_edits, nl + 1, strlen(nl + 1) + 1);
                el = (int)strlen(g_edits);
            }
            snprintf(g_edits + el, sizeof(g_edits) - el, "- %s\n", one);
        } else if (len > 6 && !strncmp(l, "FALLO ", 6) && !fail[0]) {
            int c = len < (int)sizeof(fail) - 4 ? len : (int)sizeof(fail) - 4;
            memcpy(fail, l, c);
            fail[c] = 0;
            char* nl = strchr(fail, '\n');
            if (nl) *nl = 0;
        }
        if (!e) break;
        l = e + 1;
    }
    if (bad > 0) snprintf(g_fail, sizeof(g_fail), "%s", fail);
    else g_fail[0] = 0;
}

void progress_asked(const char* want) {
    g_asked[0] = 0;
    if (!want) return;
    int n = 0;
    for (const char* p = want; *p && n < (int)sizeof(g_asked) - 2; p++)
        if (*p == '\n') g_asked[n++] = ' ';
        else g_asked[n++] = *p;
    g_asked[n] = 0;
}

void progress_notes(const char* reply) {
    if (!reply) return;
    copy_clean(g_notes, sizeof(g_notes), reply, 0);
}

int progress_has(void) { return g_goal[0] || g_notes[0] || g_edits[0]; }

void progress_clear(void) { g_goal[0] = g_last[0] = g_edits[0] = g_fail[0] = g_asked[0] = g_notes[0] = 0; }

int progress_save(void) {
    FILE* f = fopen(PROGRESS_FILE, "w");
    if (!f) return -1;
    fprintf(f, "GARLIC-PROGRESO 1\n## PETICION\n%s\n## ULTIMO\n%s\n## CAMBIOS\n%s## FALLO\n%s\n## PEDIDOS\n%s\n## NOTAS\n%s\n", g_goal,
            g_last, g_edits, g_fail, g_asked, g_notes);
    fclose(f);
    return 0;
}

int progress_load(void) {
    FILE* f = fopen(PROGRESS_FILE, "r");
    if (!f) return 0;
    static char buf[6000];
    int n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    if (strncmp(buf, "GARLIC-PROGRESO", 15)) return 0;
    progress_clear();
    char* dst = NULL;
    int cap = 0, used = 0;
    for (char* l = buf; *l;) {
        char* e = strchr(l, '\n');
        int len = e ? (int)(e - l) : (int)strlen(l);
        if (!strncmp(l, "## ", 3)) {
            const char* name = l + 3;
            dst = !strncmp(name, "PETICION", 8) ? g_goal : !strncmp(name, "ULTIMO", 6) ? g_last : !strncmp(name, "CAMBIOS", 7) ? g_edits
                  : !strncmp(name, "FALLO", 5) ? g_fail : !strncmp(name, "PEDIDOS", 7) ? g_asked : !strncmp(name, "NOTAS", 5) ? g_notes : NULL;
            cap = dst == g_goal ? (int)sizeof(g_goal) : dst == g_last ? (int)sizeof(g_last) : dst == g_edits ? (int)sizeof(g_edits)
                  : dst == g_fail ? (int)sizeof(g_fail) : dst == g_asked ? (int)sizeof(g_asked) : (int)sizeof(g_notes);
            used = 0;
        } else if (dst && used < cap - 2) {
            int c = len < cap - 2 - used ? len : cap - 2 - used;
            memcpy(dst + used, l, c);
            used += c;
            if (e) dst[used++] = '\n';
            dst[used] = 0;
        }
        if (!e) break;
        l = e + 1;
    }
    char* singles[] = {g_goal, g_last, g_fail, g_asked, g_notes};
    for (int i = 0; i < 5; i++) {
        int l = (int)strlen(singles[i]);
        while (l > 0 && singles[i][l - 1] == '\n') singles[i][--l] = 0;
    }
    return progress_has();
}

int progress_resume_text(char* out, int max, const char* report, int with_asked) {
    if (!progress_has()) {
        if (max > 0) out[0] = 0;
        return 0;
    }
    int n = 0;
#define ADD(...) do { if (n < max - 1) n += snprintf(out + n, max - n, __VA_ARGS__); } while (0)
    ADD("[REINICIO] El contexto anterior se llenó y empezó uno nuevo. Esto es lo que quedó guardado de tu trabajo.\n");
    if (g_goal[0]) ADD("TAREA: %s\n", g_goal);
    if (g_last[0]) ADD("ULTIMO MENSAJE DEL USUARIO: %s\n", g_last);
    if (g_edits[0]) ADD("CAMBIOS YA APLICADOS (no los repitas):\n%s", g_edits);
    if (g_fail[0]) ADD("ULTIMO CAMBIO QUE FALLO: %s\n", g_fail);
    if (g_notes[0]) ADD("TUS NOTAS:\n%s\n", g_notes);
    if (report && report[0]) {   // (an @ in it must not bring a file in: code often has them)
        ADD("PENDIENTE DE LA APP:\n");
        for (const char* p = report; *p && n < max - 4; p++) {
            if (*p == '@') ADD("(@)");
            else out[n++] = *p;
        }
        out[n < max ? n : max - 1] = 0;
        ADD("\n");
    }
    ADD("Sigue desde el siguiente paso de tus notas. No releas lo ya hecho: si necesitas código, pídelo.\n");
    if (with_asked && g_asked[0]) ADD("Lo último que pediste te llega ahora:\n%s\n", g_asked);
#undef ADD
    if (n >= max) n = max - 1;
    out[n] = 0;
    return n;
}

const char* progress_checkpoint_prompt(void) {
    return "PUNTO DE CONTROL: el contexto está casi lleno y se va a reiniciar. Escribe ahora tus notas para retomar el trabajo "
           "desde el mismo punto. Sin pedir archivos, sin usar @ y sin nada más que esto (máximo 150 palabras):\n"
           "OBJETIVO: lo que se quiere lograr\n"
           "HECHO: lo que ya está resuelto o cambiado\n"
           "PENDIENTE: el siguiente paso concreto\n"
           "CODIGO CLAVE: archivos y funciones con los que trabajas (ruta#funcion)\n"
           "DATOS: decisiones, nombres y errores que no se pueden perder";
}
