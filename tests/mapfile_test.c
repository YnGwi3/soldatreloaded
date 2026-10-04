// Maps loose and packed (resources/mapfile.h): a .smap made of a loose map and read back,
// a packed map's art found as the original's names find it, and the list of both.

#include <miniz.h>
#include <stdlib.h>
#include <string.h>

#include "files.h" // the launcher's
#include "host.h"
#include "net/client_net.h"
#include "test.h"

#define SCRATCH "build/test-mapfile"
#define FETCH_PORT 40061

static bool write_file(const char *path, const void *data, size_t size)
{
    if (!files_make_parents(path)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static void packed_map(void)
{
    files_remove_tree(SCRATCH); // what a run cut short left
    // ctf_Ash packed, as a server sends it, into a data folder of its own
    MapFile loose[2];
    CHECK(mapfile_find(TEST_DATA, "ctf_Ash", loose) == 1 && !loose[0].packed, "ctf_Ash lies loose in the game's data");
    Map from_disk = {0};
    CHECK(map_load_from(&from_disk, &loose[0]), "and loads");
    size_t size = 0;
    uint8_t *zip = mapfile_pack(&loose[0], &from_disk, &size);
    CHECK(zip && size > 0, "it packs (%zu bytes)", size);
    CHECK(zip && write_file(SCRATCH "/maps/ctf_Ash.smap", zip, size), "and is written as a .smap");
    free(zip);

    MapFile packed[2];
    CHECK(mapfile_find(SCRATCH, "ctf_Ash", packed) == 1 && packed[0].packed, "the .smap is found by the map's name");
    Map from_zip = {0};
    CHECK(map_load_from(&from_zip, &packed[0]), "and loads from inside the zip");
    CHECK(from_zip.poly_count == from_disk.poly_count && from_zip.scenery_count == from_disk.scenery_count &&
              strcmp(from_zip.texture, from_disk.texture) == 0 && from_zip.file.packed,
          "the same map as the loose one (%d polys, texture %s)", from_zip.poly_count, from_zip.texture);
    map_destroy(&from_zip);
    map_destroy(&from_disk);

    // the art in a packed map: any case, the .png before the name as written
    mz_zip_archive w = {0};
    void *out = NULL;
    size_t out_size = 0;
    mz_zip_writer_init_heap(&w, 0, 0);
    size_t pms_size = 0;
    uint8_t *pms = mapfile_read(&loose[0], &pms_size);
    mz_zip_writer_add_mem(&w, "maps/Custom.pms", pms, pms_size, 0);
    mz_zip_writer_add_mem(&w, "textures/Rock.BMP", "bmp", 3, 0);
    mz_zip_writer_add_mem(&w, "textures/rock.png", "png", 3, 0);
    mz_zip_writer_add_mem(&w, "scenery-gfx/Tree.bmp", "tree", 4, 0);
    mz_zip_writer_finalize_heap_archive(&w, &out, &out_size);
    mz_zip_writer_end(&w);
    free(pms);
    CHECK(out && write_file(SCRATCH "/maps/custom.smap", out, out_size), "a packed map of its own art is written");
    free(out);

    MapFile custom[2];
    CHECK(mapfile_find(SCRATCH, "custom", custom) == 1, "and found");
    size_t n = 0;
    uint8_t *art = mapfile_art(&custom[0], "textures", "rock.bmp", &n);
    CHECK(art && n == 3 && memcmp(art, "png", 3) == 0, "its texture is the .png, asked for as a .bmp in another case");
    free(art);
    art = mapfile_art(&custom[0], "scenery-gfx", "TREE.BMP", &n);
    CHECK(art && n == 4, "its scenery is found as written, in any case");
    free(art);
    CHECK(!mapfile_art(&custom[0], "scenery-gfx", "bush.bmp", &n), "and what it lacks is not");
    pms = mapfile_read(&custom[0], &n);
    CHECK(pms && n == pms_size, "its .pms is read though its name is the map's in another case");
    free(pms);

    // listed once each, loose or packed
    write_file(SCRATCH "/maps/ctf_Ash.pms", "x", 1);
    char names[8][MAPFILE_NAME];
    int count = mapfile_list(SCRATCH "/maps", names, 8);
    CHECK(count == 2 && strcmp(names[0], "ctf_Ash") == 0 && strcmp(names[1], "custom") == 0,
          "the maps are listed once each, loose and packed (%d)", count);
    CHECK(mapfile_exists(SCRATCH "/maps", "custom") && !mapfile_exists(SCRATCH "/maps", "../custom"),
          "a packed map is there, and a name reaching outside is not");
    files_remove_tree(SCRATCH);
}

// A client joining a server playing a map it lacks, and one whose copy of the map is
// another version, is sent the server's, packed, and makes its world of it.
static void map_fetched(void)
{
    CHECK(net_init(), "ENet starts");
    static Host host;
    HostSettings settings = {.port = FETCH_PORT, .mode = MATCH_DEATHMATCH, .hostname = "fetch test", .quiet = true};
    snprintf(settings.data, sizeof settings.data, "%s", TEST_DATA);
    snprintf(settings.map, sizeof settings.map, "Arena");
    if (!host_open(&host, NULL, &settings)) {
        CHECK(false, "a host on port %d", FETCH_PORT);
        return;
    }
    // the client's data folder: no Arena at all, then an Arena that isn't the server's
    for (int pass = 0; pass < 2; pass++) {
        files_remove_tree(SCRATCH);
        if (pass == 1) write_file(SCRATCH "/maps/Arena.pms", "not the server's Arena", 22);
        else files_make_parents(SCRATCH "/maps/x"); // maps/ itself, empty
        Console *con = console_create(NULL, NULL);
        static ClientNet client;
        client_net_init(&client);
        snprintf(client.data_dir, sizeof client.data_dir, "%s", SCRATCH);
        client_net_connect(&client, con, "127.0.0.1", FETCH_PORT, "Fetcher", "");
        bool fetching = false;
        for (int i = 0; i < 2000 && !client.mapped; i++) {
            host_pump(&host, TICK_SECONDS);
            client_net_poll(&client, con, NULL);
            fetching |= client.fetch.on;
        }
        CHECK(fetching && client.mapped && client.map_file.packed && client.state == CLIENT_NET_JOINED,
              "%s, the client is sent the server's map and keeps its line (%s)", pass ? "with another Arena here" : "with no Arena here",
              client.map_file.path);
        Context ctx = {0};
        bool made = client.mapped && context_load_from(&ctx, TEST_DATA, &client.map_file);
        CHECK(made && ctx.map->poly_count == host.game->ctx.map->poly_count, "and its world is made of it: the server's Arena (%d polys)",
              made ? ctx.map->poly_count : 0);
        if (made) context_destroy(&ctx);
        client_net_disconnect(&client, con);
        client_stream_free(&client.stream);
        console_destroy(con);
    }
    files_remove_tree(SCRATCH);
    host_close(&host);
    net_shutdown();
}

void mapfile_tests(void)
{
    packed_map();
    map_fetched();
}
