// A simple file explorer for the chat (phase 7): pick one or more files of /data/garlic_files (folders and the inside of
// .zip files too) and get them back as "@name @out/x.c @repo.zip:chat/main.c " for the text box. Everything here is
// logic (listing, moving, picking, mouse hit tests) so it can be tested on a PC; main.c runs it and ui.c draws it.
#ifndef FILEPICK_H
#define FILEPICK_H

#ifndef FILES_DIR
#define FILES_DIR "/data/garlic_files"
#endif

#define FP_MAX 300       // entries shown per folder
#define FP_PICK_MAX 8    // files that can be picked at once
#define FP_NAME 64

typedef struct {
    char name[FP_NAME];
    int is_dir;          // a folder or a folder inside a zip
    int is_zip;          // a .zip file (enter it to see its files, or pick it whole: its map)
    long size;
} FpEnt;

typedef struct {
    char dir[128];       // folder under FILES_DIR ("" = the top)
    char zip[96];        // the zip being browsed, relative to FILES_DIR ("" = none)
    char zdir[100];      // folder inside the zip, with its trailing '/' ("" = the top of the zip)
    int zroot;           // length of the repo folder every path of the zip starts with ("repo-main/"), 0 when none
    FpEnt ent[FP_MAX];
    int n, cur, top;
    int truncated;       // more entries than FP_MAX
    char picked[FP_PICK_MAX][140];   // the "@..." tokens
    int npicked;
    char msg[96];        // a short message for the screen ("" = none)
} FilePick;

// Layout (pixels, 1920x1080): the panel, the list rows and the three buttons. ui.c draws with the same numbers.
#define FP_X 260
#define FP_Y 116
#define FP_W 1400
#define FP_H 860
#define FP_LIST_Y (FP_Y + 96)
#define FP_ROW_H 48
#define FP_ROWS 12
#define FP_BTN_Y (FP_Y + FP_H - 76)
#define FP_BTN_H 56
#define FP_UP_X (FP_X + 30)
#define FP_UP_W 190
#define FP_DONE_W 280
#define FP_DONE_X (FP_X + FP_W - 30 - FP_DONE_W)
#define FP_CANCEL_W 210
#define FP_CANCEL_X (FP_DONE_X - 24 - FP_CANCEL_W)

void fp_open(FilePick* f);            // starts at the top of FILES_DIR, nothing picked
void fp_move(FilePick* f, int d);     // cursor up/down by d rows (clamped, scrolls)
void fp_enter(FilePick* f);           // folder / zip: go in; file: pick or unpick it
void fp_toggle(FilePick* f);          // pick or unpick the entry under the cursor (a zip: the whole zip)
void fp_up(FilePick* f);              // one folder up (out of a zip too)
int fp_is_picked(const FilePick* f, int index);

// The picked files as "@a @b:c " (NUL-terminated). Returns the bytes written (0 = nothing picked).
int fp_result(const FilePick* f, char* out, int max);

// What is at pixel (x, y): the index of an entry (>= 0), or one of these.
#define FP_HIT_NONE (-1)
#define FP_HIT_UP (-2)
#define FP_HIT_CANCEL (-3)
#define FP_HIT_DONE (-4)
#define FP_HIT_CHECK 1000   // FP_HIT_CHECK + index: the check box of an entry
int fp_hit(const FilePick* f, int x, int y);

// A click on an entry (or its check box): folders and zips are entered, files picked. Returns 1 when it did something.
int fp_click(FilePick* f, int hit);

#endif
