// Host test of chat/progress.c: the ledger, the saved file and the resume message.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "progress.h"

int main(void) {
    assert(progress_is_cmd("/progreso") == 1 && progress_is_cmd("  /Checkpoint ") == 1);
    assert(progress_is_cmd("/retomar") == 2 && progress_is_cmd("/resume\n") == 2);
    assert(progress_is_cmd("/progresos") == 0 && progress_is_cmd("/ok") == 0 && progress_is_cmd("hola") == 0);

    char out[6000];
    assert(!progress_has() && progress_resume_text(out, sizeof(out), NULL, 1) == 0 && out[0] == 0);

    progress_goal("/auto on");                 // commands are not a task
    progress_goal("ok");                       // neither is a short answer
    progress_goal("@zip:chat/main.c @repo.zip");   // nor a bare list of files
    assert(!progress_has());
    progress_goal("Arregla el reinicio del contexto en @zip:chat/main.c para que no pierda el trabajo");
    progress_goal("ahora hazlo tambien para el modo voz");
    progress_edits(2, 1,
                   "OK chat/main.c (line 792, exact match)\nOK chat/devcmd.c (line 40, indent match)\n"
                   "FALLO chat/ui.c: the SEARCH text was not found. Closest text in the file:\nint x;\n");
    progress_asked("@zip:chat/main.c#start_turn @zip:chat/files.c:100-180 ");
    progress_notes("OBJETIVO: no perder el trabajo\nHECHO: guardar notas\nPENDIENTE: probar @zip:chat/main.c#x\n");
    assert(progress_has());
    assert(progress_save() == 0);

    progress_clear();
    assert(!progress_has());
    assert(progress_load() == 1);
    int n = progress_resume_text(out, sizeof(out), NULL, 1);
    assert(n > 0 && n == (int)strlen(out));
    puts(out);
    assert(strstr(out, "TAREA: Arregla el reinicio del contexto en  para que no pierda") || strstr(out, "TAREA: Arregla el reinicio"));
    assert(!strstr(out, "@zip:chat/main.c para"));                      // @words of the task are dropped
    assert(strstr(out, "(@)zip:chat/main.c#x"));                         // an @ in the notes cannot bring a file
    assert(strstr(out, "- chat/main.c (line 792)") && strstr(out, "- chat/devcmd.c (line 40)"));
    assert(strstr(out, "ULTIMO CAMBIO QUE FALLO: FALLO chat/ui.c"));
    assert(strstr(out, "ULTIMO MENSAJE DEL USUARIO: ahora hazlo"));
    assert(strstr(out, "\n@zip:chat/main.c#start_turn @zip:chat/files.c:100-180"));   // the last request, on its own line
    n = progress_resume_text(out, sizeof(out), "Resultado de aplicar tus cambios (0 bien, 1 fallaron)", 0);
    assert(strstr(out, "PENDIENTE DE LA APP:") && !strstr(out, "Lo ultimo que pediste") && !strstr(out, "Lo último que pediste"));

    // a round without failures clears the failure line; the edit list drops its oldest lines when full
    progress_edits(1, 0, "OK a.c (line 1, exact match)\n");
    n = progress_resume_text(out, sizeof(out), NULL, 0);
    assert(!strstr(out, "CAMBIO QUE FALLO"));
    for (int i = 0; i < 80; i++) {
        char r[100];
        snprintf(r, sizeof(r), "OK src/file_number_%d.c (line %d, exact match)\n", i, i);
        progress_edits(1, 0, r);
    }
    n = progress_resume_text(out, sizeof(out), NULL, 0);
    assert(strstr(out, "file_number_79") && !strstr(out, "file_number_0.c") && n < 4200);

    // a tiny buffer never overflows
    char small[40];
    n = progress_resume_text(small, sizeof(small), "x", 1);
    assert(n < (int)sizeof(small) && n == (int)strlen(small));

    remove(PROGRESS_FILE);
    puts("progress_test: ok");
    return 0;
}
