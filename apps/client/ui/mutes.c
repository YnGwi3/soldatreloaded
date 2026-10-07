#include "ui/mutes.h"

#include "utils/utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

void mutes_load(Mutes *m, const char *path)
{
    m->count = 0;
    size_t size = 0;
    char *text = (char *)file_read_all(path, &size);
    if (!text) return;
    char *cursor = text, *line;
    while ((line = text_next_line(&cursor)) != NULL) {
        if (!line[0] || strncmp(line, "//", 2) == 0) continue;
        mutes_add(m, line);
    }
    free(text);
}

bool mutes_save(const Mutes *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "// The players I have muted (/mute), a name to a line: their chat doesn't reach my\n"
               "// screen, but for their taunts and radio calls, until /unmute. The game writes this\n"
               "// as players are muted and unmuted.\n");
    for (int i = 0; i < m->count; i++) fprintf(f, "%s\n", m->names[i]);
    return fclose(f) == 0;
}

bool mutes_has(const Mutes *m, const char *name)
{
    for (int i = 0; i < m->count; i++)
        if (same_name(m->names[i], name)) return true;
    return false;
}

bool mutes_add(Mutes *m, const char *name)
{
    if (!name[0] || mutes_has(m, name) || m->count == MUTES_MAX) return false;
    snprintf(m->names[m->count++], NET_NAME_SIZE, "%s", name);
    return true;
}

bool mutes_remove(Mutes *m, const char *name)
{
    for (int i = 0; i < m->count; i++) {
        if (!same_name(m->names[i], name)) continue;
        memmove(m->names + i, m->names + i + 1, (size_t)(m->count - i - 1) * sizeof m->names[0]);
        m->count--;
        return true;
    }
    return false;
}

bool mutes_hide(const Mutes *m, MuteKinds kinds, const char *name, Team team, Team mine, bool team_game, bool taunt)
{
    if (kinds.specs && team == TEAM_SPECTATOR) return true; // a spectator's all, binds too
    if (taunt) return false;
    if (kinds.all) return true;
    if (team != TEAM_SPECTATOR) {
        bool mate = team_game && team == mine;
        if (mate ? kinds.team : kinds.enemies) return true;
    }
    return mutes_has(m, name);
}
