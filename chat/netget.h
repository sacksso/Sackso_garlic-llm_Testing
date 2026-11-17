// The network backend for the /gh, /url and /web commands (webcmd.c). It must do one thing: an HTTP(S) GET.
// netget_stub.c holds weak versions that only report "no backend"; a file with the real functions (the port of the
// psbrowser's netclient, libSceHttp + libSceSsl) replaces them at link time with no other change.
#ifndef NETGET_H
#define NETGET_H

// GET url (http or https, redirects followed) and put the body in out, NUL-terminated, at most max-1 bytes.
// Returns the number of body bytes (>= 0) or < 0 on failure (net_last_error() says why). *status = the HTTP status
// (0 when unknown). Must time out by itself (about 15 s): the chat screen waits for it.
int net_get(const char* url, char* out, int max, int* status);

// A short text for the last failure.
const char* net_last_error(void);

// Name of the backend for the log ("none" for the stub).
const char* net_backend(void);

#endif
