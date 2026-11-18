// Projects (see workspace.h).
#include "workspace.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_name[48];
static int g_loaded;
static char g_buf[300];   // the paths handed out by ws_out_dir / ws_progress_file
static char g_zip[100];

static void mkdir_p(const char* path) {
    char p[300];
    snprintf(p, sizeof(p), "%s", path);
    for (char* c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0777);
            *c = '/';
        }
    mkdir(p, 0777);
}

int ws_valid_name(const char* name) {
    int n = (int)strlen(name);
    if (n < 1 || n > 40 || name[0] == '.') return 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return !strstr(name, "..");
}

static int read_text(const char* path, char* out, int max) {
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    int n = read(f, out, max - 1);
    close(f);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

static int write_text(const char* path, const char* text) {
    int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f < 0) return -1;
    int n = (int)strlen(text), done = 0;
    while (done < n) {
        int w = write(f, text + done, n - done);
        if (w <= 0) {
            close(f);
            return -1;
        }
        done += w;
    }
    close(f);
    return 0;
}

static void proj_path(const char* name, const char* file, char* out, int max) {
    snprintf(out, max, WS_ROOT "/%s%s%s", name, file[0] ? "/" : "", file);
}

int ws_exists(const char* name) {
    if (!ws_valid_name(name)) return 0;
    char p[300];
    proj_path(name, "", p, sizeof(p));
    DIR* d = opendir(p);
    if (!d) return 0;
    closedir(d);
    return 1;
}

static void load_zip(void) {
    g_zip[0] = 0;
    if (!g_name[0]) return;
    char p[300], t[200];
    proj_path(g_name, "proyecto.txt", p, sizeof(p));
    if (read_text(p, t, sizeof(t)) <= 0) return;
    for (char* l = strtok(t, "\n"); l; l = strtok(NULL, "\n"))
        if (!strncmp(l, "zip=", 4)) {
            snprintf(g_zip, sizeof(g_zip), "%s", l + 4);
            int k = (int)strlen(g_zip);
            while (k > 0 && (g_zip[k - 1] == '\r' || g_zip[k - 1] == ' ')) g_zip[--k] = 0;
        }
}

static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char t[100];
    if (read_text(WS_STATE_FILE, t, sizeof(t)) > 0) {
        t[strcspn(t, "\r\n")] = 0;
        if (ws_valid_name(t) && ws_exists(t)) snprintf(g_name, sizeof(g_name), "%s", t);
    }
    load_zip();
}

static void save_state(void) { write_text(WS_STATE_FILE, g_name[0] ? g_name : ""); }

const char* ws_name(void) {
    load();
    return g_name;
}

const char* ws_out_dir(void) {
    load();
    if (g_name[0]) snprintf(g_buf, sizeof(g_buf), WS_OUT_ROOT "/%s", g_name);
    else snprintf(g_buf, sizeof(g_buf), "%s", WS_OUT_ROOT);
    return g_buf;
}

static char g_prog[300];
const char* ws_progress_file(void) {
    load();
    if (g_name[0]) proj_path(g_name, "progress.txt", g_prog, sizeof(g_prog));
    else snprintf(g_prog, sizeof(g_prog), "%s", WS_LEGACY_PROGRESS);
    return g_prog;
}

const char* ws_base_zip(void) {
    load();
    return g_zip;
}

static int write_meta(const char* name, const char* zip) {
    char p[300], t[200];
    proj_path(name, "proyecto.txt", p, sizeof(p));
    snprintf(t, sizeof(t), "zip=%s\n", zip ? zip : "");
    return write_text(p, t);
}

int ws_set_base_zip(const char* zip) {
    load();
    if (!g_name[0]) return -1;
    snprintf(g_zip, sizeof(g_zip), "%s", zip ? zip : "");
    return write_meta(g_name, g_zip);
}

int ws_create(const char* name, const char* zip) {
    load();
    if (!ws_valid_name(name)) return -1;
    if (ws_exists(name)) return -2;
    char p[300];
    mkdir_p(WS_ROOT);
    proj_path(name, "", p, sizeof(p));
    mkdir_p(p);
    snprintf(p, sizeof(p), WS_OUT_ROOT "/%s", name);
    mkdir_p(p);
    if (write_meta(name, zip) != 0 || !ws_exists(name)) return -3;
    snprintf(g_name, sizeof(g_name), "%s", name);
    snprintf(g_zip, sizeof(g_zip), "%s", zip ? zip : "");
    save_state();
    return 0;
}

int ws_switch(const char* name) {
    load();
    if (!ws_exists(name)) return -1;
    char p[300];
    snprintf(p, sizeof(p), WS_OUT_ROOT "/%s", name);
    mkdir_p(p);   // (it may have been deleted over FTP)
    snprintf(g_name, sizeof(g_name), "%s", name);
    load_zip();
    save_state();
    return 0;
}

void ws_leave(void) {
    load();
    g_name[0] = 0;
    g_zip[0] = 0;
    save_state();
}

// Files and bytes under dir (down to 6 levels).
static void count_dir(const char* dir, int depth, int* files, long* bytes) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[400];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6) count_dir(p, depth + 1, files, bytes);
        } else {
            (*files)++;
            *bytes += st.st_size;
        }
    }
    closedir(d);
}

int ws_list(char* out, int max) {
    load();
    out[0] = 0;
    DIR* d = opendir(WS_ROOT);
    if (!d) return 0;
    int n = 0, o = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && o < max - 120) {
        if (e->d_name[0] == '.' || !ws_valid_name(e->d_name) || !ws_exists(e->d_name)) continue;
        char dir[300];
        snprintf(dir, sizeof(dir), WS_OUT_ROOT "/%s", e->d_name);
        int files = 0;
        long bytes = 0;
        count_dir(dir, 0, &files, &bytes);
        char z[100] = "", t[200], p[300];
        proj_path(e->d_name, "proyecto.txt", p, sizeof(p));
        if (read_text(p, t, sizeof(t)) > 0) {
            const char* zp = strstr(t, "zip=");
            if (zp) {
                snprintf(z, sizeof(z), "%s", zp + 4);
                z[strcspn(z, "\r\n")] = 0;
            }
        }
        o += snprintf(out + o, max - o, "%s %s (%d files, %ld KB)%s%s\n", !strcmp(e->d_name, g_name) ? "*" : "-", e->d_name, files,
                      (bytes + 1023) / 1024, z[0] ? " base: " : "", z);
        n++;
    }
    closedir(d);
    return n;
}

int ws_set_instructions(const char* text) {
    load();
    if (!g_name[0]) return -1;
    char p[300];
    proj_path(g_name, "INSTRUCCIONES.md", p, sizeof(p));
    if (!text || !text[0]) {
        unlink(p);
        return 0;
    }
    return write_text(p, text);
}

int ws_instructions(char* out, int max) {
    load();
    out[0] = 0;
    if (!g_name[0]) return 0;
    char p[300];
    proj_path(g_name, "INSTRUCCIONES.md", p, sizeof(p));
    int n = read_text(p, out, max);
    if (n < 0) return 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
    return n;
}

// One line per file of dir: "@out/<name>/<rel> <bytes>".
static void tree_lines(const char* dir, const char* rel, int depth, char* out, int max, int* o, int* more) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[400], r[300];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        snprintf(r, sizeof(r), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6) tree_lines(p, r, depth + 1, out, max, o, more);
        } else if (*o < max - 160) {
            *o += snprintf(out + *o, max - *o, "@out/%s/%s %ld\n", g_name, r, (long)st.st_size);
        } else *more = 1;
    }
    closedir(d);
}

int ws_context_text(char* out, int max) {
    load();
    out[0] = 0;
    if (!g_name[0]) return 0;
    int o = snprintf(out, max, "[Proyecto: %s]\n", g_name);
    char ins[1600];
    if (ws_instructions(ins, sizeof(ins)) > 0 && o < max - 100) o += snprintf(out + o, max - o, "Instrucciones del proyecto:\n%s\n", ins);
    if (g_zip[0] && o < max - 100) o += snprintf(out + o, max - o, "Repo base: %s (se pide con @zip:ruta)\n", g_zip);
    int more = 0, start = o;
    if (o < max - 100) o += snprintf(out + o, max - o, "Archivos del proyecto (ruta que se pide, bytes):\n");
    int before = o;
    tree_lines(ws_out_dir(), "", 0, out, max, &o, &more);
    if (o == before) o = start + snprintf(out + start, max - start, "Aun no hay archivos en el proyecto.\n");
    if (more && o < max - 40) o += snprintf(out + o, max - o, "... (hay mas archivos)\n");
    return o;
}

int ws_prompt_block(char* out, int max) {
    load();
    out[0] = 0;
    if (!g_name[0]) return 0;
    int o = snprintf(out, max,
                     "[PROYECTO: %s]\n"
                     "La app guarda sola lo que escribas en este proyecto: nunca digas que no puedes crear archivos, carpetas o zips (/empaquetar hace el zip). "
                     "Archivo entero (nuevo o reescrito): una linea \"Archivo: ruta/relativa.ext\" y debajo un bloque ``` con TODO su contenido. "
                     "Cambio parcial de uno que ya existe: BUSCAR/REEMPLAZAR. Lo ya creado se pide con @out/%s/ruta.\n",
                     g_name, g_name);
    char ins[700];
    if (ws_instructions(ins, sizeof(ins)) > 0 && o < max - 40) o += snprintf(out + o, max - o, "Instrucciones del proyecto: %s\n", ins);
    return o;
}
