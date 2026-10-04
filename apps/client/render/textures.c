#include "render/textures.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_MIPMAP_BIAS -0.5f // the original's r_mipmapbias default: the texture a touch sharper

static void lowercase(char *dst, const char *src, size_t size)
{
    size_t i = 0;
    for (; src[i] && i + 1 < size; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

typedef struct ImageSearch {
    char as_png[256], lower[256]; // what is wanted, lowercased: the .png first, then as written
    char found[256];              // the .png's actual name, when seen
    char fallback[256];           // the name as written, when seen
} ImageSearch;

static bool image_visit(const char *name, void *user)
{
    ImageSearch *s = user;
    char actual[256];
    lowercase(actual, name, sizeof(actual));
    if (strcmp(actual, s->as_png) == 0) {
        snprintf(s->found, sizeof(s->found), "%s", name);
        return false;
    }
    if (strcmp(actual, s->lower) == 0) snprintf(s->fallback, sizeof(s->fallback), "%s", name);
    return true;
}

bool find_image(const char *dir, const char *name, char *path, int path_size)
{
    if (!name || !name[0]) return false;

    ImageSearch s = {0};
    lowercase(s.lower, name, sizeof(s.lower));
    snprintf(s.as_png, sizeof(s.as_png), "%s", s.lower);
    char *dot = strrchr(s.as_png, '.');
    if (dot && dot != s.as_png) *dot = '\0';
    strncat(s.as_png, ".png", sizeof(s.as_png) - strlen(s.as_png) - 1);

    if (!for_each_file(dir, image_visit, &s)) return false;
    const char *best = s.found[0] ? s.found : s.fallback[0] ? s.fallback : NULL;
    if (!best) return false;
    snprintf(path, (size_t)path_size, "%s/%s", dir, best);
    return true;
}

// A map's image `name` in `dir`: the player's mod's, then the map's own (in its .smap, or
// for a loose map in the data folder's `dir`), then the default's. False if none loads.
static bool map_image_load(GfxTexture *tex, const Mod *mod, const Map *map, const char *dir, const char *name, const Rgba *key)
{
    char where[512], path[512];
    *tex = (GfxTexture){0};
    if (mod->dir[0]) {
        snprintf(where, sizeof where, "%s/%s", mod->dir, dir);
        if (find_image(where, name, path, sizeof path)) return gfx_texture_load(tex, path, key);
    }
    size_t size = 0;
    uint8_t *own = mapfile_art(&map->file, dir, name, &size);
    if (own) {
        bool ok = gfx_texture_load_memory(tex, own, size, key);
        free(own);
        if (ok) return true;
    }
    snprintf(where, sizeof where, "%s/%s", mod->fallback, dir);
    return find_image(where, name, path, sizeof path) && gfx_texture_load(tex, path, key);
}

GfxTexture map_texture_load(const Mod *mod, const Map *map)
{
    GfxTexture tex;
    if (!map_image_load(&tex, mod, map, "textures", map->texture, NULL)) {
        fprintf(stderr, "map texture '%s' not found; drawing the polygons untextured\n", map->texture);
        return tex;
    }
    gfx_texture_wrap(tex, true);
    gfx_texture_mipmap(tex);
    gfx_texture_lod_bias(tex, MAP_MIPMAP_BIAS);
    return tex;
}

GfxTexture *scenery_load(const Mod *mod, const Map *map)
{
    GfxTexture *out = calloc((size_t)(map->scenery_count ? map->scenery_count : 1), sizeof(GfxTexture));
    if (!out) return NULL;

    // Keyed on pure green rather than an alpha channel, as the original's ApplyColorKey.
    const Rgba green = {0, 255, 0, 255};
    int missing = 0;
    for (int i = 0; i < map->scenery_count; i++)
        if (!map_image_load(&out[i], mod, map, "scenery-gfx", map->scenery[i], &green)) missing++;
    if (missing > 0) fprintf(stderr, "%d of %d scenery images not found in scenery-gfx\n", missing, map->scenery_count);
    return out;
}

void scenery_unload(GfxTexture *scenery, int count)
{
    if (!scenery) return;
    for (int i = 0; i < count; i++) gfx_texture_delete(&scenery[i]);
    free(scenery);
}
