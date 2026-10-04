#include "net/discord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define PIPES 10          // discord-ipc-0 to 9: one per Discord app running, the first that answers
#define RETRY_SECONDS 15  // with no app found, looked for again this often
#define SEND_GAP 5.0      // seconds between activities at least: Discord takes five in twenty
#define FRAME_HEADER 8    // opcode and length, each a little-endian u32

enum { OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG };

// The pipe, by platform: opened, written whole, read without waiting, closed.

#ifdef _WIN32
static bool pipe_open(Discord *d)
{
    for (int i = 0; i < PIPES; i++) {
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            d->pipe = h;
            return true;
        }
    }
    return false;
}

static bool pipe_is_open(const Discord *d) { return d->pipe != NULL; }

static void pipe_close(Discord *d)
{
    if (d->pipe) CloseHandle(d->pipe);
    d->pipe = NULL;
}

static bool pipe_write(Discord *d, const void *data, size_t size)
{
    DWORD wrote = 0;
    return WriteFile(d->pipe, data, (DWORD)size, &wrote, NULL) && wrote == size;
}

// Bytes read into `out`, 0 for none waiting, -1 for the pipe gone.
static int pipe_read(Discord *d, void *out, size_t size)
{
    DWORD waiting = 0;
    if (!PeekNamedPipe(d->pipe, NULL, 0, NULL, &waiting, NULL)) return -1;
    if (waiting == 0) return 0;
    DWORD got = 0;
    if (!ReadFile(d->pipe, out, (DWORD)(waiting < size ? waiting : size), &got, NULL)) return -1;
    return (int)got;
}

static int process_id(void) { return (int)GetCurrentProcessId(); }
#else
// The runtime directory as each of these says it, and in each the places a Flatpak's or
// a Snap's Discord makes its socket.
static bool pipe_open(Discord *d)
{
    static const char *const ENVS[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    static const char *const SUBDIRS[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};
    const char *bases[sizeof ENVS / sizeof ENVS[0] + 1];
    int base_count = 0;
    for (size_t i = 0; i < sizeof ENVS / sizeof ENVS[0]; i++) {
        const char *v = getenv(ENVS[i]);
        if (v && v[0]) bases[base_count++] = v;
    }
    bases[base_count++] = "/tmp";
    for (int b = 0; b < base_count; b++) {
        for (size_t s = 0; s < sizeof SUBDIRS / sizeof SUBDIRS[0]; s++) {
            for (int i = 0; i < PIPES; i++) {
                struct sockaddr_un addr = {.sun_family = AF_UNIX};
                int n = snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%sdiscord-ipc-%d", bases[b], SUBDIRS[s], i);
                if (n < 0 || (size_t)n >= sizeof addr.sun_path) continue;
                int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) return false;
                if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
                    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                    d->pipe = fd;
                    return true;
                }
                close(fd);
            }
        }
    }
    return false;
}

static bool pipe_is_open(const Discord *d) { return d->pipe >= 0; }

static void pipe_close(Discord *d)
{
    if (d->pipe >= 0) close(d->pipe);
    d->pipe = -1;
}

static bool pipe_write(Discord *d, const void *data, size_t size)
{
    const char *p = data;
    while (size > 0) {
        ssize_t n = send(d->pipe, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false; // full or gone: a frame is a few hundred bytes, so gone
        p += n;
        size -= (size_t)n;
    }
    return true;
}

static int pipe_read(Discord *d, void *out, size_t size)
{
    ssize_t n = recv(d->pipe, out, size, 0);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

static int process_id(void) { return (int)getpid(); }
#endif

static void disconnect(Discord *d)
{
    pipe_close(d);
    d->ready = false;
    d->in_len = 0;
    d->skipping = 0;
    d->dirty = true; // what is wanted is said again to the next pipe
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool send_frame(Discord *d, uint32_t op, const char *json, size_t len)
{
    uint8_t frame[FRAME_HEADER + 2048];
    if (len > sizeof frame - FRAME_HEADER) return false;
    put_u32(frame, op);
    put_u32(frame + 4, (uint32_t)len);
    memcpy(frame + FRAME_HEADER, json, len);
    if (pipe_write(d, frame, FRAME_HEADER + len)) return true;
    disconnect(d);
    return false;
}

// `s` as a JSON string's contents onto `out` at `at`: quotes and controls escaped, and
// what isn't UTF-8 (a server's name in an old codepage) a '?', which Discord would refuse.
static size_t put_text(char *out, size_t size, size_t at, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p && at + 8 < size) {
        unsigned c = *p;
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        bool whole = extra >= 0;
        for (int i = 1; whole && i <= extra; i++) whole = (p[i] & 0xC0) == 0x80;
        if (!whole) {
            out[at++] = '?';
            p++;
        } else if (extra > 0) {
            memcpy(out + at, p, (size_t)extra + 1);
            at += (size_t)extra + 1;
            p += extra + 1;
        } else if (c == '"' || c == '\\') {
            out[at++] = '\\';
            out[at++] = (char)c;
            p++;
        } else if (c < 0x20) {
            at += (size_t)snprintf(out + at, size - at, "\\u%04x", c);
            p++;
        } else {
            out[at++] = (char)c;
            p++;
        }
    }
    out[at] = '\0';
    return at;
}

static size_t put(char *out, size_t size, size_t at, const char *s)
{
    size_t n = strlen(s);
    if (at + n >= size) n = size - at - 1;
    memcpy(out + at, s, n);
    out[at + n] = '\0';
    return at + n;
}

static bool send_activity(Discord *d)
{
    static unsigned nonce;
    const DiscordActivity *a = &d->want;
    char json[2048];
    size_t at = (size_t)snprintf(json, sizeof json,
                                 "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%d,\"activity\":{",
                                 ++nonce, process_id());
    // Discord refuses a line under two characters, so one so short goes unsaid
    if (strlen(a->details) >= 2) {
        at = put(json, sizeof json, at, "\"details\":\"");
        at = put_text(json, sizeof json, at, a->details);
        at = put(json, sizeof json, at, "\",");
    }
    if (strlen(a->state) >= 2) {
        at = put(json, sizeof json, at, "\"state\":\"");
        at = put_text(json, sizeof json, at, a->state);
        at = put(json, sizeof json, at, "\",");
    }
    if (a->since > 0) at += (size_t)snprintf(json + at, sizeof json - at, "\"timestamps\":{\"start\":%lld},", (long long)a->since);
    at = put(json, sizeof json, at, "\"assets\":{\"large_image\":\"" DISCORD_ICON "\",\"large_text\":\"Soldat Reloaded\"}}}}");
    return send_frame(d, OP_FRAME, json, at);
}

// A whole frame come back: the handshake's READY, an activity's answer, a ping, a close.
static void heard(Discord *d, uint32_t op, const char *json, size_t len)
{
    switch (op) {
    case OP_FRAME:
        if (!d->ready) {
            d->ready = true; // the handshake's answer: READY, the user's
            d->last_sent = -SEND_GAP;
        } else if (strstr(json, "\"evt\":\"ERROR\"")) {
            fprintf(stderr, "discord: %.*s\n", (int)len, json);
        }
        break;
    case OP_PING:
        send_frame(d, OP_PONG, json, len);
        break;
    case OP_CLOSE: // the handshake refused, or the app closing
        fprintf(stderr, "discord: closed: %.*s\n", (int)len, json);
        disconnect(d);
        break;
    default:
        break;
    }
}

static void read_frames(Discord *d)
{
    while (pipe_is_open(d)) {
        uint8_t chunk[2048];
        int got = pipe_read(d, chunk, sizeof chunk);
        if (got < 0) {
            disconnect(d);
            return;
        }
        if (got == 0) return;
        const uint8_t *p = chunk;
        size_t n = (size_t)got;
        while (n > 0 && pipe_is_open(d)) {
            if (d->skipping) { // the rest of a frame too big to keep
                size_t drop = n < d->skipping ? n : d->skipping;
                d->skipping -= drop;
                p += drop;
                n -= drop;
                continue;
            }
            size_t take = sizeof d->in - 1 - d->in_len;
            if (take > n) take = n;
            memcpy(d->in + d->in_len, p, take);
            d->in_len += take;
            p += take;
            n -= take;
            while (d->in_len >= FRAME_HEADER && pipe_is_open(d)) {
                uint32_t op = get_u32(d->in), len = get_u32(d->in + 4);
                if (FRAME_HEADER + (size_t)len > sizeof d->in - 1) { // never kept: its header gone, the rest skipped
                    d->skipping = FRAME_HEADER + (size_t)len - d->in_len;
                    d->in_len = 0;
                    // still a READY or a close, if that is what it was, though unread
                    if (op == OP_FRAME || op == OP_CLOSE) heard(d, op, "", 0);
                    break;
                }
                if (d->in_len < FRAME_HEADER + len) break;
                char *json = (char *)d->in + FRAME_HEADER;
                char after = json[len];
                json[len] = '\0'; // a string for strstr; the buffer keeps a byte spare for it
                heard(d, op, json, len);
                json[len] = after;
                if (!pipe_is_open(d)) return;
                d->in_len -= FRAME_HEADER + len;
                memmove(d->in, d->in + FRAME_HEADER + len, d->in_len);
            }
        }
    }
}

void discord_init(Discord *d)
{
    memset(d, 0, sizeof *d);
#ifndef _WIN32
    d->pipe = -1;
#endif
}

void discord_set(Discord *d, const DiscordActivity *a)
{
    if (memcmp(&d->want, a, sizeof *a) == 0) return;
    d->want = *a;
    d->dirty = true;
}

void discord_pump(Discord *d, double now, bool enabled)
{
    if (!enabled) {
        if (pipe_is_open(d)) disconnect(d);
        d->next_try = 0; // looked for at once when turned back on
        return;
    }
    if (!pipe_is_open(d)) {
        if (now < d->next_try) return;
        d->next_try = now + RETRY_SECONDS;
        if (!pipe_open(d)) return;
        char hello[96];
        int n = snprintf(hello, sizeof hello, "{\"v\":1,\"client_id\":\"%s\"}", DISCORD_APP_ID);
        if (!send_frame(d, OP_HANDSHAKE, hello, (size_t)n)) return;
        d->dirty = true;
    }
    read_frames(d);
    if (d->ready && d->dirty && now - d->last_sent >= SEND_GAP) {
        if (send_activity(d)) {
            d->dirty = false;
            d->last_sent = now;
        }
    }
}

void discord_close(Discord *d)
{
    pipe_close(d);
    d->ready = false;
}
