#include "verify.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_ISSUES 6

typedef struct {
    char* out;
    int max, o, n, total;
} Iss;

static void issue(Iss* s, int line, const char* fmt, ...) {
    s->total++;
    if (s->n >= MAX_ISSUES) return;
    int w = snprintf(s->out + s->o, s->max - s->o, "line %d: ", line);
    if (w < 0 || s->o + w >= s->max) return;
    va_list ap;
    va_start(ap, fmt);
    int w2 = vsnprintf(s->out + s->o + w, s->max - s->o - w, fmt, ap);
    va_end(ap);
    if (w2 < 0 || s->o + w + w2 + 2 >= s->max) {
        s->out[s->o] = 0;
        return;
    }
    s->o += w + w2;
    s->out[s->o++] = '\n';
    s->out[s->o] = 0;
    s->n++;
}

static int ends_with(const char* s, const char* ext) {   // ".ext", case-insensitive
    int l = (int)strlen(s), e = (int)strlen(ext);
    if (l <= e || s[l - e - 1] != '.') return 0;
    for (int i = 0; i < e; i++)
        if (tolower((unsigned char)s[l - e + i]) != ext[i]) return 0;
    return 1;
}

static int line_of(const char* t, int pos) {
    int l = 1;
    for (int i = 0; i < pos; i++) l += t[i] == '\n';
    return l;
}

// ---- JSON ----
typedef struct {
    const char* t;
    int len, p, depth, err_pos;
    const char* err;
} Js;

static void js_ws(Js* j) {
    while (j->p < j->len && (j->t[j->p] == ' ' || j->t[j->p] == '\n' || j->t[j->p] == '\t' || j->t[j->p] == '\r')) j->p++;
}

static int js_fail(Js* j, const char* msg) {
    if (!j->err) j->err = msg, j->err_pos = j->p;
    return 0;
}

static int js_value(Js* j);

static int js_string(Js* j) {
    j->p++;   // the opening quote
    while (j->p < j->len) {
        unsigned char c = (unsigned char)j->t[j->p];
        if (c == '"') {
            j->p++;
            return 1;
        }
        if (c == '\n') return js_fail(j, "string not closed before the end of the line");
        if (c == '\\') j->p++;
        j->p++;
    }
    return js_fail(j, "string not closed");
}

static int js_value(Js* j) {
    js_ws(j);
    if (j->p >= j->len) return js_fail(j, "value expected at the end of the file");
    char c = j->t[j->p];
    if (++j->depth > 64) return js_fail(j, "nested too deep");
    int ok = 0;
    if (c == '"') ok = js_string(j);
    else if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        js_ws(j);
        if (j->p < j->len && j->t[j->p] == close) {
            j->p++;
            ok = 1;
        } else {
            for (;;) {
                js_ws(j);
                if (c == '{') {
                    if (j->p >= j->len || j->t[j->p] != '"') {
                        ok = js_fail(j, j->p < j->len && j->t[j->p] == '}' ? "trailing comma before }" : "a \"key\" in quotes is expected");
                        break;
                    }
                    if (!js_string(j)) break;
                    js_ws(j);
                    if (j->p >= j->len || j->t[j->p] != ':') {
                        ok = js_fail(j, "':' expected after the key");
                        break;
                    }
                    j->p++;
                }
                if (c == '[' && j->p < j->len && j->t[j->p] == ']') {
                    ok = js_fail(j, "trailing comma before ]");
                    break;
                }
                if (!js_value(j)) break;
                js_ws(j);
                if (j->p < j->len && j->t[j->p] == ',') {
                    j->p++;
                    continue;
                }
                if (j->p < j->len && j->t[j->p] == close) {
                    j->p++;
                    ok = 1;
                    break;
                }
                ok = js_fail(j, j->p >= j->len ? "the file ends inside an object or array" : "',' or a closing bracket expected");
                break;
            }
        }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        int s = j->p;
        if (c == '-') j->p++;
        while (j->p < j->len && (isdigit((unsigned char)j->t[j->p]) || strchr(".eE+-", j->t[j->p]))) j->p++;
        ok = j->p > s + (c == '-');
        if (!ok) js_fail(j, "number expected");
    } else if (!strncmp(j->t + j->p, "true", 4) && j->p + 4 <= j->len) j->p += 4, ok = 1;
    else if (!strncmp(j->t + j->p, "false", 5) && j->p + 5 <= j->len) j->p += 5, ok = 1;
    else if (!strncmp(j->t + j->p, "null", 4) && j->p + 4 <= j->len) j->p += 4, ok = 1;
    else {
        ok = js_fail(j, c == '\'' ? "JSON uses \" not '" : "unexpected character");
    }
    j->depth--;
    return ok;
}

static void check_json(const char* t, int len, Iss* s) {
    Js j = {t, len, 0, 0, 0, NULL};
    int ok = js_value(&j);
    if (ok) {
        js_ws(&j);
        if (j.p < j.len) ok = js_fail(&j, "text after the end of the JSON");
    }
    if (!ok) issue(s, line_of(t, j.err_pos), "JSON: %s", j.err ? j.err : "invalid");
}

// ---- C family ----
static void check_c(const char* t, int len, int preproc, Iss* s) {
    int ifs = 0, if_line[64] = {0};
    int line = 1, i = 0;
    while (i < len) {
        char c = t[i];
        if (c == '\n') {
            line++;
            i++;
        } else if (c == '/' && i + 1 < len && t[i + 1] == '/') {
            while (i < len && t[i] != '\n') {
                if (t[i] == '\\' && i + 1 < len && t[i + 1] == '\n') line++, i++;   // (a // comment continued with a backslash)
                i++;
            }
        } else if (c == '/' && i + 1 < len && t[i + 1] == '*') {
            int start = line;
            i += 2;
            int closed = 0;
            while (i + 1 < len) {
                if (t[i] == '*' && t[i + 1] == '/') {
                    closed = 1;
                    i += 2;
                    break;
                }
                if (t[i] == '\n') line++;
                i++;
            }
            if (!closed) {
                issue(s, start, "a /* comment is never closed");
                return;
            }
        } else if (c == '"' || c == '\'') {
            int start = line;
            char q = c;
            i++;
            int closed = 0;
            while (i < len && t[i] != '\n') {
                if (t[i] == '\\') i++;
                else if (t[i] == q) {
                    closed = 1;
                    i++;
                    break;
                }
                i++;
            }
            if (!closed && i <= len && (i >= len || t[i] == '\n')) {
                if (q == '"') issue(s, start, "a string literal is not closed on its line");
            }
        } else if (c == '#' && preproc) {
            int k = i + 1;
            int at_start = 1;
            for (int b = i - 1; b >= 0 && t[b] != '\n'; b--)
                if (t[b] != ' ' && t[b] != '\t') at_start = 0;
            while (k < len && (t[k] == ' ' || t[k] == '\t')) k++;
            if (at_start) {
                if (!strncmp(t + k, "if", 2)) {   // #if #ifdef #ifndef
                    if (ifs < 64) if_line[ifs] = line;
                    ifs++;
                } else if (!strncmp(t + k, "endif", 5)) {
                    if (ifs == 0) issue(s, line, "#endif without #if");
                    else ifs--;
                }
            }
            while (i < len && t[i] != '\n') {   // the rest of the directive (and its continued lines)
                if (t[i] == '\\' && i + 1 < len && t[i + 1] == '\n') line++, i++;
                if (t[i] == '/' && i + 1 < len && t[i + 1] == '/') {
                    while (i < len && t[i] != '\n') i++;
                    break;
                }
                if (t[i] == '/' && i + 1 < len && t[i + 1] == '*') break;
                i++;
            }
        } else i++;
    }
    if (ifs > 0) issue(s, ifs <= 64 ? if_line[ifs - 1] : 1, "#if without #endif");
}

// ---- Markdown ----
static void check_md(const char* t, int len, Iss* s) {
    int fences = 0, line = 1, last = 0;
    char fence_ch = 0;
    int fence_len = 0;
    for (int i = 0; i < len;) {
        int e = i;
        while (e < len && t[e] != '\n') e++;
        int k = i;
        while (k < e && k - i < 3 && t[k] == ' ') k++;
        int run = 0;
        while (k + run < e && (t[k + run] == '`' || t[k + run] == '~') && t[k + run] == t[k]) run++;
        if (run >= 3) {
            if (!fence_ch) {
                fence_ch = t[k];
                fence_len = run;
                last = line;
                fences++;
            } else if (t[k] == fence_ch && run >= fence_len) {
                int only = 1;
                for (int q = k + run; q < e; q++)
                    if (t[q] != ' ' && t[q] != '\r') only = 0;
                if (only) fence_ch = 0, fences++;
            }
        }
        i = e + 1;
        line++;
    }
    if (fence_ch) issue(s, last, "a ``` code fence is opened and never closed");
    (void)fences;
}

// ---- YAML (and GitHub workflows) ----
#define Y_LEVELS 24
#define Y_KEYS 24
typedef struct {
    int indent;
    char keys[Y_KEYS][64];
    int nk;
} YLevel;

static int starts_with_word(const char* l, const char* w) {
    int n = (int)strlen(w);
    return !strncmp(l, w, n);
}

// Net number of flow brackets ({ [ minus } ]) opened by text, outside quotes and comments.
static int flow_delta(const char* c, int n) {
    int d = 0;
    char q = 0;
    for (int k = 0; k < n; k++) {
        char ch = c[k];
        if (q) {
            if (ch == '\\' && q == '"') k++;
            else if (ch == q) q = 0;
        } else if ((ch == '"' || ch == '\'') && (k == 0 || c[k - 1] == ' ' || c[k - 1] == '[' || c[k - 1] == '{' || c[k - 1] == ',')) q = ch;
        else if (ch == '#' && k > 0 && c[k - 1] == ' ') break;
        else if (ch == '{' || ch == '[') d++;
        else if (ch == '}' || ch == ']') d--;
    }
    return d;
}

static void check_yaml(const char* path, const char* t, int len, Iss* s) {
    int workflow = strstr(path, ".github/workflows/") != NULL;
    YLevel lv[Y_LEVELS];
    int nlv = 0;
    int block_parent = -1;   // inside a | or > block: the indent of the line that opened it
    int flow_depth = 0;      // inside a { } or [ ] that goes on over several lines
    int line = 0, tabs_reported = 0;
    // workflow state
    int has_on = 0, has_jobs = 0, in_jobs = 0, in_steps = 0, steps_ind = -1, step_ind_content = -1;
    int job_line = 0, job_has_run_on = 0, job_has_steps = 0, job_open = 0, step_open = 0, step_line = 0, step_ok = 0;
    char job_name[40] = "";
    int top_on_line = 0, jobs_ind1 = -1, job_key_ind = -1;   // (the indentation the file really uses: 2, 4...)
    for (int i = 0; i <= len;) {
        int e = i;
        while (e < len && t[e] != '\n') e++;
        line++;
        int ind = 0;
        while (i + ind < e && t[i + ind] == ' ') ind++;
        const char* l = t + i + ind;
        int ll = e - i - ind;
        while (ll > 0 && (l[ll - 1] == '\r' || l[ll - 1] == ' ')) ll--;
        i = e + 1;
        if (e >= len && ll == 0) break;
        if (ll == 0) continue;
        if (block_parent >= 0) {   // text of a block scalar: free until the indentation comes back
            if (ind > block_parent) continue;
            block_parent = -1;
        }
        if (l[0] == '#') continue;
        if (flow_depth > 0) {   // the lines of a multi-line flow collection are not checked
            flow_depth += flow_delta(l, ll);
            if (flow_depth < 0) flow_depth = 0;
            continue;
        }
        if (l[0] == '\t') {
            if (!tabs_reported++) issue(s, line, "a tab is used for indentation (YAML needs spaces)");
            continue;
        }
        if (!strncmp(l, "---", 3) && ll == 3) {
            nlv = 0;
            continue;
        }
        if (l[0] == '%' && ind == 0) continue;
        // the content after "- " items
        int dash = 0, cind = ind;
        const char* c = l;
        int cl = ll;
        int dash_pos[6], ndash = 0;
        while (cl >= 2 && c[0] == '-' && c[1] == ' ') {
            dash = 1;
            int k = 2;
            while (k < cl && c[k] == ' ') k++;
            c += k;
            cind += k;
            cl -= k;
            if (ndash < 6) dash_pos[ndash++] = cind;   // "- - x": each dash opens a level where its content starts
        }
        if (cl == 1 && c[0] == '-') {
            dash = 1;
            cl = 0;
        }
        // indentation levels
        int popped = 0;
        while (nlv > 0 && ind < lv[nlv - 1].indent) nlv--, popped = 1;
        if (nlv > 0 && ind > lv[nlv - 1].indent && popped) {   // back out of a block, but not to any level it had
            issue(s, line, "the indentation (%d spaces) matches no outer level", ind);
            nlv = 0;
            continue;
        }
        if (nlv > 0 && ind > lv[nlv - 1].indent) {
            if (nlv < Y_LEVELS) lv[nlv].indent = ind, lv[nlv].nk = 0, nlv++;
        } else if (nlv == 0) {
            lv[0].indent = ind;
            lv[0].nk = 0;
            nlv = 1;
        } else if (ind < lv[nlv - 1].indent) {   // (cannot happen after the loop)
        }
        if (nlv > 0 && ind != lv[nlv - 1].indent) {
            issue(s, line, "the indentation (%d spaces) matches no outer level", ind);
            nlv = 0;
            continue;
        }
        YLevel* cur = &lv[nlv - 1];
        if (dash) {   // a new item: its mapping starts empty, and its content opens a level of its own
            cur->nk = 0;
            for (int d = 0; d < ndash && cl > 0; d++)
                if (nlv < Y_LEVELS) {
                    lv[nlv].indent = dash_pos[d];
                    lv[nlv].nk = 0;
                    nlv++;
                }
            cur = &lv[nlv - 1];
        }
        if (cl == 0) continue;
        // key: value
        const char* colon = NULL;
        int q = 0;
        char qc = 0;
        for (int k = 0; k < cl; k++) {
            char ch = c[k];
            if (q) {
                if (ch == '\\' && qc == '"') k++;
                else if (ch == qc) q = 0;
            } else if ((ch == '"' || ch == '\'') && (k == 0 || c[k - 1] == ' ')) q = 1, qc = ch;
            else if (ch == '#' && k > 0 && c[k - 1] == ' ') {
                cl = k - 1;
                while (cl > 0 && c[cl - 1] == ' ') cl--;
                break;
            } else if (ch == ':' && (k + 1 == cl || c[k + 1] == ' ') && !colon) colon = c + k;
        }
        if (q && cl > 0 && (c[0] == '"' || c[0] == '\'')) {   // a quoted scalar that continues on the next lines is legal: only flag a value
        }
        const char* key = c;
        int klen = colon ? (int)(colon - c) : 0;
        while (klen > 0 && key[klen - 1] == ' ') klen--;   // ("run : x")
        const char* val = colon ? colon + 1 : c;
        int vlen = colon ? cl - (int)(colon - c) - 1 : cl;
        while (vlen > 0 && *val == ' ') val++, vlen--;
        {   // a flow collection that is left open continues on the next lines
            int fd = flow_delta(colon ? val : c, colon ? vlen : cl);
            if (fd > 0) flow_depth = fd;
        }
        if (colon && !(c[0] == '"' || c[0] == '\'' || c[0] == '[' || c[0] == '{')) {
            // duplicate key in the same mapping
            char kn[64];
            int kl = klen < 63 ? klen : 63;
            memcpy(kn, key, kl);
            kn[kl] = 0;
            for (int k = 0; k < cur->nk; k++)
                if (!strcmp(cur->keys[k], kn)) {
                    issue(s, line, "the key \"%s\" appears twice in the same mapping", kn);
                    break;
                }
            if (cur->nk < Y_KEYS) strcpy(cur->keys[cur->nk++], kn);
            if (vlen > 0) {
                char v0 = val[0];
                int is_block = (v0 == '|' || v0 == '>') && (vlen <= 3);
                if (is_block) block_parent = cind > ind ? ind : ind;
                if (is_block && dash) block_parent = ind;
                int simple_key = 1;
                for (int k = 0; k < klen; k++)
                    if (!isalnum((unsigned char)key[k]) && key[k] != '_' && key[k] != '-' && key[k] != '.') simple_key = 0;
                if (simple_key && !is_block && v0 != '"' && v0 != '\'' && v0 != '[' && v0 != '{' && v0 != '&' && v0 != '*' && v0 != '!') {
                    for (int k = 0; k + 1 < vlen; k++)
                        if (val[k] == ':' && val[k + 1] == ' ') {
                            issue(s, line, "the value of \"%s\" has ': ' inside and is not quoted (quote it or use a | block)", kn);
                            break;
                        }
                    if (v0 == '@' || v0 == '`') issue(s, line, "a value cannot start with '%c' unquoted", v0);
                }
                if ((v0 == '"' || v0 == '\'') && vlen >= 1) {   // quote never closed on this line (and not a multi-line scalar)
                    int closed = 0;
                    for (int k = 1; k < vlen; k++) {
                        if (v0 == '"' && val[k] == '\\') k++;
                        else if (val[k] == v0) {
                            if (v0 == '\'' && k + 1 < vlen && val[k + 1] == '\'') k++;
                            else closed = 1, k = vlen;
                        }
                    }
                    if (!closed) {   // legal when the string goes on in the next, more indented lines
                        int found = 0, p2 = i;   // (i is the start of the next line)
                        while (p2 < len && !found) {
                            int e2 = p2;
                            while (e2 < len && t[e2] != '\n') e2++;
                            int ind2 = 0;
                            while (p2 + ind2 < e2 && t[p2 + ind2] == ' ') ind2++;
                            int blank = ind2 == e2 - p2 || (e2 - p2 == ind2 + 1 && t[p2 + ind2] == '\r');
                            if (!blank && ind2 <= ind) break;
                            for (int k = p2 + ind2; k < e2; k++) {
                                if (v0 == '"' && t[k] == '\\') k++;
                                else if (t[k] == v0) {
                                    found = 1;
                                    break;
                                }
                            }
                            p2 = e2 + 1;
                        }
                        if (found) block_parent = ind;
                        else issue(s, line, "the quote that opens the value of \"%s\" is not closed on this line", kn);
                    }
                }
            }
        }
        // ---- GitHub workflow structure ----
        if (workflow) {
            if (ind == 0 && !dash && colon) {
                if (in_jobs && job_open) {
                    if (!job_has_run_on) issue(s, job_line, "job \"%s\" has no runs-on (or uses)", job_name);
                    job_open = 0;
                }
                if (step_open && !step_ok) issue(s, step_line, "a step has neither uses nor run");
                step_open = 0;
                in_steps = 0;
                in_jobs = 0;
                if (starts_with_word(key, "on") && (klen == 2 || (klen == 4 && (key[0] == '"' || key[0] == '\'')))) has_on = 1, top_on_line = line;
                else if (klen == 4 && !strncmp(key, "jobs", 4)) has_jobs = 1, in_jobs = 1, jobs_ind1 = -1;
                else if (klen == 4 && !strncmp(key, "true", 4)) has_on = 1;   // (an unquoted on: read by old YAML as true)
            } else if (in_jobs && !dash && colon && ind > 0 && (jobs_ind1 < 0 || ind == jobs_ind1)) {   // a job
                jobs_ind1 = ind;
                job_key_ind = -1;
                if (job_open && !job_has_run_on) issue(s, job_line, "job \"%s\" has no runs-on (or uses)", job_name);
                if (step_open && !step_ok) issue(s, step_line, "a step has neither uses nor run");
                step_open = 0;
                in_steps = 0;
                job_open = 1;
                job_line = line;
                job_has_run_on = job_has_steps = 0;
                snprintf(job_name, sizeof(job_name), "%.*s", klen > 39 ? 39 : klen, key);
            } else if (in_jobs && job_open && !dash && colon && ind > jobs_ind1 && (job_key_ind < 0 || ind == job_key_ind)) {
                job_key_ind = ind;
                if (step_open && !step_ok) issue(s, step_line, "a step has neither uses nor run");
                step_open = 0;
                in_steps = 0;
                if (klen == 7 && !strncmp(key, "runs-on", 7)) job_has_run_on = 1;
                else if (klen == 4 && !strncmp(key, "uses", 4)) job_has_run_on = 1;
                else if (klen == 5 && !strncmp(key, "steps", 5)) in_steps = 1, job_has_steps = 1, steps_ind = -1;
            } else if (in_steps && dash) {
                if (steps_ind < 0) steps_ind = ind;
                if (ind == steps_ind) {
                    if (step_open && !step_ok) issue(s, step_line, "a step has neither uses nor run");
                    step_open = 1;
                    step_ok = 0;
                    step_line = line;
                    step_ind_content = cind;
                }
            }
            if (in_steps && step_open && colon && (cind == step_ind_content)) {
                if ((klen == 4 && !strncmp(key, "uses", 4)) || (klen == 3 && !strncmp(key, "run", 3))) step_ok = 1;
                if (klen == 4 && !strncmp(key, "uses", 4) && vlen > 0) {
                    int has_at = 0;
                    for (int k = 0; k < vlen; k++) has_at |= val[k] == '@';
                    if (!has_at && strncmp(val, "./", 2) && strncmp(val, "docker://", 9) && val[0] != '"' && val[0] != '\'' && val[0] != '$')
                        issue(s, line, "\"uses: %.*s\" has no @version", vlen > 40 ? 40 : vlen, val);
                }
            }
        }
    }
    if (workflow) {
        if (job_open && !job_has_run_on) issue(s, job_line, "job \"%s\" has no runs-on (or uses)", job_name);
        if (step_open && !step_ok) issue(s, step_line, "a step has neither uses nor run");
        if (!has_on && line > 3) issue(s, 1, "the workflow has no top-level \"on:\"");
        if (!has_jobs && line > 3) issue(s, 1, "the workflow has no top-level \"jobs:\"");
        (void)top_on_line;
        (void)job_has_steps;
    }
}

// ---- Python ----
static const char* const PY_BLOCK[] = {"def", "class", "if", "elif", "else", "for", "while", "try", "except", "finally", "with", "async", NULL};

static void check_python(const char* t, int len, Iss* s) {
    int stack[40] = {0}, ns = 1;   // (the base level 0 is always there)
    int depth = 0, open_line[64] = {0};
    char open_ch[64];
    int line = 1, i = 0, expect_indent = 0, expect_line = 0, prev_indent = 0;
    int used_tab = 0, used_sp = 0, mixed_reported = 0;
    while (i < len) {
        // start of a logical line: measure its indentation
        int ls = i, ind = 0;
        int tabs_here = 0, sp_here = 0;
        while (i < len && (t[i] == ' ' || t[i] == '\t')) {
            if (t[i] == '\t') ind += 8 - ind % 8, tabs_here = 1;
            else ind++, sp_here = 1;
            i++;
        }
        if (i >= len) break;
        if (t[i] == '\n' || t[i] == '\r' || t[i] == '#') {   // blank or comment line
            while (i < len && t[i] != '\n') i++;
            i++;
            line++;
            continue;
        }
        if (tabs_here) used_tab = 1;
        if (sp_here) used_sp = 1;
        if (tabs_here && sp_here && !mixed_reported) mixed_reported = 1, issue(s, line, "tabs and spaces are mixed in the same indentation");
        else if (used_tab && used_sp && !mixed_reported) mixed_reported = 1, issue(s, line, "the file indents with tabs in some lines and spaces in others");
        (void)ls;
        if (expect_indent) {
            if (ind <= prev_indent) issue(s, expect_line, "a block opened here (ends with ':') has no indented body");
            expect_indent = 0;
        }
        int popped = 0;
        while (ns > 1 && ind < stack[ns - 1]) ns--, popped = 1;
        if (ind > stack[ns - 1]) {
            if (popped) issue(s, line, "unindent does not match any outer indentation level");
            else if (ns < 40) stack[ns++] = ind;
        } else if (ind < stack[ns - 1]) issue(s, line, "unindent does not match any outer indentation level");
        // the statement: up to its end at bracket depth 0
        int first_line = line, colon0 = 0, last_sig = 0;
        char word[16] = "";
        {
            int w = 0;
            int k = i;
            while (k < len && (isalpha((unsigned char)t[k]) || t[k] == '_') && w < 15) word[w++] = t[k++];
            word[w] = 0;
            if (k < len && (isalnum((unsigned char)t[k]) || t[k] == '_')) word[0] = 0;   // a longer name, not a keyword
        }
        for (;;) {
            if (i >= len) break;
            char c = t[i];
            if (c == '\n') {
                line++;
                i++;
                if (depth > 0) continue;
                break;
            }
            if (c == '\\' && i + 1 < len && (t[i + 1] == '\n' || (t[i + 1] == '\r' && i + 2 < len && t[i + 2] == '\n'))) {
                i += t[i + 1] == '\r' ? 3 : 2;
                line++;
                continue;
            }
            if (c == '#') {
                while (i < len && t[i] != '\n') i++;
                continue;
            }
            if (c == '"' || c == '\'') {
                int triple = i + 2 < len && t[i + 1] == c && t[i + 2] == c;
                int start_line = line;
                i += triple ? 3 : 1;
                int closed = 0;
                while (i < len) {
                    if (t[i] == '\\') {
                        if (t[i + 1] == '\n') line++;
                        i += 2;
                        continue;
                    }
                    if (triple) {
                        if (t[i] == c && i + 2 < len + 0 && t[i + 1] == c && t[i + 2] == c) {
                            i += 3;
                            closed = 1;
                            break;
                        }
                        if (t[i] == '\n') line++;
                    } else {
                        if (t[i] == c) {
                            i++;
                            closed = 1;
                            break;
                        }
                        if (t[i] == '\n') break;
                    }
                    i++;
                }
                if (!closed) {
                    issue(s, start_line, "a string is not closed");
                    return;
                }
                last_sig = 1;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') {
                if (depth < 64) open_line[depth] = line, open_ch[depth] = c;
                depth++;
            } else if (c == ')' || c == ']' || c == '}') {
                if (depth == 0) {
                    issue(s, line, "'%c' closes nothing", c);
                    return;
                }
                depth--;
                char o = depth < 64 ? open_ch[depth] : c;
                if ((c == ')' && o != '(') || (c == ']' && o != '[') || (c == '}' && o != '{')) {
                    issue(s, line, "'%c' does not match the '%c' opened at line %d", c, o, depth < 64 ? open_line[depth] : 0);
                    return;
                }
            } else if (c == ':' && depth == 0) colon0 = 1, last_sig = 2;
            else if (c != ' ' && c != '\t' && c != '\r') last_sig = 1;
            i++;
        }
        int is_block = 0;
        for (int k = 0; PY_BLOCK[k]; k++)
            if (!strcmp(word, PY_BLOCK[k])) is_block = 1;
        if (!strcmp(word, "async")) is_block = 1;
        if (is_block && depth == 0 && !colon0) issue(s, first_line, "\"%s\" statement without ':'", word);
        if (is_block && depth == 0 && colon0 && last_sig == 2) {
            expect_indent = 1;
            expect_line = first_line;
            prev_indent = ind;
        }
    }
    if (depth > 0) issue(s, depth <= 64 ? open_line[depth - 1] : 1, "'%c' is never closed", depth <= 64 ? open_ch[depth - 1] : '(');
    if (expect_indent) issue(s, expect_line, "the file ends after a ':' with no indented body");
}

// ---- shell ----

// Shell helpers: skip a "..." string or a $( ... ) with their nested quotes. Return the index after it, or -1 when never closed.
static int sh_skip_dq(const char* t, int len, int i, int* line);

static int sh_skip_paren(const char* t, int len, int i, int* line) {   // i is on the '(' of "$("
    int depth = 0;
    for (; i < len; i++) {
        char c = t[i];
        if (c == '\\') i++;
        else if (c == '\n') ++*line;
        else if (c == '\'') {
            for (i++; i < len && t[i] != '\''; i++)
                if (t[i] == '\n') ++*line;
            if (i >= len) return -1;
        } else if (c == '"') {
            i = sh_skip_dq(t, len, i, line);
            if (i < 0) return -1;
            i--;
        } else if (c == '(') depth++;
        else if (c == ')' && --depth == 0) return i + 1;
    }
    return -1;
}

static int sh_skip_dq(const char* t, int len, int i, int* line) {   // i is on the opening quote
    for (i++; i < len; i++) {
        char c = t[i];
        if (c == '\\') {
            if (i + 1 < len && t[i + 1] == '\n') ++*line;
            i++;
        } else if (c == '\n') ++*line;
        else if (c == '"') return i + 1;
        else if (c == '$' && i + 1 < len && t[i + 1] == '(') {
            i = sh_skip_paren(t, len, i + 1, line);
            if (i < 0) return -1;
            i--;
        } else if (c == '`') {
            for (i++; i < len && t[i] != '`'; i++) {
                if (t[i] == '\\') i++;
                else if (t[i] == '\n') ++*line;
            }
            if (i >= len) return -1;
        }
    }
    return -1;
}

static void check_shell(const char* t, int len, Iss* s) {
    int ifs = 0, loops = 0, cases = 0;
    int if_line = 0, loop_line = 0, case_line = 0;
    int line = 1;
    int i = 0;
    char heredoc[40] = "";
    int cmd_pos = 1;   // the next word is at the start of a command
    while (i < len) {
        char c = t[i];
        if (c == '\n') {
            line++;
            i++;
            cmd_pos = 1;
            if (heredoc[0]) {   // skip the body of a here-document
                for (;;) {
                    int e = i;
                    while (e < len && t[e] != '\n') e++;
                    int a = i;
                    while (a < e && (t[a] == '\t' || t[a] == ' ')) a++;
                    int hl = (int)strlen(heredoc);
                    int done = e - a >= hl && !strncmp(t + a, heredoc, hl) && (e - a == hl || t[a + hl] == '\r');
                    i = e < len ? e + 1 : len;
                    line++;
                    if (done || i >= len) break;
                }
                heredoc[0] = 0;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            continue;
        }
        if (c == '\\' && i + 1 < len && (t[i + 1] == '\n' || (t[i + 1] == '\r' && i + 2 < len && t[i + 2] == '\n'))) {   // line continuation: the command goes on
            i += t[i + 1] == '\r' ? 3 : 2;
            line++;
            continue;
        }
        if (c == '#') {
            while (i < len && t[i] != '\n') i++;
            continue;
        }
        if (c == ';' || c == '|' || c == '&' || c == '(' || c == ')' || c == '{') {
            cmd_pos = 1;
            i++;
            continue;
        }
        if (c == '<' && i + 1 < len && t[i + 1] == '<' && !(i + 2 < len && t[i + 2] == '<')) {
            int k = i + 2;
            if (k < len && t[k] == '-') k++;
            while (k < len && t[k] == ' ') k++;
            char q = (k < len && (t[k] == '\'' || t[k] == '"')) ? t[k++] : 0;
            int h = 0;
            while (k < len && h < 39 && (isalnum((unsigned char)t[k]) || t[k] == '_')) heredoc[h++] = t[k++];
            heredoc[h] = 0;
            if (q && k < len && t[k] == q) k++;
            i = k;
            continue;
        }
        // a word
        int start = i, start_line = line;
        char w[24];
        int wl = 0, quoted = 0;
        while (i < len && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != ';' && t[i] != '|' && t[i] != '&' && t[i] != '(' && t[i] != ')' && t[i] != '\r') {
            char d = t[i];
            if (d == '\\') {
                if (i + 1 < len && t[i + 1] == '\n') line++;
                i += 2;
                quoted = 1;
                continue;
            }
            if (d == '\'') {
                quoted = 1;
                i++;
                while (i < len && t[i] != '\'') {
                    if (t[i] == '\n') line++;
                    i++;
                }
                if (i >= len) {
                    issue(s, start_line, "a quote (') is never closed");
                    return;
                }
                i++;
                continue;
            }
            if (d == '"') {
                quoted = 1;
                int j = sh_skip_dq(t, len, i, &line);
                if (j < 0) {
                    issue(s, start_line, "a quote (\") is never closed");
                    return;
                }
                i = j;
                continue;
            }
            if (d == '$' && i + 1 < len && t[i + 1] == '(') {   // $( ... ): to the matching parenthesis
                int j = sh_skip_paren(t, len, i + 1, &line);
                if (j < 0) {
                    issue(s, start_line, "a $( is never closed");
                    return;
                }
                i = j;
                quoted = 1;
                continue;
            }
            if (wl < 23) w[wl++] = d;
            i++;
        }
        w[wl] = 0;
        if (i == start) i++;
        int pattern = cases > 0 && i < len && (t[i] == ')' || t[i] == '|');   // "fi)" / "fi|f)" in a case is a pattern
        if (cmd_pos && !quoted && !pattern) {
            if (!strcmp(w, "if")) ifs++, if_line = ifs == 1 ? start_line : if_line;
            else if (!strcmp(w, "fi")) {
                if (ifs == 0) {
                    issue(s, start_line, "fi without if");
                    return;
                }
                ifs--;
            } else if (!strcmp(w, "for") || !strcmp(w, "while") || !strcmp(w, "until") || !strcmp(w, "select")) loops++, loop_line = loops == 1 ? start_line : loop_line;
            else if (!strcmp(w, "done")) {
                if (loops == 0) {
                    issue(s, start_line, "done without for/while/until");
                    return;
                }
                loops--;
            } else if (!strcmp(w, "case")) cases++, case_line = cases == 1 ? start_line : case_line;
            else if (!strcmp(w, "esac")) {
                if (cases == 0) {
                    issue(s, start_line, "esac without case");
                    return;
                }
                cases--;
            }
        }
        cmd_pos = !strcmp(w, "then") || !strcmp(w, "do") || !strcmp(w, "else") || !strcmp(w, "elif") || !strcmp(w, "if") || !strcmp(w, "while") ||
                  !strcmp(w, "until") || !strcmp(w, "!") || !strcmp(w, "time");
        if (quoted) cmd_pos = 0;
    }
    if (ifs > 0) issue(s, if_line, "an if is never closed with fi");
    if (loops > 0) issue(s, loop_line, "a loop is never closed with done");
    if (cases > 0) issue(s, case_line, "a case is never closed with esac");
}

static int kind_of(const char* path) {
    static const char* const C_EXT[] = {"c", "h", "cc", "cpp", "hpp", "cxx", NULL};
    for (int i = 0; C_EXT[i]; i++)
        if (ends_with(path, C_EXT[i])) return 1;
    if (ends_with(path, "json")) return 2;
    if (ends_with(path, "yml") || ends_with(path, "yaml")) return 3;
    if (ends_with(path, "py")) return 4;
    if (ends_with(path, "sh") || ends_with(path, "bash")) return 5;
    if (ends_with(path, "md")) return 6;
    return 0;
}

int verify_known(const char* path) { return kind_of(path) != 0; }

int verify_text(const char* path, const char* text, int len, char* out, int max) {
    Iss s = {out, max, 0, 0, 0};
    if (max > 0) out[0] = 0;
    if (len > 400000) return 0;
    switch (kind_of(path)) {
    case 1: check_c(text, len, 1, &s); break;
    case 2:
        if (!strstr(path, "tsconfig") && !strstr(path, "jsconfig") && !strstr(path, ".vscode") && !strstr(path, "eslintrc")) check_json(text, len, &s);   // (those allow comments)
        break;
    case 3: check_yaml(path, text, len, &s); break;
    case 4: check_python(text, len, &s); break;
    case 5: check_shell(text, len, &s); break;
    case 6: check_md(text, len, &s); break;
    default: break;
    }
    if (s.total > s.n && s.o + 40 < max) s.o += snprintf(out + s.o, max - s.o, "(+%d more)\n", s.total - s.n);
    return s.total;
}
