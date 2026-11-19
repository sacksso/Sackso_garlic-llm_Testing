// Structural checks of a file the model wrote (phase 12): what a compiler or a YAML parser would complain about first,
// found by the app so the model can fix it before the user finds out. No file is changed. It is deliberately
// conservative: it only reports things that are wrong, never style.
//   .json                 real JSON syntax (commas, quotes, brackets)
//   .yml / .yaml          tabs, indentation that matches no outer level, duplicate keys, "a: b: c" plain scalars,
//                         unclosed quotes; GitHub workflows (.github/workflows/): on, jobs, runs-on, steps with uses/run,
//                         "uses:" without @version
//   .py                   a block opener (def, if, for...) without ':', a missing indented block, unindent that matches
//                         nothing, tabs mixed with spaces, unclosed brackets or strings
//   .sh / .bash           if/fi, for-while-until/done, case/esac, unclosed quotes
//   .c .h .cc .cpp .hpp   unterminated comment or string, #if without #endif
//   .md                   an unclosed ``` fence
#ifndef VERIFY_H
#define VERIFY_H

// Checks text (len bytes) of the file path. Writes the problems into out, one per line as "line N: message" (at most 6),
// NUL-terminated. Returns how many problems there are (0 = nothing found, or a file type that is not checked).
int verify_text(const char* path, const char* text, int len, char* out, int max);

// 1 when verify_text knows this kind of file.
int verify_known(const char* path);

#endif
