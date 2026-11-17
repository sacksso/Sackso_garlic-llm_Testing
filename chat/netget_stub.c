// Weak placeholders for the network backend (see netget.h): the app builds and the commands answer with a clear
// message until the real backend is added.
#include "netget.h"

__attribute__((weak)) int net_get(const char* url, char* out, int max, int* status) {
    (void)url;
    (void)max;
    out[0] = 0;
    *status = 0;
    return -1;
}

__attribute__((weak)) const char* net_last_error(void) { return "no network backend in this build (netget_stub.c)"; }

__attribute__((weak)) const char* net_backend(void) { return "none"; }
