// USB keyboard and mouse. See input.h.
#include "input.h"

#include <stdarg.h>
#include <stdbool.h>   // orbis/Keyboard.h uses bool without including it
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <orbis/Keyboard.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

extern void say(const char* fmt, ...);   // main.c's log

// ---- keyboard -----------------------------------------------------------------------------------------------------

static int kb_handle = -1;
#define MAX_HELD 8
static struct { uint16_t code; double down, last; } held[MAX_HELD];
static int n_held;
static InKey queue[16];
static int q_head, q_tail;

static double clock_s(void) { return sceKernelGetProcessTime() / 1e6; }

static void push(int code, int ch, unsigned mods) {
    int next = (q_tail + 1) % 16;
    if (next == q_head) return;
    queue[q_tail].code = code;
    queue[q_tail].ch = ch;
    queue[q_tail].mods = mods;
    q_tail = next;
}

// Everything but the modifiers repeats while held (like the psbrowser's text fields).
static void poll_keyboard(void) {
    if (kb_handle < 0) return;
    OrbisKeyboardData d;
    memset(&d, 0, sizeof(d));
    if (sceKeyboardReadState(kb_handle, &d) < 0) return;
    int n = d.nkeys;
    if (n < 0) n = 0;
    if (n > 32) n = 32;
    double t = clock_s();
    uint16_t cur[32];
    int nc = 0;
    for (int i = 0; i < n; i++)
        if (d.keycodes[i]) cur[nc++] = d.keycodes[i];
    for (int i = 0; i < nc; i++) {
        int h = -1;
        for (int j = 0; j < n_held; j++)
            if (held[j].code == cur[i]) h = j;
        int fire = 0;
        if (h < 0) {
            if (n_held < MAX_HELD) {
                held[n_held].code = cur[i];
                held[n_held].down = held[n_held].last = t;
                n_held++;
            }
            fire = 1;
        } else if (cur[i] != 40 && cur[i] != 41 && cur[i] != 43 && cur[i] != 88 && t - held[h].down > 0.4 &&
                   t - held[h].last > 0.055) {   // (Enter, Esc and Tab do not repeat)
            held[h].last = t;
            fire = 1;
        }
        if (!fire) continue;
        int ch = 0;
        OrbisKeyboardKey2Char k2c;
        memset(&k2c, 0, sizeof(k2c));
        if (sceKeyboardGetKey2Char(kb_handle, 0, d.locks, d.mods, cur[i], &k2c) >= 0 && k2c.ok && k2c.keycode >= 32 &&
            k2c.keycode < 127)
            ch = k2c.keycode;
        push(cur[i], ch, d.mods);
    }
    for (int j = 0; j < n_held;) {   // released keys leave the list
        int still = 0;
        for (int i = 0; i < nc; i++)
            if (cur[i] == held[j].code) still = 1;
        if (still) j++;
        else held[j] = held[--n_held];
    }
}

int input_key(InKey* k) {
    if (q_head == q_tail) poll_keyboard();
    if (q_head == q_tail) return 0;
    *k = queue[q_head];
    q_head = (q_head + 1) % 16;
    return 1;
}

int input_has_keyboard(void) { return kb_handle >= 0; }

// ---- mouse --------------------------------------------------------------------------------------------------------
// orbis/Mouse.h of the toolchain are empty stubs: the functions are looked up at run time (as in the psbrowser).

typedef int (*FnMouseInit)(void);
typedef int (*FnMouseOpen)(int, int, int, void*);
typedef int (*FnMouseRead)(int, void*, int);
static FnMouseInit mouse_init_fn;
static FnMouseOpen mouse_open_fn;
static FnMouseRead mouse_read_fn;
static int mouse_handle = -1;

// SceMouseData as the psbrowser reads it (40 bytes); confirmed working there.
typedef struct {
    uint64_t timestamp;
    uint8_t connected;
    uint8_t pad_[3];
    uint32_t buttons;
    int32_t x, y, wheel, tilt;
    uint8_t reserve[8];
} RawMouse;

int input_has_mouse(void) { return mouse_handle >= 0; }

int input_mouse(InMouse* m) {
    memset(m, 0, sizeof(*m));
    if (mouse_handle < 0) return 0;
    static unsigned last_buttons;
    int got = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t buf[64];
        memset(buf, 0, sizeof(buf));
        if (mouse_read_fn(mouse_handle, buf, 1) <= 0) break;
        const RawMouse* r = (const RawMouse*)buf;
        m->dx += r->x;
        m->dy += r->y;
        m->wheel += r->wheel;
        m->buttons = r->buttons;
        got = 1;
    }
    if (m->buttons != last_buttons) got = 1;
    last_buttons = m->buttons;
    return got;
}

static void mouse_start(int user_id) {
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOUSE);
    uint32_t mod = 0;
    const char* paths[] = {"/system/common/lib/libSceMouse.sprx", "libSceMouse.sprx"};
    for (int i = 0; i < 2 && (int)mod <= 0; i++) mod = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, NULL);
    if ((int)mod <= 0) {
        say("mouse: libSceMouse.sprx did not load (0x%x)", (unsigned)mod);
        return;
    }
    void* a = NULL;
    sceKernelDlsym((int)mod, "sceMouseInit", &a);
    mouse_init_fn = (FnMouseInit)a;
    a = NULL;
    sceKernelDlsym((int)mod, "sceMouseOpen", &a);
    mouse_open_fn = (FnMouseOpen)a;
    a = NULL;
    sceKernelDlsym((int)mod, "sceMouseRead", &a);
    mouse_read_fn = (FnMouseRead)a;
    if (!mouse_init_fn || !mouse_open_fn || !mouse_read_fn) {
        say("mouse: symbols not found");
        return;
    }
    mouse_init_fn();
    const int users[] = {user_id, 0xFE, -1, 0};   // 0xFE: the "system" user; the psbrowser tries the same list
    for (int i = 0; i < 4; i++) {
        int h = mouse_open_fn(users[i], 0, 0, NULL);
        if (h >= 0) {
            mouse_handle = h;
            say("mouse open (user %d)", users[i]);
            return;
        }
    }
    say("mouse: no user id worked");
}

void input_init(int user_id) {
    if (sceSysmoduleLoadModule(ORBIS_SYSMODULE_KEYBOARD) == 0 && sceKeyboardInit() == 0) {
        kb_handle = sceKeyboardOpen(user_id, 0, 0, NULL);
        say("keyboard %s", kb_handle >= 0 ? "open" : "not available");
    } else {
        say("keyboard: module did not start");
    }
    mouse_start(user_id);
}
