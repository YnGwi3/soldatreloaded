#pragma once

// The game's updater (updater.c), run as the game starts, before anything of it: the
// install brought up to the latest release in a small window of its own, as the launcher
// that came before it in the release did. One executable is the game and its updater, so
// starting the game is what keeps it up to date.

#include <stdbool.h>

// The update, its own arguments (--no-update, --update-only, --verify, --releases) taken
// out of argv and argc, the rest left for the game. True for the game to go on, in the
// install's directory; false for it to end with `*status` (quit, --update-only, or the
// game's new executable started in its place).
bool updater_run(int *argc, char **argv, int *status);
