// Undo for what the AI writes (phase 11). Idea taken from claude-for-ps4's trash and restore (MIT, blue0x1): nothing the
// AI changes is lost. Before a file of the out folder is overwritten its old content is copied to /data/garlic_undo/N/,
// and /deshacer puts the last batch back (a file that was new is removed again). One batch = one /aplicar, one /guardar
// or one round of the agent. The last 20 batches are kept.
#ifndef UNDO_H
#define UNDO_H

#ifndef UNDO_DIR
#ifndef UNDO_DIR
#define UNDO_DIR "/data/garlic_undo"
#endif
#endif

void undo_begin(void);        // a new batch starts (its files are recorded by undo_record)
void undo_end(void);          // the batch is closed
int undo_active(void);        // 1 while a batch is open
void undo_record(const char* path);   // called right before path is written: keeps its old content

// Puts the last batch back. note gets the line for the screen. Returns the number of files restored or removed (0 = nothing to undo).
int undo_last(char* note, int note_max);

#endif
