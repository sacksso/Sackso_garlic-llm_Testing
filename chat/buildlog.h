// Build log -> the errors with the code around them (phase 11). The loop of this project is: edit, GitHub Actions builds,
// the log comes back. A small model fixes an error much better when it sees the error line AND the code at that line,
// so the app does the digging: it reads the log (a .txt/.log or the zip of the Actions logs), finds the compiler,
// linker and Python errors, and takes the lines around each one from the repo zip.
//   /errores [log] [repo.zip]
#ifndef BUILDLOG_H
#define BUILDLOG_H

// Text for the model about the errors of the log at log_path (a text file or a zip of text files). repo_zip may be NULL
// (then only the errors are shown). Returns the length of out, or < 0 when the log cannot be read.
int buildlog_report(const char* log_path, const char* repo_zip, char* out, int max);

// The log to use when the user did not name one: the file of FILES_DIR with "log" in its name (.zip .txt .log) that
// has the biggest number in the name (build-log-10 before build-log-9). 1 = found. list gets the candidates' names.
int buildlog_pick(char* name, int name_max, char* list, int list_max);

#endif
