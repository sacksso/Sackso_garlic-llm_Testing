// API jobs (api.c queues them; included in main.c, so it sees the engine and the conversation). The protocols are
// OpenAI chat and text completions, KoboldCpp and Ollama (api_compat.c): all of them end up as a prompt and a few
// numbers here.
// A job runs on the model between the chat's turns: the request's messages are the whole conversation, so the
// conversation on screen is cleared, and the reply is written to the connection (whole, or as server-sent events).
// The reply is generated as the chat generates its own: several tokens per step with the MTP head where the app has
// it (GARLIC_SPEC), and Circle on the controller stops it. What the screen shows meanwhile is main.c's part
// (api_chat_begin / _reply / _step / _end).

static uint32_t api_ids[16384];   // the prompt of a job (byte-level tokens: at most one per byte of text)

// ---- prefix cache ("/cache on", off by default) ------------------------------------------------------------------
// A game sends the same long start every time (the character, the story so far) and a little more at the end. Without
// this every request reads the whole prompt again (about 75 tokens a second). With it the DeltaNet state is kept, in
// the work memory the dictation uses, at the end of the prompt (before the generation prompt), together with the ids
// it stands for; the next request that starts with those same ids puts the state back and reads only what is new. The
// attention cache needs nothing: it is written by position and the new tokens go in from where the state was.
// Whatever else uses the engine or that memory in between (a chat turn, thinking, dictation, a Python program) calls
// api_cache_drop.
static uint32_t api_cache_ids[16384];
static int api_cache_n;   // positions the saved state stands for; 0 = nothing saved
static void api_cache_drop(void) { api_cache_n = 0; }
static int api_cache_room(void) {
    uint64_t need = engine_state_size(&eng);
    return eng.asr.ws_end && eng.asr.ws_end - eng.asr.ws_begin >= need;
}
#define API_MAX_TEXT 15000        // bytes of message text a request may carry (the prompt must fit api_ids)

// How many bytes at the start of p (n) are whole UTF-8 characters (the rest waits for the next token).
static size_t api_utf8_whole(const uint8_t* p, size_t n) {
    size_t i = n;
    for (int back = 1; back <= 4 && i > 0; back++) {
        i--;
        uint8_t c = p[i];
        if ((c & 0xC0) == 0x80) continue;   // a continuation byte: keep looking for its lead byte
        size_t need = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        return back < (int)need ? i : n;
    }
    return n;
}

// The bytes of a server answer: head (status line included) then the body, as one write.
static void api_reply_json(int fd, const char* status_line, const Sb* body) {
    Sb h;
    sb_init(&h);
    sb_str(&h, status_line);
    sb_str(&h, "\r\nContent-Type: application/json\r\nContent-Length: ");
    sb_int(&h, (long long)body->n);
    sb_str(&h, "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    if (h.p && body->p && !h.oom && api_send(fd, h.p, h.n) == 0) api_send(fd, body->p, body->n);
    sb_free(&h);
}

static void api_error(int kind, int fd, const char* status_line, const char* message, const char* type, const char* code) {
    Sb s;
    sb_init(&s);
    api_out_http_error(kind, &s, message, type, code);
    api_reply_json(fd, status_line, &s);
    sb_free(&s);
}

// One message as <|im_start|>role\ncontent<|im_end|>\n (the ids go to api_ids from n; returns the new n).
static int api_push_msg(int n, const char* role, const char* content) {
    api_ids[n++] = IM_START;
    n = encode_into(api_ids, n, 16384, role);
    n = encode_into(api_ids, n, 16384, "\n");
    if (content) n = encode_into(api_ids, n, 16384, content);
    api_ids[n++] = IM_END;
    n = encode_into(api_ids, n, 16384, "\n");
    return n;
}

// The assistant's turn opens: "assistant\n" and, for Qwen3.5, the empty thought block (as the chat does with
// enable_thinking=false). *snap is where the generation prompt begins (the tokens before it are the history).
static int api_push_turn(int n, int* snap) {
    api_ids[n++] = IM_START;
    n = encode_into(api_ids, n, 16384, "assistant\n");
    *snap = n;
    if (eng.qwen35) {
        api_ids[n++] = THINK;
        n = encode_into(api_ids, n, 16384, "\n\n");
        api_ids[n++] = THINK_END;
        n = encode_into(api_ids, n, 16384, "\n\n");
    }
    return n;
}

// Text a game wrote for the model, as it is, except that the markers of Qwen's template are the tokens they stand for.
static int api_encode_raw(char* text) {
    const struct {   // (not static: IM_START and the others are variables in main.c, not constants)
        const char* s;
        uint32_t id;
    } marks[] = {{"<|im_start|>", IM_START}, {"<|im_end|>", IM_END}, {"<think>", THINK}, {"</think>", THINK_END}};
    int n = 0;
    char* seg = text;
    for (char* p = text; *p && n < 16384 - 8;) {
        int hit = -1;
        if (*p == '<')
            for (int m = 0; m < 4; m++)
                if (!strncmp(p, marks[m].s, strlen(marks[m].s))) hit = m;
        if (hit < 0) {
            p++;
            continue;
        }
        char c = *p;
        *p = 0;   // (the text before the marker)
        n = encode_into(api_ids, n, 16384, seg);
        *p = c;
        api_ids[n++] = marks[hit].id;
        p += strlen(marks[hit].s);
        seg = p;
    }
    return n < 16384 - 8 ? encode_into(api_ids, n, 16384, seg) : n;
}

// Builds the prompt: a chat's messages with the assistant's turn opened, or the game's own text. Returns the number
// of ids, or -1 if it does not fit. *snap: how many of them are history (the saved state may stand there).
static int api_prompt(const JNode* pool, GenRequest* g, int* snap) {
    int n = 0;
    if (g->raw) {
        if (g->prompt.n > API_MAX_TEXT) return -1;
        n = api_encode_raw(g->prompt.p ? g->prompt.p : (char*)"");
        *snap = n;
        static const char open_turn[] = "<|im_start|>assistant\n";
        if (eng.qwen35 && g->prompt.n >= sizeof(open_turn) - 1 &&
            !memcmp(g->prompt.p + g->prompt.n - (sizeof(open_turn) - 1), open_turn, sizeof(open_turn) - 1)) {
            // a ChatML prompt that ends where the assistant starts: no thinking, as for a chat
            api_ids[n++] = THINK;
            n = encode_into(api_ids, n, 16384, "\n\n");
            api_ids[n++] = THINK_END;
            n = encode_into(api_ids, n, 16384, "\n\n");
        }
        return n;
    }
    if (g->messages >= 0) {
        size_t total = 0;
        for (int m = pool[g->messages].first; m >= 0; m = pool[m].next) {
            Sb content;
            sb_init(&content);
            char role[16] = "user";
            int ok = api_message(pool, m, role, sizeof(role), &content) == 0;
            if (ok) total += content.n;
            if (!ok || total > API_MAX_TEXT) {
                sb_free(&content);
                return -1;
            }
            if (strcmp(role, "system") && strcmp(role, "assistant") && strcmp(role, "user") && strcmp(role, "tool"))
                strcpy(role, "user");
            n = api_push_msg(n, role, content.p);
            sb_free(&content);
        }
    } else {   // Ollama /api/generate without raw: the system text and the prompt as messages
        if (g->system.n + g->prompt.n > API_MAX_TEXT) return -1;
        if (g->system.n) n = api_push_msg(n, "system", g->system.p);
        n = api_push_msg(n, "user", g->prompt.p);
    }
    if (n > 16384 - 64) return -1;
    return api_push_turn(n, snap);
}

static void api_job(ApiJob* job) {
    static unsigned api_count;
    const int kind = job->kind;
    JNode* pool = malloc(sizeof(JNode) * API_MAX_NODES);
    GenRequest g;
    const char* err = NULL;
    if (!pool || api_gen_parse(kind, job->body, job->body_len, pool, API_MAX_NODES, &g, &err)) {
        if (pool) api_gen_free(&g);
        free(pool);
        api_error(kind, job->fd, "HTTP/1.1 400 Bad Request", err ? err : "Out of memory.", "invalid_request_error",
                  "invalid_body");
        return;
    }
    int snap = 0;
    int n = api_prompt(pool, &g, &snap);
    free(pool);
    if (n < 0) {
        api_gen_free(&g);
        api_error(kind, job->fd, "HTTP/1.1 400 Bad Request", "The messages are malformed or too long.",
                  "invalid_request_error", "context_length_exceeded");
        return;
    }
    if (n + g.max_tokens + 2 > (int)eng.ctx_limit) {
        int room = (int)eng.ctx_limit - n - 2;
        if (kind != API_K_OPENAI_CHAT && room >= 16) {   // the other protocols' clients ask for a length and take what fits
            g.max_tokens = room;
        } else {
            api_gen_free(&g);
            api_error(kind, job->fd, "HTTP/1.1 400 Bad Request", "The messages and max_tokens do not fit the context window.",
                      "invalid_request_error", "context_length_exceeded");
            return;
        }
    }
    // the model answers this request: the conversation on screen starts again from nothing (a request refused
    // above leaves it as it was)
    pos = 0;
    need_close = 0;
    hist_n = hist_done = 0;
    thinking = think_turn = 0;
    sys_pre = 0;
    view.scroll = 0;
    api_chat_begin();   // (clears the conversation: a note says that a request is being answered)
    if (g.has_seed) seed = g.seed;
    char id[32];
    snprintf(id, sizeof(id), "chatcmpl-%u", ++api_count);
    const char* model = api_model();
    long long created = (long long)time(NULL);
    // The prompt. With the prefix cache on, a saved state that stands for the first ids of this prompt is put back and
    // only the rest is read (see api_cache_ids).
    int reuse_n = api_cache_n;
    api_cache_n = 0;   // (until a state is saved again, or the old one is confirmed below)
    int start = 0;
    if (api_cache_on && reuse_n > 0 && reuse_n <= snap && (snap < n || reuse_n < n) &&
        !memcmp(api_cache_ids, api_ids, sizeof(uint32_t) * (size_t)reuse_n)) {
        if (engine_state_copy(&eng, eng.asr.ws_begin, 0) == 0) start = reuse_n;
        else say("api: cache: the state could not be put back (%s); the whole prompt is read", eng.error);
    }
    say("api: %s prompt of %d tokens, up to %d new, T %.2f%s%s", kind == API_K_OPENAI_CHAT ? "chat" : kind == API_K_OPENAI_TEXT ? "text" :
        kind <= API_K_KOBOLD_STREAM ? "kobold" : "ollama", n, g.max_tokens, g.temperature, g.stream ? ", streamed" : "",
        g.n_stops ? ", with stop sequences" : "");
    if (start) say("api: cache hit: %d of the %d prompt tokens come from the saved state, %d to read", start, n, n - start);
    else if (api_cache_on) say("api: cache miss: the whole prompt is read");
    if (!start) engine_reset(&eng);
    // One read of the prompt, as it always was; with the cache on and room for the state it is two: the history, where
    // the state is saved, then the generation prompt.
    int failed = 0;
    const int save = api_cache_on && snap > start && api_cache_room();
    const int upto = save ? snap : n;
    if (upto > start) failed = engine_run(&eng, api_ids + start, upto - start, start, (float)g.temperature, seed++, &next_token);
    if (!failed) {
        if (save && engine_state_copy(&eng, eng.asr.ws_begin, 1) == 0) {
            memcpy(api_cache_ids, api_ids, sizeof(uint32_t) * (size_t)snap);
            api_cache_n = snap;   // (a state that stands for this prompt's history)
        } else if (start) {
            api_cache_n = reuse_n;   // (nothing new saved: the old state still stands in the work memory)
        }
        if (save && snap < n) failed = engine_run(&eng, api_ids + snap, n - snap, snap, (float)g.temperature, seed++, &next_token);
    }
    if (failed) {
        api_cache_n = 0;
        api_error(kind, job->fd, "HTTP/1.1 500 Internal Server Error", eng.error, "server_error", NULL);
        pos = 0;
        api_chat_end();
        api_gen_free(&g);
        return;
    }
    pos = n;

    int fd = job->fd, streamed = 0, client_gone = 0, error = 0;
    const size_t hold = api_stop_hold(&g);   // (bytes of a stop sequence that may be half written stay unsent)
    if (g.stream) {
        const char* h = api_stream_head(kind);
        if (api_send(fd, h, strlen(h))) client_gone = 1;
        streamed = 1;
        Sb first;
        sb_init(&first);
        api_out_start(kind, &first, id, model, created);
        if (!client_gone && first.n && api_send(fd, first.p, first.n)) client_gone = 1;
        sb_free(&first);
    }

    Sb text, pend;   // the reply (all of it), and the bytes of the last tokens not yet sent
    sb_init(&text);
    sb_init(&pend);
    int produced = 0, stopped = 0;
    const char* finish = "length";
    // The tokens to write: the one a step drew, after those it kept from the MTP head's draft (the model has read
    // these already). Without the head a step is one token.
    uint32_t q[9];
    int nq = 1, iq = 0;
    q[0] = next_token;
    double t0 = now();
    api_chat_reply();
    while (produced < g.max_tokens) {
        uint32_t t = q[iq++];
        if (t == IM_END || t == eng.eos2 || t == eng.eos) {
            finish = "stop";
            break;
        }
        const uint8_t* bytes = NULL;
        int nb = tok_piece(&tok, t, &bytes);
        produced++;
        if (nb > 0) {
            size_t from = text.n;
            sb_add(&text, (const char*)bytes, nb);
            sb_add(&pend, (const char*)bytes, nb);
            if (g.n_stops && text.p) {
                long cut = api_find_stop(text.p, text.n, from, &g);
                if (cut >= 0) {   // a stop sequence: the reply ends before it
                    size_t removed = text.n - (size_t)cut;
                    text.n = (size_t)cut;
                    text.p[text.n] = 0;
                    pend.n = removed >= pend.n ? 0 : pend.n - removed;
                    if (pend.p) pend.p[pend.n] = 0;
                    finish = "stop";
                    break;
                }
            }
        }
        if (streamed && pend.n > hold) {
            size_t whole = api_utf8_whole((const uint8_t*)pend.p, pend.n - hold);
            if (whole) {
                Sb ev;
                sb_init(&ev);
                api_out_piece(kind, &ev, id, model, created, pend.p, whole);
                if (ev.p && !ev.oom && api_send(fd, ev.p, ev.n)) client_gone = 1;
                sb_free(&ev);
                memmove(pend.p, pend.p + whole, pend.n - whole);
                pend.n -= whole;
                pend.p[pend.n] = 0;
            }
        }
        if (client_gone || produced == g.max_tokens) break;
        if (iq < nq) continue;   // (the next token is one the last step kept)
        if (api_chat_step(produced)) {   // Circle: the reply ends here, cut short as by max_tokens
            stopped = 1;
            break;
        }
        int ne = 0;   // tokens of the draft this step keeps
#ifdef GARLIC_SPEC
        if (spec_active(&eng)) {   // (never more than max_tokens: the step keeps at most what is left of them)
            if (engine_spec_step(&eng, t, pos, (float)g.temperature, seed++, g.max_tokens - produced + 1, q, &ne,
                                 &next_token)) {
                error = 1;
                break;
            }
        } else
#endif
        if (engine_run(&eng, &t, 1, pos, (float)g.temperature, seed++, &next_token)) {
            error = 1;
            break;
        }
        pos += 1 + ne;
        q[ne] = next_token;
        nq = ne + 1, iq = 0;
    }
    double dt = now() - t0;
    say("api: %d tokens in %.1f s (%.1f tokens/s), %s", produced, dt, dt > 0 ? produced / dt : 0.0,
        error ? "engine error" : stopped ? "stopped on the console" : finish);

    if (streamed) {
        if (!client_gone && !error) {
            Sb last;
            sb_init(&last);
            size_t whole = pend.n ? api_utf8_whole((const uint8_t*)pend.p, pend.n) : 0;
            if (whole) api_out_piece(kind, &last, id, model, created, pend.p, whole);   // (what the held-back bytes were waiting for)
            api_out_end(kind, &last, id, model, created, finish, n, produced);
            if (last.p && !last.oom) api_send(fd, last.p, last.n);
            sb_free(&last);
        }
        if (error && !client_gone) {   // the headers are out: the error goes in the stream
            Sb e;
            sb_init(&e);
            api_out_error(kind, &e, eng.error);
            if (e.p) api_send(fd, e.p, e.n);
            sb_free(&e);
        }
    } else if (error) {
        api_error(kind, fd, "HTTP/1.1 500 Internal Server Error", eng.error, "server_error", NULL);
    } else {
        Sb body;
        sb_init(&body);
        api_out_whole(kind, &body, id, model, created, text.p ? text.p : "", text.n, finish, n, produced);
        api_reply_json(fd, "HTTP/1.1 200 OK", &body);
        sb_free(&body);
    }
    sb_free(&text);
    sb_free(&pend);
    api_gen_free(&g);
    // the engine's state is this request's; the conversation on screen starts again at position 0
    pos = 0;
    need_close = 0;
    api_chat_end();
}
