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
