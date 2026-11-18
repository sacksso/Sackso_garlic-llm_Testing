// The real network backend (see netget.h): an HTTP(S) GET through the console's libSceHttp + libSceSsl.
//
// The libraries are loaded and their functions looked up at run time (sceKernelLoadStartModule + sceKernelDlsym, the
// way input.c opens the mouse), so the build does not depend on which stubs the toolchain has: if something is
// missing the app still runs and net_last_error() says which step failed. Everything starts the first time a
// command needs the network, never at boot.
//
// Certificates: the first try verifies the server. If the handshake fails quickly (the console may have no usable
// CA list for a homebrew app) it tries once more without verification and remembers that for the session; the
// commands then tell the user. Create /data/garlic_web_strict.txt to forbid that fallback.
#include "netget.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

#define STRICT_FILE "/data/garlic_web_strict.txt"
#define UA "Mozilla/5.0 (PlayStation 4 13.52) AppleWebKit/605.1.15 (KHTML, like Gecko) GarlicLLM/1.2"
#define WALL_LIMIT_US 28000000ull   // the whole download, however slow the server is

typedef int (*FnNetInit)(void);
typedef int (*FnPoolCreate)(const char*, int, int);
typedef int (*FnSslInit)(size_t);
typedef int (*FnHttpInit)(int, int, size_t);
typedef int (*FnCreateTemplate)(int, const char*, int, int);
typedef int (*FnSetTimeout)(int, uint32_t);
typedef int (*FnSetInt)(int, int);
typedef int (*FnDisableOpt)(int, uint32_t);
typedef int (*FnCreateConn)(int, const char*, int);
typedef int (*FnCreateReq)(int, int, const char*, uint64_t);
typedef int (*FnAddHeader)(int, const char*, const char*, uint32_t);
typedef int (*FnSend)(int, const void*, size_t);
typedef int (*FnStatus)(int, int*);
typedef int (*FnRead)(int, void*, size_t);
typedef int (*FnDelete)(int);

static struct {
    FnNetInit net_init;
    FnPoolCreate pool_create;
    FnSslInit ssl_init;
    FnHttpInit http_init;
    FnCreateTemplate create_template;
    FnSetTimeout set_connect, set_resolve, set_send, set_recv;
    FnSetInt set_redirect;
    FnDisableOpt https_disable;
    FnCreateConn create_conn;
    FnCreateReq create_req;
    FnAddHeader add_header;
    FnSend send;
    FnStatus status;
    FnRead read;
    FnDelete del_req, del_conn, del_tmpl;
} F;

static int ready;          // 1 once the libraries are up, -1 if they could not start (the error stays in err)
static int http_ctx;
static int insecure;       // the last successful download needed the no-verification fallback
static char err[200] = "";

static void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, sizeof(err), fmt, ap);
    va_end(ap);
}

static int file_exists(const char* p) {
    int f = open(p, O_RDONLY);
    if (f < 0) return 0;
    close(f);
    return 1;
}

static int load_module(const char* name, char* why, int wm) {
    char path[96];
    snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    int h = (int)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    if (h <= 0) h = (int)sceKernelLoadStartModule(name, 0, NULL, 0, NULL, NULL);
    if (h <= 0) snprintf(why, wm, "%s did not load (0x%x)", name, (unsigned)h);
    return h;
}

static void* sym(int mod, const char* name) {
    void* a = NULL;
    sceKernelDlsym(mod, name, &a);
    return a;
}

static int setup(void) {
    if (ready) return ready > 0 ? 0 : -1;
    ready = -1;
    char why[120] = "";
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);   // as api.c does (a second load is harmless)
    int mnet = load_module("libSceNet.sprx", why, sizeof(why));
    if (mnet <= 0) return fail("%s", why), -1;
    int mssl = load_module("libSceSsl.sprx", why, sizeof(why));
    if (mssl <= 0) return fail("%s", why), -1;
    int mhttp = load_module("libSceHttp.sprx", why, sizeof(why));
    if (mhttp <= 0) return fail("%s", why), -1;

    F.net_init = (FnNetInit)sym(mnet, "sceNetInit");
    F.pool_create = (FnPoolCreate)sym(mnet, "sceNetPoolCreate");
    F.ssl_init = (FnSslInit)sym(mssl, "sceSslInit");
    F.http_init = (FnHttpInit)sym(mhttp, "sceHttpInit");
    F.create_template = (FnCreateTemplate)sym(mhttp, "sceHttpCreateTemplate");
    F.set_connect = (FnSetTimeout)sym(mhttp, "sceHttpSetConnectTimeOut");
    F.set_resolve = (FnSetTimeout)sym(mhttp, "sceHttpSetResolveTimeOut");
    F.set_send = (FnSetTimeout)sym(mhttp, "sceHttpSetSendTimeOut");
    F.set_recv = (FnSetTimeout)sym(mhttp, "sceHttpSetRecvTimeOut");
    F.set_redirect = (FnSetInt)sym(mhttp, "sceHttpSetAutoRedirect");
    F.https_disable = (FnDisableOpt)sym(mhttp, "sceHttpsDisableOption");
    F.create_conn = (FnCreateConn)sym(mhttp, "sceHttpCreateConnectionWithURL");
    F.create_req = (FnCreateReq)sym(mhttp, "sceHttpCreateRequestWithURL");
    F.add_header = (FnAddHeader)sym(mhttp, "sceHttpAddRequestHeader");
    F.send = (FnSend)sym(mhttp, "sceHttpSendRequest");
    F.status = (FnStatus)sym(mhttp, "sceHttpGetStatusCode");
    F.read = (FnRead)sym(mhttp, "sceHttpReadData");
    F.del_req = (FnDelete)sym(mhttp, "sceHttpDeleteRequest");
    F.del_conn = (FnDelete)sym(mhttp, "sceHttpDeleteConnection");
    F.del_tmpl = (FnDelete)sym(mhttp, "sceHttpDeleteTemplate");
    const char* missing = !F.net_init ? "sceNetInit" : !F.pool_create ? "sceNetPoolCreate" : !F.ssl_init ? "sceSslInit" :
        !F.http_init ? "sceHttpInit" : !F.create_template ? "sceHttpCreateTemplate" : !F.create_conn ? "sceHttpCreateConnectionWithURL" :
        !F.create_req ? "sceHttpCreateRequestWithURL" : !F.send ? "sceHttpSendRequest" : !F.status ? "sceHttpGetStatusCode" :
        !F.read ? "sceHttpReadData" : !F.del_req ? "sceHttpDeleteRequest" : !F.del_conn ? "sceHttpDeleteConnection" :
        !F.del_tmpl ? "sceHttpDeleteTemplate" : NULL;
    if (missing) return fail("symbol %s not found in the system libraries", missing), -1;

    int r = F.net_init();   // (api.c may have done it already: that error is not a problem)
    (void)r;
    int pool = F.pool_create("garlic_http", 256 * 1024, 0);
    if (pool < 0) return fail("sceNetPoolCreate 0x%08X", (unsigned)pool), -1;
    int ssl = F.ssl_init(512 * 1024);
    if (ssl < 0) return fail("sceSslInit 0x%08X", (unsigned)ssl), -1;
    http_ctx = F.http_init(pool, ssl, 512 * 1024);
    if (http_ctx < 0) return fail("sceHttpInit 0x%08X", (unsigned)http_ctx), -1;
    ready = 1;
    return 0;
}

static uint64_t now_us(void) { return (uint64_t)sceKernelGetProcessTime(); }

// One download. verify=0 turns the certificate checks off. Returns bytes (>= 0) or < 0 (err filled).
static int get_once(const char* url, char* out, int max, int* status, int verify) {
    int tmpl = F.create_template(http_ctx, UA, 2 /* HTTP/1.1 */, 1);
    if (tmpl < 0) return fail("sceHttpCreateTemplate 0x%08X", (unsigned)tmpl), -1;
    if (F.set_resolve) F.set_resolve(tmpl, 10000000);
    if (F.set_connect) F.set_connect(tmpl, 10000000);
    if (F.set_send) F.set_send(tmpl, 10000000);
    if (F.set_recv) F.set_recv(tmpl, 15000000);
    if (F.set_redirect) F.set_redirect(tmpl, 1);
    if (!verify && F.https_disable) F.https_disable(tmpl, 0x3f);
    int ret = -1;
    int conn = F.create_conn(tmpl, url, 0), req = -1;
    if (conn < 0) {
        fail("connection 0x%08X (bad address?)", (unsigned)conn);
        goto done;
    }
    req = F.create_req(conn, 0 /* GET */, url, 0);
    if (req < 0) {
        fail("request 0x%08X", (unsigned)req);
        goto done;
    }
    if (F.add_header) {
        F.add_header(req, "Accept", "text/html,application/json,text/plain;q=0.9,*/*;q=0.5", 0);
        F.add_header(req, "Accept-Language", "en,es;q=0.8", 0);
    }
    int r = F.send(req, NULL, 0);
    if (r < 0) {
        fail("send 0x%08X (no internet, DNS, or TLS)", (unsigned)r);
        goto done;
    }
    int code = 0;
    F.status(req, &code);
    *status = code;
    int total = 0;
    uint64_t t0 = now_us();
    for (;;) {
        if (total >= max - 1) break;   // the caller's buffer is full: the rest is dropped
        int n = F.read(req, out + total, (size_t)(max - 1 - total));
        if (n < 0) {
            if (total > 0) break;   // keep what came
            fail("read 0x%08X", (unsigned)n);
            goto done;
        }
        if (n == 0) break;
        total += n;
        if (now_us() - t0 > WALL_LIMIT_US) break;
    }
    out[total] = 0;
    if (total >= 2 && (unsigned char)out[0] == 0x1f && (unsigned char)out[1] == 0x8b) {
        fail("the server sent a gzip body, not supported");
        goto done;
    }
    ret = total;
done:
    if (req >= 0) F.del_req(req);
    if (conn >= 0) F.del_conn(conn);
    F.del_tmpl(tmpl);
    return ret;
}

int net_get(const char* url, char* out, int max, int* status) {
    out[0] = 0;
    *status = 0;
    err[0] = 0;
    if (setup()) return -1;
    uint64_t t0 = now_us();
    int https = !strncmp(url, "https://", 8);
    int n = get_once(url, out, max, status, insecure ? 0 : 1);
    if (n < 0 && https && !insecure && !file_exists(STRICT_FILE) && now_us() - t0 < 6000000) {
        char first[200];   // it failed fast: probably the certificate check. Try once without it.
        snprintf(first, sizeof(first), "%s", err);
        n = get_once(url, out, max, status, 0);
        if (n >= 0) insecure = 1;
        else fail("%s", first);
    }
    return n;
}

const char* net_last_error(void) { return err[0] ? err : "unknown network error"; }

const char* net_backend(void) { return ready > 0 ? (insecure ? "libSceHttp (TLS not verified)" : "libSceHttp") : "libSceHttp (not started yet)"; }
