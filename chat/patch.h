// Edits written by the model as SEARCH/REPLACE blocks, applied by the app (phase 7).
//
// A 9B model on the console is slow and unreliable at rewriting a 2000-line file, but it can say "this text becomes
// that text". The app does the exact part: it finds the text in the real file, replaces it and writes the result to
// the out folder. The model writes, per file:
//
//   Archivo: `chat/ask.c`
//   <<<<<<< BUSCAR
//   (lines exactly as they are in the file)
//   =======
//   (the new lines)
//   >>>>>>> REEMPLAZAR
//
// English markers (SEARCH / REPLACE), any number of <, = and >, and a code fence around it all are accepted.
// An empty BUSCAR on a file that does not exist creates it with the REEMPLAZAR text.
#ifndef PATCH_H
#define PATCH_H

#define PATCH_MAX_BLOCKS 24
#define PATCH_PATH_MAX 160

typedef struct {
    char path[PATCH_PATH_MAX];   // "" when the model did not say which file
    const char* search;          // points into the text given to patch_parse
    int slen;
    const char* repl;
    int rlen;
} PatchBlock;

// Finds the blocks of text. Returns how many (<= max).
int patch_parse(const char* text, PatchBlock* out, int max);

// 1 when text holds at least one complete block (cheap check).
int patch_has_blocks(const char* text);

// Applies one block to the file content in buf (len bytes now, cap bytes of room, NUL-terminated at buf[len]).
// Returns 0 = applied (*len updated; *line = first changed line, *how = "exact", "spaces", "indent" or "typo": one line of the search had a mistyped character),
//        -1 = the text was not found (hint gets the lines of the file that look most like it),
//        -2 = it matches in several places (add more lines around it), -3 = no room, -4 = empty search on existing text.
int patch_apply(char* buf, int* len, int cap, const PatchBlock* b, int file_exists, int* line, const char** how, char* hint, int hint_max);


// Phase 8: a cheap check after an edit. For C-like files (c h cpp cs java js ts css go rs kt swift json) counts the open
// minus the closed (), {} and [] outside comments and strings: out[0] = (), out[1] = {}, out[2] = []. Returns 0 when
// the file type is not checked. An edit normally leaves these numbers as they were: when they change, a block was
// probably left open or closed twice.
int patch_balance(const char* path, const char* text, int len, int out[3]);

#endif
