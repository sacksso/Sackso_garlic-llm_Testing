// Host test of chat/profiles.c: built-in profiles, own files, the model's name and the /perfil command.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "profiles.h"

#define D PROFILES_DIR

static void put(const char* name, const char* text) {
    char p[200];
    snprintf(p, sizeof(p), D "/%s", name);
    FILE* f = fopen(p, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void put_api(const char* text) {
    FILE* f = fopen(PROFILE_API_FILE, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(void) {
    system("rm -rf " D " " PROFILE_API_FILE "; mkdir -p " D);
    Profile p;
    char list[3000], name[40], arg[48];

    // built-in ones
    assert(profile_find("Narrador", &p) && p.builtin && p.temperature > 0.8 && p.max_tokens == 260);
    assert(strstr(p.text, "narrador") && strstr(p.text, "sin sermones"));
    assert(profile_find("personaje-json", &p) && strstr(p.text, "\"emocion\""));
    assert(profile_find("programador", &p) && p.temperature < 0.5 && strstr(p.text, "programación"));
    assert(!profile_find("nope", &p) && !profile_find("../etc/passwd", &p) && !profile_find("a b", &p) && !profile_find("", &p));
    assert(!profile_find("narr.ador", &p));

    // an own profile with a header
    put("maria.txt", "# la cocinera\ndesc=Maria, la cocinera del pueblo\ntemperature=1.1\nmax_tokens=90\nstop=\\nJugador:\nstop=###\n---\n\nEres Maria.\nHablas de sopa.\n");
    assert(profile_find("MARIA", &p) && !p.builtin);
    assert(p.temperature > 1.09 && p.temperature < 1.11 && p.max_tokens == 90);
    assert(p.n_stops == 2 && !strcmp(p.stops[0], "\nJugador:") && !strcmp(p.stops[1], "###"));
    assert(!strcmp(p.text, "Eres Maria.\nHablas de sopa.") && !strcmp(p.desc, "Maria, la cocinera del pueblo"));

    // a file without "---" is all prompt; a file with the name of a built-in one replaces it
    put("pirata.txt", "Habla como un pirata.\ntemperature=5 no es una clave aqui\n");
    assert(profile_find("pirata", &p) && p.temperature < 0 && p.max_tokens == 0 && strstr(p.text, "temperature=5"));
    put("narrador.txt", "temperature=0.5\n---\nSolo narras con frases cortas.");
    assert(profile_find("narrador", &p) && !p.builtin && p.temperature == 0.5 && !strcmp(p.text, "Solo narras con frases cortas."));
    // out-of-range numbers are brought into range; a too long text is cut at a line break
    char big[24000] = "temperature=9\nmax_tokens=99999\n---\n";
    for (int i = 0; i < 400; i++) strcat(big, "una linea de relleno bastante larga para llenar\n");
    put("largo.txt", big);
    assert(profile_find("largo", &p) && p.temperature == 2 && p.max_tokens == 4096);
    assert(strlen(p.text) < PROFILE_TEXT_MAX && p.text[strlen(p.text) - 1] != '\n' && strlen(p.text) > 3000);

    // the list: built-in first, replaced ones marked, own ones after
    int n = profile_list(list, sizeof(list));
    assert(n == (int)strlen(list));
    puts(list);
    assert(strstr(list, "programador - ") && strstr(list, "narrador (tuyo)") && strstr(list, "personaje - "));
    assert(strstr(list, "maria - Maria, la cocinera") && strstr(list, "pirata - perfil propio"));
    char tiny[30];
    n = profile_list(tiny, sizeof(tiny));
    assert(n < (int)sizeof(tiny) && n == (int)strlen(tiny));

    // the request's model
    assert(profile_from_model("garlic-9b:maria", name, sizeof(name)) && !strcmp(name, "maria"));
    assert(profile_from_model("ollama/pirata", name, sizeof(name)) && !strcmp(name, "pirata"));
    assert(profile_from_model("Personaje", name, sizeof(name)) && !strcmp(name, "personaje"));
    assert(profile_from_model("gpt-4o-mini", name, sizeof(name)) == 0);
    assert(profile_from_model("garlic-9b:no-existe", name, sizeof(name)) == 0);
    assert(profile_from_model("", name, sizeof(name)) == 0 && profile_from_model(NULL, name, sizeof(name)) == 0);

    // the API's default
    assert(!profile_api_default(name, sizeof(name)));
    put_api("Personaje-JSON\n");
    assert(profile_api_default(name, sizeof(name)) && !strcmp(name, "personaje-json"));
    put_api("no-existe\n");
    assert(!profile_api_default(name, sizeof(name)));

    // /perfil
    assert(profile_is_cmd("/perfil", arg, sizeof(arg)) && !arg[0]);
    assert(profile_is_cmd("  /Perfil Narrador ", arg, sizeof(arg)) && !strcmp(arg, "narrador"));
    assert(profile_is_cmd("/perfiles", arg, sizeof(arg)) && !arg[0]);
    assert(profile_is_cmd("/profile off", arg, sizeof(arg)) && !strcmp(arg, "off"));
    assert(!profile_is_cmd("/perfilx", arg, sizeof(arg)) && !profile_is_cmd("hola /perfil", arg, sizeof(arg)));

    system("rm -rf " D " " PROFILE_API_FILE);
    puts("profiles_test: ok");
    return 0;
}
