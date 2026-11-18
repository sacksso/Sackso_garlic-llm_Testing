// Host test of phase 9: projects (workspace.c), files saved into the active project (outfiles.c), the ledger path
// (progress.h) and the placeholder filter of ask.c. Built by host_tests.sh with WS_ROOT / WS_OUT_ROOT / WS_STATE_FILE
// pointing into a temporary folder.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ask.h"
#include "outfiles.h"
#include "workspace.h"
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
static int exists(const char* p) { struct stat st; return stat(p, &st) == 0; }
int main(void) {
    char buf[4000], note[400], p[400];
    mkdir(WS_OUT_ROOT, 0777);   // (the parent folders exist on the console)
    CHECK(!strcmp(ws_name(), ""));
    CHECK(!strcmp(ws_out_dir(), WS_OUT_ROOT));
    CHECK(!strcmp(ws_progress_file(), WS_LEGACY_PROGRESS));
    CHECK(ws_valid_name("twitch-port") && ws_valid_name("a.b_c-1") && !ws_valid_name("") && !ws_valid_name(".x") && !ws_valid_name("a/b") && !ws_valid_name("a b") && !ws_valid_name("..") && !ws_valid_name("a..b"));
    CHECK(ws_prompt_block(buf, sizeof(buf)) == 0 && ws_context_text(buf, sizeof(buf)) == 0);

    // create
    CHECK(ws_create("twitch-port", "") == 0);
    CHECK(!strcmp(ws_name(), "twitch-port"));
    snprintf(p, sizeof(p), "%s/twitch-port", WS_OUT_ROOT);
    CHECK(!strcmp(ws_out_dir(), p) && exists(p));
    snprintf(p, sizeof(p), "%s/twitch-port/progress.txt", WS_ROOT);
    CHECK(!strcmp(ws_progress_file(), p));
    CHECK(ws_create("twitch-port", "") == -2 && ws_create("bad name", "") == -1);

    // a reply with two named files, an edit block, and an unnamed block
    const char* reply =
        "Listo.\n\nArchivo: `.github/workflows/build.yml`\n```yaml\nname: build\non: push\n```\n\n"
        "Archivo: src/main.c\n```c\nint main(void) { return 0; }\n```\n\n"
        "Archivo: src/x.c\n<<<<<<< BUSCAR\nold\n=======\nnew\n>>>>>>> REEMPLAZAR\n\n"
        "```bash\necho sin nombre\n```\n";
    CHECK(outfiles_count(reply) == 2);
    CHECK(outfiles_save(reply, note, sizeof(note)) == 2);
    snprintf(p, sizeof(p), "%s/twitch-port/.github/workflows/build.yml", WS_OUT_ROOT);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/twitch-port/src/main.c", WS_OUT_ROOT);
    CHECK(exists(p));
    CHECK(strstr(note, "twitch-port") != NULL);   // "Saved 2 in .../twitch-port"
    snprintf(p, sizeof(p), "%s/src/main.c", WS_OUT_ROOT);
    CHECK(!exists(p));                             // nothing leaked into the shared folder
    CHECK(outfiles_count("nada que guardar\n```\nsin nombre\n```\n") == 0);

    // instructions, context, prompt block
    CHECK(ws_instructions(buf, sizeof(buf)) == 0);
    CHECK(ws_set_instructions("Responde en espanol. Usa Zig y OpenOrbis.") == 0);
    CHECK(ws_instructions(buf, sizeof(buf)) > 10 && strstr(buf, "OpenOrbis"));
    CHECK(ws_context_text(buf, sizeof(buf)) > 0 && strstr(buf, "[Proyecto: twitch-port]") && strstr(buf, "OpenOrbis") &&
          strstr(buf, "@out/twitch-port/src/main.c") && strstr(buf, "@out/twitch-port/.github/workflows/build.yml"));
    CHECK(ws_prompt_block(buf, sizeof(buf)) > 0 && strstr(buf, "twitch-port") && strstr(buf, "Archivo:") && strstr(buf, "Zig"));
    CHECK(ws_set_instructions("") == 0 && ws_instructions(buf, sizeof(buf)) == 0);

    // a second project, listing, switching, base zip, persistence
    CHECK(ws_create("otro", "repo.zip") == 0 && !strcmp(ws_base_zip(), "repo.zip"));
    CHECK(ws_context_text(buf, sizeof(buf)) > 0 && strstr(buf, "Aun no hay archivos") && strstr(buf, "repo.zip"));
    int n = ws_list(buf, sizeof(buf));
    CHECK(n == 2 && strstr(buf, "* otro") && strstr(buf, "- twitch-port (2 files") && strstr(buf, "base: repo.zip"));
    CHECK(ws_switch("twitch-port") == 0 && !strcmp(ws_name(), "twitch-port") && !strcmp(ws_base_zip(), ""));
    CHECK(ws_switch("no-existe") == -1 && !strcmp(ws_name(), "twitch-port"));
    CHECK(ws_set_base_zip("x.zip") == 0 && !strcmp(ws_base_zip(), "x.zip"));
    ws_leave();
    CHECK(!strcmp(ws_name(), "") && !strcmp(ws_out_dir(), WS_OUT_ROOT) && !strcmp(ws_progress_file(), WS_LEGACY_PROGRESS));
    CHECK(ws_switch("twitch-port") == 0);

    // the requests of the model: placeholders from the prompt are not files
    char f[200], w[200];
    CHECK(ask_collect("Usa @zip:ruta:lineas para pedir.", f, sizeof(f), w, sizeof(w)) == 0);
    CHECK(ask_collect("Pide @zip?palabras o @zip:ruta/archivo y @zip:ruta/del/archivo.c", f, sizeof(f), w, sizeof(w)) == 0);
    CHECK(ask_collect("Necesito\n@zip:chat/main.c#send_text\n@zip:chat/files.c:100-180\n", f, sizeof(f), w, sizeof(w)) == 2);
    CHECK(ask_collect("mira @out/twitch-port/src/main.c", f, sizeof(f), w, sizeof(w)) == 1);
    printf(bad ? "workspace_test: FAILED\n" : "workspace_test: ok\n");
    return bad;
}
