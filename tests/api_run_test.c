// Host test for chat/api_run.inc (the API's model side), with a mock engine and tokenizer, so the job logic runs
// without the PS4. The real file is included as main.c includes it. Every test runs twice: one token per step
// (engine_run), and several per step as with the MTP head (engine_spec_step).
// Build and run: gcc -std=gnu99 -Wall -Wextra -g -fsanitize=address,undefined -I chat tests/api_run_test.c chat/api_proto.c -o build/api_run_test && build/api_run_test
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "api_compat.h"
#include "api_proto.h"

// ---- mock model and tokenizer ----------------------------------------------------------------------------------------
#define IM_START 1000u
#define IM_END 1001u
#define THINK 1002u
#define THINK_END 1003u
#define EOS2 1004u

typedef struct {
    int qwen35, ctx_limit;
    uint32_t eos, eos2;
    char error[160];
    struct {
        uint64_t ws_begin, ws_end;   // the dictation work memory (the prefix cache keeps the saved state there)
    } asr;
} MockEngine;
static MockEngine eng = {1, 4096, 1005, EOS2, "", {0x1000, 0x1000 + 4096}};
static int api_cache_on;
typedef struct {
    int dummy;
} Tokenizer;
static Tokenizer tok;
static int pos, need_close, hist_n, hist_done, thinking, think_turn, sys_pre, msgs_cleared;
static unsigned seed = 1;
static uint32_t next_token;
typedef struct {
    int scroll;
} ChatView;
static ChatView view;
static int fail_engine_at = -1;   // the engine fails on this call (-1: never)
#define GARLIC_SPEC 1             // (as the 9B app: api_run.inc asks spec_active before every step)
static int spec_on;               // the mock MTP head is on: a step keeps up to spec_keep tokens of its draft
static int spec_keep = 2;
static int fed;                   // tokens of the reply fed to the model
// what main.c shows of a job: begun and ended (they must pair), and Circle held from stop_at tokens on (-1: never)
static int chat_begun, chat_ended, chat_replies, stop_at = -1;

// Scripted reply: the model emits script[k], then IM_END. Tokens 500/501 are the two halves of "é".
static const uint32_t* script;
static int script_len, script_k, engine_calls, prompt_len_seen;
static char wire[1 << 16];
static size_t wire_n;
static int wire_fail;   // the client has gone: sends fail
static int sends_left = -1;   // sends that still succeed (-1: all)

int api_send(int fd, const char* p, size_t n) {
    (void)fd;
    if (wire_fail) return -1;
    if (sends_left == 0) return -1;
    if (sends_left > 0) sends_left--;
    if (wire_n + n >= sizeof(wire)) return -1;
    memcpy(wire + wire_n, p, n);
    wire_n += n;
    wire[wire_n] = 0;
    return 0;
}
const char* api_model(void) { return "Qwen3.5-9B"; }
static void say(const char* fmt, ...) {
    (void)fmt;
}
static double now(void) { return 0; }
static void api_chat_begin(void) { chat_begun++, msgs_cleared++; }   // (the real one clears the conversation)
static void api_chat_reply(void) { chat_replies++; }
static int api_chat_step(int produced) { return stop_at >= 0 && produced >= stop_at; }
static void api_chat_end(void) { chat_ended++; }
static int tok_encode(const Tokenizer* t, const char* s, uint32_t* out, int max);
// as main.c
static int encode_into(uint32_t* ids, int n, int max, const char* text) { return n + tok_encode(&tok, text, ids + n, max - n); }
static int next_pos, pos_errors;  // where the next token must go; steps given another position
static int reset_calls, read_total, saved_pos, save_calls, restore_calls, fail_restore;
static void engine_reset(MockEngine* e) {
    (void)e;
    next_pos = 0;
    reset_calls++;
}
static uint64_t engine_state_size(const MockEngine* e) {
    (void)e;
    return 1000;
}
static int engine_state_copy(MockEngine* e, uint64_t buf, int save) {
    (void)buf;
    if (save) saved_pos = next_pos, save_calls++, script_k > 0 ? script_k-- : 0;   // (the token drawn after the history is thrown away)
    else {
        if (fail_restore) {
            snprintf(e->error, sizeof(e->error), "mock copy fault");
            return -1;
        }
        next_pos = saved_pos, restore_calls++;
    }
    return 0;
}

// one byte per token (id = byte + 1), so prompts are easy to count
static int tok_encode(const Tokenizer* t, const char* s, uint32_t* out, int max) {
    (void)t;
    int n = 0;
    for (const char* p = s; *p && n < max; p++) out[n++] = (uint8_t)*p + 1;
    return n;
}
static int tok_piece(const Tokenizer* t, uint32_t id, const uint8_t** bytes) {
    (void)t;
    static const uint8_t e_lead[] = {0xC3}, e_tail[] = {0xA9};
    if (id == 500) {
        *bytes = e_lead;
        return 1;
    }
    if (id == 501) {
        *bytes = e_tail;
        return 1;
    }
    if (id >= 1000) {
        *bytes = NULL;
        return 0;
    }
    static uint8_t one[1];
    one[0] = (uint8_t)(id - 1);
    *bytes = one;
    return 1;
}

static int engine_run(MockEngine* e, const uint32_t* tokens, int n, int pos0, float temperature, unsigned sd,
                      uint32_t* next) {
    (void)temperature;
    (void)sd;
    engine_calls++;
    if (engine_calls == fail_engine_at) {
        snprintf(e->error, sizeof(e->error), "mock GPU fault");
        return -1;
    }
    if (n > 1) read_total += n, prompt_len_seen = n;   // the prompt (fed in one go, or its two parts with the cache)
    else fed++;
    if (pos0 != next_pos) pos_errors++;
    next_pos += n;
    if (script_k < script_len) *next = script[script_k++];
    else *next = IM_END;
    (void)tokens;
    return 0;
}

// as engine_spec.inc: tok goes in at pos0, up to max_new - 1 tokens of the draft are kept, and the next one is drawn
static int spec_active(const MockEngine* e) {
    (void)e;
    return spec_on;
}
static int engine_spec_step(MockEngine* e, uint32_t tk, int pos0, float temperature, unsigned sd, int max_new,
                            uint32_t* out, int* n_out, uint32_t* next) {
    (void)tk;
    (void)temperature;
    (void)sd;
    engine_calls++;
    if (engine_calls == fail_engine_at) {
        snprintf(e->error, sizeof(e->error), "mock GPU fault");
        return -1;
    }
    int k = 0;
    while (k < spec_keep && k < max_new - 1 && script_k < script_len) out[k++] = script[script_k++];
    *n_out = k;
    if (pos0 != next_pos) pos_errors++;
    next_pos += 1 + k;
    fed += 1 + k;
    if (script_k < script_len) *next = script[script_k++];
    else *next = IM_END;
    return 0;
}

#include "api_run.inc"

// ---- test helpers --------------------------------------------------------------------------------------------------
static int fails, checks;
#define CHECK(c)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) {                                                           \
            fails++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);               \
        }                                                                     \
    } while (0)

static int job_kind;   // API_K_*, 0 = OpenAI chat

static void run_job(const char* body, const uint32_t* sc, int sc_len) {
    read_total = reset_calls = save_calls = restore_calls = 0;
    memset(wire, 0, sizeof(wire));
    wire_n = 0;
    script = sc;
    script_len = sc_len;
    script_k = 0;
    engine_calls = 0;
    fed = 0;
    ApiJob job;
    memset(&job, 0, sizeof(job));
    job.kind = job_kind;
    job.body = (char*)body;
    job.body_len = strlen(body);
    job.fd = 7;
    api_job(&job);
}

static const char* body_of(const char* w) {
    const char* b = strstr(w, "\r\n\r\n");
    return b ? b + 4 : w;
}

static void test_completion(void) {
    static const uint32_t sc[] = {'H' + 1, 'o' + 1, 'l' + 1, 'a' + 1};
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"max_tokens\":10}", sc, 4);
    CHECK(strncmp(wire, "HTTP/1.1 200 OK\r\nContent-Type: application/json", 46) == 0);
    CHECK(strstr(wire, "\"content\":\"Hola\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"stop\"") != NULL);
    // prompt (one id per byte in the mock): <im_start> "user" "\n" "hi" <im_end> "\n" = 10;
    // <im_start> "assistant\n" = 11; <think> "\n\n" </think> "\n\n" = 6 (Qwen3.5 without thinking). Total 27.
    CHECK(prompt_len_seen == 27);
    CHECK(strstr(wire, "\"prompt_tokens\":27") != NULL);
    CHECK(strstr(wire, "\"completion_tokens\":4") != NULL);
    CHECK(pos == 0 && msgs_cleared >= 1);
}

static void test_length(void) {
    static const uint32_t sc[] = {'H' + 1, 'e' + 1, 'l' + 1, 'l' + 1, 'o' + 1};
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"max_tokens\":3}", sc, 5);
    CHECK(strstr(wire, "\"content\":\"Hel\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"length\"") != NULL);
    CHECK(strstr(wire, "\"completion_tokens\":3") != NULL);
}

static void test_stream(void) {
    static const uint32_t sc[] = {'A' + 1, 500, 501, 'B' + 1};
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"stream\":true}", sc, 4);
    CHECK(strstr(wire, "Content-Type: text/event-stream") != NULL);
    const char* role = strstr(wire, "\"delta\":{\"role\":\"assistant\"}");
    CHECK(role != NULL);
    CHECK(strstr(wire, "\"content\":\"A\"") != NULL);
    // the split é arrives as one event, whole
    CHECK(strstr(wire, "\"content\":\"\xc3\xa9\"") != NULL);
    CHECK(strstr(wire, "\"content\":\"B\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"stop\"") != NULL);
    CHECK(strstr(wire, "data: [DONE]\n\n") != NULL);
    CHECK(strstr(wire, "data: [DONE]\n\n") > strstr(wire, "\"finish_reason\":\"stop\""));
    CHECK(strstr(body_of(wire), "\"content\":\"\xc3\xa9\"") != NULL);
}

static void test_errors(void) {
    int cleared = msgs_cleared, begun = chat_begun;
    run_job("{\"messages\":[]}", NULL, 0);
    CHECK(strncmp(wire, "HTTP/1.1 400 Bad Request", 23) == 0);
    CHECK(strstr(wire, "\"type\":\"invalid_request_error\"") != NULL);

    eng.ctx_limit = 100;
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"max_tokens\":200}", NULL, 0);
    CHECK(strncmp(wire, "HTTP/1.1 400 Bad Request", 23) == 0);
    CHECK(strstr(wire, "context_length_exceeded") != NULL);
    eng.ctx_limit = 4096;
    CHECK(msgs_cleared == cleared && chat_begun == begun);   // a refused request leaves the conversation on screen

    static const uint32_t sc[] = {'A' + 1};
    fail_engine_at = 1;
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}", sc, 1);
    fail_engine_at = -1;
    CHECK(strncmp(wire, "HTTP/1.1 500", 12) == 0);
    CHECK(strstr(wire, "mock GPU fault") != NULL);
    CHECK(pos == 0);
}

static void test_stream_error(void) {
    static const uint32_t sc[] = {'A' + 1, 'B' + 1};
    fail_engine_at = 2;   // the second step (after the first token) fails
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"stream\":true}", sc, 2);
    fail_engine_at = -1;
    CHECK(strstr(wire, "\"message\":\"mock GPU fault\"") != NULL);
    CHECK(strstr(wire, "data: [DONE]") == NULL);   // a failed stream does not pretend to finish
}

static void test_client_gone(void) {
    // the header and the role event go out; the client leaves before the first text event
    static const uint32_t sc[] = {'A' + 1, 'B' + 1, 'C' + 1};
    memset(wire, 0, sizeof(wire));
    wire_n = 0;
    sends_left = 2;
    script = sc;
    script_len = 3;
    script_k = 0;
    engine_calls = 0;
    ApiJob job;
    memset(&job, 0, sizeof(job));
    const char* body = "{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"stream\":true}";
    job.body = (char*)body;
    job.body_len = strlen(body);
    job.fd = 7;
    api_job(&job);
    sends_left = -1;
    CHECK(engine_calls == 1);                       // the prefill only: no more tokens for a client that left
    CHECK(strstr(wire, "data: [DONE]") == NULL);
    CHECK(pos == 0);
}

// Several tokens per step: the reply is the same, in fewer steps, and never past max_tokens.
static void test_spec(void) {
    if (!spec_on) return;
    static const uint32_t sc[] = {'H' + 1, 'o' + 1, 'l' + 1, 'a' + 1, ' ' + 1, 'P' + 1, 'S' + 1, '4' + 1};
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"max_tokens\":64}", sc, 8);
    CHECK(strstr(wire, "\"content\":\"Hola PS4\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"stop\"") != NULL);
    CHECK(strstr(wire, "\"completion_tokens\":8") != NULL);
    CHECK(engine_calls == 1 + 3);   // the prompt, then H + ol, a + " P", S + 4 (8 steps one token at a time)
    CHECK(fed == 8);

    spec_keep = 8;   // the draft would keep more than max_tokens allows
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"max_tokens\":4}", sc, 8);
    spec_keep = 2;
    CHECK(strstr(wire, "\"content\":\"Hola\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"length\"") != NULL);
    CHECK(strstr(wire, "\"completion_tokens\":4") != NULL);
    CHECK(fed == 4);   // the model read what was written, nothing more

    // a draft never holds the end of the turn; if one did, the reply would end there all the same
    static const uint32_t end_in_draft[] = {'A' + 1, 'B' + 1, IM_END, 'C' + 1};
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", end_in_draft, 4);
    CHECK(strstr(wire, "\"content\":\"AB\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"stop\"") != NULL);
    CHECK(strstr(wire, "\"completion_tokens\":2") != NULL);
}

// Circle on the controller: the reply ends there, cut short as by max_tokens, and the client gets what was written.
static void test_stop(void) {
    static const uint32_t sc[] = {'A' + 1, 'B' + 1, 'C' + 1, 'D' + 1, 'E' + 1, 'F' + 1};
    stop_at = 2;
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}]}", sc, 6);
    // one token per step stops at "AB"; with the MTP head the first step had already given "BCD" (B, C kept; D drawn)
    CHECK(strstr(wire, spec_on ? "\"content\":\"ABCD\"" : "\"content\":\"AB\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"length\"") != NULL);
    CHECK(strstr(wire, "HTTP/1.1 200 OK") != NULL);

    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"stream\":true}", sc, 6);
    stop_at = -1;
    CHECK(strstr(wire, "\"content\":\"A\"") != NULL);
    CHECK(strstr(wire, "\"content\":\"F\"") == NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"length\"") != NULL);
    CHECK(strstr(wire, "data: [DONE]\n\n") != NULL);   // the stream closes as usual
    CHECK(pos == 0);
}


// ---- the other protocols --------------------------------------------------------------------------------------------

static void script_of(const char* s, uint32_t* out, int* n) {
    int k = 0;
    for (const char* p = s; *p; p++) out[k++] = (uint8_t)*p + 1;
    *n = k;
}

static void test_protocols(void) {
    uint32_t sc[64];
    int n;
    // OpenAI text completions: the prompt is the model's text as it is
    script_of("Hola", sc, &n);
    job_kind = API_K_OPENAI_TEXT;
    run_job("{\"prompt\":\"Once\",\"max_tokens\":10}", sc, n);
    CHECK(strstr(wire, "\"object\":\"text_completion\"") != NULL && strstr(wire, "\"text\":\"Hola\"") != NULL);
    CHECK(prompt_len_seen == 4);
    // a ChatML prompt that ends where the assistant starts gets the empty thought block (17 = marker + "assistant\n" + 6)
    run_job("{\"prompt\":\"<|im_start|>assistant\\n\",\"max_tokens\":10}", sc, n);
    CHECK(prompt_len_seen == 17);
    // markers inside the text are the template's tokens: <|im_start|>user\nhi<|im_end|> = 1 + 8 bytes... (1 + 5 + 2 + 1)
    run_job("{\"prompt\":\"<|im_start|>user\\nhi<|im_end|>\",\"max_tokens\":10}", sc, n);
    CHECK(prompt_len_seen == 9);
    // KoboldCpp
    script_of("ABC", sc, &n);
    job_kind = API_K_KOBOLD;
    run_job("{\"prompt\":\"x\",\"max_length\":5}", sc, n);
    CHECK(strcmp(body_of(wire), "{\"results\":[{\"text\":\"ABC\"}]}") == 0);
    job_kind = API_K_KOBOLD_STREAM;
    run_job("{\"prompt\":\"x\",\"max_length\":5}", sc, n);
    CHECK(strstr(wire, "text/event-stream") != NULL);
    CHECK(strstr(wire, "event: message\ndata: {\"token\":\"A\",\"finish_reason\":null}\n\n") != NULL);
    CHECK(strstr(wire, "{\"token\":\"\",\"finish_reason\":\"stop\"}") != NULL);
    // Ollama chat: NDJSON by default, one object when stream is false
    job_kind = API_K_OLLAMA_CHAT;
    run_job("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}", sc, n);
    CHECK(strstr(wire, "application/x-ndjson") != NULL);
    CHECK(strstr(wire, "\"message\":{\"role\":\"assistant\",\"content\":\"A\"},\"done\":false}\n") != NULL);
    CHECK(strstr(wire, "\"done\":true") != NULL && strstr(wire, "\"eval_count\":3") != NULL);
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":false}", sc, n);
    CHECK(strstr(wire, "Content-Type: application/json") != NULL && strstr(wire, "\"content\":\"ABC\"") != NULL);
    // Ollama generate: system + prompt as messages (11 + 10 + 17 tokens), and an error in Ollama's shape
    job_kind = API_K_OLLAMA_GEN;
    run_job("{\"prompt\":\"hi\",\"system\":\"s\",\"stream\":false}", sc, n);
    CHECK(strstr(wire, "\"response\":\"ABC\"") != NULL && prompt_len_seen == 38);
    run_job("{\"prompt\":\"\"}", sc, n);
    CHECK(strncmp(wire, "HTTP/1.1 400", 12) == 0 && strstr(wire, "{\"error\":\"") != NULL);
    // the other protocols take what fits of max_tokens; chat refuses
    eng.ctx_limit = 100;
    job_kind = API_K_KOBOLD;
    run_job("{\"prompt\":\"x\",\"max_length\":500}", sc, n);
    CHECK(strncmp(wire, "HTTP/1.1 200", 12) == 0);
    job_kind = API_K_OPENAI_TEXT;
    run_job("{\"prompt\":\"x\",\"max_tokens\":500}", sc, n);
    CHECK(strncmp(wire, "HTTP/1.1 200", 12) == 0);
    eng.ctx_limit = 4096;
    job_kind = 0;
}

static void test_stop_sequences(void) {
    uint32_t sc[64];
    int n;
    script_of("Hi\nYou: yo", sc, &n);
    job_kind = API_K_KOBOLD;
    run_job("{\"prompt\":\"x\",\"max_length\":50,\"stop_sequence\":[\"\\nYou:\"]}", sc, n);
    CHECK(strcmp(body_of(wire), "{\"results\":[{\"text\":\"Hi\"}]}") == 0);
    job_kind = API_K_KOBOLD_STREAM;
    run_job("{\"prompt\":\"x\",\"max_length\":50,\"stop_sequence\":[\"\\nYou:\"]}", sc, n);
    CHECK(strstr(wire, "You") == NULL && strstr(wire, "yo") == NULL);   // none of the stop text leaves the console
    CHECK(strstr(wire, "\"token\":\"H\"") != NULL && strstr(wire, "\"token\":\"i\"") != NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"stop\"") != NULL);
    // a half-written stop sequence that never completes is sent in the end
    script_of("abEN", sc, &n);
    run_job("{\"prompt\":\"x\",\"stop_sequence\":[\"END\"]}", sc, n);
    CHECK(strstr(wire, "\"token\":\"EN\"") != NULL);
    // a stop in the middle of one speculative step's tokens
    script_of("abEND", sc, &n);
    run_job("{\"prompt\":\"x\",\"stop_sequence\":[\"END\"]}", sc, n);
    CHECK(strstr(wire, "\"token\":\"E") == NULL);
    // chat completions take "stop" too
    job_kind = 0;
    script_of("Hello.###more", sc, &n);
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stop\":\"###\"}", sc, n);
    CHECK(strstr(wire, "\"content\":\"Hello.\"") != NULL && strstr(wire, "more") == NULL);
    CHECK(strstr(wire, "\"finish_reason\":\"stop\"") != NULL);
    CHECK(pos == 0);
}

// The prefix cache: what is read, what is restored, and when it must not be used.
static void test_cache(void) {
    uint32_t sc[16];
    int n;
    script_of("Hola", sc, &n);
    job_kind = 0;
    api_cache_on = 1;
    api_cache_drop();
    pos_errors = 0;
    // A: user "hello": 13 + "assistant\n" opened (11) = 24 of history, then the 6-token thought block
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],\"max_tokens\":10}", sc, n);
    CHECK(read_total == 30 && save_calls == 1 && restore_calls == 0 && reset_calls == 1 && api_cache_n == 24);
    // B: the conversation grew (the old answer and a new question): 24 tokens come from the saved state
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"},{\"role\":\"assistant\",\"content\":\"Hola\"},"
            "{\"role\":\"user\",\"content\":\"mas\"}],\"max_tokens\":10}", sc, n);
    CHECK(restore_calls == 1 && reset_calls == 0 && save_calls == 1);
    CHECK(read_total == 58 - 24 && api_cache_n == 52);
    CHECK(strstr(wire, "\"prompt_tokens\":58") != NULL && strstr(wire, "\"content\":\"Hola\"") != NULL);
    // the same history with a new question at the end: the state of B (52) is not a prefix of this one: back to A's? no, A was replaced
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"other\"}],\"max_tokens\":10}", sc, n);
    CHECK(restore_calls == 0 && reset_calls == 1);   // a different start: read it all
    CHECK(read_total == 30);
    // dropped (a chat turn, thinking or dictation used the engine or the memory): read it all
    api_cache_drop();
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"other\"}],\"max_tokens\":10}", sc, n);
    CHECK(restore_calls == 0 && reset_calls == 1 && read_total == 30 && api_cache_n == 24);
    // the state cannot be put back: the whole prompt is read, and the answer is still right
    fail_restore = 1;
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"other\"},{\"role\":\"assistant\",\"content\":\"Hola\"},"
            "{\"role\":\"user\",\"content\":\"x\"}],\"max_tokens\":10}", sc, n);
    fail_restore = 0;
    CHECK(reset_calls == 1 && strstr(wire, "\"content\":\"Hola\"") != NULL);
    // off: nothing is saved or restored
    api_cache_on = 0;
    api_cache_drop();
    run_job("{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],\"max_tokens\":10}", sc, n);
    CHECK(save_calls == 0 && restore_calls == 0 && api_cache_n == 0 && read_total == 30);
    // raw prompts (games that send the story so far): the state is at the end of the prompt
    api_cache_on = 1;
    job_kind = API_K_KOBOLD;
    run_job("{\"prompt\":\"abc\",\"max_length\":4}", sc, n);
    CHECK(read_total == 3 && save_calls == 1 && api_cache_n == 3);
    run_job("{\"prompt\":\"abcdef\",\"max_length\":4}", sc, n);
    CHECK(restore_calls == 1 && read_total == 3 && api_cache_n == 6);
    // the very same prompt again: nothing new to read, so it is read again in full
    run_job("{\"prompt\":\"abcdef\",\"max_length\":4}", sc, n);
    CHECK(restore_calls == 0 && reset_calls == 1 && read_total == 6);
    // a failed read leaves no saved state behind
    fail_engine_at = 1;
    run_job("{\"prompt\":\"zzz\",\"max_length\":4}", sc, n);
    fail_engine_at = -1;
    CHECK(api_cache_n == 0 && strncmp(wire, "HTTP/1.1 500", 12) == 0);
    api_cache_on = 0;
    job_kind = 0;
    CHECK(pos_errors == 0);   // every read began where the state stood
}

static void test_utf8(void) {
    CHECK(api_utf8_whole((const uint8_t*)"\xc3", 1) == 0);
    CHECK(api_utf8_whole((const uint8_t*)"a\xc3", 2) == 1);
    CHECK(api_utf8_whole((const uint8_t*)"\xc3\xa9", 2) == 2);
    CHECK(api_utf8_whole((const uint8_t*)"\xf0\x9f\x98", 3) == 0);
    CHECK(api_utf8_whole((const uint8_t*)"ab", 2) == 2);
}

int main(void) {
    test_utf8();
    for (spec_on = 0; spec_on <= 1; spec_on++) {
        test_completion();
        test_length();
        test_stream();
        test_errors();
        test_stream_error();
        test_client_gone();
        test_spec();
        test_stop();
        test_protocols();
        test_stop_sequences();
        test_cache();
        CHECK(pos_errors == 0);            // every token went in at the position after the one before
        CHECK(chat_begun == chat_ended);   // every request that took the screen gave it back
        CHECK(chat_replies <= chat_begun);
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
