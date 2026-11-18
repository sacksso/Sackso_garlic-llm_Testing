// Developer commands and the agent loop (phase 7). The model of the console only writes; the app does the rest.
//   /proyecto [zip]       the repo zip that "@zip" means (no name: shows the current one)
//   /mapa                 the map of the project (files, sizes, functions with line numbers) for the next message
//   /grep words           the parts of the project that match the words, for the next message
//   /aplicar              applies the edit blocks (BUSCAR/REEMPLAZAR) of the last answer, into /data/garlic_files/out
//   /empaquetar [name]    packs the out folder into /data/garlic_files/<name>.zip (one file to take over FTP)
//   /auto on|off [N]      the agent: after an answer the app applies the edits or brings what the answer asked for by
//                         itself, up to N rounds (default 4), and tells the model what failed
//   /limite N             at most N tokens of context for one message (0 = as much as fits)
// Settings are kept in /data/garlic_dev.txt.
#ifndef DEVCMD_H
#define DEVCMD_H

#ifndef DEV_FLAG_FILE
#define DEV_FLAG_FILE "/data/garlic_dev.txt"
#endif

// 1 when the message is one of the commands above.
int devcmd_is_cmd(const char* text);

// Runs it. last_ai is the newest answer of the model ("" when none). note gets the line for the screen.
int devcmd_run(const char* text, const char* last_ai, char* note, int note_max);

// Text a command prepared for the next message (like webcmd's): its length, and a copy that also empties it.
int devcmd_pending_len(void);
int devcmd_take_pending(char* dst, int max);

// Settings.
int devcmd_auto_on(void);
int devcmd_token_limit(void);   // 0 = no limit set by the user

// The user typed a message: the agent's round counter starts again.
void devcmd_user_turn(void);

// After an answer: what the agent does by itself. Returns 1 when it wants to send next_msg to the model now ("/ok" to
// bring what the answer asked for, or a report of edits that failed), 0 when it has nothing more to do. note gets a
// line for the screen ("" when none). net_on says whether the network switch is on (the agent never turns it on).
int devcmd_agent_step(const char* reply, int net_on, char* next_msg, int next_max, char* note, int note_max);

// A new context begins (restart, /retomar): what the agent remembers of the last round (a repeated request) is dropped.
void devcmd_new_context(void);

// The system prompt for programming work, used when /data/garlic_system.txt is missing.
const char* devcmd_system_prompt(void);

#endif
