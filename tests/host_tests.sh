#!/bin/bash
# Host tests of the phase 7 modules (gcc + ASan/UBSan). Run from the repo root: bash tests/host_tests.sh
set -e
W=$(mktemp -d)
FL="-std=gnu99 -g -Wall -Wextra -fsanitize=address,undefined -Ichat"
gcc $FL tests/patch_test.c chat/patch.c -o $W/patch_test && $W/patch_test
gcc $FL -DPROGRESS_FILE="\"$W/progreso.txt\"" tests/progress_test.c chat/progress.c -o $W/progress_test && $W/progress_test > /dev/null && echo progress_test: ok
mkdir -p $W/src/chat $W/dir
printf 'int send_text(const char* t) {\n  return 0;\n}\nint other(void) {\n  return 1;\n}\n' > $W/src/chat/main.c
printf '# Title\n\nSome text\n' > $W/src/README.md
printf 'one\n' > $W/dir/a.txt
printf 'two\n' > $W/dir/b.txt
python3 - $W <<'PY'
import sys, zipfile
w = sys.argv[1]
with zipfile.ZipFile(w + "/t.zip", "w") as z:
    z.write(w + "/src/chat/main.c", "repo-main/chat/main.c")
    z.write(w + "/src/README.md", "repo-main/README.md")
PY
gcc $FL tests/dev_test.c chat/repomap.c chat/zipwrite.c chat/zipread.c chat/htmltext.c -o $W/dev_test
$W/dev_test $W/t.zip $W/dir $W/o.zip
unzip -tq $W/o.zip
# file explorer
mkdir -p $W/root/out/chat $W/root/web
echo a > $W/root/notas.txt; echo b > "$W/root/con espacio.txt"; echo c > $W/root/out/chat/x.c
python3 - $W <<'PY'
import sys, zipfile
w = sys.argv[1]
with zipfile.ZipFile(w + "/root/repo.zip", "w") as z:
    z.write(w + "/src/chat/main.c", "repo-main/chat/main.c")
    z.write(w + "/src/README.md", "repo-main/README.md")
PY
gcc $FL -DFILES_DIR="\"$W/root\"" tests/filepick_test.c chat/filepick.c chat/repomap.c chat/zipread.c chat/htmltext.c -o $W/filepick_test
$W/filepick_test
rm -rf $W
