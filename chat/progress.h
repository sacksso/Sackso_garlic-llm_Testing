// Progress of a piece of work, kept outside the model's 16K-token memory (phase 8).
//
// The model forgets everything when the context fills up and restarts. The app keeps a small ledger of its own
// instead: the task (what the user asked), the edits already applied, what the model asked for last, and the model's
// own notes (written on request, see progress_checkpoint_prompt). It is saved in /data/garlic_progress.txt (it
// survives a crash or a restart of the app) and turned into one message that opens the next context, so the work
// goes on from the same point.
//   /progreso   the model writes its notes now and they are saved           (progress_is_cmd = 1)
//   /retomar    a fresh context that starts from the saved progress          (progress_is_cmd = 2)
#ifndef PROGRESS_H
#define PROGRESS_H

#ifndef PROGRESS_FILE
#include "workspace.h"
#define PROGRESS_FILE ws_progress_file()   // phase 9: one ledger per project (/data/garlic_progress.txt when there is none)
#endif

// 1 for /progreso (also /checkpoint), 2 for /retomar (also /resume), else 0.
int progress_is_cmd(const char* text);

// A message the user typed: it becomes the task (the first one) or the latest message. Commands, "ok" and messages
// that are only @files are ignored; @words are dropped from it (they must not bring files in again).
void progress_goal(const char* user_text);

// The result of applying the model's edit blocks: one "OK path (...)" or "FALLO path: ..." line per block.
void progress_edits(int ok, int bad, const char* report);

// What the model asked for in its last answer ("@a.zip:x @b.zip:y "): it is brought again after a restart.
void progress_asked(const char* want);

// The model's own notes (its reply to the checkpoint prompt).
void progress_notes(const char* model_reply);

// Writes the ledger to PROGRESS_FILE. 0 = ok.
int progress_save(void);

// Reads it back. Returns 1 when there was progress to read.
int progress_load(void);

// 1 when the ledger holds something.
int progress_has(void);

// Forgets the ledger in memory (a new chat). The saved file stays until the next save.
void progress_clear(void);

// The message that opens a fresh context. report = a message the agent was about to send (a failed edit report), or
// NULL. with_asked = 1 puts the last requested @files at the end, so the app brings them with the message.
// Returns its length (0 when there is nothing to resume).
int progress_resume_text(char* out, int max, const char* report, int with_asked);

// What the model is asked to write before the context restarts.
const char* progress_checkpoint_prompt(void);

#endif
