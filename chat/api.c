// OpenAI-compatible HTTP API, server part on the console (see api.h). One thread accepts the connections and
// reads their requests; the chat requests go to the main loop one at a time (api_run.inc), which owns the model.
// The sockets are the console's sceNet calls, declared here as the OpenOrbis headers do not have them all.
// Nothing here runs until api_enable(1): no network library, no socket, no thread.
#include "api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

#include "api_compat.h"
#include "api_proto.h"
#include "ui.h"

#define API_ASSETS "/app0/assets"
#define API_KEY_FILE "/data/garlic_api_key.txt"   // optional: if it holds a key, requests need "Authorization: Bearer <key>"
#define API_PORT_FILE "/data/garlic_api_port.txt"   // optional: the port to listen on (a game may expect 5001 or 11434)
#define API_MAX_BODY (1 << 20)

// Socket constants that the SDK's headers do not name (the BSD values the console's libSceNet uses)
#define API_SOL_SOCKET 0xffff
#define API_SO_REUSEADDR 0x0004
#define API_SO_RCVTIMEO 0x1006
#define API_SO_SNDTIMEO 0x1005
#define API_IPPROTO_TCP 6
#define API_IPPROTO_UDP 17

typedef struct {                // the BSD sockaddr_in: 16 bytes, the same size as OrbisNetSockaddr
    uint8_t len, family;
    uint16_t port;              // network byte order
    uint32_t addr;
    char zero[8];
} ApiSockaddrIn;
_Static_assert(sizeof(ApiSockaddrIn) == sizeof(OrbisNetSockaddr), "sockaddr size");

static OrbisPthreadMutex api_mu;
static ApiJob* queued;          // waiting for the main loop
static int busy;                // a chat job is queued or running (api_done clears it)
static char model_name[64] = "Garlic";
static char* key;               // the API key, or NULL
static OrbisPthread api_th;
static int ready, have_thread;  // the network library, the mutex and the key are set up (once); the thread exists
static volatile int on;         // the API is on: connections are served
static OrbisNetId listen_fd = -1;   // the listening socket (changes under api_mu)
static volatile int accepting;  // the server thread is waiting for a connection on it (in sceNetAccept)
static char address[32];        // api_address()

static int ctx_tokens = 16384;   // the context window, as the information endpoints report it
static int port = API_PORT;      // the port in use (api_port): API_PORT_FILE, read each time the API is turned on

void api_set_model(const char* name) {
    snprintf(model_name, sizeof(model_name), "%s", name);
}

void api_set_ctx(int tokens) {
    if (tokens > 0) ctx_tokens = tokens;
}

int api_port(void) { return port; }

// The port from API_PORT_FILE (1024..65535), else API_PORT.
static void load_port(void) {
    port = API_PORT;
    size_t n = 0;
    uint8_t* d = read_all(API_PORT_FILE, &n);
    if (d) {
        char b[16] = {0};
        memcpy(b, d, n < 15 ? n : 15);
        int p = atoi(b);
        if (p >= 1024 && p <= 65535) port = p;
        free(d);
    }
}

// Saves the port for the next time the API is turned on. 0 = done, -1 = not a usable port or no file.
int api_set_port(int p) {
    if (p < 1024 || p > 65535) return -1;
    char b[16];
    int l = snprintf(b, sizeof(b), "%d\n", p);
    int f = open(API_PORT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (f < 0) return -1;
    int w = (int)write(f, b, (size_t)l);
    close(f);
    return w == l ? 0 : -1;
}

const char* api_model(void) { return model_name; }

int api_send(int fd, const char* p, size_t n) {
    while (n) {
        int w = sceNetSend(fd, p, n, 0);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void reply(int fd, int status, const char* reason, const char* ctype, const char* body, size_t n) {
    char head[512];
    int h = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     status, reason, ctype, n);
    if (api_send(fd, head, h) == 0) api_send(fd, body, n);
}

static void reply_kind_error(int fd, int kind, int status, const char* reason, const char* message, const char* type,
                             const char* code) {
    Sb s;
    sb_init(&s);
    api_out_http_error(kind, &s, message, type, code);
    reply(fd, status, reason, "application/json", s.p, s.n);
    sb_free(&s);
}

static void reply_json_error(int fd, int status, const char* reason, const char* message, const char* type,
                             const char* code) {
    reply_kind_error(fd, API_K_OPENAI_CHAT, status, reason, message, type, code);
}

ApiJob* api_take(void) {
    if (!ready) return NULL;   // (the API was never turned on)
    scePthreadMutexLock(&api_mu);
    ApiJob* j = queued;
    queued = NULL;
    scePthreadMutexUnlock(&api_mu);
    return j;
}

void api_done(ApiJob* job) {
    scePthreadMutexLock(&api_mu);
    busy = 0;
    scePthreadMutexUnlock(&api_mu);
    job->done = 1;
}

// the header value of Authorization must be "Bearer <key>"
static int authorized(const HttpRequest* r) {
    if (!key) return 1;
    size_t k = strlen(key);
    return strncmp(r->auth, "Bearer ", 7) == 0 && strlen(r->auth + 7) == k && !memcmp(r->auth + 7, key, k);
}

static int read_request(int fd, char** buf_out, size_t* len_out, HttpRequest* req) {
    size_t cap = 16384, len = 0;
    char* buf = malloc(cap);
    if (!buf) return -1;
    int r = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= API_MAX_BODY + 16384) break;
            char* nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        int n = sceNetRecv(fd, buf + len, cap - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
        r = api_http_parse(buf, len, req);
        if (r != 0) break;
    }
    if (r == 1) {   // the body points into buf: keep the buffer until the reply is written
        *buf_out = buf;
        *len_out = len;
        return 1;
    }
    free(buf);
    return r == 0 ? 0 : -1;
}

static void serve_gen(int fd, HttpRequest* req, int kind) {
    JNode* pool = malloc(sizeof(JNode) * API_MAX_NODES);
    GenRequest g;
    const char* err = NULL;
    if (!pool || api_gen_parse(kind, req->body, req->body_len, pool, API_MAX_NODES, &g, &err)) {
        if (pool) api_gen_free(&g);
        free(pool);
        reply_kind_error(fd, kind, 400, "Bad Request", err ? err : "Out of memory.", "invalid_request_error", "invalid_body");
        return;
    }
    int stream = g.stream;
    api_gen_free(&g);
    free(pool);
    ApiJob* job = calloc(1, sizeof(ApiJob));
    char* body = malloc(req->body_len + 1);
    if (!job || !body) {
        free(job);
        free(body);
        reply_json_error(fd, 500, "Internal Server Error", "Out of memory.", "server_error", NULL);
        return;
    }
    memcpy(body, req->body, req->body_len);
    body[req->body_len] = 0;
    job->fd = fd;
    job->kind = kind;
    job->body = body;
    job->body_len = req->body_len;
    scePthreadMutexLock(&api_mu);
    int full = busy;
    if (!full) {
        busy = 1;
        queued = job;
    }
    scePthreadMutexUnlock(&api_mu);
    if (full) {
        free(body);
        free(job);
        reply_kind_error(fd, kind, 503, "Service Unavailable", "The model is busy with another request.", "server_error",
                         "busy");
        return;
    }
    say("api: request %s, %zu bytes, %s", req->path, req->body_len, stream ? "streamed" : "whole reply");
    // the main loop writes the reply to fd; the connection is closed here once it is written
    while (!job->done) sceKernelUsleep(5000);
    free(job->body);
    free(job);
}

static void handle(int fd) {
    struct {
        long sec, usec;
    } tmo = {20, 0};
    sceNetSetsockopt(fd, API_SOL_SOCKET, API_SO_RCVTIMEO, &tmo, sizeof(tmo));
    sceNetSetsockopt(fd, API_SOL_SOCKET, API_SO_SNDTIMEO, &tmo, sizeof(tmo));
    char* buf = NULL;
    size_t len = 0;
    HttpRequest req;
    int r = read_request(fd, &buf, &len, &req);
    if (r != 1) {   // malformed, too big, or cut off before the end
        reply_json_error(fd, 400, "Bad Request", "Malformed or incomplete request.", "invalid_request_error", NULL);
        return;
    }
    int kind = api_kind_of(req.method, req.path);
    const int is_get = !strcmp(req.method, "GET");
    const int page = is_get && (!strcmp(req.path, "/index.html") || !strcmp(req.path, "/chat") ||
                                (!strcmp(req.path, "/") && req.wants_html));
    if (!strcmp(req.method, "OPTIONS")) {
        const char* h = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        api_send(fd, h, strlen(h));
    } else if (page) {   // the chat page: no key needed to load it (it asks for the key itself)
        size_t n = 0;
        uint8_t* html = read_all(API_ASSETS "/index.html", &n);
        if (html) reply(fd, 200, "OK", "text/html; charset=utf-8", (const char*)html, n);
        else reply_json_error(fd, 404, "Not Found", "The chat page is missing.", "invalid_request_error", NULL);
        free(html);
    } else if (is_get && !strcmp(req.path, "/health")) {
        reply(fd, 200, "OK", "application/json", "{\"status\":\"ok\"}", 15);
    } else if (!authorized(&req)) {
        reply_kind_error(fd, kind >= 0 ? kind : API_K_OPENAI_CHAT, 401, "Unauthorized", "Missing or wrong API key.",
                         "invalid_request_error", "invalid_api_key");
    } else if (is_get && (!strcmp(req.path, "/v1/models") || !strcmp(req.path, "/models"))) {
        Sb s;
        sb_init(&s);
        api_json_models(&s, model_name, 0);
        reply(fd, 200, "OK", "application/json", s.p, s.n);
        sb_free(&s);
    } else if (is_get && (!strcmp(req.path, "/openapi.json") || !strcmp(req.path, "/"))) {
        size_t n = 0;
        uint8_t* spec = read_all(API_ASSETS "/openapi.json", &n);
        if (spec) reply(fd, 200, "OK", "application/json", (const char*)spec, n);
        else reply_json_error(fd, 404, "Not Found", "The API description is missing.", "invalid_request_error", NULL);
        free(spec);
    } else if (kind >= 0) {
        serve_gen(fd, &req, kind);
        free(buf);
        return;   // (serve_gen keeps the connection until the main loop has written the reply)
    } else {
        Sb s;
        sb_init(&s);
        if (api_info(req.path, model_name, ctx_tokens, &s) && s.p) reply(fd, 200, "OK", "application/json", s.p, s.n);   // (Kobold and Ollama's small endpoints)
        else reply_json_error(fd, 404, "Not Found", "No such endpoint.", "invalid_request_error", NULL);
        sb_free(&s);
    }
    free(buf);
}

// The API is off: its port is closed, unless the server thread is waiting on the socket (then the thread closes it
// when its wait ends). The socket is never closed under a thread that is using it.
static void close_if_off(void) {
    scePthreadMutexLock(&api_mu);
    if (!on && !accepting && listen_fd >= 0) {
        sceNetSocketClose(listen_fd);
        listen_fd = -1;
    }
    scePthreadMutexUnlock(&api_mu);
}

static void* api_main(void* arg) {
    (void)arg;
    for (;;) {
        scePthreadMutexLock(&api_mu);
        OrbisNetId l = on ? listen_fd : -1;
        accepting = l >= 0;
        scePthreadMutexUnlock(&api_mu);
        if (l < 0) {   // the API is off: nothing to wait for
            sceKernelUsleep(100000);
            continue;
        }
        ApiSockaddrIn peer;
        OrbisNetSocklen_t plen = sizeof(peer);
        int fd = sceNetAccept(l, (OrbisNetSockaddr*)&peer, &plen);
        accepting = 0;
        if (fd >= 0) {
            if (on) handle(fd);   // (a connection that came in after the API was turned off gets no answer)
            sceNetSocketClose(fd);
        }
        close_if_off();
        if (fd < 0) sceKernelUsleep(100000);   // (an error, or api_enable ended the wait to turn the API off)
    }
    return NULL;
}

// The network library (as the SDK's net_http sample starts it), the mutex and the optional key: once, the first
// time the API is turned on.
static int api_setup(void) {
    if (ready) return 0;
    int r = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    if (r < 0) {
        say("api: network module 0x%08X; the API stays off", r);
        return -1;
    }
    r = sceNetInit();
    if (r < 0) {
        say("api: sceNetInit 0x%08X; the API stays off", r);
        return -1;
    }
    scePthreadMutexInit(&api_mu, NULL, "garlic_api");
    size_t n = 0;
    uint8_t* kd = read_all(API_KEY_FILE, &n);
    if (kd) {
        while (n && (kd[n - 1] == '\n' || kd[n - 1] == '\r' || kd[n - 1] == ' ')) n--;
        if (n) {
            key = malloc(n + 1);
            if (key) {
                memcpy(key, kd, n);
                key[n] = 0;
            }
        }
        free(kd);
    }
    ready = 1;
    return 0;
}

// A socket listening on API_PORT, on any of the console's addresses; -1 if it cannot (logged).
static OrbisNetId api_listen(void) {
    OrbisNetId s = sceNetSocket("garlic_api", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, API_IPPROTO_TCP);
    if (s < 0) {
        say("api: no socket (%d); the API stays off", s);
        return -1;
    }
    int one = 1;
    sceNetSetsockopt(s, API_SOL_SOCKET, API_SO_REUSEADDR, &one, sizeof(one));
    ApiSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.len = sizeof(a);
    a.family = ORBIS_NET_AF_INET;
    a.port = sceNetHtons((uint16_t)port);
    a.addr = 0;   // any address
    if (sceNetBind(s, (const OrbisNetSockaddr*)&a, sizeof(a)) || sceNetListen(s, 4)) {
        say("api: cannot listen on port %d; the API stays off", port);
        sceNetSocketClose(s);
        return -1;
    }
    return s;
}

// The console's address on the network, for the screen: the one the system would send from to reach another
// network. Connecting a UDP socket only makes the system choose that address; nothing is sent. Without a network
// the address stays ":8080".
static void find_address(void) {
    snprintf(address, sizeof(address), ":%d", port);
    OrbisNetId s = sceNetSocket("garlic_api_addr", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, API_IPPROTO_UDP);
    if (s < 0) return;
    static const uint8_t far[4] = {192, 0, 2, 1};   // (an address reserved for documentation: never a real host)
    ApiSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.len = sizeof(a);
    a.family = ORBIS_NET_AF_INET;
    a.port = sceNetHtons(9);
    memcpy(&a.addr, far, 4);
    OrbisNetSocklen_t len = sizeof(a);
    if (sceNetConnect(s, (const OrbisNetSockaddr*)&a, sizeof(a)) == 0 &&
        sceNetGetsockname(s, (OrbisNetSockaddr*)&a, &len) == 0 && a.addr) {
        const uint8_t* b = (const uint8_t*)&a.addr;   // (network byte order: the bytes as they are written)
        snprintf(address, sizeof(address), "%u.%u.%u.%u:%d", b[0], b[1], b[2], b[3], port);
    }
    sceNetSocketClose(s);
}

int api_enabled(void) { return on; }

const char* api_address(void) { return address; }

int api_enable(int want) {
    if (!want) {
        if (!on) return 0;
        scePthreadMutexLock(&api_mu);
        OrbisNetId l = listen_fd;
        on = 0;
        scePthreadMutexUnlock(&api_mu);
        // the server thread leaves its wait for a connection, and the port is closed (close_if_off)
        if (l >= 0) sceNetSocketAbort(l, 0);
        for (int i = 0; i < 40 && accepting; i++) sceKernelUsleep(5000);
        close_if_off();
        say("api: off");
        return 0;
    }
    if (on) return 0;
    if (api_setup()) return -1;
    load_port();
    scePthreadMutexLock(&api_mu);
    int l = listen_fd;   // (still open only if the server thread has been waiting on it since the API went off)
    if (l < 0) l = listen_fd = api_listen();
    if (l >= 0) on = 1;
    scePthreadMutexUnlock(&api_mu);
    if (l < 0) return -1;
    if (!have_thread) {
        OrbisPthreadAttr attr;
        scePthreadAttrInit(&attr);
        scePthreadAttrSetstacksize(&attr, 256 << 10);
        if (scePthreadCreate(&api_th, &attr, api_main, NULL, "garlic_api")) {
            say("api: no thread; the API stays off");
            on = 0;
            close_if_off();
            return -1;
        }
        have_thread = 1;
    }
    find_address();
    say("api: OpenAI-compatible API on %s%s", address, key ? " (API key required)" : "");
    return 0;
}
