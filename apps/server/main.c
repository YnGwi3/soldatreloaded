// The server, headless: the console around a hosted game (hosted.h), and a loop that
// pumps it until it is told to stop. It reads its config (config/) and the command line, loads
// the map, listens on sv_port, gives everyone who says Hello a soldier, plays the bots
// asked for, ticks the world with authority, and stops on `quit` or Ctrl-C.
//
//   console  the cvars and the commands (shared/console): the code's defaults,
//            config/server.cfg over them, then the command line
//   hosted   the host (the world, the line, the players, the bots and the rounds), its
//            script, the lobby's heartbeat and the weapons mod: what the game's Local Play
//            hosts with too (hosted.h)
//
// It runs from the directory that holds config/ and data/, as the game does: the install,
// found from wherever it is started (files_enter_install).
//
//   server [+map <name>] [+sv_port <port>] [+<cvar> <value>] [+<command> <args>...]

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "console/console.h"
#include "host_cvars.h"
#include "hosted.h"
#include "weapons_ini.h"
#include "files.h" // the launcher's, to find the install
#include "main_lua.h" // assets/scripts/main.lua, made into a string by xmake.lua
#include "http.h"
#include "stdin_reader.h"

// After the game's headers: GDI has a Polygon of its own.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#endif

#ifndef SOLDATRELOADED_VERSION
#define SOLDATRELOADED_VERSION "dev" // xmake.lua sets it from set_version
#endif
// The config (config/): the defaults are the code's; server.cfg over them (host_cvars.h),
// then the weapons mod (weapons.ini) and the rotation (maplist.txt). As it starts, before
// the command line, the server writes server.cfg whole, every setting with the game's
// value commented out where it holds it, and makes the files it reads where they are
// missing. The game's Local Play hosts on these same files. A server set up before
// config/ had one config.cfg: it is read once, as config/ is made.
#define CONFIG_OLD "config.cfg"
#define MAIN_SCRIPT "scripts/main.lua" // sv_script's, made from main_lua.h where missing
#define SLEEP_MS 1 // between passes of the loop, so it never spins flat out

typedef struct Server {
    Console *console; // large; on the heap
    Cvar *data;
    HostCvars cvars;  // what it hosts by, as Local Play does (host_cvars.h)
    Cvar *rope_debug; // sv_rope_debug: each soldier's rope each half second, and changes at once
    Hosted hosted;
    bool quit;
} Server;

static volatile sig_atomic_t interrupted; // Ctrl-C, or a kill

static void on_interrupt(int sig)
{
    (void)sig;
    interrupted = 1;
}

// Seconds from some fixed point: the platform's monotonic clock.
static double now(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency, count;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

// Flushed as it goes: a server's output is usually a pipe or a log file, which would
// otherwise hold it back until the buffer fills.
static void print_stdout(const char *text, void *user)
{
    (void)user;
    fputs(text, stdout);
    fflush(stdout);
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The files of config/ a server reads but never writes once there, made where missing with
// what they are for, so they are found from the first start (lists.c makes the lists').
static void config_make_missing(void)
{
    maplist_make();

    // The script, where an owner has taken it out of their unpacked package. A game's
    // install has a manifest.txt: its updater makes it there, once, so one taken out
    // stays out.
    if (!file_exists(MAIN_SCRIPT) && !file_exists("manifest.txt") && files_make_parents(MAIN_SCRIPT))
        files_write(MAIN_SCRIPT, MAIN_LUA, sizeof MAIN_LUA - 1);
}

static void cmd_quit(Console *con, int argc, char **argv, void *user)
{
    (void)con, (void)argc, (void)argv;
    ((Server *)user)->quit = true;
}

// The words, joined with single spaces.
static void words(int argc, char **argv, int first, char *out, size_t size)
{
    size_t n = 0;
    out[0] = '\0';
    for (int i = first; i < argc && n < size - 1; i++) {
        int w = snprintf(out + n, size - n, i > first ? " %s" : "%s", argv[i]);
        if (w < 0) break;
        n += (size_t)w;
    }
}

// mute, unmute, mutes: the server's console's names for an admin's servermute and the
// rest (connections_admin); in the game's, mute is a player's own.
static void cmd_mute(Console *con, int argc, char **argv, void *user)
{
    Server *sv = user;
    char text[NET_TEXT_SIZE];
    words(argc, argv, 0, text, sizeof text);
    if (!sv->hosted.running) console_print(con, "no game is being hosted\n");
    else connections_admin(&sv->hosted.host.connections, sv->hosted.host.game, -1, text);
}

// say <text...>: the server's chat to everyone.
static void cmd_say(Console *con, int argc, char **argv, void *user)
{
    Server *sv = user;
    if (argc < 2) {
        console_print(con, "usage: say <text>\n");
        return;
    }
    char text[NET_TEXT_SIZE];
    words(argc, argv, 1, text, sizeof text);
    if (sv->hosted.running) host_say(&sv->hosted.host, text);
}

// The console and what the server keeps in it, then its config and the command line
// over it. Its files are written as it starts (config/, above); nothing on the way out.
static bool console_open(Server *sv, int argc, char *argv[])
{
    Console *con = sv->console = console_create(print_stdout, NULL);
    if (!con) return false;
    sv->data = cvar_register(con, "data", "./data", 0, "what the game plays by: maps/, anims/, objects/, bots/");
    host_cvars_register(con, &sv->cvars);
    sv->rope_debug = cvar_register(con, "sv_rope_debug", "0", 0,
                                   "log each soldier's rope each half second and changes at once, with the states dropped");
    hosted_init(&sv->hosted, con, &sv->cvars, sv->data);
    console_add_command(con, "quit", cmd_quit, sv, "stop the server");
    console_add_command(con, "say", cmd_say, sv, "say something to everyone, as the server");
    console_add_command(con, "mute", cmd_mute, sv, "their chat reaches nobody, until unmuted: mute <player>");
    console_add_command(con, "unmute", cmd_mute, sv, "unmute <player, address, hardware ID or name>");
    console_add_command(con, "mutes", cmd_mute, sv, "the mute list");

    if (file_exists(CONFIG_SERVER)) console_execute_file(con, CONFIG_SERVER);
    console_mark_loaded(con); // what changes from here (an old config.cfg) goes back into it
    if (file_exists(CONFIG_OLD)) {
        // a server set up before config/: read over config/, which is written from it below.
        // A game's install shares it with the game, which moves it aside once it has read it
        // too; a server's own install has no game, so the server does.
        console_execute_file(con, CONFIG_OLD);
        maplist_take_cvar(&sv->cvars, con);
        if (!file_exists("manifest.txt")) {
            remove(CONFIG_OLD ".old");
            rename(CONFIG_OLD, CONFIG_OLD ".old");
        }
    }
    hosted_load_weapons(&sv->hosted);
    // its own files whole, as they stand before the command line, which isn't kept
    config_make_missing();
    if (!console_save_files(con, &HOST_CONFIG_FILE, 1)) console_print(con, "could not write %s\n", CONFIG_SERVER);
    if (!file_exists(CONFIG_WEAPONS) && !weapons_ini_template(CONFIG_WEAPONS))
        console_print(con, "could not write %s\n", CONFIG_WEAPONS);
    console_execute_args(con, argc, argv);
    return true;
}

int main(int argc, char *argv[])
{
    static Server sv; // large: the host's game is on the heap, the rest of it here
    // its files beside it, wherever it is started from
    if (!files_enter_install("data")) fprintf(stderr, "no data/ here, beside the executable or above it\n");

    if (!console_open(&sv, argc, argv)) return 1;
    if (!net_init()) {
        fprintf(stderr, "ENet wouldn't start\n");
        console_destroy(sv.console);
        return 1;
    }
    http_init();
    http_set_agent("soldatreloaded-server/" SOLDATRELOADED_VERSION);
    if (!hosted_open(&sv.hosted)) {
        http_cleanup();
        net_shutdown();
        console_destroy(sv.console);
        return 1;
    }
    sv.hosted.host.rope_debug = sv.rope_debug; // the host logs it, sv_rope_debug
    if (!stdin_reader_start()) fprintf(stderr, "the console won't read its input\n");
    signal(SIGINT, on_interrupt);
    signal(SIGTERM, on_interrupt);

    // The line is heard before the ticks; each tick the players' soldiers step on their
    // last keys and the bots on their own minds, and a snapshot goes to everyone; the
    // line is flushed after (host_pump).
    double last = now();
    while (!sv.quit && !interrupted) {
        double t = now();
        double dt = t - last;
        last = t;
        char line[STDIN_LINE_SIZE];
        while (stdin_reader_take(line, sizeof line)) console_execute(sv.console, line);
        if (!hosted_pump(&sv.hosted, dt, t)) sv.quit = true;
        sleep_ms(SLEEP_MS);
    }

    console_print(sv.console, "stopping\n");
    hosted_close(&sv.hosted);
    http_cleanup();
    net_shutdown();
    console_destroy(sv.console);
    return 0;
}
