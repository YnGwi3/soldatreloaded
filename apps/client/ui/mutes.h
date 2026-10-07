#pragma once

// A player's own mutes, on their screen alone (the original's /mute, which the server
// knows nothing of; an admin's, for everyone, is /servermute): players muted by name,
// kept in config/mutes.txt so a mute holds past a rejoin until /unmute; and whole kinds
// of chat, by who says it (cl_muteall, cl_muteteam, cl_muteenemies, cl_mutespecs).
// A muted player's taunts and radio calls, said by a bind and not typed, still come
// through; a spectator's, under cl_mutespecs, don't.

#include <stdbool.h>

#include "network/network.h"

#define MUTES_MAX 64

typedef struct Mutes {
    char names[MUTES_MAX][NET_NAME_SIZE];
    int count;
} Mutes;

// The kinds of chat muted.
typedef struct MuteKinds {
    bool all, team, enemies, specs;
} MuteKinds;

// The names in `path`, one to a line ("//" begins a comment); none if there is no file.
void mutes_load(Mutes *m, const char *path);
// Written whole; false if it couldn't be.
bool mutes_save(const Mutes *m, const char *path);

bool mutes_has(const Mutes *m, const char *name); // in any case
// False when it was already there (or the list is full), or wasn't there to remove.
bool mutes_add(Mutes *m, const char *name);
bool mutes_remove(Mutes *m, const char *name);

// Whether a player's line is kept off my screen: said by `name` on `team` to me on
// `mine`, by a bind (`taunt`) or typed. With no teams everyone is an enemy.
bool mutes_hide(const Mutes *m, MuteKinds kinds, const char *name, Team team, Team mine, bool team_game, bool taunt);
