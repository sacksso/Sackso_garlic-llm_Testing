// Profiles (phase 13): a name for a way of answering. A profile is a system prompt and, optionally, the sampling that
// goes with it, so the same console can be a programmer, a narrator for a text adventure or the voice of one character
// of a game, without changing the model or losing any of the others.
//   In the chat:  /perfil            lists them and shows the active one
//                 /perfil narrador   starts a new chat with that profile (L2/R2 still change the temperature)
//                 /perfil off        back to the programming prompt (what the chat does without a profile)
//   In the API:   the request's "model" names it ("garlic-9b:narrador", or just "narrador"), or
//                 /data/garlic_api_profile.txt holds the name of the one to use when the request names none.
//                 The profile's text goes first in the system message; the game's own system message follows it (the
//                 character sheet, the rules of the world), so the game decides the details and the profile the manner.
//                 Temperature and length of the profile are used only when the request gave none.
// Own profiles: /data/garlic_profiles/NAME.txt (a file with the name of a built-in one replaces it):
//     # comments
//     desc=one line shown by /perfil
//     temperature=0.9
//     max_tokens=160
//     stop=\nPlayer:            (up to two; \n is a line break)
//     ---
//     the system prompt, as many lines as needed (about 3.5 KB at most)
// A file without a "---" line is all prompt. No console calls: tests/profiles_test.c runs it on a PC.
#ifndef PROFILES_H
#define PROFILES_H

#ifndef PROFILES_DIR
#define PROFILES_DIR "/data/garlic_profiles"
#endif
#ifndef PROFILE_API_FILE
#define PROFILE_API_FILE "/data/garlic_api_profile.txt"
#endif

#define PROFILE_TEXT_MAX 3600

typedef struct {
    char name[40];                  // lower case: letters, digits, - and _
    char desc[110];
    char text[PROFILE_TEXT_MAX];    // the system prompt; "" for the programmer (the chat uses its own programming prompt)
    double temperature;             // < 0: the profile does not set it
    int max_tokens;                 // 0: not set
    char stops[2][48];
    int n_stops;
    int builtin;                    // 1 when it comes from the app and no file replaced it
} Profile;

// 1 when the profile exists (a file first, then the built-in ones); name is matched without regard to case.
int profile_find(const char* name, Profile* out);

// "name - description" per line, built-in ones first. Returns the length written.
int profile_list(char* out, int max);

// The profile a request's "model" names: the whole text, or what follows the last ':' or '/'. 1 when there is one.
int profile_from_model(const char* model, char* name, int max);

// The profile /data/garlic_api_profile.txt names (1) or none (0).
int profile_api_default(char* name, int max);

// "/perfil", "/perfiles" or "/profile" with an optional word after it: 1 and the word in arg (lower case).
int profile_is_cmd(const char* text, char* arg, int max);

#endif
