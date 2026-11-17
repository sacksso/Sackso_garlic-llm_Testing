// USB keyboard and mouse (libSceKeyboard / libSceMouse), ported from the psbrowser (app/mouse.cpp + the OpenOrbis
// 'keyboard' sample), where both work on the console. Nothing here blocks: poll once per frame.
#ifndef INPUT_H
#define INPUT_H

typedef struct {
    int code;            // USB HID usage code (ORBIS_KEYBOARD_KEY_*, letters 4..29, digits 30..39)
    int ch;              // printable ASCII (32..126) from the console's key map, else 0
    unsigned mods;       // ORBIS_KEYBOARD_MOD_*
} InKey;

typedef struct {
    int dx, dy, wheel;   // accumulated since the last call
    unsigned buttons;    // bit 0 left, bit 1 right, bit 2 middle
} InMouse;

// Call once, after the user id is known. Failures are only logged (say()): the app works with the pad alone.
void input_init(int user_id);

// The next key press (auto-repeat included) or 0 when there is none. Call in a loop until it returns 0.
int input_key(InKey* k);

// Mouse movement since the last call; returns 1 when something changed.
int input_mouse(InMouse* m);

// 1 when a USB keyboard / mouse was opened.
int input_has_keyboard(void);
int input_has_mouse(void);

#endif
