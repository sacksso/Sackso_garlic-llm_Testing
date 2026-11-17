// Text files for the chat. Upload them over FTP (GoldHEN, port 2121) to /data/garlic_files/ and mention them in a
// message as @name: the file's text goes to the model with that message (the chat shows only what you typed).
// Only that folder is ever read; names with a slash or ".." are refused. A lone "@" lists the files.
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
