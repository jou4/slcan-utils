/*
 * slcan_gateway.c
 *
 * CAN gateway: for each <channel>=<script.lua> mapping given on the
 * command line, connects to that channel's daemon RX pipe
 * (\\.\pipe\serial_rx\<channel>) and runs a dedicated Lua VM loaded
 * with that script. Every received frame is handed to the script's
 * gateway(src_channel, frame) function, which decides whether to drop
 * it, forward it (optionally edited) to one destination channel, or
 * fan it out to several -- each forwarded via that destination's TX
 * pipe (\\.\pipe\serial_tx\<dest_channel>).
 *
 * Script contract (globals a script may define):
 *
 *   initialize()
 *       Called once when the channel starts, before any frames are
 *       processed. A non-zero return, or a raised Lua error, disables
 *       that one channel only -- the other channels configured on
 *       this command line keep running.
 *
 *   gateway(src_channel, frame)
 *       Called once per received frame.
 *         src_channel - string, the channel the frame arrived on.
 *         frame       - a table with fields id/ext/rtr/fd/brs/esi/
 *                       dlc/len/data (data is a 1-indexed array of
 *                       len bytes). The script may modify id/ext/rtr/
 *                       fd/brs/esi/len/data in place to edit the
 *                       frame before it is forwarded (dlc is
 *                       recomputed from len on the way back out, so
 *                       setting it has no effect).
 *       Return value selects what happens to the frame:
 *         nil / false                    drop it
 *         "channel"                      forward the (possibly
 *                                         edited) frame to that one
 *                                         channel
 *         { "ch1", "ch2", ... }          forward the same edited
 *                                         frame to every channel
 *                                         listed (fan-out)
 *         { {channel="ch1", frame=f1},
 *           "ch2", ... }                 an array may mix plain
 *                                         channel-name strings
 *                                         (shared edited frame) with
 *                                         {channel=,frame=} tables
 *                                         (a distinct frame table per
 *                                         destination) -- this is how
 *                                         "different content per
 *                                         destination" or "several
 *                                         frames to the same
 *                                         destination" (by repeating
 *                                         the same channel name with
 *                                         different frame= content)
 *                                         are expressed.
 *
 *   finalize()
 *       Called once when the channel stops (Ctrl+C, or the source
 *       daemon's RX pipe closing). A non-zero return or raised error
 *       is logged only -- the process is already shutting down.
 *
 * A destination channel with no daemon currently listening drops
 * frames addressed to it (retrying the connection periodically, see
 * TX_RECONNECT_COOLDOWN_MS) without affecting delivery to any other
 * destination.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "slcan.h"

#define PIPE_TX_FORMAT      "\\\\.\\pipe\\serial_tx\\%s"
#define PIPE_RX_FORMAT      "\\\\.\\pipe\\serial_rx\\%s"

#define MAX_CHANNELS        16
#define CHANNEL_NAME_MAX    64

/* How long a destination TX pipe that failed to connect (or whose
 * WriteFile failed) is left alone before the next reconnect attempt.
 * Frames addressed to it are silently dropped during the cooldown,
 * rather than blocking the sending thread on a WaitNamedPipeA() retry
 * for every single frame while that destination's daemon is down. */
#define TX_RECONNECT_COOLDOWN_MS   3000
/* Short, non-disruptive probe timeout for a periodic reconnect
 * attempt (as opposed to the generous 5s used by the other slcan-
 * utils CLIs at their one-shot startup connect) -- a long block here
 * would stall this destination's mutex, and therefore every thread
 * with a frame bound for it, for the full timeout. */
#define TX_RECONNECT_PROBE_MS      300

static volatile BOOL g_running = TRUE;

/* ---- console control handler --------------------------------------
 *
 * On Ctrl+C, Ctrl+Break, or the console window closing, clear
 * g_running instead of letting the default handler terminate the
 * process immediately, so every gateway_worker() thread notices
 * (via its bounded 200ms poll) and falls through to calling the
 * script's finalize() and closing its pipes, instead of leaving pipe
 * handles and Lua states behind on an abrupt process exit. Mirrors
 * serial_daemon.c's console_ctrl_handler(). */
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_running = FALSE;
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---- per-channel (receiving) configuration ------------------------ */

typedef struct {
    char       channel[CHANNEL_NAME_MAX];
    char       script_path[MAX_PATH];
    lua_State *L;   /* owned by this channel's gateway_worker() thread */
} RxChannel;

static RxChannel g_rx_channels[MAX_CHANNELS];
static int       g_rx_channel_count = 0;

/* ---- shared destination (TX) pipe registry -------------------------
 *
 * Every gateway_worker() thread can decide, per frame, to forward to
 * any destination channel name its script returns -- not just its own
 * source channel -- so this registry (and the pipe handles in it) is
 * shared across all worker threads. g_tx_registry_mutex protects only
 * finding/creating a channel's slot; each slot then has its own mutex
 * so that connecting to / writing to different destinations never
 * blocks on each other, only concurrent writers to the *same*
 * destination serialize. */

typedef struct {
    char      name[CHANNEL_NAME_MAX];
    HANDLE    pipe;           /* INVALID_HANDLE_VALUE = not connected */
    HANDLE    mutex;          /* serializes connect+write for this one destination */
    ULONGLONG last_attempt;   /* GetTickCount64() of the last connect attempt */
} TxChannel;

static TxChannel g_tx_channels[MAX_CHANNELS];
static int       g_tx_channel_count = 0;
static HANDLE    g_tx_registry_mutex;

/* Find or create the registry slot for destination channel name.
 * Returns NULL (after logging) if the registry is full. The returned
 * pointer is stable for the process lifetime (the array never
 * shrinks/moves entries), so it's safe to use without holding
 * g_tx_registry_mutex once obtained. */
static TxChannel *get_or_create_tx_slot(const char *name)
{
    WaitForSingleObject(g_tx_registry_mutex, INFINITE);

    for (int i = 0; i < g_tx_channel_count; i++) {
        if (strcmp(g_tx_channels[i].name, name) == 0) {
            ReleaseMutex(g_tx_registry_mutex);
            return &g_tx_channels[i];
        }
    }
    if (g_tx_channel_count >= MAX_CHANNELS) {
        ReleaseMutex(g_tx_registry_mutex);
        fprintf(stderr,
                "[gateway] Error: too many distinct destination "
                "channels (max %d), dropping frame for '%s'\n",
                MAX_CHANNELS, name);
        return NULL;
    }

    TxChannel *slot = &g_tx_channels[g_tx_channel_count++];
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->pipe          = INVALID_HANDLE_VALUE;
    slot->mutex         = CreateMutex(NULL, FALSE, NULL);
    slot->last_attempt  = 0;

    ReleaseMutex(g_tx_registry_mutex);
    return slot;
}

/* Send one already-validated frame to destination channel name,
 * connecting to \\.\pipe\serial_tx\<name> on demand and reconnecting
 * (at most once every TX_RECONNECT_COOLDOWN_MS) if the connection is
 * down or a previous write failed. Never blocks the caller for longer
 * than TX_RECONNECT_PROBE_MS, so one down destination can't stall
 * frames bound for other destinations for long, though same-
 * destination callers do serialize on slot->mutex. */
static void send_to_channel(const char *name, const CanFrame *frame)
{
    TxChannel *slot = get_or_create_tx_slot(name);
    if (!slot) return;   /* already logged */

    WaitForSingleObject(slot->mutex, INFINITE);

    if (slot->pipe == INVALID_HANDLE_VALUE) {
        ULONGLONG now = GetTickCount64();
        if (now - slot->last_attempt < TX_RECONNECT_COOLDOWN_MS) {
            ReleaseMutex(slot->mutex);
            return;   /* still cooling down since the last failed attempt */
        }
        slot->last_attempt = now;

        if (!slcan_valid_channel(name)) {
            fprintf(stderr,
                    "[gateway] Warning: invalid destination channel "
                    "name '%s', dropping frame\n", name);
            ReleaseMutex(slot->mutex);
            return;
        }
        char pipebuf[32];
        int  need = snprintf(pipebuf, sizeof(pipebuf), PIPE_TX_FORMAT, name);
        if (need < 0 || need >= (int)sizeof(pipebuf)) {
            fprintf(stderr,
                    "[gateway] Warning: destination channel name too "
                    "long: '%s', dropping frame\n", name);
            ReleaseMutex(slot->mutex);
            return;
        }
        if (!WaitNamedPipeA(pipebuf, TX_RECONNECT_PROBE_MS)) {
            fprintf(stderr,
                    "[gateway] Warning: daemon not running for "
                    "destination channel '%s', dropping frame "
                    "(retrying every %ds)\n",
                    name, TX_RECONNECT_COOLDOWN_MS / 1000);
            ReleaseMutex(slot->mutex);
            return;
        }
        HANDLE hPipe = CreateFileA(pipebuf, GENERIC_WRITE, 0, NULL,
                                    OPEN_EXISTING, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr,
                    "[gateway] Warning: cannot open TX pipe for "
                    "destination channel '%s': %lu\n",
                    name, GetLastError());
            ReleaseMutex(slot->mutex);
            return;
        }
        fprintf(stderr, "[gateway] Connected to destination channel '%s'\n",
                name);
        slot->pipe = hPipe;
    }

    DWORD written;
    if (!WriteFile(slot->pipe, frame, sizeof(*frame), &written, NULL)
        || written != sizeof(*frame)) {
        fprintf(stderr,
                "[gateway] Error: pipe write failed for destination "
                "channel '%s': %lu (will retry connecting)\n",
                name, GetLastError());
        CloseHandle(slot->pipe);
        slot->pipe = INVALID_HANDLE_VALUE;
    }

    ReleaseMutex(slot->mutex);
}

static void close_all_tx_channels(void)
{
    for (int i = 0; i < g_tx_channel_count; i++) {
        if (g_tx_channels[i].pipe != INVALID_HANDLE_VALUE)
            CloseHandle(g_tx_channels[i].pipe);
        if (g_tx_channels[i].mutex)
            CloseHandle(g_tx_channels[i].mutex);
    }
}

/* ---- Lua <-> CanFrame marshaling ------------------------------------ */

/* Push a fresh Lua table representing f onto the stack, with fields
 * id/ext/rtr/fd/brs/esi/dlc/len and a 1-indexed data[] sub-table of
 * len bytes. dlc is included for the script's information only --
 * read_frame_table() below always recomputes it from len, so a
 * script setting frame.dlc has no effect. */
static void push_frame_table(lua_State *L, const CanFrame *f)
{
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)f->id); lua_setfield(L, -2, "id");
    lua_pushinteger(L, f->ext);             lua_setfield(L, -2, "ext");
    lua_pushinteger(L, f->rtr);             lua_setfield(L, -2, "rtr");
    lua_pushinteger(L, f->fd);              lua_setfield(L, -2, "fd");
    lua_pushinteger(L, f->brs);             lua_setfield(L, -2, "brs");
    lua_pushinteger(L, f->esi);             lua_setfield(L, -2, "esi");
    lua_pushinteger(L, f->dlc);             lua_setfield(L, -2, "dlc");
    lua_pushinteger(L, f->len);             lua_setfield(L, -2, "len");

    lua_newtable(L);
    for (int i = 0; i < f->len; i++) {
        lua_pushinteger(L, f->data[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "data");
}

/* Read the frame table at stack index idx back into *out, validating
 * id/len ranges the same way slcan_encode() does (see slcan.h's
 * CAN_SFF_ID_MAX/CAN_EFF_ID_MAX). dlc is always recomputed from len,
 * never read from the table. Does not pop idx.
 * Returns TRUE on success; FALSE (after logging a warning identifying
 * channel) if id or len is out of range, in which case *out's
 * contents are unspecified and must not be forwarded. */
static BOOL read_frame_table(lua_State *L, int idx, const char *channel,
                              CanFrame *out)
{
    idx = lua_absindex(L, idx);
    memset(out, 0, sizeof(*out));

    lua_getfield(L, idx, "id");  lua_Integer id  = lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, idx, "ext"); int ext = (int)lua_tointeger(L, -1);    lua_pop(L, 1);
    lua_getfield(L, idx, "rtr"); int rtr = (int)lua_tointeger(L, -1);    lua_pop(L, 1);
    lua_getfield(L, idx, "fd");  int fd  = (int)lua_tointeger(L, -1);    lua_pop(L, 1);
    lua_getfield(L, idx, "brs"); int brs = (int)lua_tointeger(L, -1);    lua_pop(L, 1);
    lua_getfield(L, idx, "esi"); int esi = (int)lua_tointeger(L, -1);    lua_pop(L, 1);
    lua_getfield(L, idx, "len"); lua_Integer len = lua_tointeger(L, -1); lua_pop(L, 1);

    uint32_t max_id = ext ? CAN_EFF_ID_MAX : CAN_SFF_ID_MAX;
    if (id < 0 || (uint32_t)id > max_id) {
        fprintf(stderr,
                "[gateway] Warning: channel '%s' script set an "
                "out-of-range CAN id %lld (ext=%d), dropping frame\n",
                channel, (long long)id, ext);
        return FALSE;
    }
    uint8_t maxlen = fd ? SLCAN_FD_MAX_DLEN : SLCAN_MAX_DLEN;
    if (len < 0 || len > maxlen) {
        fprintf(stderr,
                "[gateway] Warning: channel '%s' script set an "
                "invalid data length %lld, dropping frame\n",
                channel, (long long)len);
        return FALSE;
    }

    out->id  = (uint32_t)id;
    out->ext = ext ? 1 : 0;
    out->rtr = rtr ? 1 : 0;
    out->fd  = fd  ? 1 : 0;
    out->brs = brs ? 1 : 0;
    out->esi = esi ? 1 : 0;
    out->len = (uint8_t)len;
    out->dlc = out->fd ? canfd_len2dlc(out->len)
                        : ((out->len < 8) ? out->len : 8);

    lua_getfield(L, idx, "data");
    if (lua_istable(L, -1)) {
        for (int i = 0; i < out->len; i++) {
            lua_rawgeti(L, -1, i + 1);
            out->data[i] = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);   /* data table */

    return TRUE;
}

/* Lazily compute (and cache) the shared edited frame the first time a
 * plain channel-name-string destination is encountered, so a script
 * that never uses the string form never pays for reading the table
 * back, and an invalid edit is only warned about once per call. */
static BOOL ensure_edited(lua_State *L, int frame_idx, const char *channel,
                           CanFrame *edited, int *have_edited)
{
    if (*have_edited == 0)
        *have_edited = read_frame_table(L, frame_idx, channel, edited) ? 1 : -1;
    return *have_edited == 1;
}

/* Call gateway() for one received frame and act on its return value
 * per the contract documented at the top of this file. */
static void process_frame(lua_State *L, const char *src_channel,
                           const CanFrame *frame)
{
    push_frame_table(L, frame);
    int frame_idx = lua_gettop(L);

    lua_getglobal(L, "gateway");
    lua_pushstring(L, src_channel);
    lua_pushvalue(L, frame_idx);   /* duplicate ref: pcall consumes this
                                     * copy as the arg; our own
                                     * reference at frame_idx survives
                                     * the call so we can read back
                                     * whatever the script mutated */

    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        fprintf(stderr,
                "[gateway] Error: channel '%s' script's gateway() "
                "raised an error: %s\n", src_channel, lua_tostring(L, -1));
        lua_settop(L, frame_idx - 1);
        return;
    }
    int result_idx = lua_gettop(L);

    CanFrame edited;
    int      have_edited = 0;   /* 0 = not yet computed, 1 = valid, -1 = invalid */

    if (lua_isnil(L, result_idx) ||
        (lua_isboolean(L, result_idx) && !lua_toboolean(L, result_idx))) {
        /* drop */

    } else if (lua_isstring(L, result_idx)) {
        if (ensure_edited(L, frame_idx, src_channel, &edited, &have_edited))
            send_to_channel(lua_tostring(L, result_idx), &edited);

    } else if (lua_istable(L, result_idx)) {
        lua_Integer n = (lua_Integer)lua_rawlen(L, result_idx);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, result_idx, (int)i);
            int entry_idx = lua_gettop(L);

            if (lua_isstring(L, entry_idx)) {
                if (ensure_edited(L, frame_idx, src_channel, &edited, &have_edited))
                    send_to_channel(lua_tostring(L, entry_idx), &edited);

            } else if (lua_istable(L, entry_idx)) {
                lua_getfield(L, entry_idx, "channel");
                const char *dest = lua_tostring(L, -1);
                lua_getfield(L, entry_idx, "frame");
                CanFrame per_dest;
                if (dest && lua_istable(L, -1) &&
                    read_frame_table(L, lua_gettop(L), src_channel, &per_dest)) {
                    send_to_channel(dest, &per_dest);
                } else {
                    fprintf(stderr,
                            "[gateway] Warning: channel '%s' script "
                            "returned a malformed {channel=,frame=} "
                            "entry, skipping\n", src_channel);
                }
                lua_pop(L, 2);   /* frame field, channel field */

            } else {
                fprintf(stderr,
                        "[gateway] Warning: channel '%s' script's "
                        "gateway() returned an array entry that is "
                        "neither a string nor a table, skipping\n",
                        src_channel);
            }
            lua_pop(L, 1);   /* entry */
        }

    } else {
        fprintf(stderr,
                "[gateway] Warning: channel '%s' script's gateway() "
                "returned an unsupported type, dropping frame\n",
                src_channel);
    }

    lua_settop(L, frame_idx - 1);
}

/* ---- script lifecycle ----------------------------------------------
 *
 * Call a zero-argument lifecycle function (initialize/finalize) if
 * the script defines it as a global. Returns TRUE if the function is
 * undefined (a no-op, not an error) or ran and returned zero (or
 * nothing); returns FALSE (after logging) if it raised a Lua error,
 * or if check_retval is TRUE and it returned a non-zero integer. */
static BOOL call_lifecycle_fn(lua_State *L, const char *fn_name,
                               const char *channel, BOOL check_retval)
{
    lua_getglobal(L, fn_name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return TRUE;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        fprintf(stderr,
                "[gateway] Error: channel '%s' script's %s() raised "
                "an error: %s\n", channel, fn_name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return FALSE;
    }
    int rc = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (check_retval && rc != 0) {
        fprintf(stderr,
                "[gateway] Error: channel '%s' script's %s() returned "
                "%d (non-zero)\n", channel, fn_name, rc);
        return FALSE;
    }
    return TRUE;
}

/* ---- per-channel worker thread --------------------------------------
 *
 * Connects to cfg's RX pipe, loads and runs cfg's script in a fresh
 * Lua state, calls initialize(), then processes frames until
 * g_running clears or the RX pipe disconnects, then calls finalize()
 * and releases everything. A failure at any setup step (connect,
 * script load, missing gateway() function, initialize()) disables
 * only this channel; the thread simply returns without ever affecting
 * the other configured channels. */
static DWORD WINAPI gateway_worker(LPVOID arg)
{
    RxChannel *cfg = (RxChannel *)arg;

    char pipebuf[32];
    int  need = snprintf(pipebuf, sizeof(pipebuf), PIPE_RX_FORMAT, cfg->channel);
    if (need < 0 || need >= (int)sizeof(pipebuf)) {
        fprintf(stderr, "[gateway] Error: channel name too long: %s\n",
                cfg->channel);
        return 1;
    }
    if (!WaitNamedPipeA(pipebuf, 5000)) {
        fprintf(stderr,
                "[gateway] Error: daemon not running for channel '%s'\n",
                cfg->channel);
        return 1;
    }
    HANDLE hPipe = CreateFileA(pipebuf, GENERIC_READ, 0, NULL,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr,
                "[gateway] Error: cannot open RX pipe for channel '%s': %lu\n",
                cfg->channel, GetLastError());
        return 1;
    }

    cfg->L = luaL_newstate();
    if (!cfg->L) {
        fprintf(stderr,
                "[gateway] Error: out of memory creating Lua state for "
                "channel '%s'\n", cfg->channel);
        CloseHandle(hPipe);
        return 1;
    }
    luaL_openlibs(cfg->L);   /* full stdlib: these are the operator's own
                               * trusted scripts, not third-party content */

    if (luaL_dofile(cfg->L, cfg->script_path) != LUA_OK) {
        fprintf(stderr,
                "[gateway] Error: channel '%s': failed to load script "
                "'%s': %s\n", cfg->channel, cfg->script_path,
                lua_tostring(cfg->L, -1));
        lua_close(cfg->L);
        cfg->L = NULL;
        CloseHandle(hPipe);
        return 1;
    }

    lua_getglobal(cfg->L, "gateway");
    BOOL has_gateway_fn = lua_isfunction(cfg->L, -1);
    lua_pop(cfg->L, 1);
    if (!has_gateway_fn) {
        fprintf(stderr,
                "[gateway] Error: channel '%s': script '%s' does not "
                "define a gateway(src_channel, frame) function\n",
                cfg->channel, cfg->script_path);
        lua_close(cfg->L);
        cfg->L = NULL;
        CloseHandle(hPipe);
        return 1;
    }

    if (!call_lifecycle_fn(cfg->L, "initialize", cfg->channel, TRUE)) {
        lua_close(cfg->L);
        cfg->L = NULL;
        CloseHandle(hPipe);
        return 1;
    }

    fprintf(stderr, "[gateway] Channel '%s' running script '%s'\n",
            cfg->channel, cfg->script_path);

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    CanFrame frame;
    DWORD    n;

    while (g_running) {
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(hPipe, &frame, sizeof(frame), &n, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) break;   /* pipe gone */
            DWORD wait;
            do {
                wait = WaitForSingleObject(ov.hEvent, 200);
            } while (wait == WAIT_TIMEOUT && g_running);
            if (wait != WAIT_OBJECT_0) {
                CancelIo(hPipe);   /* shutting down with a read still pending */
                break;
            }
            if (!GetOverlappedResult(hPipe, &ov, &n, FALSE)) break;
        }
        if (n != sizeof(frame)) break;   /* short/misaligned read -> pipe gone */

        process_frame(cfg->L, cfg->channel, &frame);
    }

    CloseHandle(ov.hEvent);

    call_lifecycle_fn(cfg->L, "finalize", cfg->channel, FALSE);
    lua_close(cfg->L);
    cfg->L = NULL;
    CloseHandle(hPipe);

    fprintf(stderr, "[gateway] Channel '%s' stopped\n", cfg->channel);
    return 0;
}

/* ---- CLI ------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <channel>=<script.lua> [<channel>=<script.lua> ...]\n"
        "       %s -h | --help\n"
        "\n"
        "Runs a CAN gateway: for each <channel>=<script.lua> mapping,\n"
        "connects to that channel's daemon RX pipe\n"
        "(\\\\.\\pipe\\serial_rx\\<channel>) and calls the Lua script's\n"
        "gateway(src_channel, frame) function for every received\n"
        "frame, forwarding the result to one or more TX pipes\n"
        "(\\\\.\\pipe\\serial_tx\\<dest_channel>).\n"
        "\n"
        "Script contract (globals the script may define):\n"
        "\n"
        "  initialize()\n"
        "      Called once when the channel starts. A non-zero return\n"
        "      (or a raised error) disables that one channel only.\n"
        "\n"
        "  gateway(src_channel, frame)\n"
        "      Called once per received frame. frame is a table with\n"
        "      fields id/ext/rtr/fd/brs/esi/dlc/len/data (data is a\n"
        "      1-indexed array of len bytes) -- the script may modify\n"
        "      these fields in place to edit the frame before it is\n"
        "      forwarded (dlc is recomputed from len automatically).\n"
        "\n"
        "      Return value:\n"
        "        nil / false                  drop the frame\n"
        "        \"channel\"                    forward the (possibly\n"
        "                                      edited) frame there\n"
        "        { \"ch1\", \"ch2\", ... }        fan out the same\n"
        "                                      edited frame to each\n"
        "        { {channel=\"ch1\", frame=f1},\n"
        "          \"ch2\", ... }                mix per-destination\n"
        "                                      frame overrides with\n"
        "                                      plain channel names\n"
        "\n"
        "  finalize()\n"
        "      Called once when the channel stops. Errors are logged\n"
        "      only, since the process is already shutting down.\n"
        "\n"
        "A channel whose script fails to load or initialize disables\n"
        "only that channel. A destination with no daemon listening\n"
        "drops frames addressed to it (retried periodically) without\n"
        "affecting any other destination.\n"
        "\n"
        "Options:\n"
        "  -h, --help    Show this help message and exit\n",
        prog, prog);
}

/* Parse <channel>=<script.lua> arguments into g_rx_channels, then
 * start one gateway_worker() thread per channel and wait for all of
 * them to exit (normally triggered by Ctrl+C via console_ctrl_handler,
 * propagated through g_running). */
int main(int argc, char *argv[])
{
    const char *prog = (argc > 0 && argv[0][0] != '\0') ? argv[0]
                                                          : "slcgw.exe";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "/?") == 0) {
            print_usage(prog);
            return 0;
        }
    }

    if (argc < 2) {
        fprintf(stderr,
                "[gateway] Error: at least one channel=script.lua "
                "mapping is required\n\n");
        print_usage(prog);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (g_rx_channel_count >= MAX_CHANNELS) {
            fprintf(stderr, "[gateway] Error: too many channels (max %d)\n\n",
                    MAX_CHANNELS);
            print_usage(prog);
            return 1;
        }

        const char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i]) {
            fprintf(stderr,
                    "[gateway] Error: invalid argument '%s' (expected "
                    "channel=script.lua)\n\n", argv[i]);
            print_usage(prog);
            return 1;
        }

        size_t chlen = (size_t)(eq - argv[i]);
        if (chlen >= CHANNEL_NAME_MAX) {
            fprintf(stderr, "[gateway] Error: channel name too long in '%s'\n\n",
                    argv[i]);
            print_usage(prog);
            return 1;
        }
        char chbuf[CHANNEL_NAME_MAX];
        memcpy(chbuf, argv[i], chlen);
        chbuf[chlen] = '\0';
        if (!slcan_valid_channel(chbuf)) {
            fprintf(stderr,
                    "[gateway] Error: invalid channel name '%s' (use "
                    "letters, digits, '_' or '-', non-empty)\n\n", chbuf);
            print_usage(prog);
            return 1;
        }

        const char *script = eq + 1;
        if (*script == '\0') {
            fprintf(stderr, "[gateway] Error: missing script path in '%s'\n\n",
                    argv[i]);
            print_usage(prog);
            return 1;
        }
        FILE *probe = fopen(script, "r");
        if (!probe) {
            fprintf(stderr, "[gateway] Error: cannot open script file: %s\n",
                    script);
            return 1;
        }
        fclose(probe);

        RxChannel *cfg = &g_rx_channels[g_rx_channel_count++];
        snprintf(cfg->channel, sizeof(cfg->channel), "%s", chbuf);
        snprintf(cfg->script_path, sizeof(cfg->script_path), "%s", script);
        cfg->L = NULL;
    }

    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE))
        fprintf(stderr,
                "[gateway] WARNING: could not install Ctrl+C handler "
                "(%lu); Ctrl+C will terminate without cleanup\n",
                GetLastError());

    g_tx_registry_mutex = CreateMutex(NULL, FALSE, NULL);

    HANDLE threads[MAX_CHANNELS];
    int    thread_count = 0;
    for (int i = 0; i < g_rx_channel_count; i++) {
        HANDLE h = CreateThread(NULL, 0, gateway_worker, &g_rx_channels[i], 0, NULL);
        if (!h) {
            fprintf(stderr,
                    "[gateway] Error: failed to start worker thread for "
                    "channel '%s': %lu\n", g_rx_channels[i].channel,
                    GetLastError());
            continue;
        }
        threads[thread_count++] = h;
    }

    if (thread_count == 0) {
        fprintf(stderr, "[gateway] Error: no worker threads could be started\n");
        if (g_tx_registry_mutex) CloseHandle(g_tx_registry_mutex);
        return 1;
    }

    fprintf(stderr, "[gateway] Running %d channel(s). Press Ctrl+C to stop.\n",
            thread_count);
    WaitForMultipleObjects((DWORD)thread_count, threads, TRUE, INFINITE);

    for (int i = 0; i < thread_count; i++)
        CloseHandle(threads[i]);

    close_all_tx_channels();
    if (g_tx_registry_mutex) CloseHandle(g_tx_registry_mutex);

    return 0;
}
