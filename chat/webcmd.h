// Chat commands that use the network, and helpers (the network itself is netget.h):
//   /red on | /red off | /red   the network switch (off by default; kept in /data/garlic_web.txt)
//   /gh owner/repo [path]       a repo's file tree and README, or one file of it (GitHub, raw files)
//   /url https://...            a web page as text
//   /web words                  a web search (DuckDuckGo's HTML page), results as a list of titles, links, snippets
//   /abrir N (/o)               opens result N of the last search (as /url)
//   /wiki words (/w)            the intro of a Wikipedia article (Spanish; /wiki en:words for English)
//   /estado                     the network switch, backend and what is waiting
//   short forms: /b /u /g /w /o. A "/word" of only letters that is none of these answers "unknown command".
//   /limpiar (/clear)           drops what was fetched and not used yet
//   /ayuda (/help)              lists the commands
// What /gh, /url and /web fetch is not sent at once: it waits and goes to the model with the next message the user
// sends (like an @file), and a copy is saved in /data/garlic_files/web/ (read it again with @web/name.txt).
// Nothing leaves the console while the switch is off, and only the typed query or address ever leaves it.
#ifndef WEBCMD_H
#define WEBCMD_H

#ifndef WEB_FLAG_FILE
#define WEB_FLAG_FILE "/data/garlic_web.txt"
#endif
#ifndef WEB_DIR
#define WEB_DIR "/data/garlic_files/web"
#endif

// 1 when the message is one of the commands above.
int webcmd_is_cmd(const char* text);

// 1 when the command will use the network now (switch on): the chat shows a "working" line before it waits.
int webcmd_is_fetch(const char* text);

// Runs it. note gets the one-line result for the screen and the log. Always returns 0 (nothing goes to the model).
int webcmd_run(const char* text, char* note, int note_max);

// What was fetched and is waiting: its length, and a copy of it into dst (not NUL-terminated, at most max bytes) that
// also empties it.
int webcmd_pending_len(void);
int webcmd_take_pending(char* dst, int max);

// 1 when the network switch is on. webcmd_net_set turns it on/off (the user's own action: key F3 or /red).
int webcmd_net_on(void);
int webcmd_net_set(int on);

#endif
