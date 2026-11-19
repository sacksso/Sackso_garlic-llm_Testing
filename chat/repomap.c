// Repo map, search and symbol ranges (see repomap.h).
#include "repomap.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------
// small helpers

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ends_ci(const char* s, const char* suf) {
    int a = (int)strlen(s), b = (int)strlen(suf);
    if (a < b) return 0;
    for (int i = 0; i < b; i++)
        if (lc((unsigned char)s[a - b + i]) != suf[i]) return 0;
    return 1;
}

static int eq_ci(const char* a, const char* b) {
    while (*a && *b && lc((unsigned char)*a) == lc((unsigned char)*b)) a++, b++;
    return *a == 0 && *b == 0;
}

static int starts_with(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }

static int ident_char(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static int ilog2i(int x) {
    int r = 0;
    while (x > 1) x >>= 1, r++;
    return r;
}

static const char* base_name(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// ---------------------------------------------------------------------------------------------------------------------
// languages

enum { L_NONE, L_C, L_PY, L_JS, L_GO, L_RS, L_ZIG, L_JAVA, L_SH, L_MD, L_YAML, L_TOML, L_MAKE, L_TEXT };

static int lang_of(const char* name) {
    static const struct {
        const char* ext;
        int lang;
    } T[] = {{".c", L_C},      {".h", L_C},       {".cc", L_C},     {".cpp", L_C},    {".cxx", L_C},      {".hpp", L_C},
             {".hh", L_C},    {".inc", L_C},     {".inl", L_C},    {".m", L_C},      {".mm", L_C},       {".cu", L_C},
             {".cl", L_C},    {".glsl", L_C},    {".vert", L_C},   {".frag", L_C},   {".comp", L_C},     {".hlsl", L_C},
             {".wgsl", L_C},  {".py", L_PY},     {".pyw", L_PY},   {".js", L_JS},    {".mjs", L_JS},     {".cjs", L_JS},
             {".ts", L_JS},   {".tsx", L_JS},    {".jsx", L_JS},   {".go", L_GO},    {".rs", L_RS},      {".zig", L_ZIG},
             {".java", L_JAVA}, {".kt", L_JAVA}, {".kts", L_JAVA}, {".cs", L_JAVA},  {".swift", L_JAVA}, {".scala", L_JAVA},
             {".sh", L_SH},   {".bash", L_SH},   {".zsh", L_SH},   {".md", L_MD},    {".markdown", L_MD}, {".rst", L_MD},
             {".yml", L_YAML}, {".yaml", L_YAML}, {".toml", L_TOML}, {".ini", L_TOML}, {".cfg", L_TOML}, {".conf", L_TOML},
             {".mk", L_MAKE}, {".cmake", L_MAKE}, {".txt", L_TEXT}, {".json", L_TEXT}, {".html", L_TEXT}, {".htm", L_TEXT},
             {".css", L_TEXT}, {".scss", L_TEXT}, {".xml", L_TEXT}, {".sql", L_TEXT}, {".rb", L_TEXT},    {".php", L_TEXT},
             {".lua", L_TEXT}, {".pl", L_TEXT},  {".gradle", L_TEXT}, {".bat", L_TEXT}, {".ps1", L_TEXT},  {".s", L_TEXT},
             {".asm", L_TEXT}, {NULL, 0}};
    const char* b = base_name(name);
    if (eq_ci(b, "makefile") || eq_ci(b, "gnumakefile") || eq_ci(b, "cmakelists.txt")) return L_MAKE;
    if (eq_ci(b, "dockerfile") || eq_ci(b, ".gitignore") || eq_ci(b, "readme") || eq_ci(b, "requirements.txt")) return L_TEXT;
    for (int i = 0; T[i].ext; i++)
        if (ends_ci(b, T[i].ext)) return T[i].lang;
    return L_NONE;
}

int repo_is_text_name(const char* name) { return lang_of(name) != L_NONE; }

int repo_is_vendor_path(const char* rel) {
    static const char* const V[] = {"node_modules", "vendor", "third_party", "thirdparty", "site-packages", "__pycache__", "venv", ".venv",
                                    ".git", "dist", "build", "target", NULL};
    for (const char* p = rel; *p;) {
        const char* e = strchr(p, '/');
        if (!e) break;   // (the last part is the file name)
        int l = (int)(e - p);
        for (int i = 0; V[i]; i++)
            if ((int)strlen(V[i]) == l && !strncmp(p, V[i], l)) return 1;
        p = e + 1;
    }
    return 0;
}

// folders whose name says "somebody else's code" (MicroPython lives in chat/py/mp here)
static int vendor_dir_name(const char* p, int l) {
    static const char* const V[] = {"mp", "micropython", "external", "extern", "deps", "libs", "lib", NULL};
    for (int i = 0; V[i]; i++)
        if ((int)strlen(V[i]) == l && !strncmp(p, V[i], l)) return 1;
    return 0;
}

int repo_root_len(const ZipEnt* ents, int n) {
    if (n <= 0) return 0;
    const char* first = strchr(ents[0].name, '/');
    if (!first) return 0;
    int tl = (int)(first - ents[0].name) + 1;
    for (int i = 0; i < n; i++)
        if (strncmp(ents[i].name, ents[0].name, tl)) return 0;
    return tl;
}

// ---------------------------------------------------------------------------------------------------------------------
// symbols

#define SYM_NAME_MAX 44

typedef struct {
    int off;    // into names[]
    int line;
} Sym;

typedef struct {
    Sym* syms;
    int n, max;
    char* names;
    int noff, nmax;
    const char* want;   // search mode: stop at the first symbol with this name
    int found_line;
    int found_indent;
    int after_line;     // search mode: ignore matches at or before this line (to skip a prototype)
} Sink;

static int name_matches(const char* name, const char* want) {
    if (!strcmp(name, want)) return 1;
    const char* q = strrchr(name, ':');
    if (q && !strcmp(q + 1, want)) return 1;
    q = strchr(name, ' ');
    if (q && !strcmp(q + 1, want)) return 1;
    if (name[0] == '#' && !strcmp(name + 1, want)) return 1;
    return 0;
}

// Adds a symbol; returns 1 when the scan can stop (search mode found it, or the sink is full).
static int sink_add(Sink* k, const char* s, int len, int line) {
    if (len <= 0) return 0;
    if (len > SYM_NAME_MAX) len = SYM_NAME_MAX;
    if (k->want) {
        char tmp[SYM_NAME_MAX + 1];
        memcpy(tmp, s, len);
        tmp[len] = 0;
        if (line > k->after_line && name_matches(tmp, k->want)) {
            k->found_line = line;
            return 1;
        }
        return 0;
    }
    if (k->n >= k->max || k->noff + len + 1 > k->nmax) return 1;
    memcpy(k->names + k->noff, s, len);
    k->names[k->noff + len] = 0;
    k->syms[k->n].off = k->noff;
    k->syms[k->n].line = line;
    k->noff += len + 1;
    k->n++;
    return 0;
}

static int word_is(const char* p, const char* w) {
    int l = (int)strlen(w);
    return !strncmp(p, w, l) && !ident_char((unsigned char)p[l]);
}

static const char* skip_ws(const char* p, const char* e) {
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    return p;
}

// the identifier (letters, digits, '_', and "::" or '~' for C++) that ends just before p (exclusive); start returned
static const char* ident_before(const char* line, const char* p) {
    const char* s = p;
    while (s > line && (ident_char((unsigned char)s[-1]) || s[-1] == ':' || s[-1] == '~')) s--;
    return s;
}

static int c_keyword(const char* p) {
    static const char* const K[] = {"typedef", "return", "if", "else", "for", "while", "switch", "case", "default", "goto", "do",
                                    "extern", "using", "namespace", "template", "static_assert", "sizeof", "defined", NULL};
    for (int i = 0; K[i]; i++)
        if (word_is(p, K[i])) return 1;
    return 0;
}

// One line of C-like code at column 0: a function definition / prototype, a struct, a typedef'ed name, a macro.
// Returns 1 to stop.
static int c_line(Sink* k, const char* p, int len, int line, int header, int* macros) {
    const char* e = p + len;
    if (len == 0) return 0;
    if (p[0] == '}') {   // "} Name;" closes a typedef struct / enum
        const char* q = skip_ws(p + 1, e);
        const char* s = q;
        while (q < e && ident_char((unsigned char)*q)) q++;
        if (q > s && q < e && (*q == ';' || *q == ',') && s > p + 1) return sink_add(k, s, (int)(q - s), line);
        return 0;
    }
    if (p[0] == '#') {
        if (header && *macros < 10 && starts_with(p, "#define ")) {   // function-like macros only ("#define MAX(a, b)")
            const char* s = skip_ws(p + 8, e);
            const char* q = s;
            while (q < e && ident_char((unsigned char)*q)) q++;
            if (q > s && q < e && *q == '(') {
                char nm[SYM_NAME_MAX + 2];
                int l = (int)(q - s);
                if (l > SYM_NAME_MAX) l = SYM_NAME_MAX;
                nm[0] = '#';
                memcpy(nm + 1, s, l);
                (*macros)++;
                return sink_add(k, nm, l + 1, line);
            }
        }
        return 0;
    }
    if (!(ident_char((unsigned char)p[0]) || p[0] == '~')) return 0;
    if (c_keyword(p)) {
        // "typedef struct Name {" still names a type
        if (!word_is(p, "typedef")) return 0;
    }
    // struct / enum / union with a body: "struct Name {", "typedef enum Name {"
    {
        const char* q = p;
        if (word_is(q, "typedef")) q = skip_ws(q + 7, e);
        if (word_is(q, "static")) q = skip_ws(q + 6, e);
        const char* kw = NULL;
        if (word_is(q, "struct")) kw = "struct";
        else if (word_is(q, "enum")) kw = "enum";
        else if (word_is(q, "union")) kw = "union";
        if (kw) {
            q = skip_ws(q + strlen(kw), e);
            const char* s = q;
            while (q < e && ident_char((unsigned char)*q)) q++;
            const char* r = skip_ws(q, e);
            int has_paren = memchr(p, '(', len) != NULL;
            if (q > s && !has_paren && (r == e || *r == '{')) {
                char nm[SYM_NAME_MAX + 8];
                int l = snprintf(nm, sizeof(nm), "%s %.*s", kw, (int)(q - s) > SYM_NAME_MAX - 8 ? SYM_NAME_MAX - 8 : (int)(q - s), s);
                return sink_add(k, nm, l, line);
            }
            return 0;
        }
        if (word_is(p, "typedef")) return 0;
    }
    // function: the first '(' that is not an attribute's, with a type before the name, and no '=' or ';' before it
    const char* par = NULL;
    for (const char* q = p; q < e; q++) {
        if (*q == '=' || *q == ';' || *q == '{') break;
        if (*q != '(') continue;
        const char* s = ident_before(p, q);
        if ((int)(q - s) >= 11 && !strncmp(s, "__attribute", 11)) {   // __attribute__((weak)): skip its parentheses
            int depth = 0;
            for (; q < e; q++) {
                if (*q == '(') depth++;
                else if (*q == ')' && --depth == 0) break;
            }
            continue;
        }
        if (q == s) {   // "(" after a space: "int (*fn)(int)" is a pointer, not a function
            break;
        }
        par = q;
        break;
    }
    if (!par) return 0;
    const char* s = ident_before(p, par);
    if (s == p) return 0;   // no type before the name: a macro call such as FOO(x)
    int l = (int)(par - s);
    char c = s[-1];
    if (!(c == ' ' || c == '\t' || c == '*' || c == '&')) return 0;
    if (c_keyword(s)) return 0;
    const char* last = e;
    while (last > p && (last[-1] == ' ' || last[-1] == '\t')) last--;
    if (last > p && last[-1] == ';' && !header) return 0;   // a prototype or a call in a .c file
    // a call or an initialisation such as "x = foo(1);" was cut at '=' above; "foo(1);" has no type, handled above
    return sink_add(k, s, l, line);
}

static const char* trim_start(const char* p, const char* e, int* indent) {
    int ind = 0;
    while (p < e && (*p == ' ' || *p == '\t')) ind += *p == '\t' ? 4 : 1, p++;
    *indent = ind;
    return p;
}

static int take_ident(const char* p, const char* e, const char** end) {
    const char* s = p;
    while (p < e && (ident_char((unsigned char)*p) || *p == '$')) p++;
    *end = p;
    return (int)(p - s);
}

static int py_line(Sink* k, const char* p, int len, int line) {
    const char* e = p + len;
    int ind;
    const char* q = trim_start(p, e, &ind);
    if (ind > 4) return 0;
    if (starts_with(q, "async ")) q = skip_ws(q + 6, e);
    int cls = 0;
    if (starts_with(q, "def ")) q += 4;
    else if (starts_with(q, "class ")) q += 6, cls = 1;
    else return 0;
    q = skip_ws(q, e);
    const char* en;
    int l = take_ident(q, e, &en);
    if (l <= 0 || (q[0] == '_' && q[1] == '_')) return 0;
    k->found_indent = ind;
    char nm[SYM_NAME_MAX + 8];
    int n = snprintf(nm, sizeof(nm), "%s%.*s", cls ? "class " : "", l > SYM_NAME_MAX - 8 ? SYM_NAME_MAX - 8 : l, q);
    return sink_add(k, nm, n, line);
}

static int js_line(Sink* k, const char* p, int len, int line) {
    const char* e = p + len;
    int ind;
    const char* q = trim_start(p, e, &ind);
    if (ind > 2) return 0;
    if (starts_with(q, "export ")) q = skip_ws(q + 7, e);
    if (starts_with(q, "default ")) q = skip_ws(q + 8, e);
    if (starts_with(q, "async ")) q = skip_ws(q + 6, e);
    const char* en;
    if (starts_with(q, "function")) {
        q = skip_ws(q + 8, e);
        if (q < e && *q == '*') q++;
        q = skip_ws(q, e);
        int l = take_ident(q, e, &en);
        return l > 0 ? sink_add(k, q, l, line) : 0;
    }
    if (starts_with(q, "class ") || starts_with(q, "interface ") || starts_with(q, "type ") || starts_with(q, "enum ")) {
        q = skip_ws(strchr(q, ' '), e);
        int l = take_ident(q, e, &en);
        return l > 0 ? sink_add(k, q, l, line) : 0;
    }
    if (starts_with(q, "const ") || starts_with(q, "let ") || starts_with(q, "var ")) {
        q = skip_ws(strchr(q, ' '), e);
        int l = take_ident(q, e, &en);
        const char* r = skip_ws(en, e);
        if (l > 0 && r < e && *r == '=') {
            r = skip_ws(r + 1, e);
            if (starts_with(r, "async ")) r = skip_ws(r + 6, e);
            const char* t2;
            int l2 = take_ident(r, e, &t2);
            if (starts_with(r, "function") || (r < e && *r == '(') || (l2 > 0 && skip_ws(t2, e) + 1 < e && !strncmp(skip_ws(t2, e), "=>", 2)))
                return sink_add(k, q, l, line);
        }
    }
    return 0;
}

static int go_line(Sink* k, const char* p, int len, int line) {
    const char* e = p + len;
    const char* q = p;
    if (starts_with(q, "func ")) {
        q = skip_ws(q + 5, e);
        if (q < e && *q == '(') {   // a method: skip the receiver
            while (q < e && *q != ')') q++;
            if (q < e) q++;
            q = skip_ws(q, e);
        }
        const char* en;
        int l = take_ident(q, e, &en);
        return l > 0 ? sink_add(k, q, l, line) : 0;
    }
    if (starts_with(q, "type ")) {
        q = skip_ws(q + 5, e);
        const char* en;
        int l = take_ident(q, e, &en);
        if (l > 0) return sink_add(k, q, l, line);
    }
    return 0;
}

static int rs_line(Sink* k, const char* p, int len, int line, int zig) {
    const char* e = p + len;
    int ind;
    const char* q = trim_start(p, e, &ind);
    if (ind > 4) return 0;
    for (int again = 0; again < 6; again++) {   // modifiers
        static const char* const M[] = {"pub", "async", "unsafe", "const", "extern", "export", "inline", "comptime", NULL};
        int hit = 0;
        for (int i = 0; M[i]; i++)
            if (word_is(q, M[i]) && !(zig && !strcmp(M[i], "const") && 0)) {
                const char* t = skip_ws(q + strlen(M[i]), e);
                if (!strcmp(M[i], "const") && !word_is(t, "fn")) continue;
                q = t;
                if (*q == '(') {   // pub(crate)
                    while (q < e && *q != ')') q++;
                    if (q < e) q = skip_ws(q + 1, e);
                }
                if (q < e && *q == '"') {   // extern "C"
                    q++;
                    while (q < e && *q != '"') q++;
                    if (q < e) q = skip_ws(q + 1, e);
                }
                hit = 1;
                break;
            }
        if (!hit) break;
    }
    static const char* const KW[] = {"fn", "struct", "enum", "trait", "impl", "mod", "type", "union", NULL};
    for (int i = 0; KW[i]; i++)
        if (word_is(q, KW[i])) {
            q = skip_ws(q + strlen(KW[i]), e);
            if (!strcmp(KW[i], "impl") && q < e && *q == '<') {
                int d = 0;
                for (; q < e; q++) {
                    if (*q == '<') d++;
                    else if (*q == '>' && --d == 0) {
                        q++;
                        break;
                    }
                }
                q = skip_ws(q, e);
            }
            const char* en;
            int l = take_ident(q, e, &en);
            char nm[SYM_NAME_MAX + 8];
            if (l <= 0) return 0;
            int n = !strcmp(KW[i], "fn") ? snprintf(nm, sizeof(nm), "%.*s", l > SYM_NAME_MAX ? SYM_NAME_MAX : l, q)
                                         : snprintf(nm, sizeof(nm), "%s %.*s", KW[i], l > SYM_NAME_MAX - 8 ? SYM_NAME_MAX - 8 : l, q);
            return sink_add(k, nm, n, line);
        }
    if (zig && (word_is(q, "const") || word_is(q, "var"))) {   // const Name = struct {
        const char* t = skip_ws(q + (q[0] == 'c' ? 5 : 3), e);
        const char* en;
        int l = take_ident(t, e, &en);
        const char* r = skip_ws(en, e);
        if (l > 0 && r < e && *r == '=') {
            r = skip_ws(r + 1, e);
            if (word_is(r, "struct") || word_is(r, "enum") || word_is(r, "union") || starts_with(r, "packed ") || starts_with(r, "extern "))
                return sink_add(k, t, l, line);
        }
    }
    return 0;
}

static int java_line(Sink* k, const char* p, int len, int line) {
    const char* e = p + len;
    int ind;
    const char* q = trim_start(p, e, &ind);
    if (ind > 4) return 0;
    static const char* const M[] = {"public", "private", "protected", "internal", "static", "final", "abstract", "open", "data", "sealed", "partial", NULL};
    for (int again = 0; again < 6; again++) {
        int hit = 0;
        for (int i = 0; M[i]; i++)
            if (word_is(q, M[i])) q = skip_ws(q + strlen(M[i]), e), hit = 1;
        if (!hit) break;
    }
    static const char* const KW[] = {"class", "interface", "enum", "record", "struct", "object", "trait", "protocol", NULL};
    for (int i = 0; KW[i]; i++)
        if (word_is(q, KW[i])) {
            q = skip_ws(q + strlen(KW[i]), e);
            const char* en;
            int l = take_ident(q, e, &en);
            return l > 0 ? sink_add(k, q, l, line) : 0;
        }
    return 0;
}

static int sh_line(Sink* k, const char* p, int len, int line) {
    const char* e = p + len;
    const char* q = p;
    if (starts_with(q, "function ")) q = skip_ws(q + 9, e);
    const char* en;
    int l = take_ident(q, e, &en);
    if (l <= 0) return 0;
    const char* r = skip_ws(en, e);
    if (q != p || (r + 1 < e && r[0] == '(' && r[1] == ')')) return sink_add(k, q, l, line);
    return 0;
}

static int md_line(Sink* k, const char* p, int len, int line, int* fence) {
    if (len >= 3 && !strncmp(p, "```", 3)) {
        *fence = !*fence;
        return 0;
    }
    if (*fence || p[0] != '#') return 0;
    int h = 0;
    while (h < len && p[h] == '#') h++;
    if (h > 3 || h >= len || p[h] != ' ') return 0;
    const char* q = p + h + 1;
    int l = len - h - 1;
    while (l > 0 && (q[l - 1] == ' ' || q[l - 1] == '\r')) l--;
    return sink_add(k, q, l, line);
}

static int toml_line(Sink* k, const char* p, int len, int line) {
    if (len < 3 || p[0] != '[') return 0;
    const char* q = p;
    while (*q == '[') q++;
    const char* e = memchr(q, ']', len - (int)(q - p));
    return e ? sink_add(k, q, (int)(e - q), line) : 0;
}

static int make_line(Sink* k, const char* p, int len, int line) {
    if (len < 2) return 0;
    if (starts_with(p, "add_executable(") || starts_with(p, "add_library(") || starts_with(p, "project(")) {
        const char* q = strchr(p, '(') + 1;
        const char* e = q;
        while (e < p + len && *e != ' ' && *e != ')') e++;
        return sink_add(k, q, (int)(e - q), line);
    }
    if (!ident_char((unsigned char)p[0]) && p[0] != '/') return 0;
    const char* q = p;
    while (q < p + len && (ident_char((unsigned char)*q) || *q == '.' || *q == '/' || *q == '-')) q++;
    if (q < p + len && *q == ':' && (q + 1 == p + len || q[1] == ' ' || q[1] == '\t') && q > p) return sink_add(k, p, (int)(q - p), line);
    return 0;
}

typedef struct {
    int in_jobs;
    int jobs_indent;
    int steps;
    int top;
} YamlState;

static int yaml_line(Sink* k, const char* p, int len, int line, int workflow, YamlState* ys) {
    const char* e = p + len;
    int ind;
    const char* q = trim_start(p, e, &ind);
    if (q >= e || *q == '#') return 0;
    if (!workflow) {
        if (ind == 0 && ys->top < 14 && (ident_char((unsigned char)*q) || *q == '"')) {
            const char* c = memchr(q, ':', e - q);
            if (c && c > q) {
                ys->top++;
                return sink_add(k, q, (int)(c - q), line);
            }
        }
        return 0;
    }
    if (ind == 0) {
        ys->in_jobs = starts_with(q, "jobs:");
        if (starts_with(q, "name:")) {
            const char* v = skip_ws(q + 5, e);
            char nm[SYM_NAME_MAX + 8];
            int n = snprintf(nm, sizeof(nm), "name %.*s", (int)(e - v) > SYM_NAME_MAX - 8 ? SYM_NAME_MAX - 8 : (int)(e - v), v);
            return sink_add(k, nm, n, line);
        }
        return 0;
    }
    if (ys->in_jobs && ind <= 4 && ys->jobs_indent == 0) ys->jobs_indent = ind;
    if (ys->in_jobs && ind == ys->jobs_indent && ys->jobs_indent > 0 && ident_char((unsigned char)*q)) {
        const char* c = memchr(q, ':', e - q);
        if (c && c > q) {
            char nm[SYM_NAME_MAX + 8];
            int n = snprintf(nm, sizeof(nm), "job %.*s", (int)(c - q) > SYM_NAME_MAX - 8 ? SYM_NAME_MAX - 8 : (int)(c - q), q);
            return sink_add(k, nm, n, line);
        }
    }
    if (ind > ys->jobs_indent + 2 && ys->steps < 14) {
        const char* s = q;
        if (*s == '-') s = skip_ws(s + 1, e);
        if (starts_with(s, "name:")) {
            const char* v = skip_ws(s + 5, e);
            int vl = (int)(e - v);
            while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) vl--;
            ys->steps++;
            return sink_add(k, v, vl, line);
        }
    }
    return 0;
}

// Scans text for symbols into sink.
static void scan_symbols(Sink* k, int lang, const char* name, const char* txt, int n) {
    const char* end = txt + n;
    int line = 0, in_comment = 0, macros = 0, fence = 0;
    int header = ends_ci(name, ".h") || ends_ci(name, ".hpp") || ends_ci(name, ".hh") || ends_ci(name, ".inc");
    int workflow = lang == L_YAML && strstr(name, ".github/workflows/") != NULL;
    YamlState ys;
    memset(&ys, 0, sizeof(ys));
    for (const char* p = txt; p < end;) {
        const char* e = memchr(p, '\n', end - p);
        int len = e ? (int)(e - p) : (int)(end - p);
        const char* next = e ? e + 1 : end;
        line++;
        while (len > 0 && p[len - 1] == '\r') len--;
        int stop = 0;
        if (lang == L_C || lang == L_JS || lang == L_GO || lang == L_RS || lang == L_ZIG || lang == L_JAVA) {
            int was = in_comment;
            for (int i = 0; i + 1 < len; i++) {   // block comments (the line is skipped if it began inside one)
                if (!in_comment && p[i] == '/' && p[i + 1] == '*') in_comment = 1, i++;
                else if (in_comment && p[i] == '*' && p[i + 1] == '/') in_comment = 0, i++;
            }
            if (was) {
                p = next;
                continue;
            }
        }
        switch (lang) {
        case L_C: stop = c_line(k, p, len, line, header, &macros); break;
        case L_PY: stop = py_line(k, p, len, line); break;
        case L_JS: stop = js_line(k, p, len, line); break;
        case L_GO: stop = go_line(k, p, len, line); break;
        case L_RS: stop = rs_line(k, p, len, line, 0); break;
        case L_ZIG: stop = rs_line(k, p, len, line, 1); break;
        case L_JAVA: stop = java_line(k, p, len, line); break;
        case L_SH: stop = sh_line(k, p, len, line); break;
        case L_MD: stop = md_line(k, p, len, line, &fence); break;
        case L_YAML: stop = yaml_line(k, p, len, line, workflow, &ys); break;
        case L_TOML: stop = toml_line(k, p, len, line); break;
        case L_MAKE: stop = make_line(k, p, len, line); break;
        default: break;
        }
        if (stop) break;
        p = next;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// the index of a zip (shared by the map and the search)

#define MAXF ZIP_MAX_ENTS
#define READ_MAX 262144

static char rbuf[READ_MAX + 8];   // one file at a time

enum { K_SKIP = 0, K_SRC = 1, K_FOLD = 2 };

static const ZipEnt* g_ents;
static int g_n, g_root, g_total;
static int g_idx[MAXF];            // entries sorted by (folder, name)
static unsigned char g_kind[MAXF]; // per position in g_idx
static int g_fold_runs[64][3];     // [first position, count, dir length] of folded runs (for the "folded" lines)
static int g_nfold;

static int dir_len(const char* rel) {
    const char* s = strrchr(rel, '/');
    return s ? (int)(s - rel) + 1 : 0;
}

static int cmp_entries(const void* a, const void* b) {
    const char* x = g_ents[*(const int*)a].name + g_root;
    const char* y = g_ents[*(const int*)b].name + g_root;
    int dx = dir_len(x), dy = dir_len(y);
    int m = dx < dy ? dx : dy;
    int c = strncmp(x, y, m);
    if (c) return c;
    if (dx != dy) {   // one folder holds the other: the files of the outer one come first
        if (dx < dy) return -1;
        return 1;
    }
    return strcmp(x + dx, y + dy);
}

// Opens the zip and fills the index. Returns the number of entries or < 0.
static int load_index(const char* zip_path) {
    int total;
    int n = zip_open_dir(zip_path, &g_ents, &total);
    if (n < 0) return n;
    g_n = n;
    g_total = total;
    g_root = repo_root_len(g_ents, n);
    for (int i = 0; i < n; i++) g_idx[i] = i;
    qsort(g_idx, n, sizeof(int), cmp_entries);
    g_nfold = 0;
    int cand = 0;
    for (int p = 0; p < n; p++) {
        const char* rel = g_ents[g_idx[p]].name + g_root;
        if (!repo_is_text_name(rel)) g_kind[p] = K_SKIP;
        else if (repo_is_vendor_path(rel)) g_kind[p] = K_FOLD, cand++;
        else g_kind[p] = K_SRC, cand++;
    }
    // folders that are not the project's own code: by name (mp, lib, deps...) or because they hold 80+ files of their own
    for (int p = 0; p < n;) {
        const char* rel = g_ents[g_idx[p]].name + g_root;
        int dl = dir_len(rel);
        int q = p;
        while (q < n) {
            const char* r2 = g_ents[g_idx[q]].name + g_root;
            if (dir_len(r2) != dl || strncmp(r2, rel, dl)) break;
            q++;
        }
        int direct = 0;
        for (int i = p; i < q; i++)
            if (g_kind[i] == K_SRC) direct++;
        int fold = 0;
        if (dl > 0 && direct >= 3) {   // the last folder name, or any folder of the path
            const char* s = rel;
            while (s < rel + dl) {
                const char* e = memchr(s, '/', rel + dl - s);
                if (!e) break;
                if (vendor_dir_name(s, (int)(e - s))) fold = 1;
                s = e + 1;
            }
        }
        if (direct >= 80) fold = 1;
        if (fold && dl > 0) {
            for (int i = p; i < q; i++)
                if (g_kind[i] == K_SRC) g_kind[i] = K_FOLD;
        }
        p = q;
    }
    // a run of folded files per folder, for the one-line summaries
    for (int p = 0; p < n;) {
        if (g_kind[p] != K_FOLD) {
            p++;
            continue;
        }
        const char* rel = g_ents[g_idx[p]].name + g_root;
        int dl = dir_len(rel);
        int q = p;
        while (q < n && g_kind[q] == K_FOLD) {
            const char* r2 = g_ents[g_idx[q]].name + g_root;
            if (dir_len(r2) != dl || strncmp(r2, rel, dl)) break;
            q++;
        }
        if (g_nfold < 64) {
            g_fold_runs[g_nfold][0] = p;
            g_fold_runs[g_nfold][1] = q - p;
            g_fold_runs[g_nfold][2] = dl;
            g_nfold++;
        }
        p = q;
    }
    (void)cand;
    return n;
}

// Reads entry at position p of the index into rbuf (NUL-terminated, CRs kept). Returns the bytes, or < 0 (binary,
// damaged, empty, too big for the buffer when whole = 1). *cut = 1 if the entry is longer than what was read.
static int read_entry(const char* zip_path, int p, int* cut) {
    const ZipEnt* e = &g_ents[g_idx[p]];
    int c = 0;
    int r = zip_read(zip_path, e, rbuf, READ_MAX, &c);
    if (r <= 0) return -1;
    int lim = r < 400 ? r : 400, ctrl = 0;
    for (int i = 0; i < lim; i++) ctrl += (unsigned char)rbuf[i] < 9 || ((unsigned char)rbuf[i] > 13 && (unsigned char)rbuf[i] < 32);
    if (ctrl * 20 > lim) return -2;
    *cut = c;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// the map

#define SPOOL 7000
#define NPOOL (160 * 1024)
static Sym spool[SPOOL];
static char npool[NPOOL];

typedef struct {
    int first, n;      // symbols in spool
    int lines;         // 0 unknown
    int approx;        // lines is an estimate (the file is longer than the read buffer)
    int rank;          // smaller = more worth detail
} FInfo;
static FInfo g_info[MAXF];

static int g_rank_order[MAXF];

static int cmp_rank(const void* a, const void* b) {
    int x = g_info[*(const int*)a].rank, y = g_info[*(const int*)b].rank;
    if (x != y) return x < y ? -1 : 1;
    return *(const int*)a - *(const int*)b;
}

static int slash_count(const char* s) {
    int n = 0;
    for (; *s; s++) n += *s == '/';
    return n;
}

static int key_file(const char* rel) {
    const char* b = base_name(rel);
    if (starts_with(rel, ".github/workflows/")) return 1;
    if (!strchr(rel, '/') && (eq_ci(b, "readme.md") || eq_ci(b, "makefile") || eq_ci(b, "cmakelists.txt") || eq_ci(b, "package.json") ||
                              eq_ci(b, "build.py") || eq_ci(b, "build_chat.py") || eq_ci(b, "setup.py") || eq_ci(b, "pyproject.toml") ||
                              eq_ci(b, "cargo.toml") || eq_ci(b, "go.mod") || eq_ci(b, "build.zig") || eq_ci(b, "dockerfile")))
        return 1;
    return 0;
}

static void fmt_size(char* out, int max, unsigned bytes) {
    if (bytes < 1000) snprintf(out, max, "%uB", bytes);
    else if (bytes < 10000) snprintf(out, max, "%u.%uK", bytes / 1000, (bytes % 1000) / 100);
    else if (bytes < 1000000) snprintf(out, max, "%uK", bytes / 1000);
    else snprintf(out, max, "%u.%uM", bytes / 1000000, (bytes % 1000000) / 100000);
}

// Writes the map with up to syms symbols for the files whose rank is < detail. Returns the length or -1 on overflow.
static int render_map(const char* zip_name, char* out, int max, int syms, int detail, int n_files, int n_src, int n_skip, int n_fold) {
    int o = 0;
#define EMIT(...)                                              \
    do {                                                       \
        int w_ = snprintf(out + o, max - o, __VA_ARGS__);      \
        if (w_ < 0 || o + w_ >= max) return -1;                \
        o += w_;                                               \
    } while (0)
    EMIT("Repo map of %s: %d files (%d source/text, %d binary or assets not listed, %d in folded folders). Paths are relative%s%.*s.\n",
         zip_name, n_files, n_src, n_skip, n_fold, g_root ? " to " : "", g_root, g_root ? g_ents[0].name : "");
    EMIT("Ask for code with: @zip:path (whole file)  @zip:path:A-B (lines)  @zip:path#name (one function)  @zip?word1+word2 (search the repo).\n");
    char last_dir[160] = "\1";
    int shown_fold = 0;
    for (int p = 0; p < g_n; p++) {
        const char* rel = g_ents[g_idx[p]].name + g_root;
        int dl = dir_len(rel);
        if (g_kind[p] == K_FOLD) {
            int run = -1;
            for (int r = 0; r < g_nfold; r++)
                if (g_fold_runs[r][0] == p) run = r;
            if (run < 0) continue;
            char exts[48] = "";
            int el = 0;
            for (int i = p; i < p + g_fold_runs[run][1] && el < 40; i++) {
                const char* nm = g_ents[g_idx[i]].name;
                const char* dot = strrchr(base_name(nm), '.');
                if (dot && !strstr(exts, dot) && el + (int)strlen(dot) + 2 < (int)sizeof(exts)) el += snprintf(exts + el, sizeof(exts) - el, "%s ", dot);
            }
            EMIT("[%.*s] %d files %snot detailed (vendored or generated)\n", dl, rel, g_fold_runs[run][1], exts);
            shown_fold++;
            continue;
        }
        if (g_kind[p] != K_SRC) continue;
        char dir[160];
        snprintf(dir, sizeof(dir), "%.*s", dl < 159 ? dl : 159, rel);
        if (strcmp(dir, last_dir)) {
            EMIT("[%s]\n", dl ? dir : "./");
            snprintf(last_dir, sizeof(last_dir), "%s", dir);
        }
        const FInfo* fi = &g_info[p];
        char sz[16];
        fmt_size(sz, sizeof(sz), g_ents[g_idx[p]].usize);
        if (fi->lines) EMIT(" %s %s/%s%dL", rel + dl, sz, fi->approx ? "~" : "", fi->lines);
        else EMIT(" %s %s", rel + dl, sz);
        if (fi->rank < detail && fi->n > 0 && syms > 0) {
            int cnt = fi->n < syms ? fi->n : syms;
            EMIT(": ");
            for (int s = 0; s < cnt; s++) {
                const char* nm = npool + spool[fi->first + s].off;
                if (strchr(nm, ' ') && lang_of(rel) != L_C) EMIT("%s\"%s\"@%d", s ? ", " : "", nm, spool[fi->first + s].line);
                else EMIT("%s%s@%d", s ? ", " : "", nm, spool[fi->first + s].line);
            }
            if (fi->n > cnt) EMIT(", +%d", fi->n - cnt);
        }
        EMIT("\n");
    }
#undef EMIT
    (void)shown_fold;
    return o;
}


// ---------------------------------------------------------------------------------------------------------------------
// phase 12: what kind of project it is (languages, build files, CI, tests), as two or three lines on top of the map


// 1 when the first len bytes of s contain lit.
static int strstr_n(const char* s, int len, const char* lit) {
    int l = (int)strlen(lit);
    for (int i = 0; i + l <= len; i++)
        if (!memcmp(s + i, lit, l)) return 1;
    return 0;
}

static int base_is(const char* rel, const char* name) {
    const char* b = strrchr(rel, '/');
    b = b ? b + 1 : rel;
    return !strcmp(b, name);
}

static int has_ext(const char* rel, const char* ext) {
    int l = (int)strlen(rel), e = (int)strlen(ext);
    return l > e && rel[l - e - 1] == '.' && !strcmp(rel + l - e, ext);
}

static const char* lang_name(const char* rel) {
    static const struct { const char* ext; const char* name; } T[] = {
        {"c", "C"}, {"h", "C"}, {"cc", "C++"}, {"cpp", "C++"}, {"hpp", "C++"}, {"py", "Python"}, {"js", "JavaScript"}, {"mjs", "JavaScript"},
        {"ts", "TypeScript"}, {"tsx", "TypeScript"}, {"jsx", "JavaScript"}, {"go", "Go"}, {"rs", "Rust"}, {"zig", "Zig"}, {"java", "Java"},
        {"kt", "Kotlin"}, {"cs", "C#"}, {"sh", "Shell"}, {"lua", "Lua"}, {"rb", "Ruby"}, {"php", "PHP"}, {"swift", "Swift"}, {"rpy", "Ren'Py"},
        {"gd", "GDScript"}, {"glsl", "GLSL"}, {"cl", "OpenCL"}, {"asm", "Assembly"}, {"s", "Assembly"}, {NULL, NULL}};
    for (int i = 0; T[i].ext; i++)
        if (has_ext(rel, T[i].ext)) return T[i].name;
    return NULL;
}

// Triggers, jobs and number of steps of a GitHub Actions workflow in rbuf[0..len), as "on: push, workflow_dispatch; jobs: build (14 steps)".
static void workflow_summary(const char* t, int len, char* out, int max) {
    char on[160] = "", jobs[200] = "";
    int in_on = 0, in_jobs = 0, steps_ind = -1, steps = 0, job_steps = 0;
    char job[40] = "";
    int p = 0;
    while (p < len) {
        int e = p;
        while (e < len && t[e] != '\n') e++;
        int ind = 0;
        while (p + ind < e && t[p + ind] == ' ') ind++;
        const char* l = t + p + ind;
        int ll = e - p - ind;
        if (ll > 0 && l[ll - 1] == '\r') ll--;
        if (ll > 0 && l[0] != '#') {
            if (ind == 0) {
                in_on = in_jobs = 0;
                if (!strncmp(l, "on:", 3) || !strncmp(l, "'on':", 5) || !strncmp(l, "\"on\":", 5)) {
                    in_on = 1;
                    const char* c = memchr(l, ':', ll);
                    int rest = ll - (int)(c + 1 - l);
                    while (rest > 0 && c[1] == ' ') c++, rest--;
                    if (rest > 0) snprintf(on, sizeof(on), "%.*s", rest > 80 ? 80 : rest, c + 1);
                } else if (!strncmp(l, "jobs:", 5)) in_jobs = 1;
            } else if (in_on && ind == 2) {
                const char* c = memchr(l, ':', ll);
                int nl = c ? (int)(c - l) : ll;
                int ol = (int)strlen(on);
                if (nl > 0 && ol + nl + 3 < (int)sizeof(on)) snprintf(on + ol, sizeof(on) - ol, "%s%.*s", ol ? ", " : "", nl, l);
            } else if (in_jobs && ind == 2 && memchr(l, ':', ll) && l[0] != '-') {
                if (job[0]) {
                    int jl = (int)strlen(jobs);
                    snprintf(jobs + jl, sizeof(jobs) - jl, "%s%s (%d steps)", jl ? ", " : "", job, job_steps);
                }
                const char* c = memchr(l, ':', ll);
                snprintf(job, sizeof(job), "%.*s", (int)(c - l) > 39 ? 39 : (int)(c - l), l);
                job_steps = 0;
                steps_ind = -1;
            } else if (in_jobs && ll >= 2 && l[0] == '-' && l[1] == ' ') {
                if (steps_ind < 0 && ind >= 4) steps_ind = ind;
                if (ind == steps_ind) job_steps++, steps++;
            }
        }
        p = e + 1;
    }
    if (job[0]) {
        int jl = (int)strlen(jobs);
        snprintf(jobs + jl, sizeof(jobs) - jl, "%s%s (%d steps)", jl ? ", " : "", job, job_steps);
    }
    (void)steps;
    snprintf(out, max, "on: %s; jobs: %s", on[0] ? on : "?", jobs[0] ? jobs : "?");
}

static int build_facts(const char* zip_path, int n, char* out, int max) {
    long by_lang[32];
    const char* names[32];
    int nl = 0;
    long total = 0;
    int n_tests = 0, n_wf = 0;
    char builds[200] = "", wf[700] = "";
    static const char* const BUILD[] = {"Makefile", "CMakeLists.txt", "build.zig", "package.json", "Cargo.toml", "go.mod", "pyproject.toml",
                                        "setup.py", "requirements.txt", "pom.xml", "build.gradle", "Dockerfile", "meson.build", "build.py",
                                        "project.godot", "options.rpy", NULL};
    for (int p = 0; p < n; p++) {
        const char* rel = g_ents[g_idx[p]].name + g_root;
        if (g_kind[p] == K_FOLD) continue;
        for (int b = 0; BUILD[b]; b++)
            if (base_is(rel, BUILD[b]) && strlen(builds) + strlen(rel) + 3 < sizeof(builds)) {
                int l = (int)strlen(builds);
                if (!strstr(builds, rel)) snprintf(builds + l, sizeof(builds) - l, "%s%s", l ? ", " : "", rel);
            }
        if (!strncmp(rel, "tests/", 6) || !strncmp(rel, "test/", 5) || strstr(rel, "_test.") || strstr(rel, ".test.")) n_tests++;
        if (g_kind[p] != K_SRC) continue;
        const char* ln = lang_name(rel);
        if (ln && strncmp(rel, ".github/", 8) != 0) {
            int k = 0;
            while (k < nl && strcmp(names[k], ln)) k++;
            if (k == nl && nl < 32) names[nl] = ln, by_lang[nl++] = 0;
            if (k < 32) by_lang[k] += g_ents[g_idx[p]].usize;
            total += g_ents[g_idx[p]].usize;
        }
        if (!strncmp(rel, ".github/workflows/", 18) && (has_ext(rel, "yml") || has_ext(rel, "yaml")) && n_wf < 3 && g_ents[g_idx[p]].usize < 40000) {
            int cut = 0;
            int r = read_entry(zip_path, p, &cut);
            if (r > 0) {
                char sm[300];
                workflow_summary(rbuf, r, sm, sizeof(sm));
                int l = (int)strlen(wf);
                snprintf(wf + l, sizeof(wf) - l, "%s%s (%s)", l ? "; " : "", rel, sm);
                n_wf++;
            }
        }
    }
    for (int i = 0; i < nl; i++)   // biggest first
        for (int j = i + 1; j < nl; j++)
            if (by_lang[j] > by_lang[i]) {
                long tl = by_lang[i];
                by_lang[i] = by_lang[j];
                by_lang[j] = tl;
                const char* tn = names[i];
                names[i] = names[j];
                names[j] = tn;
            }
    int o = 0;
    char langs[160] = "";
    for (int i = 0; i < nl && i < 4 && total > 0; i++) {
        int l = (int)strlen(langs);
        int pct = (int)(by_lang[i] * 100 / total);
        if (pct < 3 && i > 0) break;
        snprintf(langs + l, sizeof(langs) - l, "%s%s %d%%", l ? ", " : "", names[i], pct);
    }
    o += snprintf(out + o, max - o, "Project: %s | build: %s | tests: %d files\n", langs[0] ? langs : "no source", builds[0] ? builds : "none found", n_tests);
    if (n_wf && o < max) o += snprintf(out + o, max - o, "CI: %s\n", wf);
    if (o >= max) o = 0;
    out[o] = 0;
    return o;
}

// All the places that use a name (an identifier), file by file, definitions first. Returns the number of files with hits
// (0 = none), < 0 as zip_open_dir does; *total gets the number of hits.
int repo_refs(const char* zip_path, const char* name, char* out, int max, int* total_hits) {
    int n = load_index(zip_path);
    if (n < 0) return n;
    char id[72];
    int il = 0;
    while (name[il] && il < 64 && (isalnum((unsigned char)name[il]) || name[il] == '_')) id[il] = name[il], il++;
    id[il] = 0;
    out[0] = 0;
    if (total_hits) *total_hits = 0;
    if (il < 2) {
        snprintf(out, max, "Give an identifier of at least 2 characters.\n");
        return 0;
    }
    int o = 0, files = 0, hits_all = 0, pass_files = 0;
    char defs_done[MAXF / 8 + 1];
    memset(defs_done, 0, sizeof(defs_done));
    for (int pass = 0; pass < 2; pass++) {   // pass 0: files that define it; pass 1: the rest
        for (int p = 0; p < n; p++) {
            if (g_kind[p] != K_SRC) continue;
            if (pass == 1 && (defs_done[p >> 3] & (1 << (p & 7)))) continue;
            if (g_ents[g_idx[p]].usize > READ_MAX) continue;
            int cut = 0;
            int r = read_entry(zip_path, p, &cut);
            if (r < 0) continue;
            const char* rel = g_ents[g_idx[p]].name + g_root;
            // collect hits of this file
            int hits = 0, def_line = 0;
            char lines_txt[1500];
            int lt = 0, lineno = 1, shown = 0;
            int ls = 0;
            for (int i = 0; i <= r; i++) {
                if (i < r && rbuf[i] != '\n') continue;
                int le = i;
                // whole-word search in [ls, le)
                int found = 0, is_def = 0;
                for (int k = ls; k + il <= le; k++) {
                    if (rbuf[k] != id[0] || memcmp(rbuf + k, id, il)) continue;
                    int before = k > ls && (isalnum((unsigned char)rbuf[k - 1]) || rbuf[k - 1] == '_');
                    int after = k + il < le && (isalnum((unsigned char)rbuf[k + il]) || rbuf[k + il] == '_');
                    if (before || after) continue;
                    found = 1;
                    int q = k + il;
                    while (q < le && rbuf[q] == ' ') q++;
                    if (q < le && rbuf[q] == '(') {   // a definition: code at the left margin or after def/func/fn/function, not indented
                        int ind = 0;
                        while (ls + ind < le && rbuf[ls + ind] == ' ') ind++;
                        const char* ln0 = rbuf + ls;
                        if ((ind == 0 && ln0[0] != '#' && ln0[0] != '}' && ln0[0] != '/' && ln0[0] != ' ' && k > ls && !strstr_n(ln0, k - ls, "=") && !strstr_n(ln0, k - ls, "return")) ||
                            strstr_n(ln0, k - ls, "def ") || strstr_n(ln0, k - ls, "func ") || strstr_n(ln0, k - ls, "fn ") || strstr_n(ln0, k - ls, "function "))
                            is_def = 1;
                    }
                    break;
                }
                if (found) {
                    hits++;
                    if (is_def && !def_line) def_line = lineno;
                    if (shown < 4 && (pass == 1 || is_def || shown < 3)) {
                        int ll = le - ls;
                        while (ll > 0 && (rbuf[ls] == ' ' || rbuf[ls] == '\t')) ls++, ll--;
                        if (ll > 110) ll = 110;
                        if (ll > 0 && rbuf[ls + ll - 1] == '\r') ll--;
                        if (lt + ll + 16 < (int)sizeof(lines_txt)) {
                            lt += snprintf(lines_txt + lt, sizeof(lines_txt) - lt, "  %d%s: %.*s\n", lineno, is_def ? " (definition)" : "", ll, rbuf + ls);
                            shown++;
                        }
                    }
                }
                ls = i + 1;
                lineno++;
            }
            if (!hits) continue;
            if ((pass == 0) != (def_line > 0)) continue;
            if (pass == 0) defs_done[p >> 3] |= (char)(1 << (p & 7));
            hits_all += hits;
            files++;
            pass_files++;
            if (files > 40 || o + lt + 120 >= max) {
                if (o + 60 < max) o += snprintf(out + o, max - o, "... (more files; narrow it with @zip?name+word)\n");
                goto done;
            }
            o += snprintf(out + o, max - o, "--- %s: %d use%s%s ---\n%s", rel, hits, hits == 1 ? "" : "s", hits > shown ? " (some shown)" : "", lines_txt);
        }
    }
done:
    (void)pass_files;
    if (total_hits) *total_hits = hits_all;
    if (!files) snprintf(out, max, "No file uses \"%s\" as a whole word.\n", id);
    return files;
}

int repomap_build(const char* zip_path, char* out, int max, int* n_source) {
    int n = load_index(zip_path);
    if (n < 0) return n;
    if (max < 600) return -3;
    int n_src = 0, n_skip = 0, n_fold = 0;
    for (int p = 0; p < n; p++) {
        if (g_kind[p] == K_SRC) n_src++;
        else if (g_kind[p] == K_FOLD) n_fold++;
        else n_skip++;
    }
    if (n_source) *n_source = n_src;
    // symbols
    int sp = 0, np = 0, read_files = 0;
    long budget_bytes = 12L * 1024 * 1024;
    for (int p = 0; p < n; p++) {
        FInfo* fi = &g_info[p];
        memset(fi, 0, sizeof(*fi));
        fi->first = sp;
        const char* rel = g_ents[g_idx[p]].name + g_root;
        {   // who gets symbols first when the map is tight: key files, then the big ones (they cannot be read whole), shallow before deep
            int big = (int)(g_ents[g_idx[p]].usize / 3000);
            fi->rank = key_file(rel) ? -100 : slash_count(rel) * 6 - (big > 20 ? 20 : big);
        }
        if (g_kind[p] != K_SRC) continue;
        if (g_ents[g_idx[p]].usize > READ_MAX || read_files >= 700 || budget_bytes <= 0) {
            if (g_ents[g_idx[p]].usize > READ_MAX) fi->rank += 1000;
            continue;
        }
        int cut = 0;
        int r = read_entry(zip_path, p, &cut);
        read_files++;
        if (r < 0) continue;
        budget_bytes -= r;
        int lines = 0;
        for (int i = 0; i < r; i++) lines += rbuf[i] == '\n';
        if (r > 0 && rbuf[r - 1] != '\n') lines++;
        fi->lines = lines;
        Sink k;
        k.syms = spool + sp;
        k.max = SPOOL - sp < 40 ? SPOOL - sp : 40;
        k.n = 0;
        k.names = npool + np;
        k.noff = 0;
        k.nmax = NPOOL - np;
        k.want = NULL;
        k.found_line = k.found_indent = 0;
        scan_symbols(&k, lang_of(rel), rel, rbuf, r);
        for (int s = 0; s < k.n; s++) k.syms[s].off += np;   // offsets into npool
        fi->n = k.n;
        sp += k.n;
        np += k.noff;
    }
    // detail ranks
    for (int p = 0; p < n; p++) g_rank_order[p] = p;
    qsort(g_rank_order, n, sizeof(int), cmp_rank);
    for (int i = 0; i < n; i++) g_info[g_rank_order[i]].rank = i;   // dense ranks: 0 = most important
    const char* zname = strrchr(zip_path, '/');
    zname = zname ? zname + 1 : zip_path;
    int flen = build_facts(zip_path, n, out, max / 6);   // phase 12: the kind of project goes first
    char* ob = out + flen;
    int omax = max - flen;
    static const int LEVELS[][2] = {{14, 1 << 30}, {8, 1 << 30}, {5, 1 << 30}, {4, 80}, {3, 40}, {2, 20}, {0, 0}};
    for (unsigned lv = 0; lv < sizeof(LEVELS) / sizeof(LEVELS[0]); lv++) {
        int o = render_map(zname, ob, omax, LEVELS[lv][0], LEVELS[lv][1], g_total, n_src, n_skip, n_fold);
        if (o >= 0) return o + flen;   // the most detailed level that fits
    }
    // not even the names fit: the head of the last level, cut at a line break
    int o = render_map(zname, ob, omax, 0, 0, g_total, n_src, n_skip, n_fold);
    if (o < 0) {
        o = (int)strlen(ob);
        while (o > 0 && ob[o - 1] != '\n') o--;
        ob[o] = 0;
    }
    return o + flen;
}

// ---------------------------------------------------------------------------------------------------------------------
// lines and symbols of one file

#define MAXL 40000
static int lstart[MAXL + 2];   // offset of the start of line i (1-based); lstart[nl + 1] = len

// Fills lstart; returns the number of lines (capped at MAXL).
static int index_lines(const char* text, int len) {
    int nl = 0;
    lstart[1] = 0;
    for (int i = 0; i < len && nl < MAXL; i++)
        if (text[i] == '\n') {
            nl++;
            lstart[nl + 1] = i + 1;
        }
    if (len > 0 && (nl == 0 || lstart[nl + 1] < len)) nl++, lstart[nl + 1] = len;
    return nl;
}

static int line_len(const char* text, int l) {   // without the line break and CR
    int n = lstart[l + 1] - lstart[l];
    while (n > 0 && (text[lstart[l] + n - 1] == '\n' || text[lstart[l] + n - 1] == '\r')) n--;
    return n;
}

int repo_extract_lines(const char* text, int len, int a, int b, char* out, int max, int* total) {
    int nl = index_lines(text, len);
    *total = nl;
    int o = 0;
    if (a < 1) a = 1;
    if (b > nl) b = nl;
    for (int l = a; l <= b; l++) {
        int ll = line_len(text, l);
        if (o + ll + 2 >= max) break;
        memcpy(out + o, text + lstart[l], ll);
        o += ll;
        out[o++] = '\n';
    }
    out[o] = 0;
    return o;
}

static int line_indent(const char* text, int l) {
    const char* p = text + lstart[l];
    int ll = line_len(text, l), ind = 0;
    for (int i = 0; i < ll && (p[i] == ' ' || p[i] == '\t'); i++) ind += p[i] == '\t' ? 4 : 1;
    return ind;
}

static int line_blank(const char* text, int l) {
    const char* p = text + lstart[l];
    int ll = line_len(text, l);
    for (int i = 0; i < ll; i++)
        if (p[i] != ' ' && p[i] != '\t') return 0;
    return 1;
}

// The line of the '}' that closes the first '{' found from line a (within 8 lines), or 0 (a prototype or no body).
static int brace_end_line(const char* text, int len, int nl, int a) {
    int i = lstart[a], depth = 0, started = 0;
    int limit_line = a + 8;
    int in_line_c = 0, in_block = 0, in_str = 0, in_chr = 0;
    for (; i < len; i++) {
        char c = text[i];
        if (c == '\n') {
            in_line_c = 0;
            if (!started) {
                int l = 1;   // which line are we on? lstart is sorted
                int lo = a, hi = nl;
                while (lo < hi) {
                    int mid = (lo + hi + 1) / 2;
                    if (lstart[mid] <= i) lo = mid;
                    else hi = mid - 1;
                }
                l = lo;
                if (l >= limit_line) return 0;
            }
            continue;
        }
        if (in_line_c) continue;
        if (in_block) {
            if (c == '*' && i + 1 < len && text[i + 1] == '/') in_block = 0, i++;
            continue;
        }
        if (in_str) {
            if (c == '\\') i++;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\') i++;
            else if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '/') in_line_c = 1, i++;
        else if (c == '/' && i + 1 < len && text[i + 1] == '*') in_block = 1, i++;
        else if (c == '"') in_str = 1;
        else if (c == '\'') in_chr = 1;
        else if (c == ';' && !started && depth == 0) return 0;
        else if (c == '{') depth++, started = 1;
        else if (c == '}' && started && --depth == 0) {
            int lo = a, hi = nl;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (lstart[mid] <= i) lo = mid;
                else hi = mid - 1;
            }
            return lo;
        }
    }
    return 0;
}

int repo_symbol_range(const char* name_of_file, const char* text, int len, const char* name, int* line_a, int* line_b) {
    int lang = lang_of(name_of_file);
    if (lang == L_NONE || !*name) return 0;
    Sink k;
    memset(&k, 0, sizeof(k));
    k.want = name;
    scan_symbols(&k, lang, name_of_file, text, len);
    if (!k.found_line) return 0;
    int nl = index_lines(text, len);
    int a = k.found_line, b = a;
    if (lang == L_C) {
        // a forward declaration ("static void f(E* e);" near the top) comes before the definition: if the match has no
        // body, look for the next one that does (up to 12 matches); else keep the first
        int cur = a;
        for (int tries = 0; tries < 12 && cur; tries++) {
            if (brace_end_line(text, len, nl, cur) > 0) {
                a = b = cur;
                break;
            }
            Sink k2;
            memset(&k2, 0, sizeof(k2));
            k2.want = name;
            k2.after_line = cur;
            scan_symbols(&k2, lang, name_of_file, text, len);
            cur = k2.found_line;
        }
    }
    if (lang == L_PY) {
        int ind = line_indent(text, a);
        b = a;
        for (int l = a + 1; l <= nl; l++) {
            if (line_blank(text, l)) continue;
            if (line_indent(text, l) <= ind) break;
            b = l;
        }
        while (a > 1 && line_indent(text, a - 1) == ind && text[lstart[a - 1] + ind] == '@') a--;
    } else if (lang == L_MD) {
        int h = 0;
        while (h < line_len(text, a) && text[lstart[a] + h] == '#') h++;
        b = a;
        for (int l = a + 1; l <= nl; l++) {
            int h2 = 0;
            while (h2 < line_len(text, l) && text[lstart[l] + h2] == '#') h2++;
            if (h2 > 0 && h2 <= h && text[lstart[l] + h2] == ' ') break;
            b = l;
        }
    } else if (lang == L_C || lang == L_JS || lang == L_GO || lang == L_RS || lang == L_ZIG || lang == L_JAVA) {
        int e = brace_end_line(text, len, nl, a);
        b = e > 0 ? e : a;
        if (b - a > 600) b = a + 600;
    } else {
        b = a + 40 < nl ? a + 40 : nl;
    }
    for (int up = 0; up < 15 && a > 1; up++) {   // comment lines right above
        const char* p = text + lstart[a - 1];
        int ll = line_len(text, a - 1), i = 0;
        while (i < ll && (p[i] == ' ' || p[i] == '\t')) i++;
        if (i < ll && (!strncmp(p + i, "//", 2) || !strncmp(p + i, "/*", 2) || p[i] == '*' || (lang == L_SH && p[i] == '#'))) a--;
        else break;
    }
    *line_a = a;
    *line_b = b;
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// keywords of a message

static const char* const STOP[] = {
    "que", "los", "las", "una", "uno", "unos", "unas", "con", "para", "por", "del", "como", "pero", "sus", "mas", "muy", "sin", "sobre",
    "entre", "esta", "este", "esto", "estos", "estas", "eso", "esa", "ese", "hay", "ser", "fue", "son", "era", "cuando", "donde",
    "porque", "puedo", "puedes", "quiero", "quieres", "necesito", "hacer", "hazme", "haz", "dame", "dime", "favor", "ahora", "aqui",
    "tambien", "todo", "todos", "toda", "todas", "cada", "algo", "otro", "otra", "otros", "tengo", "tiene", "tienen", "debe", "debes",
    "usar", "usa", "uso", "the", "and", "for", "with", "that", "this", "from", "have", "has", "are", "was", "not", "you", "your", "can",
    "could", "should", "would", "please", "make", "need", "want", "into", "about", "also", "some", "them", "they", "there", "then",
    "when", "what", "which", "where", "how", "why", "mejor", "mejora", "mejoras", "mejorar", "cambia", "cambiar", "cambio", "cambios",
    "agrega", "agregar", "anade", "anadir", "quita", "quitar", "arregla", "arreglar", "corrige", "corregir", "revisa", "revisar",
    "analiza", "analizar", "codigo", "funcion", "funciones", "repo", "zip", "proyecto", "hola", "gracias", "bien", "bueno", "dentro",
    "desde", "hasta", "segun", "tipo", "forma", "manera", "parte", "partes", "tema", "cosa", "cosas", "paso", "pasos", "primero",
    "luego", "despues", "antes", "siempre", "nunca", "solo", "sino", "aunque", "mismo", "misma", "vez", "veces", "tal", "cual",
    "quien", "ver", "veo", "vea", "dar", "doy", "dar", "poner", "pon", "crea", "crear", "genera", "generar", "escribe", "escribir",
    "explica", "explicar", "dime", "muestra", "mostrar", "necesita", "asi", "pueda", "puede", "pueden", "sea", "sean", "esté", NULL};

static const struct {
    const char* es;
    const char* en;
} SYN[] = {{"red", "net network http"},        {"internet", "net network http"},   {"archivo", "file"},        {"archivos", "file files"},
           {"contexto", "context ctx"},         {"teclado", "keyboard kb"},          {"raton", "mouse"},          {"mando", "pad controller"},
           {"pantalla", "screen draw"},         {"voz", "voice tts asr"},            {"modelo", "model engine"},  {"busqueda", "search"},
           {"buscar", "search"},                {"guardar", "save outfiles"},        {"guardado", "save"},        {"pensar", "think"},
           {"razonamiento", "think"},           {"descargar", "download get"},       {"velocidad", "speed"},      {"memoria", "memory mem"},
           {"sistema", "system"},               {"instrucciones", "system prompt"},  {"limite", "limit max"},     {"limites", "limit max"},
           {"mensaje", "message msg"},          {"mensajes", "message msg"},         {"respuesta", "reply answer"}, {"boton", "button btn"},
           {"interfaz", "ui draw"},             {"texto", "text"},                   {"lectura", "read"},         {"lista", "list"},
           {"comando", "cmd command"},          {"comandos", "cmd command"},         {"compilar", "build compile"}, {"compilacion", "build compile"},
           {"flujo", "workflow"},               {"acciones", "workflow actions"},    {"configuracion", "config"}, {"chat", "chat"},
           {"conversacion", "chat"},            {"imagen", "image"},                 {"sonido", "audio"},         {"audio", "audio"},
           {"errores", "error fail"},           {"error", "error fail"},             {"fallo", "error fail"},     {"aplicar", "patch apply"},
           {"parche", "patch apply"},           {"adjuntar", "attach files"},        {"adjunto", "attach files"}, {NULL, NULL}};

static int fold_char(const unsigned char* s, int* adv) {   // one character of a token: ASCII (lower case), or a folded accent
    *adv = 1;
    unsigned char c = s[0];
    if (c < 0x80) return lc(c);
    if (c == 0xC3 && s[1]) {
        *adv = 2;
        switch (s[1]) {
        case 0xA1: case 0x81: return 'a';
        case 0xA9: case 0x89: return 'e';
        case 0xAD: case 0x8D: return 'i';
        case 0xB3: case 0x93: return 'o';
        case 0xBA: case 0x9A: case 0xBC: case 0x9C: return 'u';
        case 0xB1: case 0x91: return 'n';
        }
    }
    return 0;   // another character: ends the token
}

static int in_list(const char* const* l, const char* w) {
    for (int i = 0; l[i]; i++)
        if (!strcmp(l[i], w)) return 1;
    return 0;
}

static int kw_add(char* out, int max, int* o, int* count, const char* w) {
    if (*count >= 12) return 0;
    int wl = (int)strlen(w);
    // already there?
    for (const char* p = out; *p;) {
        const char* e = strchr(p, ' ');
        int l = e ? (int)(e - p) : (int)strlen(p);
        if (l == wl && !strncmp(p, w, l)) return 1;
        p = e ? e + 1 : p + l;
    }
    if (*o + wl + 2 >= max) return 0;
    if (*o) out[(*o)++] = ' ';
    memcpy(out + *o, w, wl);
    *o += wl;
    out[*o] = 0;
    (*count)++;
    return 1;
}

int repo_keywords(const char* text, char* out, int max) {
    int o = 0, count = 0;
    out[0] = 0;
    const unsigned char* s = (const unsigned char*)text;
    while (*s && count < 12) {
        while (*s) {   // start of a token
            int adv;
            int c = fold_char(s, &adv);
            if (c && (ident_char(c) || c == '.' || c == '/' || c == '-')) break;
            s += adv;
        }
        if (!*s) break;
        char prev = s > (const unsigned char*)text ? (char)s[-1] : ' ';
        char tok[64];
        int tl = 0;
        while (*s) {
            int adv;
            int c = fold_char(s, &adv);
            if (!c || !(ident_char(c) || c == '.' || c == '/' || c == '-')) break;
            if (tl < 63) tok[tl++] = (char)c;
            s += adv;
        }
        tok[tl] = 0;
        while (tl > 0 && (tok[tl - 1] == '.' || tok[tl - 1] == '-' || tok[tl - 1] == '/')) tok[--tl] = 0;
        if (tl < 3 || prev == '@' || tok[0] == '/' || tok[0] == '.' || tok[0] == '-') continue;   // @names and /commands are not topics
        int digits = 1;
        for (int i = 0; i < tl; i++) digits &= tok[i] >= '0' && tok[i] <= '9';
        if (digits) continue;
        int syn = 0;
        for (int i = 0; SYN[i].es; i++)
            if (!strcmp(SYN[i].es, tok)) {
                syn = 1;
                char buf[64];
                snprintf(buf, sizeof(buf), "%s", SYN[i].en);
                for (char* w = strtok(buf, " "); w; w = strtok(NULL, " ")) kw_add(out, max, &o, &count, w);
            }
        if (!syn && in_list(STOP, tok)) continue;
        if (!syn || !in_list(STOP, tok)) kw_add(out, max, &o, &count, tok);
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// search

void repo_seen_add(RepoSeenSet* s, const char* path, int a, int b) {
    if (!s) return;
    if (s->n >= REPO_SEEN_MAX) {   // the oldest goes
        memmove(&s->seen[0], &s->seen[1], sizeof(RepoSeen) * (REPO_SEEN_MAX - 1));
        s->n = REPO_SEEN_MAX - 1;
    }
    snprintf(s->seen[s->n].path, sizeof(s->seen[0].path), "%s", path);
    s->seen[s->n].a = a;
    s->seen[s->n].b = b;
    s->n++;
}

static int seen_covered(const RepoSeenSet* s, const char* path, int a, int b) {   // lines of a..b already shown
    if (!s) return 0;
    int covered = 0;
    for (int l = a; l <= b; l++) {
        for (int i = 0; i < s->n; i++)
            if (!strcmp(s->seen[i].path, path) && l >= s->seen[i].a && l <= s->seen[i].b) {
                covered++;
                break;
            }
    }
    return b >= a ? covered * 100 / (b - a + 1) : 100;
}

#define MAXT 12
static unsigned short g_cnt[MAXF][MAXT];
static unsigned char g_pathhit[MAXF][MAXT];
static int g_score[MAXF];
static char lbuf[READ_MAX + 8];
static unsigned short lw[MAXL + 2];
static unsigned short lmask[MAXL + 2];
static unsigned char lcov[MAXL + 2];

static int count_ci(const char* hay_lower, int n, const char* needle, int nl) {
    int c = 0;
    const char* p = hay_lower;
    const char* end = hay_lower + n;
    while (p + nl <= end) {
        const char* f = memchr(p, needle[0], end - nl + 1 - p);
        if (!f) break;
        if (!memcmp(f, needle, nl)) c++;
        p = f + 1;
    }
    return c;
}

// Moves a (a line) up to the line of the function it is inside, when that is close.
static int extend_to_header(int lang, const char* text, int a) {
    if (a <= 1) return a;
    int lim = a - 30 < 1 ? 1 : a - 30;
    if (lang == L_PY) {
        int ind = line_indent(text, a);
        for (int l = a; l >= lim; l--) {
            if (line_blank(text, l)) continue;
            const char* p = text + lstart[l];
            int i = 0, li = line_indent(text, l);
            while (p[i] == ' ' || p[i] == '\t') i++;
            if (li < ind && (!strncmp(p + i, "def ", 4) || !strncmp(p + i, "class ", 6) || !strncmp(p + i, "async def ", 10))) return l;
        }
        return a;
    }
    if (lang != L_C && lang != L_JS && lang != L_GO && lang != L_RS && lang != L_ZIG && lang != L_JAVA) return a;
    for (int l = a; l >= lim; l--) {
        const char* p = text + lstart[l];
        int ll = line_len(text, l);
        if (ll == 0 || !ident_char((unsigned char)p[0])) continue;
        if (!memchr(p, '(', ll)) continue;
        if (p[ll - 1] == ';') continue;
        int up = 0;
        while (l > 1 && up < 3) {   // its comment
            const char* q = text + lstart[l - 1];
            int ql = line_len(text, l - 1), i = 0;
            while (i < ql && (q[i] == ' ' || q[i] == '\t')) i++;
            if (i < ql && (!strncmp(q + i, "//", 2) || q[i] == '*' || !strncmp(q + i, "/*", 2))) l--, up++;
            else break;
        }
        return l;
    }
    return a;
}

typedef struct {
    int a, b;
} Win;

static int cmp_win(const void* x, const void* y) { return ((const Win*)x)->a - ((const Win*)y)->a; }

int repo_search(const char* zip_path, const char* query, char* out, int max, int max_files, RepoSeenSet* seen, char paths[][100]) {
    out[0] = 0;
    char terms[MAXT][44];
    int nt = 0;
    for (const char* p = query; *p && nt < MAXT;) {
        while (*p == ' ' || *p == '+' || *p == ',' || *p == '\n') p++;
        const char* s = p;
        while (*p && *p != ' ' && *p != '+' && *p != ',' && *p != '\n') p++;
        int l = (int)(p - s);
        if (l < 2) continue;
        if (l > 40) l = 40;
        for (int i = 0; i < l; i++) terms[nt][i] = (char)lc((unsigned char)s[i]);
        terms[nt][l] = 0;
        int dup = 0;
        for (int i = 0; i < nt; i++) dup |= !strcmp(terms[i], terms[nt]);
        if (!dup) nt++;
    }
    const char* zname = strrchr(zip_path, '/');
    zname = zname ? zname + 1 : zip_path;
    if (nt == 0) {
        snprintf(out, max, "Search in %s: no words to look for.\n", zname);
        return 0;
    }
    int n = load_index(zip_path);
    if (n < 0) return n;
    if (max_files > 8) max_files = 8;
    int N = 0, matched = 0;
    long budget = 10L * 1024 * 1024;
    int df[MAXT];
    memset(df, 0, sizeof(df));
    for (int p = 0; p < n; p++) {
        g_score[p] = 0;
        memset(g_cnt[p], 0, sizeof(g_cnt[p]));
        memset(g_pathhit[p], 0, sizeof(g_pathhit[p]));
        if (g_kind[p] != K_SRC || g_ents[g_idx[p]].usize > READ_MAX || budget <= 0) continue;
        int cut = 0;
        int r = read_entry(zip_path, p, &cut);
        if (r < 0) continue;
        budget -= r;
        N++;
        for (int i = 0; i < r; i++) lbuf[i] = (char)lc((unsigned char)rbuf[i]);
        lbuf[r] = 0;
        char rel[130];
        const char* nm = g_ents[g_idx[p]].name + g_root;
        int rl = (int)strlen(nm);
        if (rl > 128) rl = 128;
        for (int i = 0; i < rl; i++) rel[i] = (char)lc((unsigned char)nm[i]);
        rel[rl] = 0;
        int any = 0;
        for (int t = 0; t < nt; t++) {
            int c = count_ci(lbuf, r, terms[t], (int)strlen(terms[t]));
            g_cnt[p][t] = (unsigned short)(c > 60000 ? 60000 : c);
            g_pathhit[p][t] = strstr(rel, terms[t]) != NULL;
            if (c || g_pathhit[p][t]) df[t]++, any = 1;
        }
        if (any) matched++;
    }
    int top[8], ntop = 0;
    for (int p = 0; p < n; p++) {
        if (g_kind[p] != K_SRC) continue;
        int score = 0, distinct = 0;
        for (int t = 0; t < nt; t++) {
            if (!g_cnt[p][t] && !g_pathhit[p][t]) continue;
            int w = strpbrk(terms[t], "_./0123456789") ? 3 : 2;
            int idf = ilog2i(((N + 1) * 8) / (df[t] + 1)) + 1;
            int cf = g_cnt[p][t] ? 1 + ilog2i(g_cnt[p][t] + 1) : 0;
            if (cf > 4) cf = 4;
            score += w * idf * cf + (g_pathhit[p][t] ? w * idf * 3 : 0);
            distinct++;
        }
        if (!distinct) continue;
        score += distinct * 5;
        score = score * 100 / (100 + (int)(g_ents[g_idx[p]].usize / 400));
        if (score < 1) score = 1;
        g_score[p] = score;
        int pos = ntop;   // insert into the sorted top list
        while (pos > 0 && g_score[top[pos - 1]] < score) pos--;
        if (pos < max_files) {
            if (ntop < max_files) ntop++;
            for (int i = ntop - 1; i > pos; i--) top[i] = top[i - 1];
            top[pos] = p;
        }
    }
    int o = snprintf(out, max, "Search '");
    for (int t = 0; t < nt && o < max - 60; t++) o += snprintf(out + o, max - o, "%s%s", t ? " " : "", terms[t]);
    if (ntop == 0) {
        snprintf(out + o, max - o, "' in %s: no file matched (%d scanned). Try other words, or @zip for the repo map.\n", zname, N);
        return 0;
    }
    o += snprintf(out + o, max - o, "' in %s: %d of %d files matched, the %d best follow.\n", zname, matched, N, ntop);
    int written = 0;
    for (int ti = 0; ti < ntop && o < max - 300; ti++) {
        int p = top[ti];
        int cut = 0;
        int r = read_entry(zip_path, p, &cut);
        if (r < 0) continue;
        const char* nm = g_ents[g_idx[p]].name + g_root;
        int lang = lang_of(nm);
        int nl = index_lines(rbuf, r);
        for (int i = 0; i < r; i++) lbuf[i] = (char)lc((unsigned char)rbuf[i]);
        memset(lw, 0, sizeof(unsigned short) * (nl + 2));
        memset(lmask, 0, sizeof(unsigned short) * (nl + 2));
        memset(lcov, 0, nl + 2);
        for (int t = 0; t < nt; t++) {
            int tl = (int)strlen(terms[t]);
            int w = strpbrk(terms[t], "_./0123456789") ? 3 : 2;
            const char* q = lbuf;
            const char* end = lbuf + r;
            while (q + tl <= end) {
                const char* f = memchr(q, terms[t][0], end - tl + 1 - q);
                if (!f) break;
                if (!memcmp(f, terms[t], tl)) {
                    int off = (int)(f - lbuf), lo = 1, hi = nl;
                    while (lo < hi) {
                        int mid = (lo + hi + 1) / 2;
                        if (lstart[mid] <= off) lo = mid;
                        else hi = mid - 1;
                    }
                    if (lw[lo] < 60000) lw[lo] = (unsigned short)(lw[lo] + w);
                    lmask[lo] |= (unsigned short)(1u << t);
                    q = lbuf + lstart[lo + 1];   // one hit per term per line
                    if (q <= f) q = f + 1;
                } else q = f + 1;
            }
        }
        int per_file = (max - o - 100) / (ntop - ti);
        if (per_file < 400) per_file = 400;
        Win wins[10];
        int nw = 0, bytes = 0;
        if (r <= per_file && nl <= 150 && !cut) {   // a small file: all of it
            wins[nw].a = 1, wins[nw].b = nl, nw++;
            bytes = r;
        } else {
            for (int pick = 0; pick < 6 && nw < 8 && bytes < per_file; pick++) {
                int best = 0, bl = 0;
                for (int l = 1; l <= nl; l++) {
                    if (lcov[l] || !lw[l]) continue;
                    int m = lmask[l], pc = 0;
                    while (m) pc += m & 1, m >>= 1;
                    int sc = lw[l] + 4 * pc;
                    if (sc > best) best = sc, bl = l;
                }
                if (!best) break;
                int a = bl - 2 < 1 ? 1 : bl - 2, b = bl + 6 > nl ? nl : bl + 6;
                a = extend_to_header(lang, rbuf, a);
                for (int l = a; l <= b; l++) lcov[l] = 1;
                wins[nw].a = a, wins[nw].b = b;
                bytes += lstart[b + 1] - lstart[a];
                nw++;
            }
            qsort(wins, nw, sizeof(Win), cmp_win);
            int m = 0;   // merge windows that touch
            for (int i = 0; i < nw; i++) {
                if (m > 0 && wins[i].a <= wins[m - 1].b + 2) {
                    if (wins[i].b > wins[m - 1].b) wins[m - 1].b = wins[i].b;
                } else wins[m++] = wins[i];
            }
            nw = m;
        }
        // drop what the model has already seen; keep the rest in order
        Win keep[10];
        int nk = 0;
        for (int i = 0; i < nw; i++)
            if (seen_covered(seen, nm, wins[i].a, wins[i].b) < 70) keep[nk++] = wins[i];
        if (nk == 0) continue;
        char sz[16];
        fmt_size(sz, sizeof(sz), g_ents[g_idx[p]].usize);
        char head[300];
        int hl = snprintf(head, sizeof(head), "--- %s (%s, %d lines) lines ", nm, sz, nl);
        for (int i = 0; i < nk; i++) hl += snprintf(head + hl, sizeof(head) - hl, "%s%d-%d", i ? ", " : "", keep[i].a, keep[i].b);
        hl += snprintf(head + hl, sizeof(head) - hl, " ---\n");
        if (o + hl + 120 >= max) break;
        memcpy(out + o, head, hl);
        int o2 = o + hl;
        int fits = 1;
        for (int i = 0; i < nk && fits; i++) {
            int ll = lstart[keep[i].b + 1] - lstart[keep[i].a];
            if (o2 + ll + 8 >= max) {   // cut the last window to what fits
                ll = max - o2 - 8;
                while (ll > 0 && rbuf[lstart[keep[i].a] + ll - 1] != '\n') ll--;
                fits = 0;
                if (ll <= 0) break;
            }
            if (i) o2 += snprintf(out + o2, max - o2, "...\n");
            for (int j = 0; j < ll; j++) {
                char c = rbuf[lstart[keep[i].a] + j];
                if (c != '\r') out[o2++] = c;
            }
            if (o2 > 0 && out[o2 - 1] != '\n') out[o2++] = '\n';
            repo_seen_add(seen, nm, keep[i].a, keep[i].b);
        }
        out[o2] = 0;
        o = o2;
        if (paths && written < max_files) snprintf(paths[written], 100, "%s", nm);
        written++;
    }
    return written;
}
