#include "api_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- reading -------------------------------------------------------------------------------------------------------

static int get_num(const JNode* pool, int obj, const char* key, double* v) {
    int n = obj >= 0 ? json_get(pool, obj, key) : -1;
    if (n < 0 || pool[n].type != J_NUM) return 0;
    *v = pool[n].num;
    return 1;
}

static int get_bool(const JNode* pool, int obj, const char* key, int* v) {
    int n = obj >= 0 ? json_get(pool, obj, key) : -1;
    if (n < 0 || pool[n].type != J_BOOL) return 0;
    *v = pool[n].b;
    return 1;
}

static void add_stop(GenRequest* g, const Sb* s) {
    if (!s->p || !s->n || g->n_stops >= GEN_STOPS) return;
    size_t n = s->n < GEN_STOP_LEN - 1 ? s->n : GEN_STOP_LEN - 1;
    memcpy(g->stops[g->n_stops], s->p, n);
    g->stops[g->n_stops][n] = 0;
    g->n_stops++;
}

// A stop given as a string or as an array of strings.
static void read_stops(const JNode* pool, int node, GenRequest* g) {
    if (node < 0) return;
    if (pool[node].type == J_STR) {
        Sb t;
        sb_init(&t);
        json_str_sb(pool, node, &t);
        add_stop(g, &t);
        sb_free(&t);
    } else if (pool[node].type == J_ARR) {
        for (int c = pool[node].first; c >= 0; c = pool[c].next) {
            if (pool[c].type != J_STR) continue;
            Sb t;
            sb_init(&t);
            json_str_sb(pool, c, &t);
            add_stop(g, &t);
            sb_free(&t);
        }
    }
}

static void set_temperature(GenRequest* g, double t) { g->temperature = t < 0 ? 0 : t > 2 ? 2 : t; }
static void set_max_tokens(GenRequest* g, double m) {
    if (m < 0) m = 1024;   // Ollama's -1 / -2: no limit asked, the context is the limit
    g->max_tokens = m < 1 ? 1 : m > 16384 ? 16384 : (int)m;
}

static int is_obj(const JNode* pool, int n) { return n >= 0 && pool[n].type == J_OBJ; }

int api_gen_parse(int kind, const char* body, size_t len, JNode* pool, int max, GenRequest* g, const char** err) {
    memset(g, 0, sizeof(*g));
    sb_init(&g->prompt);
    sb_init(&g->system);
    g->kind = kind;
    g->temperature = 0.7;
    g->max_tokens = 512;
    g->messages = -1;
    double d;
    int b;
    if (kind == API_K_OPENAI_CHAT || kind == API_K_OLLAMA_CHAT) {
        ChatRequest cr;
        if (api_chat_parse(body, len, pool, max, &cr, err)) return -1;
        g->root = cr.root;
        g->messages = cr.messages;
        g->temperature = cr.temperature;
        g->max_tokens = cr.max_tokens;
        g->stream = cr.stream;
        g->seed = cr.seed;
        g->has_seed = cr.has_seed;
        if (kind == API_K_OPENAI_CHAT) {
            read_stops(pool, json_get(pool, g->root, "stop"), g);
            return 0;
        }
        // Ollama: streaming is the default; the numbers are in "options"
        g->stream = 1;
        if (get_bool(pool, g->root, "stream", &b)) g->stream = b;
        int o = json_get(pool, g->root, "options");
        if (is_obj(pool, o)) {
            if (get_num(pool, o, "temperature", &d)) set_temperature(g, d);
            if (get_num(pool, o, "num_predict", &d)) set_max_tokens(g, d);
            if (get_num(pool, o, "seed", &d) && d >= 0) g->seed = (unsigned)d, g->has_seed = 1;
            read_stops(pool, json_get(pool, o, "stop"), g);
        }
        return 0;
    }
    int root = json_parse(body, len, pool, max);
    if (root < 0 || pool[root].type != J_OBJ) {
        *err = "The body must be a JSON object.";
        return -1;
    }
    g->root = root;
    if (kind == API_K_OPENAI_TEXT) {
        int p = json_get(pool, root, "prompt");
        if (p >= 0 && pool[p].type == J_ARR) p = pool[p].first;   // (a list of prompts: the first)
        if (p < 0 || pool[p].type != J_STR) {
            *err = "'prompt' must be a string.";
            return -1;
        }
        json_str_sb(pool, p, &g->prompt);
        g->raw = 1;
        if (get_num(pool, root, "temperature", &d)) set_temperature(g, d);
        if (get_num(pool, root, "max_tokens", &d)) set_max_tokens(g, d);
        if (get_bool(pool, root, "stream", &b)) g->stream = b;
        if (get_num(pool, root, "seed", &d) && d >= 0) g->seed = (unsigned)d, g->has_seed = 1;
        read_stops(pool, json_get(pool, root, "stop"), g);
    } else if (kind == API_K_KOBOLD || kind == API_K_KOBOLD_STREAM) {
        int p = json_get(pool, root, "prompt");
        if (p < 0 || pool[p].type != J_STR) {
            *err = "'prompt' must be a string.";
            return -1;
        }
        json_str_sb(pool, p, &g->prompt);
        g->raw = 1;
        g->stream = kind == API_K_KOBOLD_STREAM;
        g->max_tokens = 80;   // KoboldCpp's default length
        if (get_num(pool, root, "temperature", &d)) set_temperature(g, d);
        if (get_num(pool, root, "max_length", &d)) set_max_tokens(g, d);
        if (get_num(pool, root, "sampler_seed", &d) && d >= 0) g->seed = (unsigned)d, g->has_seed = 1;
        read_stops(pool, json_get(pool, root, "stop_sequence"), g);
    } else if (kind == API_K_OLLAMA_GEN) {
        int p = json_get(pool, root, "prompt");
        if (p >= 0 && pool[p].type == J_STR) json_str_sb(pool, p, &g->prompt);
        else if (p >= 0 && pool[p].type != J_NULL) {
            *err = "'prompt' must be a string.";
            return -1;
        }
        int s = json_get(pool, root, "system");
        if (s >= 0 && pool[s].type == J_STR) json_str_sb(pool, s, &g->system);
        if (get_bool(pool, root, "raw", &b)) g->raw = b;
        g->stream = 1;
        if (get_bool(pool, root, "stream", &b)) g->stream = b;
        int o = json_get(pool, root, "options");
        if (is_obj(pool, o)) {
            if (get_num(pool, o, "temperature", &d)) set_temperature(g, d);
            if (get_num(pool, o, "num_predict", &d)) set_max_tokens(g, d);
            if (get_num(pool, o, "seed", &d) && d >= 0) g->seed = (unsigned)d, g->has_seed = 1;
            read_stops(pool, json_get(pool, o, "stop"), g);
        }
        if (!g->prompt.n && !g->system.n) {   // (Ollama answers this one with an empty, done reply: nothing to run)
            *err = "'prompt' is empty.";
            return -1;
        }
    } else {
        *err = "Unknown endpoint.";
        return -1;
    }
    return 0;
}

void api_gen_free(GenRequest* g) {
    sb_free(&g->prompt);
    sb_free(&g->system);
}

int api_gen_given(const JNode* pool, const GenRequest* g, const char* const* keys) {
    if (g->root < 0) return 0;
    int o = json_get(pool, g->root, "options");
    for (int i = 0; keys[i]; i++) {
        int n = json_get(pool, g->root, keys[i]);
        if (n >= 0 && pool[n].type == J_NUM) return 1;
        if (is_obj(pool, o)) {
            n = json_get(pool, o, keys[i]);
            if (n >= 0 && pool[n].type == J_NUM) return 1;
        }
    }
    return 0;
}

// ---- stop sequences ------------------------------------------------------------------------------------------------

long api_find_stop(const char* text, size_t n, size_t from, const GenRequest* g) {
    long best = -1;
    for (int k = 0; k < g->n_stops; k++) {
        size_t L = strlen(g->stops[k]);
        if (!L || n < L) continue;
        size_t i = from >= L - 1 ? from - (L - 1) : 0;
        for (; i + L <= n; i++)
            if (text[i] == g->stops[k][0] && !memcmp(text + i, g->stops[k], L)) {
                if (best < 0 || (long)i < best) best = (long)i;
                break;
            }
    }
    return best;
}

size_t api_stop_hold(const GenRequest* g) {
    size_t h = 0;
    for (int k = 0; k < g->n_stops; k++) {
        size_t L = strlen(g->stops[k]);
        if (L > 1 && L - 1 > h) h = L - 1;
    }
    return h;
}

// ---- time ----------------------------------------------------------------------------------------------------------

void api_iso8601(long long t, char out[24]) {
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) rem += 86400, days--;
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, 24, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)(y % 10000), (int)m, (int)d, (int)(rem / 3600), (int)(rem % 3600 / 60),
             (int)(rem % 60));
}

// ---- answers -------------------------------------------------------------------------------------------------------

static int is_ollama(int kind) { return kind == API_K_OLLAMA_CHAT || kind == API_K_OLLAMA_GEN; }

const char* api_stream_head(int kind) {
    if (is_ollama(kind))
        return "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nCache-Control: no-cache\r\n"
               "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
}

static void sse(Sb* out, const char* json) {
    sb_str(out, "data: ");
    sb_str(out, json);
    sb_str(out, "\n\n");
}

// {"id":..,"object":"text_completion",..,"choices":[{"index":0,"text":..,"finish_reason":..}]}
static void text_chunk(Sb* o, const char* id, const char* model, long long created, const char* text, size_t n,
                       const char* finish) {
    sb_str(o, "{\"id\":");
    sb_json_str(o, id, strlen(id));
    sb_str(o, ",\"object\":\"text_completion\",\"created\":");
    sb_int(o, created);
    sb_str(o, ",\"model\":");
    sb_json_str(o, model, strlen(model));
    sb_str(o, ",\"choices\":[{\"index\":0,\"text\":");
    sb_json_str(o, text, n);
    sb_str(o, ",\"logprobs\":null,\"finish_reason\":");
    if (finish) sb_json_str(o, finish, strlen(finish));
    else sb_str(o, "null");
    sb_str(o, "}]}");
}

// One Ollama object: chat has "message", generate has "response"; done adds the counters.
static void ollama_obj(int kind, Sb* o, const char* model, long long created, const char* text, size_t n, int done,
                       const char* finish, int pt, int ct) {
    char iso[24];
    api_iso8601(created, iso);
    sb_str(o, "{\"model\":");
    sb_json_str(o, model, strlen(model));
    sb_str(o, ",\"created_at\":\"");
    sb_str(o, iso);
    sb_str(o, "\",");
    if (kind == API_K_OLLAMA_CHAT) {
        sb_str(o, "\"message\":{\"role\":\"assistant\",\"content\":");
        sb_json_str(o, text, n);
        sb_str(o, "}");
    } else {
        sb_str(o, "\"response\":");
        sb_json_str(o, text, n);
    }
    sb_str(o, done ? ",\"done\":true" : ",\"done\":false");
    if (done) {
        sb_str(o, ",\"done_reason\":");
        sb_json_str(o, finish, strlen(finish));
        sb_str(o, ",\"total_duration\":0,\"load_duration\":0,\"prompt_eval_count\":");
        sb_int(o, pt);
        sb_str(o, ",\"eval_count\":");
        sb_int(o, ct);
    }
    sb_str(o, "}");
}

void api_out_start(int kind, Sb* out, const char* id, const char* model, long long created) {
    if (kind != API_K_OPENAI_CHAT) return;
    Sb j;
    sb_init(&j);
    api_json_chunk(&j, id, model, created, "assistant", "", 0, NULL);
    if (j.p) sse(out, j.p);
    sb_free(&j);
}

void api_out_piece(int kind, Sb* out, const char* id, const char* model, long long created, const char* text, size_t n) {
    Sb j;
    sb_init(&j);
    if (kind == API_K_OPENAI_CHAT) {
        api_json_chunk(&j, id, model, created, NULL, text, n, NULL);
        if (j.p) sse(out, j.p);
    } else if (kind == API_K_OPENAI_TEXT) {
        text_chunk(&j, id, model, created, text, n, NULL);
        if (j.p) sse(out, j.p);
    } else if (kind == API_K_KOBOLD_STREAM || kind == API_K_KOBOLD) {
        sb_str(&j, "{\"token\":");
        sb_json_str(&j, text, n);
        sb_str(&j, ",\"finish_reason\":null}");
        if (j.p) {
            sb_str(out, "event: message\ndata: ");
            sb_str(out, j.p);
            sb_str(out, "\n\n");
        }
    } else {
        ollama_obj(kind, &j, model, created, text, n, 0, NULL, 0, 0);
        if (j.p) {
            sb_str(out, j.p);
            sb_str(out, "\n");
        }
    }
    sb_free(&j);
}

void api_out_end(int kind, Sb* out, const char* id, const char* model, long long created, const char* finish,
                 int pt, int ct) {
    Sb j;
    sb_init(&j);
    if (kind == API_K_OPENAI_CHAT) {
        api_json_chunk(&j, id, model, created, NULL, "", 0, finish);
        if (j.p) sse(out, j.p);
        sb_str(out, "data: [DONE]\n\n");
    } else if (kind == API_K_OPENAI_TEXT) {
        text_chunk(&j, id, model, created, "", 0, finish);
        if (j.p) sse(out, j.p);
        sb_str(out, "data: [DONE]\n\n");
    } else if (kind == API_K_KOBOLD_STREAM || kind == API_K_KOBOLD) {
        sb_str(&j, "{\"token\":\"\",\"finish_reason\":");
        sb_json_str(&j, finish, strlen(finish));
        sb_str(&j, "}");
        if (j.p) {
            sb_str(out, "event: message\ndata: ");
            sb_str(out, j.p);
            sb_str(out, "\n\n");
        }
    } else {
        ollama_obj(kind, &j, model, created, "", 0, 1, finish, pt, ct);
        if (j.p) {
            sb_str(out, j.p);
            sb_str(out, "\n");
        }
    }
    sb_free(&j);
}

void api_out_error(int kind, Sb* out, const char* message) {
    Sb j;
    sb_init(&j);
    if (is_ollama(kind)) {
        sb_str(&j, "{\"error\":");
        sb_json_str(&j, message, strlen(message));
        sb_str(&j, "}");
        if (j.p) sb_str(out, j.p), sb_str(out, "\n");
    } else {
        api_json_error(&j, message, "server_error", NULL);
        if (j.p) sse(out, j.p);
    }
    sb_free(&j);
}

void api_out_whole(int kind, Sb* out, const char* id, const char* model, long long created, const char* text, size_t n,
                   const char* finish, int pt, int ct) {
    if (kind == API_K_OPENAI_CHAT) {
        api_json_completion(out, id, model, created, text, n, finish, pt, ct);
    } else if (kind == API_K_OPENAI_TEXT) {
        sb_str(out, "{\"id\":");
        sb_json_str(out, id, strlen(id));
        sb_str(out, ",\"object\":\"text_completion\",\"created\":");
        sb_int(out, created);
        sb_str(out, ",\"model\":");
        sb_json_str(out, model, strlen(model));
        sb_str(out, ",\"choices\":[{\"index\":0,\"text\":");
        sb_json_str(out, text, n);
        sb_str(out, ",\"logprobs\":null,\"finish_reason\":");
        sb_json_str(out, finish, strlen(finish));
        sb_str(out, "}],\"usage\":{\"prompt_tokens\":");
        sb_int(out, pt);
        sb_str(out, ",\"completion_tokens\":");
        sb_int(out, ct);
        sb_str(out, ",\"total_tokens\":");
        sb_int(out, (long long)pt + ct);
        sb_str(out, "}}");
    } else if (kind == API_K_KOBOLD || kind == API_K_KOBOLD_STREAM) {
        sb_str(out, "{\"results\":[{\"text\":");
        sb_json_str(out, text, n);
        sb_str(out, "}]}");
    } else {
        ollama_obj(kind, out, model, created, text, n, 1, finish, pt, ct);
    }
}

void api_out_http_error(int kind, Sb* out, const char* message, const char* type, const char* code) {
    if (is_ollama(kind)) {
        sb_str(out, "{\"error\":");
        sb_json_str(out, message, strlen(message));
        sb_str(out, "}");
    } else if (kind == API_K_KOBOLD || kind == API_K_KOBOLD_STREAM) {
        sb_str(out, "{\"detail\":{\"msg\":");
        sb_json_str(out, message, strlen(message));
        sb_str(out, ",\"type\":\"service_unavailable\"}}");
    } else {
        api_json_error(out, message, type, code);
    }
}

// ---- information endpoints -----------------------------------------------------------------------------------------

static void model_details(Sb* o) {
    sb_str(o, "\"details\":{\"parent_model\":\"\",\"format\":\"gguf\",\"family\":\"qwen\",\"families\":[\"qwen\"],"
              "\"parameter_size\":\"9B\",\"quantization_level\":\"Q4\"}");
}

int api_info(const char* path, const char* model, int ctx, Sb* out) {
    char iso[24];
    api_iso8601(0, iso);
    if (!strcmp(path, "/api/tags") || !strcmp(path, "/api/ps")) {
        sb_str(out, "{\"models\":[{\"name\":");
        sb_json_str(out, model, strlen(model));
        sb_str(out, ",\"model\":");
        sb_json_str(out, model, strlen(model));
        sb_str(out, ",\"modified_at\":\"");
        sb_str(out, iso);
        sb_str(out, "\",\"size\":0,\"digest\":\"garlic\",");
        model_details(out);
        sb_str(out, "}]}");
    } else if (!strcmp(path, "/api/version")) {
        sb_str(out, "{\"version\":\"0.6.0\"}");
    } else if (!strcmp(path, "/api/show")) {
        sb_str(out, "{\"modelfile\":\"\",\"parameters\":\"\",\"template\":\"{{ .Prompt }}\",");
        model_details(out);
        sb_str(out, ",\"model_info\":{\"general.architecture\":\"qwen35\",\"qwen35.context_length\":");
        sb_int(out, ctx);
        sb_str(out, "},\"capabilities\":[\"completion\"]}");
    } else if (!strcmp(path, "/api/v1/model")) {
        sb_str(out, "{\"result\":");
        char name[160];
        snprintf(name, sizeof(name), "koboldcpp/%s", model);
        sb_json_str(out, name, strlen(name));
        sb_str(out, "}");
    } else if (!strcmp(path, "/api/v1/config/max_context_length") || !strcmp(path, "/api/extra/true_max_context_length")) {
        sb_str(out, "{\"value\":");
        sb_int(out, ctx);
        sb_str(out, "}");
    } else if (!strcmp(path, "/api/v1/config/max_length")) {
        sb_str(out, "{\"value\":512}");
    } else if (!strcmp(path, "/api/extra/version")) {
        sb_str(out, "{\"result\":\"KoboldCpp\",\"version\":\"1.80\"}");
    } else if (!strcmp(path, "/api/v1/info/version")) {
        sb_str(out, "{\"result\":\"1.2.5\"}");
    } else if (!strcmp(path, "/api/extra/abort")) {
        sb_str(out, "{\"success\":\"true\"}");
    } else {
        return 0;
    }
    return 1;
}

int api_kind_of(const char* method, const char* path) {
    if (strcmp(method, "POST")) return -1;
    if (!strcmp(path, "/v1/chat/completions") || !strcmp(path, "/chat/completions")) return API_K_OPENAI_CHAT;
    if (!strcmp(path, "/v1/completions") || !strcmp(path, "/completions")) return API_K_OPENAI_TEXT;
    if (!strcmp(path, "/api/v1/generate")) return API_K_KOBOLD;
    if (!strcmp(path, "/api/extra/generate/stream")) return API_K_KOBOLD_STREAM;
    if (!strcmp(path, "/api/chat")) return API_K_OLLAMA_CHAT;
    if (!strcmp(path, "/api/generate")) return API_K_OLLAMA_GEN;
    return -1;
}
