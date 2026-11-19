// GoldHEN cheats and game patches of a game (phase 11), read-only. Taken from the idea of claude-for-ps4's GoldHEN
// manager (MIT, blue0x1): before editing a saved game, see whether GoldHEN already has a cheat or a patch for it.
//   /trucos CUSA06548
#ifndef GHINFO_H
#define GHINFO_H

#ifndef GH_DIR
#define GH_DIR "/data/GoldHEN"
#endif

// Text with the cheat files (json / shn / mc4) and the game patches of the title (CUSAxxxxx), the names of what each one
// does. Returns the length, or < 0 when title is not a title id.
int gh_game_info(const char* title, char* out, int max);

#endif
