#include "net/client_net.h"
#include "net/hwid.h"
#include "ui/hud_data.h" // the colours of the line's word

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/systems/systems.h"
#include "sha256.h" // the launcher's, for a map's hash

static void fetch_stop(ClientNet *n);

bool client_net_init(ClientNet *n)
{
    *n = (ClientNet){.slot = -1};
    return net_init() && client_stream_init(&n->stream);
}

void client_net_shutdown(ClientNet *n)
{
    net_close(&n->link);
    client_stream_free(&n->stream);
    net_shutdown();
}

void client_net_connect(ClientNet *n, Console *con, const char *address, uint16_t port, const char *name,
                        const char *password)
{
    if (n->link.host) client_net_disconnect(n, con);
    snprintf(n->name, sizeof n->name, "%s", name);
    snprintf(n->password, sizeof n->password, "%s", password ? password : "");
    if (!net_connect(&n->link, address, port)) {
        console_print(con, "couldn't connect to %s:%u\n", address, port);
        return;
    }
    n->state = CLIENT_NET_CONNECTING;
    n->slot = -1;
    n->had_map = false;
    fetch_stop(n);
    n->map_changing = n->map_replied = false;
    n->weapons_heard = false; // a new server says its own
    n->vote = (MsgVote){.kind = VOTE_NONE};
    snprintf(n->address, sizeof n->address, "%s", address);
    n->port = port;
    console_print(con, "connecting to %s:%u...\n", address, port);
}

void client_net_disconnect(ClientNet *n, Console *con)
{
    if (n->playback) {
        n->playback = false;
        n->state = CLIENT_NET_OFF;
        n->slot = -1;
        n->vote.kind = VOTE_NONE;
        return;
    }
    if (!n->link.host) return;
    net_close(&n->link);
    n->state = CLIENT_NET_OFF;
    n->slot = -1;
    n->vote.kind = VOTE_NONE;
    fetch_stop(n);
    console_print(con, "disconnected\n");
}

// --- the round's map: here, or fetched ---------------------------------------------

#define FETCH_AHEAD 64 // parts asked for ahead of the one awaited: some 64 KB in flight
#define FETCH_STEP 32  // asked for again once this many of them have come

static void fetch_stop(ClientNet *n)
{
    free(n->fetch.data);
    memset(&n->fetch, 0, sizeof n->fetch);
}

static bool hash_any(const uint8_t hash[NET_MAP_HASH])
{
    for (int i = 0; i < NET_MAP_HASH; i++)
        if (hash[i]) return false;
    return true;
}

// Whether the map in `f` is the one the server's hash names.
static bool map_matches(const MapFile *f, const uint8_t hash[NET_MAP_HASH])
{
    if (hash_any(hash)) return true;
    size_t size = 0;
    uint8_t *pms = mapfile_read(f, &size);
    if (!pms) return false;
    uint8_t mine[32];
    Sha256 s;
    sha256_init(&s);
    sha256_feed(&s, pms, size);
    sha256_finish(&s, mine);
    free(pms);
    return memcmp(mine, hash, sizeof mine) == 0;
}

// The map the server named, as it has it, among this data folder's: loose, then packed.
static bool map_here(ClientNet *n, const char *name, const uint8_t hash[NET_MAP_HASH])
{
    MapFile found[2];
    int count = mapfile_find(n->data_dir, name, found);
    for (int i = 0; i < count; i++) {
        if (!map_matches(&found[i], hash)) continue;
        n->map_file = found[i];
        return true;
    }
    return false;
}

static void route_map_fetch(NetBuf *b, void *m) { msg_map_fetch(b, m); }

static void fetch_ask(ClientNet *n, uint32_t count)
{
    MsgMapFetch f = {.round = n->fetch.round, .part = n->fetch.asked, .count = count};
    uint8_t buf[NET_MTU];
    NetBuf b = netbuf_writer(buf, sizeof buf);
    MsgKind kind = MSG_MAP_FETCH;
    msg_kind(&b, &kind);
    route_map_fetch(&b, &f);
    if (!netbuf_ok(&b)) return;
    net_send(n->link.peer, MSG_MAP_FETCH, buf, netbuf_bytes(&b));
    n->fetch.asked += count;
}

static void fetch_start(ClientNet *n, Console *con, const MsgMap *m)
{
    fetch_stop(n);
    n->fetch.on = true;
    n->fetch.round = m->round;
    snprintf(n->fetch.name, sizeof n->fetch.name, "%s", m->map);
    memcpy(n->fetch.hash, m->hash, sizeof n->fetch.hash);
    console_print_color(con, HUD_COLOR_CLIENT, "Downloading map %s...\n", m->map);
    fetch_ask(n, FETCH_AHEAD);
}

static void fetch_fail(ClientNet *n, Console *con, const char *why)
{
    console_print_color(con, HUD_COLOR_WARNING, "Couldn't download map %s: %s\n", n->fetch.name, why);
    fetch_stop(n);
    client_net_disconnect(n, con);
}

// The packed map whole: written beside the others as <name>.smap, once its .pms is the
// one the server named, and the world made of it.
static void fetch_done(ClientNet *n, Console *con)
{
    char maps[MAPFILE_PATH], part[MAPFILE_PATH + NET_MAP_SIZE + 16], final[MAPFILE_PATH + NET_MAP_SIZE + 16];
    path_join(maps, sizeof maps, n->data_dir, "maps", NULL);
    snprintf(part, sizeof part, "%s/%s%s.part", maps, n->fetch.name, MAPFILE_EXT);
    snprintf(final, sizeof final, "%s/%s%s", maps, n->fetch.name, MAPFILE_EXT);
    FILE *f = fopen(part, "wb");
    bool written = f && fwrite(n->fetch.data, 1, n->fetch.total, f) == n->fetch.total;
    if (f && fclose(f) != 0) written = false;
    if (!written) {
        remove(part);
        fetch_fail(n, con, "it couldn't be written into data/maps/");
        return;
    }
    MapFile got = {.packed = true};
    snprintf(got.path, sizeof got.path, "%s", part);
    snprintf(got.data, sizeof got.data, "%s", n->data_dir);
    snprintf(got.name, sizeof got.name, "%s", n->fetch.name);
    if (!map_matches(&got, n->fetch.hash)) {
        remove(part);
        fetch_fail(n, con, "what came isn't the server's map");
        return;
    }
    remove(final); // a .smap of that name, another version of the map
    if (rename(part, final) != 0) {
        remove(part);
        fetch_fail(n, con, "it couldn't be put in data/maps/");
        return;
    }
    snprintf(got.path, sizeof got.path, "%s", final);
    n->map_file = got;
    console_print_color(con, HUD_COLOR_CLIENT, "Downloaded map %s (%u KB)\n", n->fetch.name, (unsigned)((n->fetch.total + 1023) / 1024));
    fetch_stop(n);
    n->mapped = true; // the world is made of it now
}

static void fetch_part(ClientNet *n, Console *con, const MsgMapPart *p)
{
    if (!n->fetch.on || p->round != n->fetch.round || p->part != n->fetch.next) return;
    if (!n->fetch.data) {
        if (p->total == 0 || p->total > NET_MAP_MAX) {
            fetch_fail(n, con, "the server's map is too large");
            return;
        }
        n->fetch.data = malloc(p->total);
        if (!n->fetch.data) {
            fetch_fail(n, con, "out of memory");
            return;
        }
        n->fetch.total = p->total;
    }
    size_t at = (size_t)p->part * NET_MAP_PART;
    if (p->total != n->fetch.total || at + p->size > n->fetch.total) return;
    memcpy(n->fetch.data + at, p->data, p->size);
    n->fetch.next++;
    size_t got = at + p->size;
    int quarter = (int)(got * 4 / n->fetch.total);
    if (quarter > n->fetch.told && quarter < 4) {
        n->fetch.told = quarter;
        console_print_color(con, HUD_COLOR_CLIENT, "Downloading map %s: %d%%\n", n->fetch.name, quarter * 25);
    }
    if (got == n->fetch.total) {
        fetch_done(n, con);
        return;
    }
    uint32_t parts = (n->fetch.total + NET_MAP_PART - 1) / NET_MAP_PART;
    if (n->fetch.asked < parts && n->fetch.asked - n->fetch.next <= FETCH_AHEAD - FETCH_STEP) fetch_ask(n, FETCH_STEP);
}

static void send_hello(ClientNet *n)
{
    uint8_t buf[NET_MTU];
    NetBuf b = netbuf_writer(buf, sizeof buf);
    MsgKind kind = MSG_HELLO;
    MsgHello m = {.version = NET_VERSION};
    snprintf(m.name, sizeof m.name, "%s", n->name);
    snprintf(m.password, sizeof m.password, "%s", n->password);
    m.look = n->look;
    m.gear = n->gear;
    m.primary = n->primary;
    m.secondary = n->secondary;
    hwid_get(m.hwid); // this machine's, for the server's bans and mutes
    msg_kind(&b, &kind);
    msg_hello(&b, &m);
    if (netbuf_ok(&b)) net_send(n->link.peer, MSG_HELLO, buf, netbuf_bytes(&b));
    n->state = CLIENT_NET_JOINING;
}

static void heard(ClientNet *n, Console *con, Game *g, const uint8_t *data, size_t size)
{
    NetBuf b = netbuf_reader(data, size);
    MsgKind kind;
    msg_kind(&b, &kind);
    if (!netbuf_ok(&b)) return;
    switch (kind) {
    case MSG_WELCOME: {
        MsgWelcome m = {0};
        msg_welcome(&b, &m);
        if (!netbuf_done(&b)) return;
        n->slot = m.slot;
        n->tick = m.tick;
        n->state = CLIENT_NET_JOINED;
        console_print_color(con, HUD_COLOR_CLIENT, "Connection accepted to %s:%u\n", n->address, n->port);
        break;
    }
    case MSG_MAP: {
        MsgMap m = {0};
        msg_map(&b, &m);
        if (!netbuf_done(&b)) return;
        n->round = m.round;
        snprintf(n->map, sizeof n->map, "%s", m.map);
        snprintf(n->hostname, sizeof n->hostname, "%s", m.hostname);
        n->rope = m.rope;
        client_stream_reset(&n->stream, m.round);
        n->had_map = true;
        // the world is made of the map here, or of the server's once it has come; a demo
        // plays on whatever copy of its map is here
        fetch_stop(n);
        memset(&n->map_file, 0, sizeof n->map_file);
        if (map_here(n, m.map, m.hash) || n->playback) n->mapped = true;
        else fetch_start(n, con, &m);
        break;
    }
    case MSG_MAP_PART: {
        static MsgMapPart m; // a part's bytes
        memset(&m, 0, sizeof m);
        msg_map_part(&b, &m);
        if (netbuf_done(&b)) fetch_part(n, con, &m);
        break;
    }
    case MSG_MAP_CHANGE: { // the round is over: said as the original's ClientHandleMapChange says it
        MsgMapChange m = {0};
        msg_map_change(&b, &m);
        if (!netbuf_done(&b)) return;
        n->map_change = m;
        n->map_changing = true;
        console_print_color(con, HUD_COLOR_GAME, "Next map: %s\n", m.map);
        break;
    }
    case MSG_WEAPONS: { // kept for the worlds to come, and taken by this one at once
        static MsgWeapons m; // large
        memcpy(m.stats, n->weapons, sizeof m.stats);
        msg_weapons(&b, &m);
        if (!netbuf_done(&b)) return;
        memcpy(n->weapons, m.stats, sizeof n->weapons);
        n->weapons_heard = true;
        if (g) client_net_weapons(n, g);
        break;
    }
    case MSG_MAP_REPLY: {
        MsgMapReply m = {0};
        msg_map_reply(&b, &m);
        if (!netbuf_done(&b)) return;
        n->map_reply = m;
        n->map_replied = true;
        break;
    }
    case MSG_DENIED: {
        MsgDenied m = {0};
        msg_denied(&b, &m);
        if (netbuf_done(&b)) console_print(con, "denied: %s\n", m.reason);
        break;
    }
    case MSG_VOTE: {
        MsgVote m = {0};
        msg_vote(&b, &m);
        if (!netbuf_done(&b)) return;
        if (m.kind != VOTE_NONE && (n->vote.kind == VOTE_NONE || strcmp(n->vote.target, m.target) != 0)) n->vote_seq++;
        n->vote = m;
        break;
    }
    case MSG_CHAT: {
        MsgChat m = {0};
        msg_chat(&b, &m);
        if (!netbuf_done(&b)) return;
        if (n->inbox_count == CLIENT_NET_INBOX) { // full: the oldest is lost
            memmove(n->inbox, n->inbox + 1, sizeof n->inbox - sizeof n->inbox[0]);
            n->inbox_count--;
        }
        n->inbox[n->inbox_count++] = m;
        break;
    }
    case MSG_SNAPSHOT:
        if (n->state == CLIENT_NET_JOINED && n->round && !n->mapped && !n->fetch.on && g) client_stream_hear(&n->stream, g, n->slot, data, size);
        break;
    default: break;
    }
}

void client_net_poll(ClientNet *n, Console *con, Game *g)
{
    if (!n->link.host) return;
    NetEvent e;
    while (net_poll(&n->link, &e, 0) != NET_EVENT_NONE) {
        switch (e.kind) {
        case NET_EVENT_CONNECT:
            send_hello(n);
            break;
        case NET_EVENT_DISCONNECT:
            console_print_color(con, HUD_COLOR_WARNING, n->state == CLIENT_NET_CONNECTING ? "Connection timeout\n" : "Connection problem\n");
            net_close(&n->link);
            n->state = CLIENT_NET_OFF;
            n->slot = -1;
            fetch_stop(n);
            return;
        case NET_EVENT_MESSAGE:
            if (n->tap) n->tap(n->tap_user, e.data, e.size, e.msg);
            heard(n, con, g, e.data, e.size);
            break;
        default: break;
        }
    }
}

bool client_net_take_map(ClientNet *n)
{
    if (!n->mapped) return false;
    n->mapped = false;
    return true;
}

bool client_net_take_chat(ClientNet *n, MsgChat *out)
{
    if (n->inbox_count == 0) return false;
    *out = n->inbox[0];
    memmove(n->inbox, n->inbox + 1, sizeof n->inbox - sizeof n->inbox[0]);
    n->inbox_count--;
    return true;
}

void client_net_play(ClientNet *n, int slot)
{
    n->playback = true;
    n->state = CLIENT_NET_JOINED;
    n->slot = slot;
    n->round = 0;
    n->had_map = n->mapped = false;
    n->map_changing = n->map_replied = false;
    n->weapons_heard = false; // a new server says its own
    n->inbox_count = 0;
    n->vote = (MsgVote){.kind = VOTE_NONE};
    n->map_reply = (MsgMapReply){0};
    n->hostname[0] = '\0';
    snprintf(n->address, sizeof n->address, "demo");
    n->port = 0;
}

void client_net_feed(ClientNet *n, Console *con, Game *g, const uint8_t *data, size_t size) { heard(n, con, g, data, size); }

// Joined to a server, with a line to say things down: not a demo's playback.
static bool live(const ClientNet *n) { return n->state == CLIENT_NET_JOINED && !n->playback; }
// joined and with the round's world: not while its map is fetched
static bool playing(const ClientNet *n) { return live(n) && !n->fetch.on; }

void client_net_tick(ClientNet *n, const Game *g)
{
    if (!playing(n)) return; // the world being fetched has nothing to say of me
    client_stream_collect(&n->stream, g, n->slot);
    const Soldier *me = &g->world.soldiers[n->slot];
    if (!me->active) return; // nothing to say of a soldier the server hasn't placed yet
    uint8_t buf[NET_MTU];
    size_t size = client_stream_state(&n->stream, me, buf, sizeof buf);
    if (size) net_send(n->link.peer, MSG_CLIENT_STATE, buf, size);
}

void client_net_flush(ClientNet *n) { net_flush(&n->link); }

bool client_net_joined(const ClientNet *n) { return n->state == CLIENT_NET_JOINED; }

bool client_net_say(ClientNet *n, const char *text, bool team)
{
    if (!live(n)) return false;
    uint8_t buf[NET_MTU];
    NetBuf b = netbuf_writer(buf, sizeof buf);
    MsgKind kind = MSG_CHAT;
    MsgChat m = {.slot = (uint8_t)n->slot, .team = team};
    snprintf(m.text, sizeof m.text, "%s", text);
    msg_kind(&b, &kind);
    msg_chat(&b, &m);
    return netbuf_ok(&b) && net_send(n->link.peer, MSG_CHAT, buf, netbuf_bytes(&b));
}

bool client_net_take_map_change(ClientNet *n)
{
    if (!n->map_changing) return false;
    n->map_changing = false;
    return true;
}

void client_net_map_query(ClientNet *n, int index)
{
    if (!live(n) || index < 0) return;
    uint8_t buf[NET_MTU];
    NetBuf b = netbuf_writer(buf, sizeof buf);
    MsgKind kind = MSG_MAP_QUERY;
    MsgMapQuery m = {.index = (uint16_t)index};
    msg_kind(&b, &kind);
    msg_map_query(&b, &m);
    if (netbuf_ok(&b)) net_send(n->link.peer, MSG_MAP_QUERY, buf, netbuf_bytes(&b));
}

bool client_net_map_replied(ClientNet *n)
{
    if (!n->map_replied) return false;
    n->map_replied = false;
    return true;
}

void client_net_weapons(const ClientNet *n, Game *g)
{
    if (n->weapons_heard) weapons_apply(&g->ctx.weapons, n->weapons);
}
