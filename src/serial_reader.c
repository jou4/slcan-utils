/*
 * serial_reader.c
 *
 * Receive CanFrame from daemon, then output to stdout
 *
 * Outpur Formats:
 *
 *   Classic:
 *     (timestamp) can0 123#DEADBEEF
 *
 *   CAN FD (without BRS):
 *     (timestamp) can0 123##DEADBEEF...
 *
 *   CAN FD (with BRS):
 *     (timestamp) can0 123##*DEADBEEF...
 *
 *   ESI Flag (error passive):
 *     (timestamp) can0 123##*!DEADBEEF...   (* = BRS, ! = ESI)
 *
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "slcan.h"

#define DEFAULT_CHANNEL      "can0"
#define PIPE_RX_FORMAT       "\\\\.\\pipe\\serial_rx\\%s"

/* Channel name is embedded verbatim in the named pipe path, so keep it
 * restricted to a safe charset (no backslashes / control chars) and
 * non-empty. */
static int is_valid_channel(const char *s)
{
    if (!s || *s == '\0') return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Format the current system time as "(seconds.microseconds)", e.g.
 * "(1735689600.123456)" - a candump-style Unix timestamp with
 * microsecond resolution. 116444736000000000ULL is the number of
 * 100ns FILETIME ticks between the Windows epoch (1601-01-01) and
 * the Unix epoch (1970-01-01), used to convert FILETIME to Unix
 * time before splitting into seconds/microseconds. buf must be at
 * least ~24 bytes; buflen is the usual snprintf destination size. */
static void get_timestamp(char *buf, int buflen)
{
    FILETIME       ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uint64_t us = (uli.QuadPart - 116444736000000000ULL) / 10;
    snprintf(buf, buflen, "(%llu.%06llu)",
             (unsigned long long)(us / 1000000),
             (unsigned long long)(us % 1000000));
}

/* Print one received frame to stdout in candump-like text form (see
 * the format table in the file header comment above). channel is the
 * CAN interface name to print in the second field - it must be the
 * channel this reader actually connected to (passed down from main's
 * ch), not a hardcoded name, since a reader can be pointed at any
 * channel via its command-line argument. */
static void print_frame(const CanFrame *f, const char *channel)
{
    char ts[32];
    get_timestamp(ts, sizeof(ts));

    /* ID */
    if (f->ext)
        printf("%s %s %08X#", ts, channel, f->id);
    else
        printf("%s %s %03X#", ts, channel, f->id);

    if (f->rtr) {
        /* RTR */
        printf("R\n");
        return;
    }

    if (f->fd) {
        /* CAN FD: ## + BRS(*) + ESI(!) */
        printf("#");
        if (f->brs) printf("*");
        if (f->esi) printf("!");
        for (int i = 0; i < f->len; i++)
            printf("%02X", f->data[i]);
    } else {
        /* Classic CAN */
        for (int i = 0; i < f->len; i++)
            printf("%02X", f->data[i]);
    }

    printf("\n");
    fflush(stdout);
}

/* Print --help text to stderr. prog is the name to show in the usage
 * line - pass argv[0], or a hardcoded fallback if unavailable. */
static void print_usage(const char *prog)
{
    char pipe_rx_ex[48];
    snprintf(pipe_rx_ex, sizeof(pipe_rx_ex), PIPE_RX_FORMAT, DEFAULT_CHANNEL);

    fprintf(stderr,
        "Usage: %s [channel]\n"
        "       %s -h | --help\n"
        "\n"
        "  channel   CAN channel name, must match the daemon's channel\n"
        "            (letters, digits, '_' or '-' only; default: %s)\n"
        "\n"
        "Connects to the daemon's RX named pipe for that channel\n"
        "(e.g. %s), receives CanFrame records, and prints them to\n"
        "stdout in candump log file format:\n"
        "\n"
        "  Classic:              (timestamp) can0 123#DEADBEEF\n"
        "  CAN FD (no BRS):      (timestamp) can0 123##DEADBEEF...\n"
        "  CAN FD (BRS):         (timestamp) can0 123##*DEADBEEF...\n"
        "  ESI (error passive):  (timestamp) can0 123##*!DEADBEEF...\n"
        "\n"
        "serial_daemon.exe must already be running for that channel.\n"
        "\n"
        "Options:\n"
        "  -h, --help   Show this help message and exit\n",
        prog, prog, DEFAULT_CHANNEL, pipe_rx_ex);
}

/* Parse [channel] / -h|--help, connect to the daemon's RX pipe for
 * that channel (\\.\pipe\serial_rx\<channel>, waiting up to 5s for
 * the daemon to be listening), then print every CanFrame received
 * from it until the pipe closes (daemon exit) or a short/misaligned
 * read occurs. */
int main(int argc, char *argv[])
{
    const char *prog = (argc > 0 && argv[0][0] != '\0') ? argv[0]
                                                          : "slcr.exe";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "/?") == 0) {
            print_usage(prog);
            return 0;
        }
    }
    if (argc > 2) {
        fprintf(stderr, "[reader] Error: too many arguments\n\n");
        print_usage(prog);
        return 1;
    }

	const char *ch = DEFAULT_CHANNEL;
    if (argc >= 2) {
        if (!is_valid_channel(argv[1])) {
            fprintf(stderr,
                    "[reader] Error: invalid channel name '%s' "
                    "(use letters, digits, '_' or '-', non-empty)\n\n",
                    argv[1]);
            print_usage(prog);
            return 1;
        }
        ch = argv[1];
    }

    char pipebuf[32];
    int  need = snprintf(pipebuf, sizeof(pipebuf), PIPE_RX_FORMAT, ch);
    if (need < 0 || need >= (int)sizeof(pipebuf)) {
        fprintf(stderr, "[reader] Error: channel name too long: %s\n", ch);
        return 1;
    }
	const char *pipe_rx = pipebuf;

    if (!WaitNamedPipeA(pipe_rx, 5000)) {
        fprintf(stderr, "[reader] daemon not running\n");
        return 1;
    }

    HANDLE hPipe = CreateFileA(pipe_rx,
                               GENERIC_READ,
                               0, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[reader] Cannot open pipe: %lu\n", GetLastError());
        return 1;
    }

    CanFrame frame;
    DWORD    n;

    while (ReadFile(hPipe, &frame, sizeof(frame), &n, NULL)
           && n == sizeof(frame)) {
        print_frame(&frame, ch);
    }

    CloseHandle(hPipe);
    return 0;
}
