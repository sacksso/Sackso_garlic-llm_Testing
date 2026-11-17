// Saving files from a reply. Type /save (or /guardar) as a message: every fenced block of the newest answer that has a
// file name is written under /data/garlic_files/out/ (a subfolder tree is kept: .github/workflows/x.yml works).
// The name comes from, in this order: the fence line (```yaml path/name.ext), a first line "# file: path" or
// "// file: path" inside the block, or the line just before the block with a `path` in backticks
// ("Archivo: `chat/foo.c`"). Blocks without a name are skipped and the note says so. Nothing is sent to the model.
// Only the out/ folder is ever written; absolute paths, ".." and hidden names such as ".." are refused.
#ifndef OUTFILES_H
#define OUTFILES_H

#ifndef OUT_DIR
#define OUT_DIR "/data/garlic_files/out"
#endif

// 1 when the message is the save command (/save or /guardar, case-insensitive, spaces around allowed).
int outfiles_is_cmd(const char* text);

// Writes the named blocks of reply into OUT_DIR. note gets a one-line summary for the screen.
// Returns the number of files written (0 when none; the note says why).
int outfiles_save(const char* reply, char* note, int note_max);

#endif
