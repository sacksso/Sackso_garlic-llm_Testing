// What a person (or Claude) does with a repo before touching it: look at its shape, search it, read the right part of
// the right file. The model of the console cannot open a zip by itself, so the app does those steps and hands over
// the result (phase 7). Everything here works on a .zip on disk through zipread.h, with no allocation.
//
//   repomap_build   one line per source file: size, lines and its main symbols (functions, classes, headings, jobs of a
//                   workflow...) with their line numbers, so the model can ask for "@zip:chat/main.c:1262-1346".
//                   Big vendored folders are folded into a single line. Output stays under max bytes.
//   repo_search     the files that best match a few words (names, identifiers, Spanish words for common code ideas)
//                   with only the parts around the matches, header of the enclosing function included.
//   repo_symbol_range / repo_extract_lines   "@zip:file#function" and "@zip:file:A-B".
#ifndef REPOMAP_H
#define REPOMAP_H

#include "zipread.h"

// Length of the folder every path of the zip starts with ("repo-main/" as GitHub makes them), 0 when there is none.
int repo_root_len(const ZipEnt* ents, int n);

// 1 when the file name (by its extension or well-known name such as Makefile) is text a person edits.
int repo_is_text_name(const char* name);

// 1 when the path belongs to a folder that is not worth reading (node_modules, vendor, build...).
int repo_is_vendor_path(const char* rel);

// Map of the zip into out (NUL-terminated). Returns the length, or < 0 as zip_open_dir does. *n_source gets the number
// of source files seen (0 = the zip holds no source: the caller shows a plain list instead).
int repomap_build(const char* zip_path, char* out, int max, int* n_source);

// Where the search has already shown something (so a follow-up question does not get the same lines again).
typedef struct {
    char path[100];
    int a, b;   // lines shown, 1-based inclusive
} RepoSeen;
#define REPO_SEEN_MAX 96

typedef struct {
    RepoSeen seen[REPO_SEEN_MAX];
    int n;
} RepoSeenSet;

void repo_seen_add(RepoSeenSet* s, const char* path, int a, int b);

// Searches the zip for the words of query (separated by spaces or '+'). Writes up to max_files files (best first) as
//   --- path (12K, 340 lines) lines 120-160, 300-330 ---
//   <the lines>
// into out, at most max bytes. seen (may be NULL) is read to skip lines already shown and updated with the new ones.
// Returns the number of files written (0 = nothing matched, out says so), or < 0 as zip_open_dir does.
// paths (may be NULL) gets the paths of the files written, 100 bytes each, up to max_files.
int repo_search(const char* zip_path, const char* query, char* out, int max, int max_files, RepoSeenSet* seen,
                char paths[][100]);

// The lines of a function / class / heading called name inside text (NUL-terminated, len bytes), with the comment
// lines above it. 1 = found (line_a..line_b, 1-based), 0 = not found.
int repo_symbol_range(const char* name_of_file, const char* text, int len, const char* name, int* line_a, int* line_b);

// Lines a..b (1-based, inclusive) of text into out (NUL-terminated, at most max bytes). Returns the bytes written;
// *total gets the number of lines of text.
int repo_extract_lines(const char* text, int len, int a, int b, char* out, int max, int* total);

// The words of a message that are worth searching for: lower-case, without stop words, @names and /commands, with the
// Spanish words of common code ideas also given in English ("red" -> "net network"). Returns the number of words;
// they are written into out separated by single spaces.
int repo_keywords(const char* text, char* out, int max);

// Phase 12: every use of an identifier (whole word) in the source files of the zip, file by file with the first lines,
// the files that define it first ("what else must change if I change this?"). Returns the number of files, < 0 as
// zip_open_dir does; *total_hits (may be NULL) gets the number of lines that use it.
int repo_refs(const char* zip_path, const char* name, char* out, int max, int* total_hits);

#endif
