// Saving files from a reply. Type /save (or /guardar) as a message: every fenced block of the newest answer that has a
// file name is written under /data/garlic_files/out/ (a subfolder tree is kept: .github/workflows/x.yml works).
// The name comes from, in this order: the fence line (```yaml path/name.ext), a first line "# file: path" or
// "// file: path" inside the block, or the line just before the block with a `path` in backticks
// ("Archivo: `chat/foo.c`"). Blocks without a name are skipped and the note says so. Nothing is sent to the model.
// Only the out/ folder is ever written; absolute paths, ".." and hidden names such as ".." are refused.
#ifndef OUTFILES_H
#define OUTFILES_H

#ifndef OUT_DIR
#include "workspace.h"
#define OUT_DIR ws_out_dir()   // phase 9: the files of the active project (the shared out/ folder when there is none)
#endif

// 1 when the message is the save command (/save or /guardar, case-insensitive, spaces around allowed).
int outfiles_is_cmd(const char* text);

// Writes the named blocks of reply into OUT_DIR. note gets a one-line summary for the screen.
// Returns the number of files written (0 when none; the note says why).
int outfiles_save(const char* reply, char* note, int note_max);

// How many named files of reply /guardar or the agent would write, without writing anything.
int outfiles_count(const char* reply);

// Writes one file under OUT_DIR (folders are made). The path must pass outfiles_path_ok. 0 = ok, < 0 = refused or failed.
int outfiles_write(const char* rel, const char* data, int n);

// 1 when rel is a safe relative path (no "..", no absolute path, limited depth and characters).
int outfiles_path_ok(const char* rel);

// Phase 12: the files the last outfiles_save wrote (paths under OUT_DIR), so they can be checked.
int outfiles_saved_count(void);
const char* outfiles_saved_path(int i);

#endif
