// A player's own mutes (client/ui/mutes.h): whose chat is kept off the screen and whose
// taunts come through, the list kept past a rejoin, and the taunt said on the wire.

#include <stdio.h>
#include <string.h>

#include "files.h" // the launcher's
#include "test.h"
#include "ui/mutes.h"

#define MUTES_FILE "build/test-mutes.txt"

static bool hide(const Mutes *m, MuteKinds k, const char *name, Team team, bool taunt)
{
    return mutes_hide(m, k, name, team, TEAM_ALPHA, true, taunt);
}

void mutes_tests(void)
{
    Mutes m = {0};
    MuteKinds none = {0};
    CHECK(mutes_add(&m, "Crow") && !mutes_add(&m, "crow") && mutes_has(&m, "CROW"), "a player is muted once, by name in any case");
    CHECK(hide(&m, none, "Crow", TEAM_BRAVO, false) && !hide(&m, none, "Crow", TEAM_BRAVO, true),
          "their typed chat is kept off, their taunts come through");
    CHECK(!hide(&m, none, "Mabuse", TEAM_BRAVO, false), "and nobody else's is touched");

    CHECK(hide(&m, (MuteKinds){.all = true}, "Mabuse", TEAM_ALPHA, false) && !hide(&m, (MuteKinds){.all = true}, "Mabuse", TEAM_ALPHA, true),
          "muteall keeps off everyone's chat but taunts");
    MuteKinds team = {.team = true}, enemies = {.enemies = true}, specs = {.specs = true};
    CHECK(hide(&m, team, "Mate", TEAM_ALPHA, false) && !hide(&m, team, "Foe", TEAM_BRAVO, false) && !hide(&m, team, "Mate", TEAM_ALPHA, true),
          "muteteam keeps off my team's chat alone, not its taunts");
    CHECK(hide(&m, enemies, "Foe", TEAM_BRAVO, false) && !hide(&m, enemies, "Mate", TEAM_ALPHA, false) &&
              !hide(&m, enemies, "Spec", TEAM_SPECTATOR, false),
          "muteenemies keeps off the enemies' chat alone");
    CHECK(mutes_hide(&m, enemies, "Anyone", TEAM_NONE, TEAM_NONE, false, false), "and with no teams, everyone else is an enemy");
    CHECK(hide(&m, specs, "Spec", TEAM_SPECTATOR, false) && hide(&m, specs, "Spec", TEAM_SPECTATOR, true) &&
              !hide(&m, specs, "Foe", TEAM_BRAVO, false),
          "mutespecs keeps off the spectators' chat, taunts too");

    // kept past a rejoin: written and read back
    mutes_add(&m, "Major Tom");
    CHECK(mutes_save(&m, MUTES_FILE), "the mutes are written");
    Mutes back = {0};
    mutes_load(&back, MUTES_FILE);
    CHECK(back.count == 2 && mutes_has(&back, "Crow") && mutes_has(&back, "major tom"), "and read back, a name with a space whole (%d)",
          back.count);
    CHECK(mutes_remove(&back, "crow") && !mutes_has(&back, "Crow") && !mutes_remove(&back, "Crow"), "unmute lets a player back once");
    files_remove_tree(MUTES_FILE);
    Mutes empty = {.count = 5};
    mutes_load(&empty, MUTES_FILE);
    CHECK(empty.count == 0, "no file is no mutes");

    // a taunt says it is one, on the wire
    uint8_t buf[NET_MTU];
    NetBuf w = netbuf_writer(buf, sizeof buf);
    MsgChat said = {.slot = 3, .team = true, .taunt = true};
    snprintf(said.text, sizeof said.text, "Cover me!");
    msg_chat(&w, &said);
    NetBuf r = netbuf_reader(buf, netbuf_bytes(&w));
    MsgChat heard = {0};
    msg_chat(&r, &heard);
    CHECK(netbuf_done(&r) && heard.taunt && heard.team && strcmp(heard.text, "Cover me!") == 0, "a taunt is heard as a taunt");
}
