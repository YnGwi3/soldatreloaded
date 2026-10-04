-- Soldat Reloaded: the game (which keeps a player's copy up to date as it starts, and hosts
-- Local Play), the dedicated server, the simulation they share, and the tests.
--
--   xmake                the game and the server
--   xmake run client     in assets/, where data/, mods/, config/ and scripts/ are
--   xmake run server
--   xmake test           the headless checks in tests/
--   xmake dist           the packages, in build/release/: one for players, one for a server,
--                        and the manifest the game's updater reads (launcher/update.h)
--
-- The game finds everything beside itself, in the directory it runs from: data/, what it
-- plays by (maps, animations, skeletons, bots); mods/, what it looks and sounds like
-- (mods/default/ and a player's own beside it); config/ and scripts/. assets/ holds them
-- as an install lays them out, so that is assets/ under xmake run (set_rundir) and the
-- package's own directory once unpacked, and nothing is passed on the command line. What
-- the game writes there as it plays (config/, demos/) and a player's mods are the
-- player's, and not the project's.

set_project("soldatreloaded")
set_version("0.8.1")
includes("@builtin/xpack")

add_rules("mode.debug", "mode.release")
set_languages("c11")
set_warnings("all")

if is_plat("windows") then
    add_defines("_CRT_SECURE_NO_WARNINGS")
else
    -- -std=c11 hides what glibc has beyond ISO C (dirent's d_type, clock_gettime, nanosleep)
    add_defines("_DEFAULT_SOURCE")
end

-- The libraries, built static so a package is the executables and nothing to find at
-- run time. SDL2 gives the client its window, input and GL context and stb its images,
-- as in the original (opensoldat's client/Gfx.pas); OpenGL itself is loaded at run time
-- through SDL_GL_GetProcAddress, so nothing links against a GL library. ENet is the
-- wire, for the netcode as it is ported. On Linux SDL2 builds against the system's X11,
-- Wayland and audio development headers, which have to be installed first.
add_requires("libsdl2", "enet", {configs = {shared = false}})
add_requires("stb")
-- The server's script runs on Lua (server/script.c), and its requests go through
-- libcurl; curl uses the system's TLS on Windows and macOS, and mbedTLS, built in,
-- elsewhere. The client needs neither.
add_requires("lua 5.4.x", {configs = {shared = false}})
add_requires("libcurl", {configs = {shared = false, mbedtls = not is_plat("windows", "macosx")}})
-- The launcher downloads with the same curl, and reads the packages with miniz: a zip,
-- whole or by range, and the deflate inside a tar.gz, as releases before had them.
add_requires("miniz")

-- assets/scripts/main.lua as a C string, main_lua.h: a server unpacked from its own package
-- makes scripts/main.lua from it where it is missing (apps/server/main.c), so the file
-- the game's package ships is the one source of it. Written only when it changes.
rule("main_lua")
    on_load(function (target)
        local text = io.readfile(path.join(os.projectdir(), "assets", "scripts", "main.lua")):gsub("\r", "")
        local lines = {}
        for line in (text .. "\n"):gmatch("(.-)\n") do
            table.insert(lines, '    "' .. line:gsub("\\", "\\\\"):gsub('"', '\\"') .. '\\n"')
        end
        if lines[#lines] == '    "\\n"' then table.remove(lines) end -- the end of the file's last line
        local header = "// Made by xmake.lua from assets/scripts/main.lua.\nstatic const char MAIN_LUA[] =\n"
                       .. table.concat(lines, "\n") .. ";\n"
        local out = path.join(target:autogendir(), "main_lua.h")
        if not os.isfile(out) or io.readfile(out) ~= header then io.writefile(out, header) end
        target:add("includedirs", target:autogendir())
    end)

-- The game's icon, assets/data/icon.ico, built into an executable on Windows: the one
-- Explorer, the taskbar and the window show, as SDL takes a window's icon from the first
-- in its executable. The resource script that names it is written here, at build time.
rule("icon")
    on_load(function (target)
        if not target:is_plat("windows") then return end
        local ico = path.join(os.projectdir(), "assets", "data", "icon.ico"):gsub("\\", "/")
        local rc = path.join(target:autogendir(), "icon.rc")
        local text = ("1 ICON \"%s\"\n"):format(ico)
        -- written only when it changes: a new one relinks the executable, a new one each time
        if not os.isfile(rc) or io.readfile(rc) ~= text then io.writefile(rc, text) end
        target:add("files", rc)
    end)

-- The simulation and the data it reads, shared by the client and the server. No
-- rendering, audio or networking dependencies.
target("shared")
    set_kind("static")
    add_files("apps/shared/**.c")
    add_includedirs("apps/shared", {public = true})
    add_packages("enet", {public = true}) -- the transport (shared/network) is ENet's
    add_packages("miniz", {public = true}) -- a packed map (.smap) is a zip (resources/mapfile.h)
    if not is_plat("windows") then
        add_syslinks("m", {public = true})
    end

-- The game, what a player starts: Soldat Reloaded.exe on Windows, soldatreloaded on
-- Linux. SDL2 for the window and input, OpenGL 2.1 for the drawing, and audio. Its
-- updater runs first (launcher/updater.h), so starting the game keeps it up to date. The
-- server's hosting is built in (server/hosted.h), for Local Play: the game hosts a game
-- and joins it over the loopback.
--   xmake run client [+map <name>] [+<cvar> <value>] [+<command> <args>...]
target("client")
    set_kind("binary")
    set_basename(is_plat("windows") and "Soldat Reloaded" or "soldatreloaded")
    add_rules("icon")
    add_deps("shared")
    -- the server but its console and loop (server/hosted.h), for Local Play: a game hosted
    -- here as a dedicated server hosts it, its script with it
    add_files("apps/client/**.c", "apps/server/*.c|main.c|stdin_reader.c")
    -- the updater, and its HTTPS for the server browser's list from the lobby too
    add_files("apps/launcher/*.c")
    add_includedirs("apps/client", "apps/server", "apps/launcher")
    add_packages("libsdl2", "stb", "libcurl", "lua", "miniz")
    add_defines('SOLDATRELOADED_RELEASES="https://github.com/soldatreloaded/soldatreloaded/releases"')
    if is_plat("windows") then
        add_syslinks("advapi32") -- the machine's ID, for its hardware ID (client/net/hwid.c)
    else
        add_syslinks("pthread") -- curl's resolver
    end
    -- the version the escape menu and the updater show, and the platform whose manifest the
    -- updater asks for (latest-windows-x64.txt)
    on_load(function (target)
        import("core.project.project")
        target:add("defines", 'SOLDATRELOADED_VERSION="' .. project.version() .. '"')
        target:add("defines", 'SOLDATRELOADED_PLATFORM="' .. target:plat() .. "-" .. target:arch() .. '"')
    end)
    if is_plat("windows") then
        -- SDL2main provides main and WinMain; with neither in our objects the linker can't
        -- infer the subsystem. A release is a windowed program, with no console window
        -- beside the game (the game has its own); a debug build keeps one for stderr.
        if is_mode("debug") then
            add_ldflags("/SUBSYSTEM:CONSOLE")
        else
            add_ldflags("/SUBSYSTEM:WINDOWS")
        end
    end
    set_rundir("$(projectdir)/assets")

-- The game server, headless: the same simulation with authority, ticked on its own
-- clock. Nothing but the console and the world until the netcode is ported.
--   xmake run server [+map <name>] [+<cvar> <value>] [+<command> <args>...]
target("server")
    add_rules("main_lua")
    set_kind("binary")
    add_deps("shared")
    add_files("apps/server/**.c")
    -- the launcher's HTTPS, for the lobby's heartbeat (server/lobby.c): it finds Linux's
    -- certificates for curl's mbedTLS
    add_files("apps/launcher/http.c", "apps/launcher/files.c", "apps/launcher/sha256.c")
    add_includedirs("apps/server", "apps/launcher")
    add_packages("lua", "libcurl")
    -- the version its requests say
    on_load(function (target)
        import("core.project.project")
        target:add("defines", 'SOLDATRELOADED_VERSION="' .. project.version() .. '"')
    end)
    if not is_plat("windows") then
        add_syslinks("pthread") -- the console's reader, the script's requests and the lobby's
    end
    set_rundir("$(projectdir)/assets")

-- The tests: headless checks of what shared/ holds, of the server's join, streams and
-- rounds over the loopback (the server's systems are built into them), and of the
-- launcher's manifests, archives and updates. Not built by default; run them with
--   xmake test
target("tests")
    set_kind("binary")
    set_default(false)
    add_deps("shared")
    add_files("tests/*.c", "apps/server/connections.c", "apps/server/lists.c", "apps/server/rounds.c",
              "apps/server/bots.c", "apps/server/host.c", "apps/server/script.c", "apps/server/lobby.c",
              "apps/server/host_cvars.c", "apps/server/weapons_ini.c")
    add_files("apps/launcher/*.c|updater.c") -- the update, not its window
    -- the client's line and its demos, for the demo's round trip (tests/demo_test.c),
    -- and its taunts, for the taunt editor's round trip (tests/taunts_test.c)
    add_files("apps/client/net/client_net.c", "apps/client/net/demo.c", "apps/client/net/hwid.c",
              "apps/client/ui/taunts.c", "apps/client/ui/mutes.c")
    add_includedirs("tests", "apps/server", "apps/launcher", "apps/client")
    add_packages("lua", "libcurl", "miniz")
    if is_plat("windows") then
        add_syslinks("advapi32") -- the machine's ID, for its hardware ID (client/net/hwid.c)
    else
        add_syslinks("pthread") -- the script's requests
    end
    set_rundir("$(projectdir)")
    add_tests("default")

-- The release packages (xpack): zips, each laid out as an install under one directory named
-- after it, soldatreloaded-<version>-<plat>-<arch>/: the launcher drops that directory as
-- it unpacks (launcher/archive.h), so a package without it would scatter. A zip says where
-- each file in it lies, so the launcher brings an update's files alone out of it, by range
-- (launcher/update.h); on Linux it keeps the executables' bit, as Info-ZIP writes it. What
-- an install holds is assets/'s data/, mods/default/ and scripts/, flat, which is how
-- the game expects to find them (docs/git.md, Releases); and config/.
--
--   soldatreloaded          the game, a player's: its one executable at the top, which updates
--                           the install as it starts and hosts Local Play itself, and
--                           manifest.txt, what it all is, which an update is brought out of;
--                           on Linux soldatreloaded-launcher beside it, the name players started
--                           when the launcher was apart from the game, which starts the game
--   soldatreloaded-server   a headless server's: the server at the top, its one executable;
--                           data/, config/ and scripts/, and no mods/, no art and no sound
--                           but what data/textures/ and scenery-gfx/ hold of custom maps
--
-- `xmake dist` packs them into build/release/, beside the manifest the launcher reads:
-- the game's, with the package named in it. The formats are launcher/manifest.h's.
local function release_package(name, suffix, bindir)
    xpack(name)
        set_formats("zip")
        set_basename("soldatreloaded-$(version)-$(plat)-$(arch)" .. suffix)
        set_prefixdir("soldatreloaded-$(version)-$(plat)-$(arch)" .. suffix)
        set_bindir(bindir)
        add_installfiles("license.md")
        -- the server's scripts: the examples, the release's (main.lua, which runs them, is the
        -- owner's, each package's own below)
        add_installfiles("assets/(scripts/examples/**)")
end

-- What every package's install is given last, as xpack lays it out: version.txt, and on
-- Linux the executables' bit, which nothing else is sure to keep; xpack's debug symbols go,
-- a player having no use for them. The game's package writes its manifest, every file of
-- the install but the manifest itself by hash (what the launcher does with each is
-- launcher/update.h's), and leaves it in build/.xpack/manifest.txt for latest-<plat>-<arch>.txt. (Each
-- step runs in a sandbox of its own, batchcmds:call's, so what it needs is local to it.)
local function finish_install(manifest, scripts)
    after_installcmd(function (package, batchcmds)
        local stash = path.join(import("core.project.config").builddir(), ".xpack", "manifest.txt")

        local function finish(installdir, version, executables, linux)
            io.writefile(path.join(installdir, "version.txt"), version .. "\n")
            for _, file in ipairs(os.files(path.join(installdir, "**.pdb"))) do
                os.rm(file)
            end
            for _, name in ipairs(linux and executables or {}) do
                os.vrunv("chmod", {"+x", path.join(installdir, name)})
            end
        end

        -- "file <sha256> <bytes> <path>", every file the release ships: what the launcher does
        -- with each, by where it lies, is its own (launcher/update.h). The player's own files
        -- (config/, their mods) are in no manifest.
        local function write_manifest(installdir, version, stash)
            local names = {}
            for _, file in ipairs(os.files(path.join(installdir, "**"))) do
                local name = path.relative(file, installdir):gsub("\\", "/")
                if name ~= "manifest.txt" then table.insert(names, name) end
            end
            table.sort(names)
            local lines = {"// What this install holds, which the launcher checks it against.", "version " .. version}
            for _, name in ipairs(names) do
                local file = path.join(installdir, name)
                table.insert(lines, ("file %s %d %s"):format(hash.sha256(file), os.filesize(file), name))
            end
            local text = table.concat(lines, "\n") .. "\n"
            io.writefile(path.join(installdir, "manifest.txt"), text)
            io.writefile(stash, text)
        end

        -- the executables, at the top, and the scripts that run like them
        local executables = {}
        for _, target in ipairs(package:targets()) do
            table.insert(executables, target:filename())
        end
        for _, name in ipairs(scripts or {}) do
            table.insert(executables, name)
        end
        batchcmds:call(finish, {package:installdir(), package:version(), executables, not package:is_plat("windows", "mingw")})
        if manifest then
            batchcmds:call(write_manifest, {package:installdir(), package:version(), stash})
        end
    end)
end

-- The icons: the .ico only builds the executables, which hold it on Windows; the .png is
-- the windows' elsewhere, and a server has neither.
release_package("soldatreloaded", "", ".")
    add_targets("client")
    add_installfiles("assets/(data/**)|icon.ico|icon.png")
    if not is_plat("windows") then
        add_installfiles("assets/(data/icon.png)")
        -- the launcher's name when it was apart from the game, which a player's shortcut may start, and which that
        -- launcher brings first as its own when it updates: it starts the game
        add_installfiles("apps/launcher/soldatreloaded-launcher.sh", {filename = "soldatreloaded-launcher"})
    end
    add_installfiles("assets/(mods/default/**)") -- the game's; a player's mods beside it are theirs
    -- the settings, at their defaults: the player's once changed, which the launcher then leaves
    -- be (launcher/update.h)
    add_installfiles("assets/(config/*)")
    add_installfiles("assets/(scripts/main.lua)") -- the owner's once they change it (launcher/update.h)
    finish_install(true, not is_plat("windows") and {"soldatreloaded-launcher"} or nil)

release_package("soldatreloaded-server", "-server", ".")
    add_targets("server")
    add_installfiles("assets/(data/**)|icon.ico|icon.png")
    -- the server's settings and its script, at their defaults, to be seen and changed from
    -- the start; unpacking a later release over a server puts them back as they came
    add_installfiles("assets/(config/*)|client.cfg")
    add_installfiles("assets/(scripts/main.lua)")
    finish_install(false)

-- xmake dist: the two packages, in build/release/, and latest-<plat>-<arch>.txt, the
-- manifest the launcher reads: the game's, with its package named in it.
task("dist")
    set_category("action")
    set_menu({usage = "xmake dist", description = "package the client and the server for this platform"})
    on_run(function ()
        import("core.project.config")
        import("core.project.project")

        config.load()
        local outputdir = path.join(config.builddir(), "release")
        -- built once, and packed as built: each package carries the executables the
        -- manifest names by hash
        os.execv(os.programfile(), {"build", "-y", "client", "server"})
        for _, name in ipairs({"soldatreloaded", "soldatreloaded-server"}) do
            os.execv(os.programfile(), {"pack", "-y", "--autobuild=n", "-o", outputdir, name})
        end

        local version, plat, arch = project.version(), config.plat(), config.arch()
        local package = path.join(outputdir, ("soldatreloaded-%s-%s-%s.zip"):format(version, plat, arch))
        local lines = io.readfile(path.join(config.builddir(), ".xpack", "manifest.txt")):split("\n")
        table.remove(lines, 1) -- its comment, for one of the release's own
        table.insert(lines, 2, ("package full %s %d %s"):format(hash.sha256(package), os.filesize(package), path.filename(package)))
        local latest = path.join(outputdir, ("latest-%s-%s.txt"):format(plat, arch))
        io.writefile(latest, ("// Soldat Reloaded %s for %s %s, for the launcher (launcher/update.h).\n"):format(version, plat, arch)
                             .. table.concat(lines, "\n") .. "\n")
        print("listed " .. path.absolute(latest))
    end)
