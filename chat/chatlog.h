// Conversation log: every chat turn is appended to /data/chats/chat_NNNN.jsonl (one JSON object per line), so the
// conversations survive the app and can be read over FTP. Off when /data/garlic_chatlog.txt starts with "0".
// The API's requests are not logged (they have their own client).
#ifndef CHATLOG_H
#define CHATLOG_H

// A new conversation starts: picks the next number (/data/chats/counter.txt) and opens its file name.
void chatlog_begin(void);

// Appends {"n":<turn>,"role":"user"|"assistant","cut":0|1,"text":"..."}. cut = 1: a reply that was stopped.
// Does nothing before chatlog_begin or when the log is off.
void chatlog_add(const char* role, const char* text, int cut);

#endif
