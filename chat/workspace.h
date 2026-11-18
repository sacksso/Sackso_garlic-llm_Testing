// Projects (workspaces): a named place for what the AI creates, like the projects of a chat assistant (phase 9).
//   /data/garlic_files/out/<name>/        the files of the project (what the AI writes: .github/workflows/x.yml, src/a.c...)
//   /data/garlic_projects/<name>/         INSTRUCCIONES.md (instructions for the model), progress.txt (the ledger of
//                                         progress.h, one per project) and proyecto.txt (zip=<repo zip the project is based on>)
// The files live under out/ on purpose: the explorer, "@out/<name>/path" and FTP already know that folder.
// With no project active everything goes to the shared /data/garlic_files/out and /data/garlic_progress.txt as before.
// The active project is kept in /data/garlic_proyecto.txt.
#ifndef WORKSPACE_H
#define WORKSPACE_H

#ifndef WS_ROOT
#define WS_ROOT "/data/garlic_projects"
#endif
#ifndef WS_OUT_ROOT
#define WS_OUT_ROOT "/data/garlic_files/out"
#endif
#ifndef WS_STATE_FILE
#define WS_STATE_FILE "/data/garlic_proyecto.txt"
#endif
#ifndef WS_LEGACY_PROGRESS
#define WS_LEGACY_PROGRESS "/data/garlic_progress.txt"
#endif

// 1 when name is usable as a project name: 1-40 characters, letters digits . _ - (no leading dot).
int ws_valid_name(const char* name);

// The active project ("" when none), where its files go, and where its progress ledger is kept.
const char* ws_name(void);
const char* ws_out_dir(void);
const char* ws_progress_file(void);

// The repo zip the active project is based on ("" when none).
const char* ws_base_zip(void);
int ws_set_base_zip(const char* zip);

// 1 when the project exists.
int ws_exists(const char* name);

// Makes a project and makes it the active one. zip may be NULL or "". 0 = ok, -1 bad name, -2 exists, -3 cannot create.
int ws_create(const char* name, const char* zip);

// Makes an existing project the active one. 0 = ok, -1 it does not exist.
int ws_switch(const char* name);

// No active project (back to the shared out/ folder).
void ws_leave(void);

// One line per project into out: "* name (3 files, 4 KB) base: x.zip" ('*' marks the active one). Returns how many.
int ws_list(char* out, int max);

// Instructions of the active project (INSTRUCCIONES.md). Set: text = NULL or "" removes them. 0 = ok.
int ws_set_instructions(const char* text);
int ws_instructions(char* out, int max);   // length, 0 when none

// Text for the model about the active project: the instructions and the files it holds, with the "@out/<name>/path"
// that brings each one. Returns its length (0 when no project).
int ws_context_text(char* out, int max);

// The short block that goes at the end of the system prompt (name, how to deliver files, instructions). Returns its
// length (0 when no project).
int ws_prompt_block(char* out, int max);

#endif
