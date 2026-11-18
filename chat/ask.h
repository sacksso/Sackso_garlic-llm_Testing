// The model asks, the app brings (phase 6). A small model cannot open files by itself, but it can say which ones it needs:
// a line such as @repo.zip:chat/main.c (or /gh user/repo path, /url https://..., /web words) in its answer.
// The user then types /ok (or just "ok" / "si" right after such an answer): the app reads every @file the answer
// asked for, runs the network commands (only if /red is on) and sends it all back to the model in one message.
// Nothing happens without that /ok, and the files still come only from /data/garlic_files.
#ifndef ASK_H
#define ASK_H

// 1 when text is exactly "/ok" (also "/si", "/sí", "/continuar"), spaces around allowed.
int ask_is_cmd(const char* text);

// 1 when text is a plain "ok", "si", "sí", "dale" or "vale" (only meaningful right after an answer with requests).
int ask_is_plain_yes(const char* text);

// Looks for requests in an answer. files gets the @names separated by spaces ("@a.zip:x @b.yml "), at most 6, and
// webs gets the network command lines ("/gh a/b c\n/url https://...\n"), at most 2. Returns the number of
// requests found (files + web).
int ask_collect(const char* reply, char* files, int files_max, char* webs, int webs_max);

#endif
