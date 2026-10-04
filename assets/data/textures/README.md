# textures/

A custom map's polygon textures, for a map whose `.pms` lies loose in `maps/`. A map's
texture is looked for in the player's mod, then here, then in `mods/default/textures/`.

A map packed as a `.smap` (in `maps/`) carries its own textures inside it and needs
nothing here. A server sends the maps it plays to the players who lack them, packed with
what it finds of their textures here.
