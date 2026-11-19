// Requests found in an answer (see ask.h).
#include "ask.h"

#include <stdio.h>
#include <string.h>

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int name_char(unsigned char c) {   // the same set files.c accepts after an @
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/' || c == ':' || c == '*' || c == '#' || c == '?' || c == '+' || c == '=' || c == '~' || c == '!';
}

static const char* trim(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    return s;
}

static int eq_word(const char* text, const char* w) {
    const char* p = trim(text);
    int n = (int)strlen(w);
    for (int i = 0; i < n; i++)
        if (lc((unsigned char)p[i]) != w[i]) return 0;
    p += n;
    return *trim(p) == 0;
}

int ask_is_cmd(const char* text) {
    return eq_word(text, "/ok") || eq_word(text, "/si") || eq_word(text, "/continuar") || eq_word(text, "/sí");
}

int ask_is_plain_yes(const char* text) {
    return eq_word(text, "ok") || eq_word(text, "si") || eq_word(text, "sí") || eq_word(text, "dale") || eq_word(text, "vale");
}

// "@zip:ruta", "@zip:ruta:lineas", "@zip?palabras": the words of a format description, not a file of the project.
static int placeholder(const char* tok) {
    static const char* const BAD[] = {"ruta", "path", "archivo", "file", "nombre", "name", "lineas", "funcion", "palabras", "words", "ruta/archivo", "ruta/del/archivo", "path/to/file", NULL};
    const char* s = strpbrk(tok, ":?");
    if (!s) return 0;
    s++;
    char part[64];
    int l = 0;
    while (s[l] && s[l] != ':' && s[l] != '#' && l < 62) part[l] = (char)lc((unsigned char)s[l]), l++;
    part[l] = 0;
    for (int i = 0; BAD[i]; i++)
        if (!strcmp(part, BAD[i])) return 1;
    l = (int)strlen(part);
    return (l > 5 && !strncmp(part, "ruta/", 5) && !strchr(part + 5, '.')) || !strncmp(part, "ruta/del/", 9);
}

int ask_collect(const char* reply, char* files, int files_max, char* webs, int webs_max) {
    files[0] = 0;
    webs[0] = 0;
    int nf = 0, nw = 0, fl = 0, wl = 0;
    for (const char* p = reply; *p; p++) {
        // network commands: a line that starts (after spaces, backticks, "- ") with /gh /url /web /wiki
        if (p == reply || p[-1] == '\n') {
            const char* q = p;
            while (*q == ' ' || *q == '\t' || *q == '`' || *q == '-' || *q == '*') q++;
            if (*q == '/' && nw < 2) {
                static const char* const CMD[] = {"/gh ", "/url ", "/web ", "/wiki ", NULL};
                for (int k = 0; CMD[k]; k++) {
                    int cl = (int)strlen(CMD[k]);
                    if (strncmp(q, CMD[k], cl)) continue;
                    const char* e = q;
                    while (*e && *e != '\n' && *e != '`') e++;
                    int len = (int)(e - q);
                    while (len > 0 && (q[len - 1] == ' ' || q[len - 1] == '.')) len--;
                    if (len > cl && len < 200 && wl + len + 2 < webs_max) {
                        memcpy(webs + wl, q, len);
                        wl += len;
                        webs[wl++] = '\n';
                        webs[wl] = 0;
                        nw++;
                    }
                    break;
                }
            }
        }
        if (*p != '@') continue;
        if (p != reply && p[-1] != ' ' && p[-1] != '\n' && p[-1] != '\t' && p[-1] != '`' && p[-1] != '(' && p[-1] != '"' && p[-1] != '\'') continue;
        char tok[100];
        int l = 0;
        while (l < 95 && name_char((unsigned char)p[1 + l])) l++;
        memcpy(tok, p + 1, l);
        tok[l] = 0;
        while (l > 0 && (tok[l - 1] == '.' || tok[l - 1] == ':' || tok[l - 1] == '-' || tok[l - 1] == '?' || tok[l - 1] == '+' || tok[l - 1] == '#' || tok[l - 1] == '!')) tok[--l] = 0;   // sentence punctuation
        // a file ("name.ext"), or the zip alias with a request: "@zip:path", "@zip?words", "@repo#..." (not @Override, @param, emails...)
        int alias = (!strncmp(tok, "zip", 3) || !strncmp(tok, "repo", 4)) && (strchr(tok, ':') || strchr(tok, '?') || strchr(tok, '!'));
        if (l < 3 || placeholder(tok) || (!strchr(tok, '.') && !alias) || strstr(tok, "..") || tok[0] == '/' || tok[0] == '*') continue;
        char item[104];
        int il = snprintf(item, sizeof(item), "@%s ", tok);
        if (strstr(files, item) || nf >= 6 || fl + il >= files_max) continue;
        memcpy(files + fl, item, il + 1);
        fl += il;
        nf++;
    }
    return nf + nw;
}
