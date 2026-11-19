// Host test for chat/api_compat.c (the Kobold, Ollama and OpenAI-text protocols).
// gcc -std=gnu99 -Wall -Wextra -g -fsanitize=address,undefined -I chat tests/api_compat_test.c chat/api_compat.c chat/api_proto.c -o build/api_compat_test && build/api_compat_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_compat.h"

static int fails, checks;
#define CHECK(c)                                                \
    do {                                                        \
        checks++;                                               \
        if (!(c)) {                                             \
            fails++;                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                       \
    } while (0)

static JNode pool[API_MAX_NODES];

static int parse(int kind, const char* body, GenRequest* g, const char** err) {
    return api_gen_parse(kind, body, strlen(body), pool, API_MAX_NODES, g, err);
}

static void test_parse(void) {
    GenRequest g;
    const char* err = NULL;
    // OpenAI text
    CHECK(parse(API_K_OPENAI_TEXT, "{\"prompt\":\"Once upon\",\"max_tokens\":20,\"temperature\":0.2,\"stop\":[\"\\n\\n\",\"END\"]}", &g, &err) == 0);
    CHECK(g.raw && g.prompt.n == 9 && !strcmp(g.prompt.p, "Once upon"));
    CHECK(g.max_tokens == 20 && g.temperature == 0.2 && !g.stream);
    CHECK(g.n_stops == 2 && !strcmp(g.stops[0], "\n\n") && !strcmp(g.stops[1], "END"));
    api_gen_free(&g);
    CHECK(parse(API_K_OPENAI_TEXT, "{\"prompt\":[\"a\",\"b\"],\"stop\":\"x\"}", &g, &err) == 0 && !strcmp(g.prompt.p, "a") && g.n_stops == 1);
    api_gen_free(&g);
    CHECK(parse(API_K_OPENAI_TEXT, "{\"prompt\":5}", &g, &err) == -1 && err);
    api_gen_free(&g);
    // OpenAI chat keeps its stop
    CHECK(parse(API_K_OPENAI_CHAT, "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stop\":\"\\nYou:\"}", &g, &err) == 0);
    CHECK(!g.raw && g.messages >= 0 && g.n_stops == 1 && !strcmp(g.stops[0], "\nYou:"));
    api_gen_free(&g);
    // Kobold: max_length, stop_sequence, stream follows the endpoint; out-of-range numbers are brought in
    CHECK(parse(API_K_KOBOLD, "{\"prompt\":\"x\",\"max_length\":120,\"max_context_length\":8192,\"temperature\":5,\"stop_sequence\":[\"You:\"],\"rep_pen\":1.1}", &g, &err) == 0);
    CHECK(g.raw && g.max_tokens == 120 && g.temperature == 2 && !g.stream && g.n_stops == 1);
    api_gen_free(&g);
    CHECK(parse(API_K_KOBOLD_STREAM, "{\"prompt\":\"x\"}", &g, &err) == 0 && g.stream && g.max_tokens == 80);
    api_gen_free(&g);
    CHECK(parse(API_K_KOBOLD, "{\"prompt\":\"x\",\"max_length\":999999}", &g, &err) == 0 && g.max_tokens == 16384);
    api_gen_free(&g);
    // Ollama chat: streams unless told not to; the numbers are in options
    CHECK(parse(API_K_OLLAMA_CHAT, "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"options\":{\"temperature\":0.3,\"num_predict\":64,\"seed\":9,\"stop\":[\"###\"]}}", &g, &err) == 0);
    CHECK(g.stream == 1 && g.temperature == 0.3 && g.max_tokens == 64 && g.has_seed && g.seed == 9 && g.n_stops == 1);
    api_gen_free(&g);
    CHECK(parse(API_K_OLLAMA_CHAT, "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":false,\"options\":{\"num_predict\":-1}}", &g, &err) == 0);
    CHECK(g.stream == 0 && g.max_tokens == 1024);
    api_gen_free(&g);
    CHECK(parse(API_K_OLLAMA_CHAT, "{\"messages\":[]}", &g, &err) == -1);
    api_gen_free(&g);
    // Ollama generate
    CHECK(parse(API_K_OLLAMA_GEN, "{\"model\":\"m\",\"prompt\":\"hola\",\"system\":\"eres\",\"stream\":false}", &g, &err) == 0);
    CHECK(!g.raw && !g.stream && !strcmp(g.prompt.p, "hola") && !strcmp(g.system.p, "eres"));
    api_gen_free(&g);
    CHECK(parse(API_K_OLLAMA_GEN, "{\"prompt\":\"x\",\"raw\":true}", &g, &err) == 0 && g.raw && g.stream);
    api_gen_free(&g);
    CHECK(parse(API_K_OLLAMA_GEN, "{}", &g, &err) == -1);
    api_gen_free(&g);
    CHECK(parse(API_K_KOBOLD, "[1]", &g, &err) == -1);
    api_gen_free(&g);
    // too many / too long stops are cut, not an error
    CHECK(parse(API_K_KOBOLD, "{\"prompt\":\"x\",\"stop_sequence\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"]}", &g, &err) == 0 && g.n_stops == GEN_STOPS);
    api_gen_free(&g);
}

static void test_stops(void) {
    GenRequest g;
    const char* err;
    CHECK(parse(API_K_KOBOLD, "{\"prompt\":\"x\",\"stop_sequence\":[\"\\nYou:\",\"END\"]}", &g, &err) == 0);
    CHECK(api_stop_hold(&g) == 4);   // "\nYou:" is 5 bytes: 4 may already be written
    const char* t = "Hello there.\nYou: more";
    CHECK(api_find_stop(t, strlen(t), 0, &g) == 12);
    CHECK(api_find_stop("abc END", 7, 0, &g) == 4);
    CHECK(api_find_stop("abc EN", 6, 0, &g) == -1);
    // searching only from the new bytes still sees a stop that began before them
    CHECK(api_find_stop("abc EN", 6, 5, &g) == -1);
    CHECK(api_find_stop("abc END", 7, 6, &g) == 4);
    // the earliest of several
    CHECK(api_find_stop("xEND\nYou:", 9, 0, &g) == 1);
    api_gen_free(&g);
}

static void test_time(void) {
    char iso[24];
    api_iso8601(0, iso);
    CHECK(!strcmp(iso, "1970-01-01T00:00:00Z"));
    api_iso8601(1700000000, iso);
    CHECK(!strcmp(iso, "2023-11-14T22:13:20Z"));
    api_iso8601(1709164799, iso);   // the last second of Feb 28 2024 (a leap year follows)
    CHECK(!strcmp(iso, "2024-02-28T23:59:59Z"));
    api_iso8601(1709164800, iso);
    CHECK(!strcmp(iso, "2024-02-29T00:00:00Z"));
    api_iso8601(1709164800 + 86400, iso);
    CHECK(!strcmp(iso, "2024-03-01T00:00:00Z"));
}

static void test_output(void) {
    Sb o;
    // OpenAI chat: unchanged from the chat completions chunks
    sb_init(&o);
    api_out_start(API_K_OPENAI_CHAT, &o, "id1", "M", 5);
    CHECK(strstr(o.p, "data: {") == o.p && strstr(o.p, "\"delta\":{\"role\":\"assistant\"}") && strstr(o.p, "\n\n"));
    sb_free(&o);
    sb_init(&o);
    api_out_end(API_K_OPENAI_CHAT, &o, "id1", "M", 5, "stop", 3, 4);
    CHECK(strstr(o.p, "\"finish_reason\":\"stop\"") && strstr(o.p, "data: [DONE]\n\n"));
    sb_free(&o);
    // OpenAI text
    sb_init(&o);
    api_out_piece(API_K_OPENAI_TEXT, &o, "id", "M", 5, "hola", 4);
    CHECK(strstr(o.p, "\"object\":\"text_completion\"") && strstr(o.p, "\"text\":\"hola\""));
    sb_free(&o);
    sb_init(&o);
    api_out_whole(API_K_OPENAI_TEXT, &o, "id", "M", 5, "hola", 4, "length", 3, 4);
    CHECK(strstr(o.p, "\"text\":\"hola\"") && strstr(o.p, "\"finish_reason\":\"length\"") && strstr(o.p, "\"total_tokens\":7"));
    sb_free(&o);
    // Kobold
    sb_init(&o);
    api_out_whole(API_K_KOBOLD, &o, "id", "M", 5, "He said \"hi\"\n", 13, "stop", 1, 1);
    CHECK(!strcmp(o.p, "{\"results\":[{\"text\":\"He said \\\"hi\\\"\\n\"}]}"));
    sb_free(&o);
    sb_init(&o);
    api_out_piece(API_K_KOBOLD_STREAM, &o, "id", "M", 5, "ab", 2);
    CHECK(!strcmp(o.p, "event: message\ndata: {\"token\":\"ab\",\"finish_reason\":null}\n\n"));
    sb_free(&o);
    sb_init(&o);
    api_out_end(API_K_KOBOLD_STREAM, &o, "id", "M", 5, "stop", 1, 1);
    CHECK(strstr(o.p, "\"finish_reason\":\"stop\"") != NULL);
    sb_free(&o);
    // Ollama chat and generate
    sb_init(&o);
    api_out_piece(API_K_OLLAMA_CHAT, &o, "id", "M", 1700000000, "x", 1);
    CHECK(!strcmp(o.p, "{\"model\":\"M\",\"created_at\":\"2023-11-14T22:13:20Z\",\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":false}\n"));
    sb_free(&o);
    sb_init(&o);
    api_out_end(API_K_OLLAMA_GEN, &o, "id", "M", 5, "stop", 7, 9);
    CHECK(strstr(o.p, "\"response\":\"\"") && strstr(o.p, "\"done\":true") && strstr(o.p, "\"done_reason\":\"stop\"") &&
          strstr(o.p, "\"prompt_eval_count\":7") && strstr(o.p, "\"eval_count\":9") && o.p[o.n - 1] == '\n');
    sb_free(&o);
    sb_init(&o);
    api_out_whole(API_K_OLLAMA_CHAT, &o, "id", "M", 5, "full", 4, "stop", 1, 2);
    CHECK(strstr(o.p, "\"content\":\"full\"") && strstr(o.p, "\"done\":true") && o.p[o.n - 1] == '}');
    sb_free(&o);
    // heads and errors
    CHECK(strstr(api_stream_head(API_K_OLLAMA_CHAT), "application/x-ndjson") != NULL);
    CHECK(strstr(api_stream_head(API_K_KOBOLD_STREAM), "text/event-stream") != NULL);
    sb_init(&o);
    api_out_http_error(API_K_OLLAMA_CHAT, &o, "busy", "server_error", "busy");
    CHECK(!strcmp(o.p, "{\"error\":\"busy\"}"));
    sb_free(&o);
    sb_init(&o);
    api_out_http_error(API_K_OPENAI_TEXT, &o, "busy", "server_error", "busy");
    CHECK(strstr(o.p, "\"error\":{\"message\":\"busy\"") != NULL);
    sb_free(&o);
    sb_init(&o);
    api_out_error(API_K_OLLAMA_GEN, &o, "gpu");
    CHECK(!strcmp(o.p, "{\"error\":\"gpu\"}\n"));
    sb_free(&o);
}

static void test_info(void) {
    Sb o;
    sb_init(&o);
    CHECK(api_info("/api/tags", "Qwen3.5-9B", 16384, &o) && strstr(o.p, "\"name\":\"Qwen3.5-9B\""));
    sb_free(&o);
    sb_init(&o);
    CHECK(api_info("/api/v1/model", "Qwen3.5-9B", 16384, &o) && !strcmp(o.p, "{\"result\":\"koboldcpp/Qwen3.5-9B\"}"));
    sb_free(&o);
    sb_init(&o);
    CHECK(api_info("/api/v1/config/max_context_length", "M", 16384, &o) && !strcmp(o.p, "{\"value\":16384}"));
    sb_free(&o);
    sb_init(&o);
    CHECK(api_info("/api/extra/version", "M", 1, &o) && strstr(o.p, "KoboldCpp"));
    sb_free(&o);
    sb_init(&o);
    CHECK(api_info("/api/show", "M", 4096, &o) && strstr(o.p, "\"qwen35.context_length\":4096"));
    sb_free(&o);
    sb_init(&o);
    CHECK(!api_info("/nope", "M", 1, &o));
    sb_free(&o);
    CHECK(api_kind_of("POST", "/v1/completions") == API_K_OPENAI_TEXT);
    CHECK(api_kind_of("POST", "/api/v1/generate") == API_K_KOBOLD);
    CHECK(api_kind_of("POST", "/api/extra/generate/stream") == API_K_KOBOLD_STREAM);
    CHECK(api_kind_of("POST", "/api/chat") == API_K_OLLAMA_CHAT);
    CHECK(api_kind_of("POST", "/api/generate") == API_K_OLLAMA_GEN);
    CHECK(api_kind_of("POST", "/v1/chat/completions") == API_K_OPENAI_CHAT);
    CHECK(api_kind_of("GET", "/api/chat") == -1);
}

int main(void) {
    test_parse();
    test_stops();
    test_time();
    test_output();
    test_info();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
