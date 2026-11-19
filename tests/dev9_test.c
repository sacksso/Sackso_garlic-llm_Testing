// Host test of phase 9 through the commands: /proyecto, /proyectos, /instrucciones, /mapa, /aplicar, /empaquetar and the
// agent saving whole files. Built by host_tests.sh (see there for the folders).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "devcmd.h"
#include "files.h"
#include "outfiles.h"
#include "workspace.h"
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
static int exists(const char* p) { struct stat st; return stat(p, &st) == 0; }
static char note[2000];
static void cmd(const char* t, const char* last) { devcmd_run(t, last, note, sizeof(note)); }
int main(void) {
    static char pend[30000];
    char p[400], nm[300], nx[300];
    cmd("/proyecto", "");
    CHECK(strstr(note, "No project") != NULL);
    cmd("/proyecto nuevo mal nombre", "");
    CHECK(strstr(note, "No such zip") != NULL && !strcmp(ws_name(), ""));   // the second word is a zip, and there is none
    cmd("/proyecto nuevo bad/name", "");
    CHECK(strstr(note, "Usage") != NULL && !strcmp(ws_name(), ""));
    cmd("/proyecto nuevo demo", "");
    CHECK(strstr(note, "created and active") != NULL && !strcmp(ws_name(), "demo"));
    cmd("/proyecto nuevo demo", "");
    CHECK(strstr(note, "already exists") != NULL);
    cmd("/instrucciones", "");
    CHECK(strstr(note, "no instructions") != NULL);
    cmd("/instrucciones Responde breve y usa Zig.", "");
    CHECK(strstr(note, "saved") != NULL);
    int n = devcmd_take_pending(pend, sizeof(pend));
    pend[n] = 0;
    CHECK(strstr(pend, "[Proyecto: demo]") && strstr(pend, "Zig"));
    cmd("/mapa", "");
    n = devcmd_take_pending(pend, sizeof(pend));
    pend[n] = 0;
    CHECK(n > 0 && strstr(pend, "Aun no hay archivos"));

    // the agent saves whole files by itself and does not ask for more
    CHECK(devcmd_auto_on() == 0);
    cmd("/auto on 3", "");
    CHECK(devcmd_auto_on() == 1);
    const char* reply = "Aqui va.\n\nArchivo: .github/workflows/build.yml\n```yaml\nname: build\n```\n\nArchivo: README.md\n```md\n# Demo\n```\n";
    int r = devcmd_agent_step(reply, 0, nx, sizeof(nx), nm, sizeof(nm));
    CHECK(r == 0 && strstr(nm, "2 file(s) saved") != NULL);
    snprintf(p, sizeof(p), "%s/demo/.github/workflows/build.yml", WS_OUT_ROOT);
    CHECK(exists(p));
    // an edit of that file, then a file and an edit together through /aplicar
    const char* reply2 = "Archivo: README.md\n<<<<<<< BUSCAR\n# Demo\n=======\n# Demo 2\n>>>>>>> REEMPLAZAR\n\nArchivo: src/a.c\n```c\nint a;\n```\n";
    cmd("/aplicar", reply2);
    CHECK(strstr(note, "1 edit(s) applied, 0 failed; 1 file(s) saved") != NULL);
    snprintf(p, sizeof(p), "%s/demo/src/a.c", WS_OUT_ROOT);
    CHECK(exists(p));
    cmd("/aplicar", "nada que aplicar");
    CHECK(strstr(note, "no edit blocks") != NULL);

    // listing, packing under the project's name, switching, a base zip
    cmd("/proyectos", "");
    CHECK(strstr(note, "* demo (3 files") != NULL || strstr(note, "* demo (4 files") != NULL);
    cmd("/empaquetar", "");
    CHECK(strstr(note, "demo.zip") != NULL);
    snprintf(p, sizeof(p), "%s/demo.zip", FILES_DIR);
    CHECK(exists(p));
    cmd("/proyecto nuevo otro", "");
    CHECK(!strcmp(ws_name(), "otro") && !exists("/nonexistent"));
    cmd("/proyecto demo", "");
    CHECK(!strcmp(ws_name(), "demo") && strstr(note, "now active") != NULL);
    cmd("/proyecto repo.zip", "");
    CHECK(strstr(note, "now based on") != NULL && !strcmp(ws_base_zip(), "repo.zip") && !strcmp(files_project(), "repo.zip"));
    cmd("/mapa", "");
    n = devcmd_take_pending(pend, sizeof(pend));
    pend[n] = 0;
    CHECK(strstr(pend, "[Repo map]") && strstr(pend, "send_text") && strstr(pend, "@out/demo/src/a.c"));   // the zip's map and the project's own files
    cmd("/proyecto salir", "");
    CHECK(!strcmp(ws_name(), ""));
    cmd("/proyecto repo.zip", "");   // no project: a zip makes one named after it
    CHECK(!strcmp(ws_name(), "repo") && !strcmp(ws_base_zip(), "repo.zip"));

    // phase 12: the agent checks what it wrote (YAML, workflows) and tells the model; /verificar shows it again
    cmd("/proyecto nuevo chk", "");
    cmd("/auto on 9", "");
    const char* wf_bad = "Archivo: .github/workflows/ci.yml\n```yaml\nname: CI\non: push\njobs:\n  build:\n    steps:\n      - name: nothing\n      - run: make\n```\n";
    r = devcmd_agent_step(wf_bad, 0, nx, sizeof(nx), nm, sizeof(nm));
    CHECK(r == 1 && strstr(nx, "runs-on") != NULL && strstr(nx, "neither uses nor run") != NULL);
    cmd("/verificar", "");
    CHECK(strstr(note, "problem") != NULL && strstr(note, "runs-on") != NULL);
    const char* wf_ok = "Archivo: .github/workflows/ci.yml\n```yaml\nname: CI\non: push\njobs:\n  build:\n    runs-on: ubuntu-latest\n    steps:\n      - uses: actions/checkout@v4\n      - run: make\n```\n";
    r = devcmd_agent_step(wf_ok, 0, nx, sizeof(nx), nm, sizeof(nm));
    CHECK(r == 0);
    cmd("/verificar .github/workflows/ci.yml", "");
    CHECK(strstr(note, "no problems") != NULL);
    // an edit that breaks the file (a tab) is reported, one that does not is not
    const char* e_bad = "Archivo: .github/workflows/ci.yml\n<<<<<<< BUSCAR\n      - run: make\n=======\n      - run: make\n\t  - run: test\n>>>>>>> REEMPLAZAR\n";
    r = devcmd_agent_step(e_bad, 0, nx, sizeof(nx), nm, sizeof(nm));
    CHECK(r == 1 && strstr(nx, "tab") != NULL);
    const char* e_ok = "Archivo: .github/workflows/ci.yml\n<<<<<<< BUSCAR\n      - run: make\n=======\n      - run: make -j2\n>>>>>>> REEMPLAZAR\n";
    cmd("/deshacer", "");
    r = devcmd_agent_step(e_ok, 0, nx, sizeof(nx), nm, sizeof(nm));
    CHECK(r == 0 && strstr(nm, "applied") != NULL);
    cmd("/auto off", "");
    printf(bad ? "dev9_test: FAILED\n" : "dev9_test: ok\n");
    return bad;
}
