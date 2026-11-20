#!/bin/bash
# Host tests of the phase 7 modules (gcc + ASan/UBSan). Run from the repo root: bash tests/host_tests.sh
set -e
W=$(mktemp -d)
FL="-std=gnu99 -g -Wall -Wextra -fsanitize=address,undefined -Ichat"
gcc $FL tests/patch_test.c chat/patch.c -o $W/patch_test && $W/patch_test
gcc $FL -DPROGRESS_FILE="\"$W/progreso.txt\"" tests/progress_test.c chat/progress.c -o $W/progress_test && $W/progress_test > /dev/null && echo progress_test: ok
# phase 9: projects, files into the active project, placeholder filter
mkdir -p $W/ws/garlic_files
gcc $FL -DWS_ROOT="\"$W/ws/garlic_projects\"" -DWS_OUT_ROOT="\"$W/ws/garlic_files/out\"" -DWS_STATE_FILE="\"$W/ws/proyecto.txt\"" -DWS_LEGACY_PROGRESS="\"$W/ws/progreso.txt\"" tests/workspace_test.c chat/workspace.c chat/outfiles.c chat/ask.c chat/undo.c -o $W/workspace_test && $W/workspace_test
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
# phase 9 through the commands
mkdir -p $W/d9/files $W/d9/ws
cp $W/t.zip $W/d9/files/repo.zip
gcc $FL -DFILES_DIR="\"$W/d9/files\"" -DDEV_FLAG_FILE="\"$W/d9/dev.txt\"" -DUNDO_DIR="\"$W/d9/undo\"" -DWS_ROOT="\"$W/d9/ws/projects\"" -DWS_OUT_ROOT="\"$W/d9/files/out\"" -DWS_STATE_FILE="\"$W/d9/ws/proyecto.txt\"" -DWS_LEGACY_PROGRESS="\"$W/d9/ws/progreso.txt\"" tests/dev9_test.c chat/devcmd.c chat/files.c chat/repomap.c chat/zipread.c chat/zipwrite.c chat/htmltext.c chat/progress.c chat/patch.c chat/ask.c chat/workspace.c chat/outfiles.c chat/bintools.c chat/buildlog.c chat/ghinfo.c chat/undo.c chat/verify.c -o $W/dev9_test && $W/dev9_test
# the API: protocols (Kobold, Ollama, OpenAI text), stop sequences, prefix cache
gcc $FL tests/api_proto_test.c chat/api_proto.c -o $W/api_proto_test && $W/api_proto_test
gcc $FL tests/api_compat_test.c chat/api_compat.c chat/api_proto.c -o $W/api_compat_test && $W/api_compat_test
gcc $FL -DPROFILES_DIR="\"$W/profiles\"" -DPROFILE_API_FILE="\"$W/api_profile.txt\"" tests/api_run_test.c chat/api_proto.c chat/api_compat.c chat/profiles.c -o $W/api_run_test && $W/api_run_test
gcc $FL -DPROFILES_DIR="\"$W/profiles2\"" -DPROFILE_API_FILE="\"$W/api_profile2.txt\"" tests/profiles_test.c chat/profiles.c -o $W/profiles_test && $W/profiles_test > /dev/null && echo profiles_test: ok
python3 tests/savetool_test.py | tail -1
gcc $FL tests/verify_test.c chat/verify.c -o $W/verify_test && $W/verify_test
rm -rf $W
