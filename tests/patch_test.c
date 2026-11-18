// Host test of patch.c: gcc -std=gnu99 -Ichat tests/patch_test.c chat/patch.c && ./a.out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "patch.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int apply1(const char* reply, char* file, int cap, const char** how, char* hint) {
    PatchBlock b[4];
    int n = patch_parse(reply, b, 4);
    if (n != 1) return -100;
    int len = (int)strlen(file), line;
    int r = patch_apply(file, &len, cap, &b[0], 1, &line, how, hint, 400);
    return r;
}

int main(void) {
    const char* how;
    char hint[400];
    static char f[4000];

    // parse: spanish markers, name label, fence
    const char* r1 = "Voy a cambiar esto.\n\nArchivo: `chat/ask.c`\n```c\n<<<<<<< BUSCAR\nint a = 1;\n=======\nint a = 2;\n>>>>>>> REEMPLAZAR\n```\n";
    PatchBlock b[4];
    CHECK(patch_parse(r1, b, 4) == 1);
    CHECK(!strcmp(b[0].path, "chat/ask.c"));
    CHECK(b[0].slen == 11 && !strncmp(b[0].search, "int a = 1;\n", 11));
    CHECK(patch_has_blocks(r1));
    CHECK(!patch_has_blocks("nada que aplicar\n```c\nint x;\n```\n"));
    // english markers and two files
    const char* r2 = "### a/b.py\n<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE\nFile: c.h\n<<<<<<<<<\nz\n=========\nw\n>>>>>>>>>\n";
    CHECK(patch_parse(r2, b, 4) == 2);
    CHECK(!strcmp(b[0].path, "a/b.py") && !strcmp(b[1].path, "c.h"));
    // an incomplete block is not a block
    CHECK(patch_parse("<<<<<<< BUSCAR\nx\n=======\ny\n", b, 4) == 0);

    // exact
    strcpy(f, "int main() {\n    int a = 1;\n    return a;\n}\n");
    CHECK(apply1("f.c\n<<<<<<< BUSCAR\n    int a = 1;\n=======\n    int a = 2;\n    int c = 3;\n>>>>>>> REEMPLAZAR\n", f, sizeof f, &how, hint) == 0);
    CHECK(!strcmp(f, "int main() {\n    int a = 2;\n    int c = 3;\n    return a;\n}\n"));
    CHECK(!strcmp(how, "exact"));

    // trailing spaces differ in the file
    strcpy(f, "a();   \nb();\t\nc();\n");
    CHECK(apply1("x.c\n<<<<<<< BUSCAR\na();\nb();\n=======\nd();\n>>>>>>> REEMPLAZAR\n", f, sizeof f, &how, hint) == 0);
    CHECK(!strcmp(f, "d();\nc();\n"));
    CHECK(!strcmp(how, "spaces"));

    // indentation differs: the model wrote 2 spaces, the file has 8 (the block is shifted by 6; its inner steps stay as written)
    strcpy(f, "void g() {\n        if (x) {\n            y();\n        }\n}\n");
    CHECK(apply1("x.c\n<<<<<<< BUSCAR\n  if (x) {\n    y();\n  }\n=======\n  if (z) {\n    w();\n  }\n>>>>>>> REEMPLAZAR\n", f, sizeof f, &how, hint) == 0);
    CHECK(!strcmp(f, "void g() {\n        if (z) {\n          w();\n        }\n}\n"));
    CHECK(!strcmp(how, "indent"));

    // ambiguous
    strcpy(f, "x();\ny();\nx();\n");
    CHECK(apply1("x.c\n<<<<<<< BUSCAR\nx();\n=======\nz();\n>>>>>>> REEMPLAZAR\n", f, sizeof f, &how, hint) == -2);
    CHECK(!strcmp(f, "x();\ny();\nx();\n"));   // untouched

    // not found, with a hint
    strcpy(f, "alpha();\nbeta_call(1, 2);\ngamma();\n");
    CHECK(apply1("x.c\n<<<<<<< BUSCAR\nbeta_call(1, 3);\n=======\nz();\n>>>>>>> REEMPLAZAR\n", f, sizeof f, &how, hint) == -1);
    CHECK(strstr(hint, "beta_call(1, 2)") != NULL);

    // deleting lines
    strcpy(f, "a\nb\nc\n");
    CHECK(apply1("x\n<<<<<<< BUSCAR\nb\n=======\n>>>>>>> REEMPLAZAR\n", f, sizeof f, &how, hint) == 0);
    CHECK(!strcmp(f, "a\nc\n"));

    // CRLF file keeps its line breaks
    strcpy(f, "a\r\nb\r\nc\r\n");
    CHECK(apply1("x\n<<<<<<< BUSCAR\nb\n=======\nb1\nb2\n>>>>>>> REEMPLAZAR\n", f, sizeof f, &how, hint) == 0);
    CHECK(!strcmp(f, "a\r\nb1\r\nb2\r\nc\r\n"));

    // creating a file
    PatchBlock nb;
    char nf[200] = "";
    int nlen = 0, line;
    const char* r3 = "new.txt\n<<<<<<< BUSCAR\n=======\nhello\n>>>>>>> REEMPLAZAR\n";
    CHECK(patch_parse(r3, &nb, 1) == 1);
    CHECK(patch_apply(nf, &nlen, sizeof nf, &nb, 0, &line, &how, hint, 400) == 0 && !strcmp(nf, "hello\n"));
    CHECK(patch_apply(nf, &nlen, sizeof nf, &nb, 1, &line, &how, hint, 400) == -4);   // empty search on existing text

    // no room
    strcpy(f, "a\n");
    char small[8];
    strcpy(small, "a\n");
    PatchBlock big;
    CHECK(patch_parse("x\n<<<<<<< BUSCAR\na\n=======\n0123456789012345\n>>>>>>> REEMPLAZAR\n", &big, 1) == 1);
    int sl = 2;
    CHECK(patch_apply(small, &sl, sizeof small, &big, 1, &line, &how, hint, 400) == -3);

    printf(fails ? "patch_test: %d FAILED\n" : "patch_test: ok\n", fails);
    return fails != 0;
}
