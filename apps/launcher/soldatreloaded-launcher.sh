#!/bin/sh
# The launcher's name in the releases that had one apart from the game, which a player's shortcut may still
# start, and which that launcher brought first as its own when it updated: the game is
# soldatreloaded now, beside this, and keeps itself up to date as it starts.
cd "$(dirname "$0")" && exec ./soldatreloaded "$@"
