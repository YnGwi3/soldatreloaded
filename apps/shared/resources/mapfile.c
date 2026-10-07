#include "resources/mapfile.h"

#include <ctype.h>
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resources/map.h"
#include "utils/utils.h"

#define ENTRY_SIZE 512

static void lowercase(char *dst, const char *src, size_t size)
{
    size_t i = 0;
    for (; src[i] && i + 1 < size; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

// What an image's name is looked for as: the .png of it first, then as written, both
// lowercased (the names are compared lowercased).
typedef struct ImageWant {
    char as_png[256], as_written[256];
} ImageWant;

static ImageWant image_want(const char *name)
{
    ImageWant w;
    lowercase(w.as_written, name, sizeof w.as_written);
    snprintf(w.as_png, sizeof w.as_png, "%s", w.as_written);
    char *dot = strrchr(w.as_png, '.');
    if (dot && dot != w.as_png) *dot = '\0';
    strncat(w.as_png, ".png", sizeof w.as_png - strlen(w.as_png) - 1);
    return w;
}

static bool exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

bool mapfile_name_ok(const char *name)
{
    return name && name[0] && !strchr(name, '/') && !strchr(name, '\\') && !strstr(name, "..") && strlen(name) < MAPFILE_NAME;
}

int mapfile_find(const char *data_dir, const char *name, MapFile out[2])
{
    if (!mapfile_name_ok(name)) return 0;
    int n = 0;
    static const char *const EXTS[] = {".pms", MAPFILE_EXT};
    for (int k = 0; k < 2; k++) {
        char file[MAPFILE_NAME + 8];
        snprintf(file, sizeof file, "%s%s", name, EXTS[k]);
        MapFile f = {.packed = k == 1};
        path_join(f.path, sizeof f.path, data_dir, "maps", file);
        if (!exists(f.path)) continue;
        snprintf(f.data, sizeof f.data, "%s", data_dir);
        snprintf(f.name, sizeof f.name, "%s", name);
        out[n++] = f;
    }
    return n;
}

// --- the zip -----------------------------------------------------------------------

// The entry `dir`/`name` of a zip, in any case; with `image`, its .png before the name as
// written. -1 if there is none.
static int zip_entry(mz_zip_archive *zip, const char *dir, const char *name, bool image)
{
    ImageWant w = image_want(name);
    if (!image) snprintf(w.as_png, sizeof w.as_png, "%s", w.as_written);
    char prefix[64];
    lowercase(prefix, dir, sizeof prefix);
    strncat(prefix, "/", sizeof prefix - strlen(prefix) - 1);
    size_t plen = strlen(prefix);

    int fallback = -1;
    mz_uint count = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < count; i++) {
        char entry[ENTRY_SIZE], lower[ENTRY_SIZE];
        mz_zip_reader_get_filename(zip, i, entry, sizeof entry);
        for (char *c = entry; *c; c++)
            if (*c == '\\') *c = '/';
        lowercase(lower, entry, sizeof lower);
        const char *at = lower;
        if (strncmp(at, "./", 2) == 0) at += 2;
        if (strncmp(at, prefix, plen) != 0) continue;
        at += plen;
        if (strcmp(at, w.as_png) == 0) return (int)i;
        if (strcmp(at, w.as_written) == 0) fallback = (int)i;
    }
    return fallback;
}

static uint8_t *zip_take(mz_zip_archive *zip, int entry, size_t *size)
{
    if (entry < 0) return NULL;
    size_t n = 0;
    void *data = mz_zip_reader_extract_to_heap(zip, (mz_uint)entry, &n, 0);
    if (data && size) *size = n;
    return data;
}

// The .pms in a packed map: maps/<name>.pms, or failing that the one .pms it holds.
static int zip_map_entry(mz_zip_archive *zip, const char *name)
{
    char file[MAPFILE_NAME + 8];
    snprintf(file, sizeof file, "%s.pms", name);
    int entry = zip_entry(zip, "maps", file, false);
    if (entry >= 0) return entry;
    mz_uint count = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < count; i++) {
        char lower[ENTRY_SIZE];
        mz_zip_reader_get_filename(zip, i, lower, sizeof lower);
        lowercase(lower, lower, sizeof lower);
        size_t n = strlen(lower);
        if (n > 4 && strcmp(lower + n - 4, ".pms") == 0) return (int)i;
    }
    return -1;
}

uint8_t *mapfile_read(const MapFile *f, size_t *size)
{
    if (!f->packed) return file_read_all(f->path, size);
    mz_zip_archive zip = {0};
    if (!mz_zip_reader_init_file(&zip, f->path, 0)) return NULL;
    uint8_t *data = zip_take(&zip, zip_map_entry(&zip, f->name), size);
    mz_zip_reader_end(&zip);
    return data;
}

// --- the art -----------------------------------------------------------------------

typedef struct ImageSearch {
    ImageWant want;
    char found[256];    // the .png's name as it lies, when seen
    char fallback[256]; // the name as written, as it lies, when seen
} ImageSearch;

static bool image_visit(const char *name, void *user)
{
    ImageSearch *s = user;
    char actual[256];
    lowercase(actual, name, sizeof actual);
    if (strcmp(actual, s->want.as_png) == 0) {
        snprintf(s->found, sizeof s->found, "%s", name);
        return false;
    }
    if (strcmp(actual, s->want.as_written) == 0) snprintf(s->fallback, sizeof s->fallback, "%s", name);
    return true;
}

bool mapfile_find_image(const char *dir, const char *name, char *path, size_t path_size)
{
    if (!name || !name[0]) return false;
    ImageSearch s = {.want = image_want(name)};
    if (!for_each_file(dir, image_visit, &s)) return false;
    const char *best = s.found[0] ? s.found : s.fallback[0] ? s.fallback : NULL;
    if (!best) return false;
    snprintf(path, path_size, "%s/%s", dir, best);
    return true;
}

uint8_t *mapfile_art(const MapFile *f, const char *dir, const char *name, size_t *size)
{
    if (!f->path[0] || !name || !name[0]) return NULL;
    if (!f->packed) {
        char where[MAPFILE_PATH], path[MAPFILE_PATH + 256];
        path_join(where, sizeof where, f->data, dir, NULL);
        return mapfile_find_image(where, name, path, sizeof path) ? file_read_all(path, size) : NULL;
    }
    mz_zip_archive zip = {0};
    if (!mz_zip_reader_init_file(&zip, f->path, 0)) return NULL;
    uint8_t *data = zip_take(&zip, zip_entry(&zip, dir, name, true), size);
    mz_zip_reader_end(&zip);
    return data;
}

// --- the list and the pack ---------------------------------------------------------

static int name_compare(const void *a, const void *b) { return strcmp(a, b); }

static bool same_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

bool mapfile_exists(const char *maps_dir, const char *name)
{
    if (!mapfile_name_ok(name)) return false;
    char path[MAPFILE_PATH + MAPFILE_NAME + 8];
    snprintf(path, sizeof path, "%s/%s.pms", maps_dir, name);
    if (exists(path)) return true;
    snprintf(path, sizeof path, "%s/%s%s", maps_dir, name, MAPFILE_EXT);
    return exists(path);
}

int mapfile_list(const char *maps_dir, char (*names)[MAPFILE_NAME], int max)
{
    const char *dir = maps_dir;
    int n = list_files(dir, ".pms", names, max);
    char (*packed)[MAPFILE_NAME] = malloc((size_t)max * MAPFILE_NAME);
    if (!packed) return n;
    int p = list_files(dir, MAPFILE_EXT, packed, max);
    for (int i = 0; i < p && n < max; i++) {
        bool seen = false;
        for (int j = 0; j < n && !seen; j++) seen = strcmp(names[j], packed[i]) == 0;
        if (!seen) snprintf(names[n++], MAPFILE_NAME, "%s", packed[i]);
    }
    free(packed);
    qsort(names, (size_t)n, MAPFILE_NAME, name_compare);
    return n;
}

// The image `name` from the data folder's `dir`, into the zip as `dir`/<its name>, if
// there is one there.
static void pack_art(mz_zip_archive *zip, const MapFile *f, const char *dir, const char *name)
{
    char where[MAPFILE_PATH], path[MAPFILE_PATH + 256];
    path_join(where, sizeof where, f->data, dir, NULL);
    if (!mapfile_find_image(where, name, path, sizeof path)) return;
    char entry[ENTRY_SIZE];
    snprintf(entry, sizeof entry, "%s/%s", dir, path + strlen(where) + 1);
    size_t size = 0;
    uint8_t *data = file_read_all(path, &size);
    if (data) mz_zip_writer_add_mem(zip, entry, data, size, MZ_BEST_SPEED);
    free(data);
}

uint8_t *mapfile_pack(const MapFile *f, const struct Map *map, size_t *size)
{
    if (f->packed) return file_read_all(f->path, size);
    size_t pms_size = 0;
    uint8_t *pms = file_read_all(f->path, &pms_size);
    if (!pms) return NULL;

    mz_zip_archive zip = {0};
    void *out = NULL;
    size_t out_size = 0;
    if (mz_zip_writer_init_heap(&zip, 0, 0)) {
        char entry[MAPFILE_NAME + 16];
        snprintf(entry, sizeof entry, "maps/%s.pms", f->name);
        bool ok = mz_zip_writer_add_mem(&zip, entry, pms, pms_size, MZ_BEST_SPEED);
        if (ok && map) {
            pack_art(&zip, f, "textures", map->texture);
            for (int i = 0; i < map->scenery_count; i++) {
                bool again = false; // a scenery named twice goes in once
                for (int j = 0; j < i && !again; j++) again = same_nocase(map->scenery[j], map->scenery[i]);
                if (!again) pack_art(&zip, f, "scenery-gfx", map->scenery[i]);
            }
        }
        if (!ok || !mz_zip_writer_finalize_heap_archive(&zip, &out, &out_size)) out = NULL;
        mz_zip_writer_end(&zip);
    }
    free(pms);
    if (out && size) *size = out_size;
    return out;
}
