#pragma once

// What the player is doing, shown on their Discord profile: "Playing Soldat Reloaded",
// the game's icon, and a line or two of where. Said to the Discord app running on this
// machine over its local pipe (Windows' \\.\pipe\discord-ipc-N, elsewhere a Unix socket
// of that name in the runtime directory), as frames of JSON: a handshake with the game's
// application ID, then SET_ACTIVITY whenever what is shown changes. Nothing goes over
// the network from here, and nothing waits on Discord: with no app running the pipe
// isn't there, and it is looked for again every so often. The browser's Discord has no
// pipe, so it sees none of this. Discord clears what is shown when the pipe closes, as
// the game quits.
//
// The icon is the application's art asset named DISCORD_ICON, uploaded on Discord's
// Developer Portal under the application's Rich Presence, Art Assets.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISCORD_APP_ID "1555217040036339732"
#define DISCORD_ICON "logo"
#define DISCORD_TEXT 128 // a line shown, at most

typedef struct DiscordActivity {
    char details[DISCORD_TEXT]; // the first line, empty for none
    char state[DISCORD_TEXT];   // the second, empty for none
    int64_t since;              // the time it began, Unix seconds, for the "elapsed"; 0 for none
} DiscordActivity;

typedef struct Discord {
#ifdef _WIN32
    void *pipe; // a HANDLE; NULL while closed
#else
    int pipe; // -1 while closed
#endif
    bool ready;       // the handshake answered: activities are heard
    double next_try;  // when to look for the app again
    double last_sent; // when the last activity went, for Discord's rate limit
    DiscordActivity want; // what is to be shown
    bool dirty;           // and not yet said
    uint8_t in[8192];     // what has come back, a frame at a time
    size_t in_len;
    size_t skipping;      // the rest of a frame too big to keep, thrown away as it comes
} Discord;

void discord_init(Discord *d);
// What to show; said once it differs from what was, as soon as Discord will hear it.
void discord_set(Discord *d, const DiscordActivity *a);
// Each frame: connecting, the replies, and what is to be said. `enabled` false closes the
// pipe, and with it what is shown.
void discord_pump(Discord *d, double now, bool enabled);
void discord_close(Discord *d);
