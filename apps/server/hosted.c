#include "hosted.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/game.h"
#include "game/systems/systems.h"
#include "rounds.h"
#include "weapons_ini.h"

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The words from `first` on, joined with single spaces.
static void words_from(int argc, char **argv, int first, char *out, size_t size)
{
    size_t n = 0;
    out[0] = '\0';
    for (int i = first; i < argc && n < size - 1; i++) {
        int w = snprintf(out + n, size - n, i > first ? " %s" : "%s", argv[i]);
        if (w < 0) break;
        n += (size_t)w; // past the end once it is full, and the loop ends
    }
}

static bool hosting(Hosted *h, Console *con)
{
    if (h->running && h->host.game) return true;
    console_print(con, "no game is being hosted\n");
    return false;
}

// kick, ban, banip, banhw, unban, servermute, serverunmute, bans, admins: the admin
// commands, as an admin says them in the chat (connections_admin), answered here.
static void cmd_admin(Console *con, int argc, char **argv, void *user)
{
    Hosted *h = user;
    char text[NET_TEXT_SIZE];
    words_from(argc, argv, 0, text, sizeof text);
    if (hosting(h, con)) connections_admin(&h->host.connections, h->host.game, -1, text);
}

// nextmap: the round ends now and the next begins.
static void cmd_nextmap(Console *con, int argc, char **argv, void *user)
{
    (void)argc, (void)argv;
    Hosted *h = user;
    if (hosting(h, con)) host_end_round(&h->host);
}

// addbot [name] / addbot1 [name] / addbot2 [name]: a bot, on the emptier side, on
// alpha, or on bravo; one of data/bots at random unless named (the original's commands).
static void cmd_addbot(Console *con, int argc, char **argv, void *user)
{
    Hosted *h = user;
    if (!h->running) {
        console_print(con, "not hosting: bots join a game hosted here (Local Play, or `host`)\n");
        return;
    }
    Team team = argv[0][6] == '1' ? TEAM_ALPHA : argv[0][6] == '2' ? TEAM_BRAVO : TEAM_NONE;
    host_add_bot(&h->host, team, argc > 1 ? argv[1] : NULL);
}

// pause / unpause: the game stands still, nobody moving and the clock stopped, or goes
// on; everyone is told.
static void cmd_pause(Console *con, int argc, char **argv, void *user)
{
    (void)argc;
    Hosted *h = user;
    if (!hosting(h, con)) return;
    bool pause = strcmp(argv[0], "pause") == 0;
    if (host_pause(&h->host, pause)) connections_say_kind(&h->host.connections, CHAT_GAME, (Rgba){0}, pause ? "Game paused" : "Game unpaused");
}

// The script sv_script names, if there is one. A path set by hand that isn't there is
// said; the default's absence is nothing.
static void script_start(Hosted *h)
{
    const char *path = h->cvars->script->value;
    if (!path[0]) return;
    if (!file_exists(path)) {
        if (strcmp(path, h->cvars->script->default_value) != 0) console_print(h->console, "no script at %s\n", path);
        return;
    }
    script_open(&h->script, &h->host, h->console, path);
}

// script_reload: the script read again, from the start; its state is lost.
static void cmd_script_reload(Console *con, int argc, char **argv, void *user)
{
    (void)argc, (void)argv;
    Hosted *h = user;
    if (!hosting(h, con)) return;
    script_close(&h->script);
    script_start(h);
}

// lua <code...>: a line of Lua run in the script's state.
static void cmd_lua(Console *con, int argc, char **argv, void *user)
{
    Hosted *h = user;
    if (!h->script.L) {
        console_print(con, "no script is running\n");
        return;
    }
    char code[CONSOLE_TEXT_SIZE];
    words_from(argc, argv, 1, code, sizeof code);
    script_run(&h->script, code, "console");
}

// weapon <name> <field> <value> [<field> <value>...]: a weapons mod's line. The fields are
// WEAPON_FIELDS'; while a game is on it takes the new numbers at once, and everyone on
// is told.
static void cmd_weapon(Console *con, int argc, char **argv, void *user)
{
    Hosted *h = user;
    if (argc < 4 || argc % 2 != 0) {
        console_print(con, "usage: weapon <name> <field> <value> [<field> <value>...]; weaponlist shows them all\n");
        return;
    }
    WeaponId id = weapon_named(argv[1]);
    if (id == WEAPON_NONE && strcmp(argv[1], "Hands") != 0) {
        console_print(con, "weapon: no weapon \"%s\"\n", argv[1]);
        return;
    }
    if (!weapons_moddable(id)) {
        console_print(con, "weapon: %s follows another weapon's numbers\n", argv[1]);
        return;
    }
    WeaponStats *stats = &h->weapons[id];
    for (int a = 2; a + 1 < argc; a += 2) {
        const NetField *f = NULL;
        for (int k = 0; k < WEAPON_FIELD_COUNT && !f; k++)
            if (strcmp(WEAPON_FIELDS[k].name, argv[a]) == 0) f = &WEAPON_FIELDS[k];
        if (!f) {
            console_print(con, "weapon: no field \"%s\"\n", argv[a]);
            continue;
        }
        uint8_t *at = (uint8_t *)stats + f->offset;
        if (f->kind == NET_F32) *(float *)at = (float)atof(argv[a + 1]);
        else *(int32_t *)at = (int32_t)atoi(argv[a + 1]);
    }
    h->weapons_mod = true;
    if (h->running && h->host.game) { // on the game as it plays, and told
        weapons_apply(&h->host.game->ctx.weapons, h->weapons);
        h->host.settings.weapons_mod = true;
        memcpy(h->host.settings.weapons, h->weapons, sizeof h->weapons);
        connections_send_weapons(&h->host.connections, h->host.game);
    }
}

// A weapon's line, as `weapon` reads it: every field of `stats`.
static void weapon_line(char *line, size_t size, const char *name, const WeaponStats *stats)
{
    int n = snprintf(line, size, "weapon \"%s\"", name);
    for (int k = 0; k < WEAPON_FIELD_COUNT && n < (int)size; k++) {
        const NetField *f = &WEAPON_FIELDS[k];
        const uint8_t *at = (const uint8_t *)stats + f->offset;
        if (f->kind == NET_F32) n += snprintf(line + n, size - (size_t)n, " %s %g", f->name, *(const float *)at);
        else n += snprintf(line + n, size - (size_t)n, " %s %d", f->name, *(const int32_t *)at);
    }
}

// weaponlist: every weapon a mod may change, as the lines that set it.
static void cmd_weaponlist(Console *con, int argc, char **argv, void *user)
{
    (void)argc, (void)argv;
    Hosted *h = user;
    Weapons names;
    weapons_default(&names);
    for (int id = 0; id < WEAPON_COUNT; id++) {
        if (!weapons_moddable((WeaponId)id)) continue;
        char line[CONSOLE_TEXT_SIZE];
        weapon_line(line, sizeof line, names.info[id].name, &h->weapons[id]);
        console_print(con, "%s\n", line);
    }
}

void hosted_init(Hosted *h, Console *con, HostCvars *cvars, Cvar *data)
{
    *h = (Hosted){.console = con, .cvars = cvars, .data = data};
    Weapons own; // the mod starts from the game's own numbers
    weapons_default(&own);
    weapons_stats(&own, h->weapons);

    console_add_command(con, "nextmap", cmd_nextmap, h, "end the round and begin the next");
    console_add_command(con, "kick", cmd_admin, h, "put a player off: kick <player> [reason]");
    console_add_command(con, "ban", cmd_admin, h, "put a player off and bar their address and machine: ban <player> [minutes] [reason]; no minutes for ever");
    console_add_command(con, "banip", cmd_admin, h, "bar an address: banip <address> [minutes] [reason]");
    console_add_command(con, "banhw", cmd_admin, h, "bar a machine: banhw <hardware ID> [minutes] [reason]");
    console_add_command(con, "unban", cmd_admin, h, "lift a ban: unban <address, hardware ID or name>");
    console_add_command(con, "servermute", cmd_admin, h, "their chat reaches nobody, until unmuted: servermute <player>");
    console_add_command(con, "serverunmute", cmd_admin, h, "serverunmute <player, address, hardware ID or name>");
    console_add_command(con, "bans", cmd_admin, h, "the ban list");
    console_add_command(con, "admins", cmd_admin, h, "the admins (config/admins.txt)");
    console_add_command(con, "weapon", cmd_weapon, h, "a weapons mod's line: weapon <name> <field> <value> [<field> <value>...]");
    console_add_command(con, "weaponlist", cmd_weaponlist, h, "every weapon's numbers, as the lines that set them");
    console_add_command(con, "addbot", cmd_addbot, h, "add a bot: addbot [name]");
    console_add_command(con, "addbot1", cmd_addbot, h, "add a bot to alpha: addbot1 [name]");
    console_add_command(con, "addbot2", cmd_addbot, h, "add a bot to bravo: addbot2 [name]");
    console_add_command(con, "pause", cmd_pause, h, "stop the game where it stands");
    console_add_command(con, "unpause", cmd_pause, h, "let it go on");
    console_add_command(con, "script_reload", cmd_script_reload, h, "read the script again, from the start");
    console_add_command(con, "lua", cmd_lua, h, "run a line of Lua in the script: lua <code>");
}

static void weapons_passed(void *user, const char *what)
{
    console_print(((Hosted *)user)->console, "%s: %s, passed over\n", CONFIG_WEAPONS, what);
}

void hosted_load_weapons(Hosted *h)
{
    // from the game's own numbers each time: a Local Play hosted again reads the file anew
    Weapons game;
    weapons_default(&game);
    weapons_stats(&game, h->weapons);
    h->weapons_mod = false;
    WeaponStats own[WEAPON_COUNT];
    memcpy(own, h->weapons, sizeof own);
    char name[64];
    if (!weapons_ini_read(CONFIG_WEAPONS, h->weapons, name, sizeof name, weapons_passed, h)) return;
    h->weapons_mod = memcmp(own, h->weapons, sizeof own) != 0;
    if (h->weapons_mod) console_print(h->console, "weapons mod %s\n", name[0] ? name : CONFIG_WEAPONS);
}

// What the cvars say the game is.
static HostSettings settings_from_cvars(const Hosted *h)
{
    const HostCvars *c = h->cvars;
    HostSettings s = {
        .port = (uint16_t)c->port->integer,
        .mode = c->gamemode->integer == 1 ? MATCH_DEATHMATCH : c->gamemode->integer == 2 ? MATCH_CTF : MATCH_MODE_COUNT,
        .time_limit = c->timelimit->integer,
        .score_limit = c->killlimit->integer,
        .bots_noteam = clampi(c->bots_noteam->integer, 0, MAX_PLAYERS),
        .bots_alpha = clampi(c->bots_alpha->integer, 0, MAX_PLAYERS),
        .bots_bravo = clampi(c->bots_bravo->integer, 0, MAX_PLAYERS),
        .bots_difficulty = c->bots_difficulty->integer,
        .bots_chat = c->bots_chat->integer != 0,
        .vote_percent = c->votepercent->integer,
        .flood_packets = c->floodingpackets->integer,
        .flood_warnings = c->warnings_flood->integer,
        .rope = c->rope->integer != 0,
    };
    snprintf(s.data, sizeof s.data, "%s", h->data->value);
    snprintf(s.ip, sizeof s.ip, "%s", c->ip->value);
    snprintf(s.map, sizeof s.map, "%s", c->map->value);
    // the rotation: sv_maps when it is set, else maplist.txt's
    if (c->maps->value[0]) snprintf(s.maps, sizeof s.maps, "%s", c->maps->value);
    else maplist_read(s.maps, sizeof s.maps);
    // the first map: `map` as given (the command line), else the rotation's first
    if (!strcmp(c->map->value, c->map->default_value) && s.maps[0]) rounds_next_map(s.maps, "", s.map, sizeof s.map);
    snprintf(s.hostname, sizeof s.hostname, "%s", c->hostname->value);
    snprintf(s.lists_dir, sizeof s.lists_dir, "%s", CONFIG_LISTS); // the bans, mutes and admins, kept
    s.weapons_mod = h->weapons_mod;
    memcpy(s.weapons, h->weapons, sizeof s.weapons);
    return s;
}

bool hosted_open(Hosted *h)
{
    if (h->running) return true;
    HostSettings settings = settings_from_cvars(h);
    if (!host_open(&h->host, h->console, &settings)) return false;
    h->running = true;
    script_start(h);
    lobby_init(&h->lobby, h->console);
    return true;
}

bool hosted_pump(Hosted *h, double dt, double now)
{
    if (!h->running) return false;
    if (!host_pump(&h->host, dt)) return false;
    script_pump(&h->script); // the answers to its requests
    cvar_set(h->console, "map", host_map(&h->host));
    LobbySettings lobby = {.public = h->cvars->public->integer != 0, .url = h->cvars->lobby_url->value,
                           .address = h->cvars->lobby_ip->value, .port = h->host.settings.port};
    lobby_pump(&h->lobby, &lobby, now);
    return true;
}

void hosted_close(Hosted *h)
{
    if (!h->running) return;
    lobby_close(&h->lobby);
    script_close(&h->script); // before the host it listens to
    host_close(&h->host);
    h->running = false;
}
