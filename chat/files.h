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

#endif
