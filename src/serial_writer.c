/*
 * serial_writer.c
 *
 * Read CAN / CAN FD Frame from stdin, then send to daemon
 *
 * Input Formats:
 *
 *   Classic CAN:
 *     123#DEADBEEF          Standard
 *     00000123#DEADBEEF     Extended
 *     123#R                 RTR
 *
 *   CAN FD (without BRS):
 *     123##DEADBEEF...      Standard FD
 *     00000123##DEADBEEF    Extended FD
 *
 *   CAN FD (with BRS):
 *     123##*DEADBEEF...     Standard FD + BRS
 *     00000123##*DEADBEEF   Extended FD + BRS
 *
 *   Comment (Ignored): start with # or ;
 *
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include "slcan.h"

#define DEFAULT_CHANNEL      "can0"
#define PIPE_TX_FORMAT       "\\\\.\\pipe\\serial_tx\\%s"

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

/* CAN ID range limits (ISO 11898-1: 11-bit standard, 29-bit
 * extended) - kept in sync with slcan.c's own encode-side check.
 * Rejecting an out-of-range ID here, at parse time, gives immediate
 * feedback to whoever is typing/piping frames into this CLI; without
 * it the frame would still get caught later by slcan_encode() inside
 * the daemon, but that failure is logged to the *daemon's* console
 * ("[tx] encode error"), which this writer's user may never see. */
#define CAN_SFF_ID_MAX  0x7FFu
#define CAN_EFF_ID_MAX  0x1FFFFFFFu

/* Parse one input line (see the format table in the file header
 * comment) into a CanFrame. line must contain a '#' separating the
 * hex CAN ID from the frame-type/data suffix; id and data digits are
 * validated as hex (a typo like "12G#.." is rejected rather than
 * silently truncated by strtoul()'s lenient parsing), and the id
 * must fit the 11-bit/29-bit range implied by its digit count.
 * Returns 0 on success, -1 on any parse error (f's contents are
 * then unspecified - the caller should not use it). */
static int parse_frame(const char *line, CanFrame *f)
{
    memset(f, 0, sizeof(*f));

    const char *hash = strchr(line, '#');
    if (!hash) return -1;

	// ID
    char id_str[16] = {0};
    size_t id_len = (size_t)(hash - line);
    if (id_len == 0 || id_len >= sizeof(id_str)) return -1;
    for (size_t i = 0; i < id_len; i++) {
        if (!isxdigit((unsigned char)line[i])) return -1;
    }
    memcpy(id_str, line, id_len);
    f->id = (uint32_t)strtoul(id_str, NULL, 16);

	// ID of Extended Frame
    f->ext = (id_len == 8) ? 1 : 0;

    if (f->id > (f->ext ? CAN_EFF_ID_MAX : CAN_SFF_ID_MAX)) return -1;

	// Frame Type
    const char *payload = hash + 1;

    if (*payload == '#') {
        f->fd  = 1;
        f->rtr = 0;
        payload++;
        if (*payload == '*') {
			// BRS
            f->brs = 1;
            payload++;
        }
    } else if (*payload == 'R' || *payload == 'r') {
		// RTR
        f->rtr = 1;
        f->fd  = 0;
        f->len = 0;
        f->dlc = 0;
        return 0;
    }

	// Data
    uint8_t maxlen = f->fd ? SLCAN_FD_MAX_DLEN : SLCAN_MAX_DLEN;
    while (*payload && *payload != '\r' && *payload != '\n'
           && f->len < maxlen) {
        if (*(payload + 1) == '\0' ||
            *(payload + 1) == '\r' ||
            *(payload + 1) == '\n') break;
        if (!isxdigit((unsigned char)payload[0]) ||
            !isxdigit((unsigned char)payload[1])) return -1;
        char hex[3] = { payload[0], payload[1], '\0' };
        f->data[f->len++] = (uint8_t)strtoul(hex, NULL, 16);
        payload += 2;
    }

    f->dlc = canfd_len2dlc(f->len);
    return 0;
}

/* Print --help text to stderr. prog is the name to show in the usage
 * line - pass argv[0], or a hardcoded fallback if unavailable. */
static void print_usage(const char *prog)
{
    char pipe_tx_ex[48];
    snprintf(pipe_tx_ex, sizeof(pipe_tx_ex), PIPE_TX_FORMAT, DEFAULT_CHANNEL);

    fprintf(stderr,
        "Usage: %s [channel]\n"
        "       %s -h | --help\n"
        "\n"
        "  channel   CAN channel name, must match the daemon's channel\n"
        "            (letters, digits, '_' or '-' only; default: %s)\n"
        "\n"
        "Reads CAN / CAN FD frame lines from stdin and sends them to the\n"
        "daemon's TX named pipe for that channel (e.g. %s).\n"
        "One frame per line:\n"
        "\n"
        "  Classic CAN:\n"
        "    123#DEADBEEF          Standard\n"
        "    00000123#DEADBEEF     Extended\n"
        "    123#R                 RTR\n"
        "\n"
        "  CAN FD (without BRS):\n"
        "    123##DEADBEEF...      Standard FD\n"
        "    00000123##DEADBEEF    Extended FD\n"
        "\n"
        "  CAN FD (with BRS):\n"
        "    123##*DEADBEEF...     Standard FD + BRS\n"
        "    00000123##*DEADBEEF   Extended FD + BRS\n"
        "\n"
        "  Comment (ignored): line starts with # or ;\n"
        "\n"
        "serial_daemon.exe must already be running for that channel.\n"
        "\n"
        "Options:\n"
        "  -h, --help   Show this help message and exit\n",
        prog, prog, DEFAULT_CHANNEL, pipe_tx_ex);
}

/* Parse [channel] / -h|--help, connect to the daemon's TX pipe for
 * that channel (\\.\pipe\serial_tx\<channel>, waiting up to 5s for
 * the daemon to be listening), then read frame lines from stdin
 * until EOF, parsing and forwarding each one via the pipe. A parse
 * error is logged and that line is skipped (the connection stays
 * up); a pipe write error is fatal (the loop stops - the daemon side
 * of the connection is presumed gone). */
int main(int argc, char *argv[])
{
    const char *prog = (argc > 0 && argv[0][0] != '\0') ? argv[0]
                                                          : "slcw.exe";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "/?") == 0) {
            print_usage(prog);
            return 0;
        }
    }
    if (argc > 2) {
        fprintf(stderr, "[writer] Error: too many arguments\n\n");
        print_usage(prog);
        return 1;
    }

	const char *ch = DEFAULT_CHANNEL;
    if (argc >= 2) {
        if (!is_valid_channel(argv[1])) {
            fprintf(stderr,
                    "[writer] Error: invalid channel name '%s' "
                    "(use letters, digits, '_' or '-', non-empty)\n\n",
                    argv[1]);
            print_usage(prog);
            return 1;
        }
        ch = argv[1];
    }

    char pipebuf[32];
    int  need = snprintf(pipebuf, sizeof(pipebuf), PIPE_TX_FORMAT, ch);
    if (need < 0 || need >= (int)sizeof(pipebuf)) {
        fprintf(stderr, "[writer] Error: channel name too long: %s\n", ch);
        return 1;
    }
	const char *pipe_tx = pipebuf;

    if (!WaitNamedPipeA(pipe_tx, 5000)) {
        fprintf(stderr, "[writer] daemon not running\n");
        return 1;
    }

    HANDLE hPipe = CreateFileA(pipe_tx,
                               GENERIC_WRITE,
                               0, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[writer] Cannot open pipe: %lu\n", GetLastError());
        return 1;
    }

    char     line[256];
    CanFrame frame;
    DWORD    written;

    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '\n' || line[0] == '\r') continue;
        if (line[0] == '#'  || line[0] == ';')  continue;

        if (parse_frame(line, &frame) != 0) {
            fprintf(stderr, "[writer] parse error: %s", line);
            continue;
        }

        if (!WriteFile(hPipe, &frame, sizeof(frame), &written, NULL)
            || written != sizeof(frame)) {
            fprintf(stderr, "[writer] pipe write error: %lu\n",
                    GetLastError());
            break;
        }
    }

    CloseHandle(hPipe);
    return 0;
}
