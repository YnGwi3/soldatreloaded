# scenery-gfx/

A custom map's scenery images, for a map whose `.pms` lies loose in `maps/`. A map's
scenery is looked for in the player's mod, then here, then in `mods/default/scenery-gfx/`.

A map packed as a `.smap` (in `maps/`) carries its own scenery inside it and needs nothing
here. A server sends the maps it plays to the players who lack them, packed with what it
finds of their scenery here.
