#!/bin/sh
# The launcher's name in the releases that had one apart from the game, which a player's
# shortcut may still start: the game is soldatreloaded now, beside this, and keeps itself
# up to date as it starts.
#
# The old launcher updates itself first and alone, into this, and moves itself aside to
# tmp; the rest of that update is the old launcher's to finish, which it does run again
# by that name (it isn't its own name in the release, so it brings everything), and
# then the game is here.
cd "$(dirname "$0")" || exit 1
if [ ! -x ./soldatreloaded ] && [ -x ./tmp ]; then
    ./tmp --update-only "$@" # its own flags among them (--releases, --verify); the game's it passes over
fi
exec ./soldatreloaded "$@"
