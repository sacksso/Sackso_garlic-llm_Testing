// Conversation log (see chatlog.h). Plain open/write so a crash loses at most the line being written.
#include "chatlog.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHAT_DIR "/data/chats"
#define COUNTER_FILE CHAT_DIR "/counter.txt"
#define OFF_FILE "/data/garlic_chatlog.txt"

static char log_path[64];
static int log_on = -1;   // -1: not read yet
static int turn_n;

static int enabled(void) {
    if (log_on < 0) {
        char c = '1';
        int f = open(OFF_FILE, O_RDONLY);
        if (f >= 0) {
            if (read(f, &c, 1) != 1) c = '1';
            close(f);
        }
        log_on = c != '0';
    }
    return log_on;
}

void chatlog_begin(void) {
    log_path[0] = 0;
    turn_n = 0;
    if (!enabled()) return;
    mkdir(CHAT_DIR, 0777);
    int n = 0;
    char buf[16] = {0};
    int f = open(COUNTER_FILE, O_RDONLY);
    if (f >= 0) {
        int r = read(f, buf, sizeof(buf) - 1);
        if (r > 0) n = atoi(buf);
        close(f);
    }
    n++;
    f = open(COUNTER_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f >= 0) {
        int len = snprintf(buf, sizeof(buf), "%d\n", n);
        write(f, buf, len);
        close(f);
    }
    snprintf(log_path, sizeof(log_path), CHAT_DIR "/chat_%04d.jsonl", n);
}

// Writes s as a JSON string body (quotes, backslashes and control characters escaped; UTF-8 is kept as it is).
static void put_escaped(int f, const char* s) {
    char out[512];
    int o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (o > (int)sizeof(out) - 8) {
            write(f, out, o);
            o = 0;
        }
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\', out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\', out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\', out[o++] = 't';
        } else if (c < 0x20) {
            o += snprintf(out + o, 8, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    if (o) write(f, out, o);
}

void chatlog_add(const char* role, const char* text, int cut) {
    if (!log_path[0] || !text) return;
    int f = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (f < 0) return;
    char head[96];
    int h = snprintf(head, sizeof(head), "{\"n\":%d,\"role\":\"%s\",\"cut\":%d,\"text\":\"", turn_n++, role, cut ? 1 : 0);
    write(f, head, h);
    put_escaped(f, text);
    write(f, "\"}\n", 3);
    close(f);
}
