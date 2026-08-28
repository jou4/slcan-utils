/*
 * slcan_player.c
 *
 * Replay a candump-like log file (as produced by slcr.exe, e.g.
 * "slcr.exe can0 > log.txt") back onto the daemon(s)' TX named pipes,
 * reproducing the original relative timing between frames:
 *
 *   (1735689600.123456) can0 123#DEADBEEF
 *
 * One TX pipe is opened per distinct channel name found in the log
 * (\\.\pipe\serial_tx\<channel>), the first time a frame for that
 * channel is actually replayed, so a single log spanning multiple
 * channels/daemons can be replayed by one slcplay.exe process.
 *
 * The relative-timing replay scheduler (SlcTimeval + tv_cmp/tv_diff/
 * tv_add and the two-level replay loop in main()) works by
 * establishing a fixed offset between wall-clock time and the log's
 * own timestamps once per pass, then repeatedly projecting "now" into
 * log-time coordinates and sending every frame whose logged timestamp
 * has already been reached. Frame text is decoded with
 * slcan_parse_frame() from slcan.c (also used by serial_writer.c).
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "slcan.h"

#define PIPE_TX_FORMAT      "\\\\.\\pipe\\serial_tx\\%s"

#define MAX_CHANNELS        16
#define CHANNEL_NAME_MAX    64

#define LOG_LINE_MAX        512
#define LOG_FRAME_MAX       256

static volatile BOOL g_running = TRUE;

/* ---- gettimeofday()-style replay clock -------------------------- */

typedef struct {
    long sec;
    long usec;
} SlcTimeval;

/* Three-way compare of two SlcTimeval values: <0 if a is before b,
 * 0 if equal, >0 if a is after b. */
static int tv_cmp(const SlcTimeval *a, const SlcTimeval *b)
{
    if (a->sec < b->sec) return -1;
    if (a->sec > b->sec) return 1;
    return (int)(a->usec - b->usec);
}

/* diff = b - a. Not normalized (diff->usec may be negative); the only
 * consumer, tv_add(), normalizes it back on the way in. */
static void tv_diff(const SlcTimeval *a, const SlcTimeval *b, SlcTimeval *diff)
{
    diff->sec  = b->sec  - a->sec;
    diff->usec = b->usec - a->usec;
}

/* a += b, normalizing a->usec back into [0, 1000000). */
static void tv_add(SlcTimeval *a, const SlcTimeval *b)
{
    a->sec  += b->sec;
    a->usec += b->usec;

    if (a->usec < 0) {
        a->sec  -= 1;
        a->usec += 1000000L;
    }
    if (a->usec >= 1000000L) {
        a->sec  += 1;
        a->usec -= 1000000L;
    }
}

/* Read the current system time as (seconds, microseconds) since the
 * Unix epoch -- this project's gettimeofday() substitute (Windows has
 * no POSIX gettimeofday()). Same FILETIME/Unix-epoch conversion as
 * serial_reader.c's get_timestamp(), just returned as numeric fields
 * instead of a formatted string. Note: sec is a 32-bit long on
 * Windows, so this is subject to the year-2038 rollover. */
static void tv_now(SlcTimeval *tv)
{
    FILETIME       ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uint64_t us = (uli.QuadPart - 116444736000000000ULL) / 10;
    tv->sec  = (long)(us / 1000000);
    tv->usec = (long)(us % 1000000);
}

/* ---- per-channel TX pipe registry -------------------------------- */

typedef struct {
    char   name[CHANNEL_NAME_MAX];
    HANDLE hPipe;   /* INVALID_HANDLE_VALUE = connect failed; not retried */
} ChannelPipe;

static ChannelPipe g_channels[MAX_CHANNELS];
static int         g_channel_count = 0;

/* Optional channel allow-list from positional CLI arguments. Empty
 * (g_filter_count == 0) means "replay every channel found in the
 * log". */
static char g_filter[MAX_CHANNELS][CHANNEL_NAME_MAX];
static int  g_filter_count = 0;

static int channel_allowed(const char *name)
{
    if (g_filter_count == 0) return 1;
    for (int i = 0; i < g_filter_count; i++) {
        if (strcmp(g_filter[i], name) == 0) return 1;
    }
    return 0;
}

/* Look up the TX pipe handle for channel name, connecting to
 * \\.\pipe\serial_tx\<name> (waiting up to 5s for the daemon to be
 * listening) the first time this channel is seen. A channel that
 * fails to validate/connect is remembered as INVALID_HANDLE_VALUE so
 * later frames for it are skipped without retrying the connect (and
 * without repeating the warning) for the rest of the replay.
 * Returns INVALID_HANDLE_VALUE if the channel is unusable (already
 * warned about by this call or a previous one). */
static HANDLE get_channel_pipe(const char *name)
{
    for (int i = 0; i < g_channel_count; i++) {
        if (strcmp(g_channels[i].name, name) == 0) return g_channels[i].hPipe;
    }

    if (g_channel_count >= MAX_CHANNELS) {
        fprintf(stderr,
                "[player] Error: too many distinct channels in log "
                "(max %d), skipping '%s'\n", MAX_CHANNELS, name);
        return INVALID_HANDLE_VALUE;
    }

    ChannelPipe *ch = &g_channels[g_channel_count++];
    snprintf(ch->name, sizeof(ch->name), "%s", name);
    ch->hPipe = INVALID_HANDLE_VALUE;

    if (!slcan_valid_channel(name)) {
        fprintf(stderr,
                "[player] Warning: invalid channel name in log: '%s' "
                "(skipping its frames)\n", name);
        return INVALID_HANDLE_VALUE;
    }

    char pipebuf[32];
    int  need = snprintf(pipebuf, sizeof(pipebuf), PIPE_TX_FORMAT, name);
    if (need < 0 || need >= (int)sizeof(pipebuf)) {
        fprintf(stderr,
                "[player] Warning: channel name too long: '%s' "
                "(skipping its frames)\n", name);
        return INVALID_HANDLE_VALUE;
    }

    if (!WaitNamedPipeA(pipebuf, 5000)) {
        fprintf(stderr,
                "[player] Warning: daemon not running for channel '%s' "
                "(skipping its frames)\n", name);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE hPipe = CreateFileA(pipebuf, GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr,
                "[player] Warning: cannot open TX pipe for channel "
                "'%s': %lu (skipping its frames)\n", name, GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    fprintf(stderr, "[player] Connected to channel '%s'\n", name);
    ch->hPipe = hPipe;
    return hPipe;
}

/* Send one already-decoded frame to channel's TX pipe (connecting on
 * first use via get_channel_pipe()). A write failure disables that
 * channel for the rest of the replay (closes the handle and marks the
 * registry entry INVALID_HANDLE_VALUE) rather than aborting playback
 * of the other channels. */
static void send_to_channel(const char *name, const CanFrame *cf)
{
    HANDLE hPipe = get_channel_pipe(name);
    if (hPipe == INVALID_HANDLE_VALUE) return;

    DWORD written;
    if (!WriteFile(hPipe, cf, sizeof(*cf), &written, NULL)
        || written != sizeof(*cf)) {
        fprintf(stderr,
                "[player] Error: pipe write failed for channel '%s': "
                "%lu (channel disabled for rest of replay)\n",
                name, GetLastError());
        CloseHandle(hPipe);
        for (int i = 0; i < g_channel_count; i++) {
            if (strcmp(g_channels[i].name, name) == 0) {
                g_channels[i].hPipe = INVALID_HANDLE_VALUE;
                break;
            }
        }
    }
}

static void close_all_channels(void)
{
    for (int i = 0; i < g_channel_count; i++) {
        if (g_channels[i].hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(g_channels[i].hPipe);
            g_channels[i].hPipe = INVALID_HANDLE_VALUE;
        }
    }
}

/* ---- misc CLI helpers --------------------------------------------- */

/* Strict decimal parse of a non-negative integer (used for -l/-g
 * option values). Unlike atoi(), rejects trailing garbage ("5x") and
 * out-of-range values instead of silently truncating them.
 * Returns 1 and sets *out on success, 0 on any parse error. */
static int parse_nonneg_long(const char *s, long *out)
{
    if (!s || *s == '\0') return 0;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || errno == ERANGE || v < 0) return 0;
    *out = v;
    return 1;
}

/* Console control handler: on Ctrl+C, Ctrl+Break, or the console
 * window closing, clear g_running instead of letting the default
 * handler terminate the process immediately, so the replay loop in
 * main() notices and falls through to its normal cleanup (closing
 * every open TX pipe and the log file) instead of leaving pipe
 * handles open on an abrupt process exit. Mirrors serial_daemon.c's
 * console_ctrl_handler(). */
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

/* Print --help text to stderr. prog is the name to show in the usage
 * line -- pass argv[0], or a hardcoded fallback if unavailable. */
static void print_usage(const char *prog)
{
    char pipe_tx_ex[48];
    snprintf(pipe_tx_ex, sizeof(pipe_tx_ex), PIPE_TX_FORMAT, "can0");

    fprintf(stderr,
        "Usage: %s -I <infile> [options] [channel ...]\n"
        "       %s -h | --help\n"
        "\n"
        "Replays a candump-like log file (as produced by slcr.exe, e.g.\n"
        "\"slcr.exe can0 > log.txt\") back onto the daemon(s)' TX named\n"
        "pipes, reproducing the original relative timing between frames.\n"
        "Each log line looks like:\n"
        "\n"
        "  (1735689600.123456) can0 123#DEADBEEF\n"
        "\n"
        "One TX pipe (e.g. %s) is opened per distinct channel name found\n"
        "in the log, the first time a frame for that channel is\n"
        "replayed, so a single log spanning multiple channels/daemons\n"
        "can be replayed by one %s process. A channel whose daemon\n"
        "isn't listening is skipped (with a warning) rather than\n"
        "aborting the whole replay.\n"
        "\n"
        "Options:\n"
        "  -I <infile>   Log file to replay (required)\n"
        "  -l <num>      Replay the file this many times (default: 1).\n"
        "                Use 'i' to loop forever.\n"
        "  -g <ms>       Scheduler poll interval in milliseconds\n"
        "                (default: 1). Frames whose logged timestamp\n"
        "                has already elapsed are sent immediately; this\n"
        "                only bounds how often the schedule is\n"
        "                rechecked, not the gap between frames.\n"
        "  -h, --help    Show this help message and exit\n"
        "\n"
        "Arguments:\n"
        "  channel       Only replay lines for these channel(s) (may be\n"
        "                given more than once). Omit to replay every\n"
        "                channel found in the log.\n",
        prog, prog, pipe_tx_ex, prog);
}

/* ---- main ----------------------------------------------------------
 *
 * Parse -I/-l/-g and any positional channel filters, then replay the
 * log file's frames with their original relative timing, opening one
 * TX pipe per channel encountered on demand. Runs until the file is
 * exhausted count times, a malformed log line is hit, or Ctrl+C sets
 * g_running to FALSE (checked at every loop boundary below). */
#ifdef _MSC_VER
/* MSVC flags sscanf() as unsafe (C4996), pushing sscanf_s() instead.
 * Both calls below only use width-limited "%ld"/"%s" conversions into
 * fixed-size destinations (sec/usec longs, and chbuf/frbuf bounded by
 * their own field widths) -- see slcan.c's slcan_decode() for the same
 * reasoning -- so sscanf_s() would take the same argument list here
 * too and offers no real safety benefit. Silence the warning locally
 * rather than switching to the MSVC-only sscanf_s() (which would need
 * an #ifdef anyway to keep this file building under mingw-w64/gcc). */
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

int main(int argc, char *argv[])
{
    const char *prog = (argc > 0 && argv[0][0] != '\0') ? argv[0]
                                                          : "slcplay.exe";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "/?") == 0) {
            print_usage(prog);
            return 0;
        }
    }

    const char *filepath = NULL;
    long count = 1;   /* replay pass count; negative = infinite */
    long gap   = 1;   /* scheduler poll interval, ms */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-I") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[player] Error: missing value after -I\n\n");
                print_usage(prog);
                return 1;
            }
            filepath = argv[++i];

        } else if (strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[player] Error: missing value after -l\n\n");
                print_usage(prog);
                return 1;
            }
            i++;
            if (strcmp(argv[i], "i") == 0) {
                count = -1;
            } else if (!parse_nonneg_long(argv[i], &count) || count == 0) {
                fprintf(stderr,
                        "[player] Error: invalid -l value '%s' "
                        "(expected a positive integer or 'i')\n\n", argv[i]);
                print_usage(prog);
                return 1;
            }

        } else if (strcmp(argv[i], "-g") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[player] Error: missing value after -g\n\n");
                print_usage(prog);
                return 1;
            }
            i++;
            if (!parse_nonneg_long(argv[i], &gap)) {
                fprintf(stderr,
                        "[player] Error: invalid -g value '%s' "
                        "(expected a non-negative integer)\n\n", argv[i]);
                print_usage(prog);
                return 1;
            }

        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[player] Error: unknown option '%s'\n\n", argv[i]);
            print_usage(prog);
            return 1;

        } else {
            if (g_filter_count >= MAX_CHANNELS) {
                fprintf(stderr,
                        "[player] Error: too many channel filters "
                        "(max %d)\n\n", MAX_CHANNELS);
                print_usage(prog);
                return 1;
            }
            if (!slcan_valid_channel(argv[i])) {
                fprintf(stderr,
                        "[player] Error: invalid channel name '%s' "
                        "(use letters, digits, '_' or '-', non-empty)\n\n",
                        argv[i]);
                print_usage(prog);
                return 1;
            }
            snprintf(g_filter[g_filter_count], sizeof(g_filter[0]), "%s",
                      argv[i]);
            g_filter_count++;
        }
    }

    if (!filepath) {
        fprintf(stderr, "[player] Error: -I <infile> is required\n\n");
        print_usage(prog);
        return 1;
    }

    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE))
        fprintf(stderr,
                "[player] WARNING: could not install Ctrl+C handler (%lu); "
                "Ctrl+C will terminate without cleanup\n", GetLastError());

    FILE *infile = fopen(filepath, "r");
    if (!infile) {
        fprintf(stderr, "[player] Error: cannot open log file: %s\n",
                filepath);
        return 1;
    }

    fprintf(stderr, "[player] Replaying %s. Press Ctrl+C to stop.\n",
            filepath);

    char       buf[LOG_LINE_MAX];
    char       chbuf[CHANNEL_NAME_MAX];
    char       frbuf[LOG_FRAME_MAX];
    long       sec, usec;
    SlcTimeval base_tv, log_tv, diff_tv;
    /* NULL so an interrupted (g_running cleared) skip-loop below,
     * which then never assigns fret via fgets(), is correctly treated
     * the same as "nothing more to read" by the following "if (!fret)"
     * checks, instead of reading an indeterminate pointer. */
    char      *fret = NULL;
    int        fatal = 0;
    long       pass = 0;

    for (;;) {
        if (!g_running || fatal) break;
        if (!(count < 0 || pass < count)) break;
        pass++;

        /* Skip blank lines / anything not in "(sec.usec) ..." form. */
        while (g_running &&
               (fret = fgets(buf, sizeof(buf), infile)) != NULL &&
               buf[0] != '(') {
        }
        if (!fret) break;   /* nothing (more) to replay this pass */

        if (sscanf(buf, "(%ld.%ld) %63s %255s",
                   &sec, &usec, chbuf, frbuf) != 4) {
            fprintf(stderr,
                    "[player] Error: malformed log line, aborting: %s", buf);
            fatal = 1;
            break;
        }
        log_tv.sec = sec; log_tv.usec = usec;

        /* Fix the wall-clock <-> log-time offset for this pass, then
         * seed base_tv == log_tv so the first frame fires immediately. */
        tv_now(&base_tv);
        tv_diff(&base_tv, &log_tv, &diff_tv);
        base_tv = log_tv;

        int eof = 0;
        while (g_running && !fatal && !eof) {
            while (g_running && !fatal &&
                   tv_cmp(&base_tv, &log_tv) >= 0) {
                CanFrame cf;
                if (channel_allowed(chbuf)) {
                    if (slcan_parse_frame(frbuf, &cf) != 0) {
                        fprintf(stderr,
                                "[player] Warning: skipping unparsable "
                                "frame: %s\n", frbuf);
                    } else {
                        send_to_channel(chbuf, &cf);
                    }
                }

                while (g_running &&
                       (fret = fgets(buf, sizeof(buf), infile)) != NULL &&
                       buf[0] != '(') {
                }
                if (!fret) { eof = 1; break; }

                if (sscanf(buf, "(%ld.%ld) %63s %255s",
                           &sec, &usec, chbuf, frbuf) != 4) {
                    fprintf(stderr,
                            "[player] Error: malformed log line, "
                            "aborting: %s", buf);
                    fatal = 1;
                    break;
                }
                log_tv.sec = sec; log_tv.usec = usec;
            }
            if (eof || fatal || !g_running) break;

            Sleep((DWORD)gap);
            tv_now(&base_tv);
            tv_add(&base_tv, &diff_tv);
        }

        if (fatal || !g_running) break;
        rewind(infile);
    }

    int exit_code = fatal ? 1 : 0;
    close_all_channels();
    fclose(infile);
    return exit_code;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
