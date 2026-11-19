// Host test of chat/verify.c: files that are right give no problems, and each classic mistake is found.
#include <stdio.h>
#include <string.h>
#include "verify.h"

static int bad;
static char out[1500];

static void expect(const char* path, const char* text, int want, const char* word) {
    int n = verify_text(path, text, (int)strlen(text), out, sizeof(out));
    int ok = want ? (n > 0 && (!word || strstr(out, word))) : n == 0;
    if (!ok) {
        printf("FAIL %s (want %s %s): got %d: %s\n", path, want ? "problem" : "none", word ? word : "", n, out);
        bad++;
    }
}

int main(void) {
    // JSON
    expect("a.json", "{\"a\": [1, 2, {\"b\": null}], \"c\": \"x\\\"y\"}", 0, NULL);
    expect("a.json", "{\"a\": 1,}", 1, "trailing comma");
    expect("a.json", "{'a': 1}", 1, "key");
    expect("a.json", "{\"a\": [1, 2}", 1, NULL);
    expect("a.json", "{\"a\": 1} x", 1, "after the end");
    // YAML
    expect("x.yml", "name: demo\nitems:\n  - a: 1\n    b: 2\n  - a: 3\nrun: |\n  echo hi: there\n  if x; then\n    y\n  fi\n", 0, NULL);
    expect("x.yml", "a:\n\tb: 1\n", 1, "tab");
    expect("x.yml", "a:\n    b: 1\n  c: 2\n", 1, "matches no outer");
    expect("x.yml", "a: 1\nb: 2\na: 3\n", 1, "twice");
    expect("x.yml", "steps:\n  - run: echo done: yes\n", 1, "': '");
    expect("x.yml", "name: \"abc\nb: 1\n", 1, "not closed");
    expect("x.yml", "text: \"one\n  two\"\nb: 1\n", 0, NULL);
    // GitHub workflows
    const char* wf_ok = "name: CI\non:\n  push:\n    branches: [main]\n  workflow_dispatch:\njobs:\n  build:\n    runs-on: ubuntu-latest\n    steps:\n      - uses: actions/checkout@v4\n      - name: Build\n        run: make\n";
    expect(".github/workflows/ci.yml", wf_ok, 0, NULL);
    expect(".github/workflows/ci.yml", "name: CI\non: push\njobs:\n  build:\n    steps:\n      - run: make\n", 1, "runs-on");
    expect(".github/workflows/ci.yml", "name: CI\non: push\njobs:\n  build:\n    runs-on: x\n    steps:\n      - name: nothing\n      - run: make\n", 1, "neither uses nor run");
    expect(".github/workflows/ci.yml", "name: CI\non: push\njobs:\n  build:\n    runs-on: x\n    steps:\n      - uses: actions/checkout\n", 1, "@version");
    expect(".github/workflows/ci.yml", "name: CI\njobs:\n  build:\n    runs-on: x\n    steps:\n      - run: make\n", 1, "\"on:\"");
    expect(".github/workflows/ci.yml", "name: CI\non: push\njobs:\n  build:\n    runs-on: x\n    steps:\n      - uses: ./local\n      - uses: docker://alpine:3\n", 0, NULL);
    // Python
    expect("a.py", "import os\n\n\ndef f(x):\n    if x:\n        return [1,\n                2]\n    return {'a': x}\n\nclass A:\n    pass\n", 0, NULL);
    expect("a.py", "def f(x)\n    return 1\n", 1, "without ':'");
    expect("a.py", "def f(x):\nreturn 1\n", 1, "indented body");
    expect("a.py", "x = foo(1,\n  2\n", 1, "never closed");
    expect("a.py", "if a:\n        x = 1\n    y = 2\n", 1, "unindent");
    expect("a.py", "if a:\n\tx = 1\n        y = 2\n", 1, "tabs");
    expect("a.py", "s = 'abc\nx = 1\n", 1, "string");
    expect("a.py", "x = (1 + 2))\n", 1, "closes nothing");
    expect("a.py", "if x: return 1\nelse: return 2\n", 0, NULL);
    // shell
    expect("a.sh", "#!/bin/sh\nif [ -f x ]; then\n  echo \"a $(date)\"\nfi\nfor i in 1 2; do echo $i; done\ncat <<EOF\nif no\nEOF\ncase $1 in\n  fi|f) echo;;\nesac\n", 0, NULL);
    expect("a.sh", "if true; then\n  echo hi\n", 1, "fi");
    expect("a.sh", "for i in 1 2; do\n  echo $i\n", 1, "done");
    expect("a.sh", "echo \"abc\n", 1, "quote");
    expect("a.sh", "echo hi\nfi\n", 1, "fi without if");
    // C
    expect("a.c", "#include <stdio.h>\n#ifdef X\nint a = 1; /* ok */ // c\n#endif\nchar* s = \"a\\\"b\";\nchar c = '\"';\n", 0, NULL);
    expect("a.c", "/* never\nint a;\n", 1, "comment");
    expect("a.c", "#ifdef X\nint a;\n", 1, "#endif");
    expect("a.c", "#endif\n", 1, "without #if");
    expect("a.c", "char* s = \"abc;\nint a;\n", 1, "string");
    // Markdown
    expect("a.md", "# T\n\n```c\nint a;\n```\n\ntext\n", 0, NULL);
    expect("a.md", "# T\n\n```c\nint a;\n", 1, "fence");
    // unknown kinds are not checked
    expect("a.txt", "{{{", 0, NULL);
    printf(bad ? "verify_test: FAILED (%d)\n" : "verify_test: ok\n", bad);
    return bad != 0;
}
