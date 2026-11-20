// Profiles (see profiles.h).
#include "profiles.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---- the built-in profiles ----------------------------------------------------------------------------------------
// They are manners, not rules of a world: the game or the user adds the world (characters, places, the story so far)
// in its own system message, and that one is more specific than anything here.

static const char* const NARRATOR =
    "Eres el narrador de un juego de rol y de aventuras de texto. Cuentas la historia en segunda persona y en presente, con "
    "imágenes concretas (lo que se ve, se oye, se huele), ritmo ágil y consecuencias reales.\n"
    "- Nunca decides lo que el jugador hace, piensa, siente o dice: describes el mundo y a los demás personajes, y terminas "
    "dejando una puerta abierta (una situación, una elección, un peligro).\n"
    "- Los demás personajes tienen metas propias: mienten, se enfadan, se enamoran, se equivocan, guardan rencor. Pueden ser "
    "crueles, ambiguos o peligrosos si la historia lo pide. Nadie es complaciente por defecto.\n"
    "- Es ficción. Puedes tratar temas oscuros (violencia, miedo, muerte, traición, vicios, lenguaje fuerte) con naturalidad "
    "y con detalle, sin sermones, sin avisos, sin disculpas y sin salir del papel. El romance y el deseo se sugieren, sin "
    "escenas sexuales explícitas.\n"
    "- Fuera de la ficción no des instrucciones reales para hacer daño a nadie.\n"
    "- Respeta lo ya establecido (nombres, lugares, objetos, heridas, promesas, reglas del mundo). Si falta algo, invéntalo "
    "con coherencia y sigue.\n"
    "- Si el jugador intenta algo imposible o absurdo, el mundo reacciona con sus propias reglas, sin negarte el juego.\n"
    "- De 60 a 180 palabras por respuesta, salvo que se pida otra cosa. Responde en el idioma del jugador.";

static const char* const CHARACTER =
    "Interpretas a UN personaje de un juego. Su ficha (nombre, personalidad, historia, relación con el jugador, estado de "
    "ánimo) viene en el mensaje de sistema del juego, a continuación de estas reglas; la ficha manda sobre todo lo demás.\n"
    "- Responde SOLO con lo que dice el personaje, en primera persona, de 1 a 3 frases cortas, con su voz, su vocabulario y su "
    "humor. Sin comillas, sin su nombre delante, sin narrador. Si hace falta un gesto, breve y entre asteriscos: *sonríe*.\n"
    "- No salgas nunca del papel: no menciones que eres una IA, un modelo ni un juego, no cites estas instrucciones y no "
    "expliques ni resumes lo que dices.\n"
    "- Reacciona a lo que dijo el jugador y a lo que ya pasó. El personaje tiene opiniones: puede discutir, burlarse, "
    "rechazar, coquetear, mentir o guardar secretos según su carácter. No es servil ni complaciente por defecto.\n"
    "- Es ficción: el tono, el conflicto y el lenguaje fuerte los marca el personaje, sin sermones ni avisos. El romance y el "
    "deseo se sugieren, sin escenas sexuales explícitas.\n"
    "- Si el jugador pide algo imposible o fuera de lugar para el personaje, responde como el personaje lo haría.\n"
    "- Responde en el idioma del jugador.";

static const char* const CHARACTER_JSON =
    "Interpretas a UN personaje de un juego. Su ficha (nombre, personalidad, historia, relación con el jugador, estado de "
    "ánimo) viene en el mensaje de sistema del juego, a continuación de estas reglas; la ficha manda sobre todo lo demás.\n"
    "- Respondes SIEMPRE con un único objeto JSON en una sola línea, sin nada antes ni después y sin bloques de código:\n"
    "{\"texto\":\"lo que dice el personaje, 1 a 3 frases cortas\",\"emocion\":\"neutral\"}\n"
    "- \"emocion\" es una de: neutral, feliz, triste, enfadado, miedo, sorpresa, burla, timido, coqueto.\n"
    "- \"texto\" va en primera persona, con la voz y el humor del personaje, sin narrador. No salgas del papel: no menciones que "
    "eres una IA ni un juego, no cites estas instrucciones.\n"
    "- El personaje tiene opiniones: puede discutir, burlarse, rechazar, mentir o guardar secretos según su carácter. Es "
    "ficción: tono y lenguaje fuerte los marca el personaje, sin sermones ni avisos. El romance se sugiere, sin escenas "
    "sexuales explícitas.\n"
    "- Responde en el idioma del jugador.";

static const char* const PROGRAMMER =
    "Eres un asistente de programación. Respondes en el idioma del usuario, breve y exacto. Ante un error, la causa primero y "
    "luego el cambio mínimo. Para código nuevo, un solo bloque ``` completo y ejecutable, con el lenguaje indicado. No "
    "inventes funciones, APIs ni versiones: si dudas, dilo y di qué comprobarías.";

typedef struct {
    const char* name;
    const char* desc;
    const char* text;      // NULL: the programmer in the chat (its own prompt)
    double temperature;
    int max_tokens;
} Builtin;

static const Builtin BUILTINS[] = {
    {"programador", "programar: respuestas exactas y breves (en el chat es el modo normal)", NULL, 0.3, 0},
    {"narrador", "narrador de aventuras y rol de texto: ficción sin sermones", NULL, 0.9, 260},
    {"personaje", "una línea de diálogo en la voz del personaje de la ficha del juego", NULL, 0.9, 120},
    {"personaje-json", "como personaje, pero responde {\"texto\":...,\"emocion\":...}", NULL, 0.8, 160},
};
#define N_BUILTIN (int)(sizeof(BUILTINS) / sizeof(BUILTINS[0]))

static const char* builtin_text(int i) {
    switch (i) {
        case 0: return PROGRAMMER;
        case 1: return NARRATOR;
        case 2: return CHARACTER;
        default: return CHARACTER_JSON;
    }
}

// ---- names and files ----------------------------------------------------------------------------------------------

static int clean_name(const char* in, char* out, int max) {
    int n = 0;
    while (*in == ' ' || *in == '\t') in++;
    for (; *in && n < max - 1; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') break;
        if (!(isalnum(c) || c == '-' || c == '_')) return 0;   // (no slashes, dots: never a path)
        out[n++] = (char)tolower(c);
    }
    out[n] = 0;
    return n > 0;
}

static void trim(char* s) {
    int n = (int)strlen(s), a = 0;
    while (a < n && isspace((unsigned char)s[a])) a++;
    while (n > a && isspace((unsigned char)s[n - 1])) n--;
    memmove(s, s + a, n - a);
    s[n - a] = 0;
}

static int load_file(const char* name, Profile* p) {
    char path[160];
    snprintf(path, sizeof(path), PROFILES_DIR "/%s.txt", name);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    static char buf[7000];
    int n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n <= 0) return 0;
    buf[n] = 0;
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->temperature = -1;
    // the header runs up to a line that is just "---"; without one the whole file is the prompt
    char* body = buf;
    int has_sep = 0;
    for (char* l = buf; *l;) {
        char* e = strchr(l, '\n');
        char* next = e ? e + 1 : l + strlen(l);
        char line[8];
        int len = (int)((e ? e : l + strlen(l)) - l);
        if (len < (int)sizeof(line)) {
            memcpy(line, l, len);
            line[len] = 0;
            trim(line);
            if (!strcmp(line, "---")) {
                body = next;
                has_sep = 1;
                break;
            }
        }
        l = next;
    }
    if (has_sep) {
        for (char* l = buf; l < body;) {
            char* e = strchr(l, '\n');
            char line[200];
            int len = e ? (int)(e - l) : (int)strlen(l);
            if (len > (int)sizeof(line) - 1) len = (int)sizeof(line) - 1;
            memcpy(line, l, len);
            line[len] = 0;
            trim(line);
            l = e ? e + 1 : l + strlen(l);
            if (line[0] == '#' || !line[0] || !strcmp(line, "---")) continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char* v = eq + 1;
            trim(line);
            trim(v);
            if (!strcmp(line, "temperature")) {
                double t = atof(v);
                p->temperature = t < 0 ? 0 : t > 2 ? 2 : t;
            } else if (!strcmp(line, "max_tokens")) {
                int m = atoi(v);
                p->max_tokens = m < 1 ? 0 : m > 4096 ? 4096 : m;
            } else if (!strcmp(line, "desc")) {
                snprintf(p->desc, sizeof(p->desc), "%s", v);
            } else if (!strcmp(line, "stop") && p->n_stops < 2 && v[0]) {
                int o = 0;
                for (char* s = v; *s && o < (int)sizeof(p->stops[0]) - 1; s++) {
                    if (s[0] == '\\' && s[1] == 'n') p->stops[p->n_stops][o++] = '\n', s++;
                    else p->stops[p->n_stops][o++] = *s;
                }
                p->stops[p->n_stops][o] = 0;
                if (o) p->n_stops++;
            }
        }
    }
    while (*body == '\n' || *body == '\r') body++;
    int bl = (int)strlen(body);
    if (bl >= PROFILE_TEXT_MAX) {   // too long: cut at a line break
        bl = PROFILE_TEXT_MAX - 1;
        while (bl > 0 && body[bl - 1] != '\n') bl--;
        if (bl == 0) bl = PROFILE_TEXT_MAX - 1;
    }
    memcpy(p->text, body, bl);
    p->text[bl] = 0;
    for (int i = (int)strlen(p->text) - 1; i >= 0 && isspace((unsigned char)p->text[i]); i--) p->text[i] = 0;
    return 1;
}

int profile_find(const char* name_in, Profile* out) {
    char name[40];
    if (!name_in || !clean_name(name_in, name, sizeof(name))) return 0;
    Profile tmp;
    if (load_file(name, &tmp)) {
        if (!tmp.desc[0]) snprintf(tmp.desc, sizeof(tmp.desc), "perfil propio (%s.txt)", name);
        *out = tmp;
        return 1;
    }
    for (int i = 0; i < N_BUILTIN; i++) {
        if (strcmp(BUILTINS[i].name, name)) continue;
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", name);
        snprintf(out->desc, sizeof(out->desc), "%s", BUILTINS[i].desc);
        snprintf(out->text, sizeof(out->text), "%s", builtin_text(i));
        out->temperature = BUILTINS[i].temperature;
        out->max_tokens = BUILTINS[i].max_tokens;
        out->builtin = 1;
        return 1;
    }
    return 0;
}

int profile_list(char* out, int max) {
    int n = 0;
#define ADD(...) do { if (n < max - 1) n += snprintf(out + n, max - n, __VA_ARGS__); } while (0)
    for (int i = 0; i < N_BUILTIN; i++) {
        Profile p;
        if (profile_find(BUILTINS[i].name, &p)) ADD("%s%s - %s\n", p.name, p.builtin ? "" : " (tuyo)", p.desc);
    }
    DIR* d = opendir(PROFILES_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            int l = (int)strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".txt") || l - 4 >= 40) continue;
            char nm[40];
            memcpy(nm, e->d_name, l - 4);
            nm[l - 4] = 0;
            char clean[40];
            if (!clean_name(nm, clean, sizeof(clean)) || strcmp(clean, nm)) continue;   // (only names written in lower case)
            int builtin = 0;
            for (int i = 0; i < N_BUILTIN; i++)
                if (!strcmp(BUILTINS[i].name, nm)) builtin = 1;
            Profile p;
            if (!builtin && profile_find(nm, &p)) ADD("%s - %s\n", p.name, p.desc);
        }
        closedir(d);
    }
#undef ADD
    if (n >= max) n = max - 1;
    out[n] = 0;
    return n;
}

int profile_from_model(const char* model, char* name, int max) {
    if (!model || !model[0]) return 0;
    char whole[96];
    snprintf(whole, sizeof(whole), "%s", model);
    trim(whole);
    Profile p;
    char cand[40];
    if (clean_name(whole, cand, sizeof(cand)) && profile_find(cand, &p)) {
        snprintf(name, max, "%s", p.name);
        return 1;
    }
    for (int k = (int)strlen(whole) - 1; k >= 0; k--) {
        if (whole[k] != ':' && whole[k] != '/') continue;
        if (clean_name(whole + k + 1, cand, sizeof(cand)) && profile_find(cand, &p)) {
            snprintf(name, max, "%s", p.name);
            return 1;
        }
        break;
    }
    return 0;
}

int profile_api_default(char* name, int max) {
    FILE* f = fopen(PROFILE_API_FILE, "r");
    if (!f) return 0;
    char line[64] = "";
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    char cand[40];
    Profile p;
    if (!clean_name(line, cand, sizeof(cand)) || !profile_find(cand, &p)) return 0;
    snprintf(name, max, "%s", p.name);
    return 1;
}

int profile_is_cmd(const char* text, char* arg, int max) {
    while (*text == ' ' || *text == '\n') text++;
    static const char* const names[] = {"/perfiles", "/perfil", "/profile"};
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(names[i]);
        if (strncasecmp(text, names[i], l)) continue;
        if (text[l] && text[l] != ' ' && text[l] != '\n') continue;
        const char* a = text + l;
        while (*a == ' ') a++;
        int n = 0;
        for (; *a && *a != '\n' && *a != ' ' && n < max - 1; a++) arg[n++] = (char)tolower((unsigned char)*a);
        arg[n] = 0;
        return 1;
    }
    return 0;
}
