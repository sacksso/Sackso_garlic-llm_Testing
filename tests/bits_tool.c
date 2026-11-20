// Command line for the bit-level tools (chat/bitscan.c), used by tests/bits_test.py.
//   bits_tool info ZIP ENTRY | find ZIP ENTRY SPEC | diff ZIPA ZIPB ENTRY | sum ZIP ENTRY OFFSET
//   bits_tool patch ZIP ENTRY BYTEOFF BIT WIDTH VALUE lsb|msb DSTZIP
// and the bintools.c entry points the app calls (they go through the same cache):
//   bits_tool b_find ZIP FILE NUMBER | b_bits ZIP FILE SPEC | b_hex ZIP FILE A B | b_cmp ZIPA ZIPB [HINT]
//   bits_tool b_pokebits ZIP FILE WHERE WIDTH VALUE ORDER DST | b_poke ZIP FILE OFF TYPE VALUE DST | b_sum ZIP FILE OFF
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bintools.h"
#include "bitscan.h"
static const ZipEnt* ent(const char* zip, const char* name, ZipEnt* keep) {
    const ZipEnt* es; int tot, n = zip_open_dir(zip, &es, &tot);
    for (int i = 0; i < n; i++) if (!strcmp(es[i].name, name)) { *keep = es[i]; return keep; }
    return NULL;
}
int main(int argc, char** argv) {
    static char out[20000], raw[300], raw2[300], msg[400];
    ZipEnt e, e2;
    if (argc < 4) return 2;
    const char* cmd = argv[1];
    if (!strncmp(cmd, "b_", 2)) {
        int r = 0;
        if (!strcmp(cmd, "b_cmp")) r = bin_zip_compare_hint(argv[2], argv[3], argc > 4 ? argv[4] : NULL, out, sizeof out);
        else if (!strcmp(cmd, "b_find")) r = bin_zip_find(argv[2], argv[3], argv[4], out, sizeof out);
        else if (!strcmp(cmd, "b_bits")) r = bin_zip_bits(argv[2], argv[3], argv[4], out, sizeof out);
        else if (!strcmp(cmd, "b_sum")) r = bin_checksum(argv[2], argv[3], argv[4], out, sizeof out);
        else if (!strcmp(cmd, "b_hex")) {
            if (!ent(argv[2], argv[3], &e)) { puts("no entry"); return 3; }
            r = bin_entry_view(argv[2], &e, strtol(argv[4], 0, 0), strtol(argv[5], 0, 0), 1, out, sizeof out);
        } else if (!strcmp(cmd, "b_pokebits")) { r = bin_pokebits(argv[2], argv[8], argv[3], argv[4], argv[5], argv[6], argv[7], out, sizeof out); }
        else if (!strcmp(cmd, "b_poke")) { r = bin_poke(argv[2], argv[7], argv[3], argv[4], argv[5], argv[6], out, sizeof out); }
        printf("ret=%d\n", r);
        if (r >= 0 && (!strcmp(cmd, "b_cmp") || !strcmp(cmd, "b_find") || !strcmp(cmd, "b_bits") || !strcmp(cmd, "b_sum") || !strcmp(cmd, "b_hex"))) fputs(out, stdout);
        else puts(out);
        return 0;
    }
    if (!strcmp(cmd, "diff")) {   // diff ZIPA ZIPB ENTRY [OLD>NEW]
        if (!ent(argv[2], argv[4], &e) || !ent(argv[3], argv[4], &e2)) { puts("no entry"); return 3; }
        if (bits_raw(argv[2], &e, raw, sizeof raw) || bits_raw(argv[3], &e2, raw2, sizeof raw2)) { puts("raw failed"); return 4; }
        bits_diff(raw, raw2, argv[4], argc > 5 ? argv[5] : NULL, out, sizeof out);
        fputs(out, stdout);
        return 0;
    }
    if (!ent(argv[2], argv[3], &e)) { puts("no entry"); return 3; }
    if (bits_raw(argv[2], &e, raw, sizeof raw)) { puts("raw failed"); return 4; }
    if (!strcmp(cmd, "info")) bits_report(raw, argv[3], out, sizeof out), fputs(out, stdout);
    else if (!strcmp(cmd, "find")) { int r = bits_find(raw, argv[3], argv[4], out, sizeof out); if (r < 0) printf("err %d\n", r); else fputs(out, stdout); }
    else if (!strcmp(cmd, "sum")) bits_checksum(raw, strtol(argv[4], 0, 0), out, sizeof out), fputs(out, stdout);
    else if (!strcmp(cmd, "patch")) {
        unsigned long long old = 0;
        snprintf(raw2, sizeof raw2, "%s.patched", raw);
        int r = bits_patch(raw, raw2, strtol(argv[4], 0, 0), atoi(argv[5]), atoi(argv[6]), strtoull(argv[7], 0, 0), !strcmp(argv[8], "msb"), &old);
        if (r) { printf("patch err %d\n", r); return 5; }
        r = bits_zip_replace(argv[2], argv[9], e.name, raw2, msg, sizeof msg);
        printf("old=%llu replace=%d %s\n", old, r, msg);
    }
    return 0;
}
