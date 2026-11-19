// Host test of the phase 7 modules on a small zip made by host_tests.sh: map, search, symbol, lines, zip writer.
#include <stdio.h>
#include <string.h>
#include "repomap.h"
#include "zipwrite.h"
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    static char out[40000];
    int ns = 0;
    int n = repomap_build(argv[1], out, 8000, &ns);
    CHECK(n > 0 && ns >= 2);
    CHECK(strstr(out, "send_text") != NULL);
    CHECK(strstr(out, "Project:") != NULL && strstr(out, "C 100%") != NULL);   // phase 12: the kind of project on top
    {
        static char refs[4000];
        int th = 0;
        int rf = repo_refs(argv[1], "send_text", refs, sizeof(refs), &th);
        CHECK(rf == 1 && th == 1 && strstr(refs, "(definition)") != NULL && strstr(refs, "chat/main.c") != NULL);
        CHECK(repo_refs(argv[1], "no_such_name", refs, sizeof(refs), &th) == 0 && strstr(refs, "No file uses") != NULL);
        CHECK(repo_refs(argv[1], "send", refs, sizeof(refs), &th) == 0);   // whole words only
    }
    RepoSeenSet seen = {.n = 0};
    int f = repo_search(argv[1], "send_text", out, 8000, 3, &seen, NULL);
    CHECK(f >= 1 && strstr(out, "send_text") != NULL);
    const char* src = "int a(void) {\n  return 1;\n}\n\nint b(void) {\n  return 2;\n}\n";
    int la, lb;
    CHECK(repo_symbol_range("x.c", src, (int)strlen(src), "b", &la, &lb) == 1 && la == 5 && lb == 7);
    int total;
    CHECK(repo_extract_lines(src, (int)strlen(src), 1, 3, out, sizeof(out), &total) > 0 && total == 7);
    long bytes = 0;
    CHECK(zipwrite_dir(argv[2], argv[3], &bytes) == 2 && bytes > 100);
    printf(bad ? "dev_test: FAILED\n" : "dev_test: ok\n");
    return bad;
}
