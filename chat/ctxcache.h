// Context cache on disk. A new conversation that starts with attached files (the repo map, a README...) spends minutes
// reading them (prefill ~65 tokens/s: 7K tokens = ~2 min). The state of the model after that prefix is only the KV
// cache (TurboQuant, the full-attention layers) and the Gated DeltaNet states, so it can be written to
// /data/garlic_cache/<key>.kv and put back in seconds the next time the same prefix comes (same system prompt, same
// files: the key is a hash of the prefix's token ids and of the model's shape and weights file).
//
// start_turn (main.c) calls ctxcache_load with the tokens before the user's question; on a miss it runs that prefix
// alone and calls ctxcache_save. At most 3 snapshots are kept (the oldest used goes first). A snapshot is ~116 MiB
// (50 MiB of states + 66 MiB for the first 8K positions; +66 MiB when the prefix is longer than 8192 tokens).
// /data/garlic_cache.txt starting with "0" turns it off.
#ifndef CTXCACHE_H
#define CTXCACHE_H

#include "engine.h"

#define CTXCACHE_MIN_TOKENS 1200   // shorter prefixes are read faster than a snapshot is restored

int ctxcache_enabled(void);

// ids[0..pre) are the prefix of a new conversation (engine_reset done, nothing run). 1 = a snapshot of exactly that
// prefix was put back into the engine (the caller continues at position pre and calls engine_spec_follow);
// 0 = none (the engine is untouched); -1 = it failed halfway (the engine is damaged: engine_reset and read it all).
int ctxcache_load(Engine* e, const uint32_t* ids, int pre);

// The engine has just run ids[0..pre) from an empty conversation: writes the snapshot. 1 = saved, 0 = not saved
// (disabled, too big, disk full, or this console's CPU reads GPU memory too slowly: the reason goes to the log).
int ctxcache_save(Engine* e, const uint32_t* ids, int pre);

#endif
