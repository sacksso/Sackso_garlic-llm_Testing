// Text files for the chat. Upload them over FTP (GoldHEN, port 2121) to /data/garlic_files/ and mention them in a
// message as @name: the file's text goes to the model with that message (the chat shows only what you typed).
// Only that folder (and the folders inside it, e.g. @out/build-pkg.yml, where /save puts the files of an answer,
// see outfiles.h, and web/ with what /gh, /url and /web saved, see webcmd.h) is ever read; names that start with a slash or contain ".." are refused. A lone "@" lists the files.
// Zips and documents: @repo.zip lists the files of the zip, @repo.zip:dir/ or @repo.zip:*.yml lists a part,
// @repo.zip:path/file brings one file (the full path, the path without the repo-main/ folder, or just the file name
// when it is unique); @doc.docx, .pptx, .xlsx, .odt, .ods, .odp bring the text of the document.
#ifndef FILES_H
#define FILES_H

#ifndef FILES_DIR
#define FILES_DIR "/data/garlic_files"
#endif

// Looks for @name words in text (an @ at the start or after a space). For each file found, its text is added to ctx
// as "File name:\n```\n...\n```\n". note gets a one-line summary for the screen (attached / truncated / not found,
// or the list for a lone @). Returns the number of files attached, or -1 when the message was only a "list the
// files" request (nothing should go to the model). The total in ctx stays under ctx_max; a file that does not fit
// is cut at a line break and the note says so.
int files_expand(const char* text, char* ctx, int ctx_max, char* note, int note_max);

// ---- phase 7 ----
// @repo.zip alone now brings a MAP of the repo (every source file with its size, lines and main functions with their
// line numbers, big vendored folders folded) plus the README and the first workflow. Then:
//   @repo.zip:path            the whole file (a cut file says so, and how to ask for the rest)
//   @repo.zip:path:120-200    lines 120 to 200            @repo.zip:path#send_text    one function / class / heading
//   @repo.zip?dns+tls         search the repo: the best files, only the parts around the matches
// Phase 8: how much of a whole project file one request brings (a range or a #function is not limited by it).
#ifndef FILES_WHOLE_CAP
#define FILES_WHOLE_CAP 7000
#endif

#define FILES_ATT_MAX 12
typedef struct {
    char path[100];   // inside the zip, without the repo-main/ folder
    int a, b;         // lines attached (1 .. 1<<29 = all)
} FilesAttached;

// What the last files_expand attached from a zip (so the search does not repeat it).
int files_attached(FilesAttached* out, int max);

// The active project: the zip that "@zip" means. "" when none.
void files_set_project(const char* zip_name);
const char* files_project(void);

// Resolves a typed zip name ("" = the last or only one, a start of the name, a typo) to a file of FILES_DIR.
// 1 = ok (name_out), 0 = not found (sugg holds a reason or close names).
int files_resolve_zip(const char* typed, char* name_out, int max, char* sugg, int sugg_max);

// Path inside the zip of what spec names (a path, a path without the repo folder, a unique file name).
// 1 found, 0 not found, -3 no project.
int files_project_resolve(const char* spec, char* rel, int rel_max);

// A whole file of the project zip, raw (CRs kept), for the edits. 1 ok, 0 not found, -1 bigger than max, -2 binary or
// damaged, -3 no project. rel gets its path inside the zip.
int files_project_read(const char* spec, char* out, int max, int* len, char* rel, int rel_max);


#endif
