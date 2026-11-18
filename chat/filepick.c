#include "filepick.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "repomap.h"
#include "zipread.h"

static int ends_zip(const char* s) {
    int l = (int)strlen(s);
    return l > 4 && (s[l - 4] == '.') && (s[l - 3] | 32) == 'z' && (s[l - 2] | 32) == 'i' && (s[l - 1] | 32) == 'p';
}

static int cmp_ent(const void* a, const void* b) {
    const FpEnt *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static int add_ent(FilePick* f, const char* name, int is_dir, long size) {
    if (!name[0]) return 0;
    if (f->n >= FP_MAX) {
        f->truncated = 1;
        return 0;
    }
    FpEnt* e = &f->ent[f->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->is_dir = is_dir;
    e->is_zip = !is_dir && !f->zip[0] && ends_zip(name);
    e->size = size;
    return 1;
}

static void load(FilePick* f) {
    f->n = f->cur = f->top = f->truncated = 0;
    if (f->zip[0]) {
        char zp[300];
        snprintf(zp, sizeof(zp), "%s/%s", FILES_DIR, f->zip);
        const ZipEnt* ents;
        int total;
        int n = zip_open_dir(zp, &ents, &total);
        if (n < 0) {
            snprintf(f->msg, sizeof(f->msg), "No se pudo abrir el zip");
            f->zip[0] = f->zdir[0] = 0;
            f->zroot = 0;
        } else {
            f->zroot = repo_root_len(ents, n);
            if (!f->zdir[0] && f->zroot) snprintf(f->zdir, sizeof(f->zdir), "%.*s", f->zroot, ents[0].name);
            int zl = (int)strlen(f->zdir);
            for (int i = 0; i < n; i++) {
                const char* nm = ents[i].name;
                if (strncmp(nm, f->zdir, zl)) continue;
                const char* rest = nm + zl;
                const char* sl = strchr(rest, '/');
                char seg[FP_NAME];
                int len = sl ? (int)(sl - rest) : (int)strlen(rest);
                if (len <= 0) continue;
                if (len >= FP_NAME) len = FP_NAME - 1;
                memcpy(seg, rest, len);
                seg[len] = 0;
                int dup = 0;
                for (int k = 0; k < f->n && !dup; k++) dup = f->ent[k].is_dir == (sl != NULL) && !strcmp(f->ent[k].name, seg);
                if (!dup) add_ent(f, seg, sl != NULL, sl ? 0 : (long)ents[i].usize);
            }
            qsort(f->ent, f->n, sizeof(FpEnt), cmp_ent);
            return;
        }
    }
    char dir[300];
    snprintf(dir, sizeof(dir), "%s%s%s", FILES_DIR, f->dir[0] ? "/" : "", f->dir);
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* nm = e->d_name;
        if (nm[0] == '.') continue;
        char p[400];
        snprintf(p, sizeof(p), "%s/%s", dir, nm);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        add_ent(f, nm, S_ISDIR(st.st_mode), (long)st.st_size);
    }
    closedir(d);
    qsort(f->ent, f->n, sizeof(FpEnt), cmp_ent);
}

void fp_open(FilePick* f) {
    memset(f, 0, sizeof(*f));
    load(f);
}

void fp_move(FilePick* f, int d) {
    if (f->n == 0) return;
    f->cur += d;
    if (f->cur < 0) f->cur = 0;
    if (f->cur >= f->n) f->cur = f->n - 1;
    if (f->cur < f->top) f->top = f->cur;
    if (f->cur >= f->top + FP_ROWS) f->top = f->cur - FP_ROWS + 1;
}

// The "@..." token of entry i, or "" when it cannot be written as one (spaces and so on).
static int token(const FilePick* f, int i, char* out, int max) {
    const char* nm = f->ent[i].name;
    for (const char* c = nm; *c; c++) {
        unsigned char u = (unsigned char)*c;
        int ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-';
        if (!ok) {
            out[0] = 0;
            return 0;
        }
    }
    if (f->zip[0]) {
        for (const char* c = f->zip; *c; c++) {
            unsigned char u = (unsigned char)*c;
            int ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-' || u == '/';
            if (!ok) {
                out[0] = 0;
                return 0;
            }
        }
        const char* sub = f->zdir + (f->zroot < (int)strlen(f->zdir) ? f->zroot : (int)strlen(f->zdir));
        for (const char* c = sub; *c; c++) {
            unsigned char u = (unsigned char)*c;
            if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-' || u == '/')) {
                out[0] = 0;
                return 0;
            }
        }
        snprintf(out, max, "@%s:%s%s", f->zip, sub, nm);
    } else {
        for (const char* c = f->dir; *c; c++) {
            unsigned char u = (unsigned char)*c;
            if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-' || u == '/')) {
                out[0] = 0;
                return 0;
            }
        }
        snprintf(out, max, "@%s%s%s", f->dir, f->dir[0] ? "/" : "", nm);
    }
    return 1;
}

int fp_is_picked(const FilePick* f, int i) {
    char t[140];
    if (i < 0 || i >= f->n || !token(f, i, t, sizeof(t))) return 0;
    for (int k = 0; k < f->npicked; k++)
        if (!strcmp(f->picked[k], t)) return 1;
    return 0;
}

void fp_toggle(FilePick* f) {
    f->msg[0] = 0;
    if (f->cur < 0 || f->cur >= f->n) return;
    const FpEnt* e = &f->ent[f->cur];
    if (e->is_dir) {
        snprintf(f->msg, sizeof(f->msg), "Las carpetas se abren; marca los archivos de dentro");
        return;
    }
    char t[140];
    if (!token(f, f->cur, t, sizeof(t))) {
        snprintf(f->msg, sizeof(f->msg), "Ese nombre tiene espacios o simbolos: renombralo para adjuntarlo");
        return;
    }
    for (int k = 0; k < f->npicked; k++)
        if (!strcmp(f->picked[k], t)) {
            memmove(f->picked[k], f->picked[k + 1], (size_t)(f->npicked - k - 1) * sizeof(f->picked[0]));
            f->npicked--;
            return;
        }
    if (f->npicked >= FP_PICK_MAX) {
        snprintf(f->msg, sizeof(f->msg), "Maximo %d archivos a la vez", FP_PICK_MAX);
        return;
    }
    snprintf(f->picked[f->npicked++], sizeof(f->picked[0]), "%s", t);
}

void fp_enter(FilePick* f) {
    f->msg[0] = 0;
    if (f->cur < 0 || f->cur >= f->n) return;
    const FpEnt e = f->ent[f->cur];
    if (e.is_zip) {
        snprintf(f->zip, sizeof(f->zip), "%s%s%s", f->dir, f->dir[0] ? "/" : "", e.name);
        f->zdir[0] = 0;
        load(f);
    } else if (e.is_dir) {
        if (f->zip[0]) {
            int l = (int)strlen(f->zdir);
            snprintf(f->zdir + l, sizeof(f->zdir) - l, "%s/", e.name);
        } else {
            int l = (int)strlen(f->dir);
            snprintf(f->dir + l, sizeof(f->dir) - l, "%s%s", l ? "/" : "", e.name);
        }
        load(f);
    } else {
        fp_toggle(f);
    }
}

void fp_up(FilePick* f) {
    f->msg[0] = 0;
    char last[FP_NAME] = "";
    if (f->zip[0]) {
        int l = (int)strlen(f->zdir);
        if (l > f->zroot) {   // a folder inside the zip
            int e = l - 1;   // the trailing '/'
            int s = e;
            while (s > f->zroot && f->zdir[s - 1] != '/') s--;
            snprintf(last, sizeof(last), "%.*s", e - s, f->zdir + s);
            f->zdir[s] = 0;
        } else {   // out of the zip
            const char* sl = strrchr(f->zip, '/');
            snprintf(last, sizeof(last), "%s", sl ? sl + 1 : f->zip);
            f->zip[0] = f->zdir[0] = 0;
            f->zroot = 0;
        }
    } else {
        char* sl = strrchr(f->dir, '/');
        snprintf(last, sizeof(last), "%s", sl ? sl + 1 : f->dir);
        if (sl) *sl = 0;
        else f->dir[0] = 0;
        if (!last[0]) return;
    }
    load(f);
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->ent[i].name, last)) {
            fp_move(f, i);
            break;
        }
}

int fp_result(const FilePick* f, char* out, int max) {
    int n = 0;
    out[0] = 0;
    for (int k = 0; k < f->npicked; k++) {
        int w = snprintf(out + n, max - n, "%s ", f->picked[k]);
        if (w >= max - n) {
            out[n] = 0;
            break;
        }
        n += w;
    }
    return n;
}

int fp_hit(const FilePick* f, int x, int y) {
    if (y >= FP_BTN_Y && y < FP_BTN_Y + FP_BTN_H) {
        if (x >= FP_UP_X && x < FP_UP_X + FP_UP_W) return FP_HIT_UP;
        if (x >= FP_CANCEL_X && x < FP_CANCEL_X + FP_CANCEL_W) return FP_HIT_CANCEL;
        if (x >= FP_DONE_X && x < FP_DONE_X + FP_DONE_W) return FP_HIT_DONE;
        return FP_HIT_NONE;
    }
    if (x < FP_X || x >= FP_X + FP_W || y < FP_LIST_Y || y >= FP_LIST_Y + FP_ROWS * FP_ROW_H) return FP_HIT_NONE;
    int i = f->top + (y - FP_LIST_Y) / FP_ROW_H;
    if (i >= f->n) return FP_HIT_NONE;
    return x < FP_X + 90 ? FP_HIT_CHECK + i : i;
}

int fp_click(FilePick* f, int hit) {
    if (hit == FP_HIT_UP) {
        fp_up(f);
        return 1;
    }
    if (hit < 0) return 0;
    int i = hit >= FP_HIT_CHECK ? hit - FP_HIT_CHECK : hit;
    if (i >= f->n) return 0;
    f->cur = i;
    if (hit >= FP_HIT_CHECK && !f->ent[i].is_dir) fp_toggle(f);
    else fp_enter(f);
    return 1;
}
