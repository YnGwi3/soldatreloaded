#pragma once

// A game hosted, as the dedicated server hosts it and the game's Local Play alike: the
// host (host.h), the script that runs with it (sv_script), the lobby it lists itself with
// (sv_public), and the weapons mod it plays (config/weapons.ini), all by the hosting
// settings (host_cvars.h) and the files of config/. The dedicated server is a console and
// a loop around one (server/main.c); the game holds one for Local Play and joins it over
// the loopback, pumping it each frame before it polls its own line, so a game hosted from
// the main menu is the one a server from this install hosts.

#include "console/console.h"
#include "host.h"
#include "host_cvars.h"
#include "lobby.h"
#include "script.h"

#define CONFIG_LISTS "config"               // banlist.txt, mutelist.txt, admins.txt (lists.h)
#define CONFIG_WEAPONS "config/weapons.ini" // a weapons mod, as Soldat's (weapons_ini.h)

typedef struct Hosted {
    Console *console;
    HostCvars *cvars;
    Cvar *data; // what the game plays by
    // the weapons mod: the game's own numbers, changed by config/weapons.ini and `weapon`
    WeaponStats weapons[WEAPON_COUNT];
    bool weapons_mod;
    Host host;
    Script script;
    Lobby lobby;
    bool running;
} Hosted;

// The hosting commands onto `con` (nextmap, kick, ban and the rest of an admin's,
// servermute, weapon, addbot, pause, lua...), the weapons at the game's own numbers.
void hosted_init(Hosted *h, Console *con, HostCvars *cvars, Cvar *data);

// config/weapons.ini over the game's own numbers, a mod if it changes any; from the game's
// own each time.
void hosted_load_weapons(Hosted *h);

// The game hosted by the cvars as they stand: the host on its port, the script, the lobby.
// False if the host couldn't be opened (the map, the port).
bool hosted_open(Hosted *h);

// A frame of it: `dt` seconds of the host, the script's answers, the lobby's heartbeat at
// `now`. False when the host has stopped.
bool hosted_pump(Hosted *h, double dt, double now);

void hosted_close(Hosted *h);
