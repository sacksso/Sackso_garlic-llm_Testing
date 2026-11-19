// GoldHEN cheat and patch lists (see ghinfo.h).
#include "ghinfo.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_all(const char* path, long cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char* b = (char*)malloc(cap + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    long n = (long)fread(b, 1, cap, f);
    fclose(f);
    b[n] = 0;
    return b;
}

// the string value after "key": in a JSON text (no escapes handled beyond \")
static const char* json_str(const char* from, const char* key, char* out, int max) {
    char k[64];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char* p = strstr(from, k);
    if (!p) return NULL;
    p += strlen(k);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < max - 1) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return p;
}

// attribute of an XML tag that starts at tag ("<Cheat Text=\"...\"")
static void attr(const char* tag, const char* key, char* out, int max) {
    out[0] = 0;
    const char* end = strchr(tag, '>');
    char k[48];
    snprintf(k, sizeof(k), " %s=\"", key);
    const char* p = strstr(tag, k);
    if (!p || (end && p > end)) return;
    p += strlen(k);
    int n = 0;
    while (*p && *p != '"' && n < max - 1) out[n++] = *p++;
    out[n] = 0;
}

static int valid_title(const char* t) {
    if (strlen(t) != 9) return 0;
    for (int i = 0; i < 4; i++)
        if (!((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= 'a' && t[i] <= 'z'))) return 0;
    for (int i = 4; i < 9; i++)
        if (t[i] < '0' || t[i] > '9') return 0;
    return 1;
}

int gh_game_info(const char* title, char* out, int max) {
    if (!valid_title(title)) return -1;
    char tid[10];
    for (int i = 0; i < 9; i++) tid[i] = title[i] >= 'a' && title[i] <= 'z' ? title[i] - 32 : title[i];
    tid[9] = 0;
    int o = snprintf(out, max, "GoldHEN info for %s (read-only):\n", tid);
    int found = 0;
    static const char* const KINDS[] = {"json", "shn", "mc4", NULL};
    for (int k = 0; KINDS[k]; k++) {
        char dir[200];
        snprintf(dir, sizeof(dir), GH_DIR "/cheats/%s", KINDS[k]);
        DIR* d = opendir(dir);
        struct dirent* de;
        while (d && (de = readdir(d)) != NULL && o < max - 300) {
            if (strncmp(de->d_name, tid, 9) != 0 || de->d_name[9] != '_') continue;
            char path[400];
            snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            found++;
            if (k == 2) {
                o += snprintf(out + o, max - o, "Cheats %s (mc4 format: names not shown here)\n", de->d_name);
                continue;
            }
            char* t = read_all(path, 512 * 1024);
            if (!t) continue;
            char ver[40] = "?", nm[160];
            if (k == 0) {
                json_str(t, "version", ver, sizeof(ver));
                o += snprintf(out + o, max - o, "Cheats %s (game version %s):\n", de->d_name, ver);
                const char* mods = strstr(t, "\"mods\"");
                int cnt = 0;
                for (const char* p = mods; p && (p = json_str(p + 1, "name", nm, sizeof(nm))) != NULL;) {
                    if (cnt < 40 && o < max - 200) o += snprintf(out + o, max - o, "  - %s\n", nm);
                    cnt++;
                }
                if (cnt > 40) o += snprintf(out + o, max - o, "  ... and %d more\n", cnt - 40);
                if (!mods) o += snprintf(out + o, max - o, "  (no list of cheats in this file)\n");
            } else {
                const char* tr = strstr(t, "<Trainer");
                attr(tr ? tr : t, "Version", ver, sizeof(ver));
                o += snprintf(out + o, max - o, "Cheats %s (game version %s):\n", de->d_name, ver[0] ? ver : "?");
                int cnt = 0;
                for (const char* p = strstr(t, "<Cheat "); p; p = strstr(p + 1, "<Cheat ")) {
                    attr(p, "Text", nm, sizeof(nm));
                    if (cnt < 40 && o < max - 200) o += snprintf(out + o, max - o, "  - %s\n", nm);
                    cnt++;
                }
                if (cnt > 40) o += snprintf(out + o, max - o, "  ... and %d more\n", cnt - 40);
            }
            free(t);
        }
        if (d) closedir(d);
    }
    char pp[200];
    snprintf(pp, sizeof(pp), GH_DIR "/patches/xml/%s.xml", tid);
    char* px = read_all(pp, 2 * 1024 * 1024);
    if (px) {
        int cnt = 0;
        o += snprintf(out + o, max - o, "Game patches (%s.xml):\n", tid);
        for (const char* m = strstr(px, "<Metadata"); m && o < max - 300; m = strstr(m + 1, "<Metadata")) {
            char nm[120], note[160], ver[24], au[60];
            attr(m, "Name", nm, sizeof(nm));
            attr(m, "Note", note, sizeof(note));
            attr(m, "AppVer", ver, sizeof(ver));
            attr(m, "Author", au, sizeof(au));
            if (cnt < 40) o += snprintf(out + o, max - o, "  - %s (game version %s)%s%s%s%s\n", nm, ver[0] ? ver : "?", note[0] ? ": " : "", note, au[0] ? " by " : "", au);
            cnt++;
        }
        free(px);
        found++;
    }
    if (!found)
        o += snprintf(out + o, max - o, "No cheat file or patch for this game in " GH_DIR ". GoldHEN can download them (Settings > GoldHEN > Cheats > Download cheats; Plugins > Game Patch > Download patches).\n");
    else
        o += snprintf(out + o, max - o, "They are turned on in GoldHEN's in-game menu, and only for the matching game version. If one of them does what you want (money, health...), you do not need to edit the saved game.\n");
    return o;
}
