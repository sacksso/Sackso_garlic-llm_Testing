// The other protocols the console's API speaks besides OpenAI chat: OpenAI text completions, KoboldCpp
// (/api/v1/generate and its stream) and Ollama (/api/chat, /api/generate, /api/tags...), so that games and apps
// written for those servers (and any OpenAI-compatible client) can use the console as their local AI.
// Pure C, no console calls: tests/api_compat_test.c runs it on a PC. api.c routes the requests, api_run.inc runs
// them on the model and writes the answers with these functions.
#ifndef CHAT_API_COMPAT_H
#define CHAT_API_COMPAT_H

#include "api_proto.h"

enum {
    API_K_OPENAI_CHAT,     // POST /v1/chat/completions
    API_K_OPENAI_TEXT,     // POST /v1/completions
    API_K_KOBOLD,          // POST /api/v1/generate
    API_K_KOBOLD_STREAM,   // POST /api/extra/generate/stream
    API_K_OLLAMA_CHAT,     // POST /api/chat
    API_K_OLLAMA_GEN,      // POST /api/generate
};
#define GEN_STOPS 6
#define GEN_STOP_LEN 64

// One request, whatever its protocol. The JSON nodes live in the pool the caller gave to api_gen_parse; the strings
// (prompt, system) belong to the request: api_gen_free.
typedef struct {
    int kind;
    int root;
    int messages;               // chat kinds: the messages array node (-1 otherwise)
    int raw;                    // 1: prompt is the text the model continues, as it is (no chat template)
    Sb prompt;                  // raw: the text to continue; Ollama generate without raw: the user's message
    Sb system;                  // Ollama generate without raw: the system message ("" if none)
    double temperature;         // default 0.7
    int max_tokens;             // default 512; 1..16384
    int stream;
    unsigned seed;
    int has_seed;
    char stops[GEN_STOPS][GEN_STOP_LEN];   // stop sequences: the reply ends before the first one that appears
    int n_stops;
} GenRequest;

// Returns 0 with g filled, or -1 with the reason in err (a fixed string). Non-OpenAI kinds are forgiving: numbers
// out of range are brought into range instead of refused (games send what they have).
int api_gen_parse(int kind, const char* body, size_t len, JNode* pool, int max, GenRequest* g, const char** err);
void api_gen_free(GenRequest* g);

// 1 when the request itself carries a number under one of these names (NULL-ended list), at the top level or inside
// Ollama's "options". A profile (profiles.h) fills in the temperature and the length only when the game gave none.
int api_gen_given(const JNode* pool, const GenRequest* g, const char* const* keys);

// Stop sequences. api_find_stop: where the first stop sequence begins in text[0..n) (-1 if none), looking only at
// matches that reach past `from` (the bytes before it were already checked). api_stop_hold: how many bytes of the
// newest text a stream must keep back (a stop sequence may be half written).
long api_find_stop(const char* text, size_t n, size_t from, const GenRequest* g);
size_t api_stop_hold(const GenRequest* g);

// ---- answers ----------------------------------------------------------------------------------------------------
// Streamed: api_stream_head is the whole HTTP head; then api_out_start (may add nothing), api_out_piece for each
// piece of text, api_out_end. Every function appends the bytes to out (complete events / lines).
const char* api_stream_head(int kind);
void api_out_start(int kind, Sb* out, const char* id, const char* model, long long created);
void api_out_piece(int kind, Sb* out, const char* id, const char* model, long long created, const char* text, size_t n);
void api_out_end(int kind, Sb* out, const char* id, const char* model, long long created, const char* finish,
                 int prompt_tokens, int completion_tokens);
void api_out_error(int kind, Sb* out, const char* message);   // an error inside a stream whose head is already out
// Not streamed: the body of the 200 answer (JSON).
void api_out_whole(int kind, Sb* out, const char* id, const char* model, long long created, const char* text, size_t n,
                   const char* finish, int prompt_tokens, int completion_tokens);
// The body of an error answer, in the shape the protocol's clients read ({"error":"..."} for Ollama).
void api_out_http_error(int kind, Sb* out, const char* message, const char* type, const char* code);

// ---- the small information endpoints ------------------------------------------------------------------------------
// Fills out with the JSON for the path and returns 1 when it is one of: /api/tags /api/version /api/show
// /api/ps /api/v1/model /api/v1/config/max_context_length /api/v1/config/max_length /api/extra/true_max_context_length
// /api/extra/version /api/v1/info/version /api/extra/abort (any method). Returns 0 for other paths.
int api_info(const char* path, const char* model, int ctx, Sb* out);
// Which generation endpoint the path is (API_K_*), or -1.
int api_kind_of(const char* method, const char* path);

// "2026-11-18T21:44:05Z" for a Unix time.
void api_iso8601(long long t, char out[24]);

#endif
