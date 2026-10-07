#pragma once

// Where a map lies under the data folder, and what lies with it. A map is either
//
//   loose    data/maps/<name>.pms, its art in data/textures/ and data/scenery-gfx/ (or,
//            as the game's own maps', in the mods'), or
//   packed   data/maps/<name>.smap, a zip of maps/<name>.pms with its own textures/ and
//            scenery-gfx/ beside it: OpenSoldat's custom maps, as they are passed around,
//            and what a server sends a player who lacks its map.
//
// A map's art is found as the original's names find it: in any case, a .png before the
// name as written (its .bmp).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAPFILE_PATH 512
#define MAPFILE_NAME 64
#define MAPFILE_EXT ".smap"

typedef struct MapFile {
    char path[MAPFILE_PATH]; // the .pms, or the .smap; empty for none
    char data[MAPFILE_PATH]; // the data folder it lies in
    char name[MAPFILE_NAME];
    bool packed;
} MapFile;

// Whether `name` can be a map's: not empty, and nothing that reaches outside maps/.
bool mapfile_name_ok(const char *name);

// The map `name` in `data_dir`/maps: the loose .pms, then the .smap, as many as there are
// (0 to 2) into `out`, in that order.
int mapfile_find(const char *data_dir, const char *name, MapFile out[2]);

// The .pms's bytes, to free; NULL if it can't be read.
uint8_t *mapfile_read(const MapFile *f, size_t *size);

// An image of the map's own in its `dir` ("textures", "scenery-gfx"), found as above, to
// free: a packed map's from its zip, a loose map's from the data folder's `dir`. NULL if it
// has none of that name.
uint8_t *mapfile_art(const MapFile *f, const char *dir, const char *name, size_t *size);

// The image `name` in the directory `dir` on disk, found as above, into `path`.
bool mapfile_find_image(const char *dir, const char *name, char *path, size_t path_size);

// Whether the map `name` lies in `maps_dir` (a data folder's maps/), loose or packed.
bool mapfile_exists(const char *maps_dir, const char *name);

// The maps in `maps_dir`, loose and packed, each once by name, sorted; how many.
int mapfile_list(const char *maps_dir, char (*names)[MAPFILE_NAME], int max);

// A packed map made of a loose one (a packed one's file as it is): the zip a server sends,
// maps/<name>.pms and what the data folder has of `map`'s texture and scenery. To free;
// NULL on failure.
struct Map;
uint8_t *mapfile_pack(const MapFile *f, const struct Map *map, size_t *size);
