#include "slcan.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* CAN_SFF_ID_MAX / CAN_EFF_ID_MAX (11-bit / 29-bit range limits) are
 * defined in slcan.h -- used below to reject a CanFrame whose id
 * doesn't actually fit the frame's ext flag before we format it into
 * a fixed-width hex field ("%03X"/"%08X" don't truncate an oversized
 * value, they just widen the output, which would produce a malformed
 * SLCAN line), and again in slcan_parse_frame()'s own range check. */

/* -------------------------------------------------------
 * slcan_encode
 *
 * Classic CAN:
 *   t<ID3><DLC><DATA>\r   Standard
 *   T<ID8><DLC><DATA>\r   Extended
 *   r<ID3><DLC>\r         Standard RTR
 *   R<ID8><DLC>\r         Extended RTR
 *
 * CAN FD:
 *   d<ID3><DLC><DATA>\r   Standard FD
 *   D<ID8><DLC><DATA>\r   Extended FD
 *   b<ID3><DLC><DATA>\r   Standard FD (BRS)
 *   B<ID8><DLC><DATA>\r   Extended FD (BRS)
 *
 * DLC Code: 0-8 = Classic, 9=12B A=16B B=20B C=24B D=32B E=48B F=64B
 *
 * See slcan.h for the full parameter/return-value contract.
 * ------------------------------------------------------- */
int slcan_encode(const CanFrame *f, char *out, int maxlen)
{
    char tmp[SLCAN_LINE_MAX];
    int  pos = 0;
    uint8_t dlc_code;

    if (!f || !out || maxlen <= 0) return -1;

    /* id must fit the field width ("%03X"/"%08X" below don't
     * truncate an oversized value, they'd just print more hex
     * digits than the frame type allows). */
    if (f->id > (f->ext ? CAN_EFF_ID_MAX : CAN_SFF_ID_MAX)) return -1;

    if (f->fd) {
		// CAN FD
        dlc_code = canfd_len2dlc(f->len);

        if (f->ext) {
            char type = f->brs ? 'B' : 'D';
            pos = snprintf(tmp, sizeof(tmp), "%c%08X%X",
                           type, f->id, dlc_code);
        } else {
            char type = f->brs ? 'b' : 'd';
            pos = snprintf(tmp, sizeof(tmp), "%c%03X%X",
                           type, f->id, dlc_code);
        }
        for (int i = 0; i < f->len && i < SLCAN_FD_MAX_DLEN; i++)
            pos += snprintf(tmp + pos, sizeof(tmp) - pos,
                            "%02X", f->data[i]);

    } else if (f->rtr) {
		// RTR
        dlc_code = f->dlc;
        if (f->ext)
            pos = snprintf(tmp, sizeof(tmp), "R%08X%X", f->id, dlc_code);
        else
            pos = snprintf(tmp, sizeof(tmp), "r%03X%X", f->id, dlc_code);

    } else {
		// Classic
        dlc_code = (f->len < 8) ? f->len : 8;
        if (f->ext)
            pos = snprintf(tmp, sizeof(tmp), "T%08X%X", f->id, dlc_code);
        else
            pos = snprintf(tmp, sizeof(tmp), "t%03X%X", f->id, dlc_code);

        for (int i = 0; i < (int)dlc_code; i++)
            pos += snprintf(tmp + pos, sizeof(tmp) - pos,
                            "%02X", f->data[i]);
    }

    if (pos + 2 > maxlen) return -1;
    tmp[pos++] = '\r';
    tmp[pos]   = '\0';
    memcpy(out, tmp, pos + 1);
    return pos;
}


/* Decode one ASCII hex byte "XX" at p[0..1] into its numeric value.
 * Caller (slcan_decode) is responsible for guaranteeing p[0] and
 * p[1] are both valid hex digits within bounds first (see
 * hex_span_valid below)  -  this function does not itself validate
 * or report a parse failure, it just mirrors strtoul()'s lenient
 * "stop at the first non-hex char" behavior. */
static uint8_t hex2byte(const char *p)
{
    char hex[3] = { p[0], p[1], '\0' };
    return (uint8_t)strtoul(hex, NULL, 16);
}

/* Check that the nbytes*2 characters at p are all valid hex digits,
 * so the data loops below never decode uninitialized/stale bytes
 * from a line whose declared DLC doesn't match what was actually
 * received. Callers must already know p[0..nbytes*2-1] are within
 * bounds (see the strlen() check in slcan_decode). */
static int hex_span_valid(const char *p, int nbytes)
{
    for (int i = 0; i < nbytes * 2; i++) {
        if (!isxdigit((unsigned char)p[i])) return 0;
    }
    return 1;
}

#ifdef _MSC_VER
/* MSVC flags sscanf() as unsafe (C4996), pushing sscanf_s() instead.
 * Every call below only uses width-limited "%X" (numeric) conversions
 * into fixed unsigned int* destinations -- no "%s"/"%c"/"%[" that could
 * overflow a buffer -- so sscanf_s() would take the exact same argument
 * list and offers no real safety benefit here. Silence the warning
 * locally rather than switching to the MSVC-only sscanf_s() (which
 * would need an #ifdef anyway to keep this file building under
 * mingw-w64/gcc). */
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

int slcan_decode(const char *line, CanFrame *out)
{
    if (!line || !out || line[0] == '\0') return -1;
    memset(out, 0, sizeof(*out));

    char     type = line[0];
    unsigned id   = 0;
    unsigned dlc_code = 0;
    size_t   linelen = strlen(line);

    switch (type) {

		// Classic
		case 't':
			out->fd = 0; out->ext = 0; out->rtr = 0;
			if (sscanf(line + 1, "%3X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = (out->dlc > 8) ? 8 : (uint8_t)dlc_code;
			/* reject a line too short for the data it claims to carry,
			 * rather than reading past what was actually received */
			if (linelen < 5 + (size_t)out->len * 2) return -1;
			if (!hex_span_valid(line + 5, out->len)) return -1;
			for (int i = 0; i < out->len; i++)
				out->data[i] = hex2byte(line + 5 + i * 2);
			break;

		case 'T':
			out->fd = 0; out->ext = 1; out->rtr = 0;
			if (sscanf(line + 1, "%8X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = (out->dlc > 8) ? 8 : (uint8_t)dlc_code;
			if (linelen < 10 + (size_t)out->len * 2) return -1;
			if (!hex_span_valid(line + 10, out->len)) return -1;
			for (int i = 0; i < out->len; i++)
				out->data[i] = hex2byte(line + 10 + i * 2);
			break;

		case 'r':
			out->fd = 0; out->ext = 0; out->rtr = 1;
			if (sscanf(line + 1, "%3X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = 0;
			break;

		case 'R':
			out->fd = 0; out->ext = 1; out->rtr = 1;
			if (sscanf(line + 1, "%8X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = 0;
			break;

		// CAN FD
		case 'd':   /* Standard FD */
			out->fd = 1; out->ext = 0; out->brs = 0;
			if (sscanf(line + 1, "%3X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = canfd_dlc2len(out->dlc);
			if (linelen < 5 + (size_t)out->len * 2) return -1;
			if (!hex_span_valid(line + 5, out->len)) return -1;
			for (int i = 0; i < out->len; i++)
				out->data[i] = hex2byte(line + 5 + i * 2);
			break;

		case 'D':   /* Extended FD */
			out->fd = 1; out->ext = 1; out->brs = 0;
			if (sscanf(line + 1, "%8X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = canfd_dlc2len(out->dlc);
			if (linelen < 10 + (size_t)out->len * 2) return -1;
			if (!hex_span_valid(line + 10, out->len)) return -1;
			for (int i = 0; i < out->len; i++)
				out->data[i] = hex2byte(line + 10 + i * 2);
			break;

		case 'b':   /* Standard FD BRS */
			out->fd = 1; out->ext = 0; out->brs = 1;
			if (sscanf(line + 1, "%3X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = canfd_dlc2len(out->dlc);
			if (linelen < 5 + (size_t)out->len * 2) return -1;
			if (!hex_span_valid(line + 5, out->len)) return -1;
			for (int i = 0; i < out->len; i++)
				out->data[i] = hex2byte(line + 5 + i * 2);
			break;

		case 'B':   /* Extended FD BRS */
			out->fd = 1; out->ext = 1; out->brs = 1;
			if (sscanf(line + 1, "%8X%1X", &id, &dlc_code) != 2) return -1;
			out->id  = id;
			out->dlc = (uint8_t)dlc_code;
			out->len = canfd_dlc2len(out->dlc);
			if (linelen < 10 + (size_t)out->len * 2) return -1;
			if (!hex_span_valid(line + 10, out->len)) return -1;
			for (int i = 0; i < out->len; i++)
				out->data[i] = hex2byte(line + 10 + i * 2);
			break;

			/* ESI ('S' suffix) is not handled here: serial_daemon.c's
			 * rx_thread strips the trailing 'S' and sets out->esi
			 * itself before this decoded frame is broadcast, since
			 * ESI is a suffix on the whole line, not part of any
			 * single type's fixed-width header/data fields above. */

		default:
			return -1;
	}

    return 0;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

/* Channel name is embedded verbatim in a named pipe path, so keep it
 * restricted to a safe charset (no backslashes / control chars) and
 * non-empty. Shared by every slcan-utils CLI; see slcan.h. */
int slcan_valid_channel(const char *s)
{
    if (!s || *s == '\0') return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* See slcan.h for the full parameter/return-value contract. Shared by
 * serial_writer.c (one frame per stdin line) and slcan_player.c (the
 * frame column of a replayed log line). */
int slcan_parse_frame(const char *line, CanFrame *f)
{
    memset(f, 0, sizeof(*f));

    const char *hash = strchr(line, '#');
    if (!hash) return -1;

    /* ID */
    char id_str[16] = {0};
    size_t id_len = (size_t)(hash - line);
    if (id_len == 0 || id_len >= sizeof(id_str)) return -1;
    for (size_t i = 0; i < id_len; i++) {
        if (!isxdigit((unsigned char)line[i])) return -1;
    }
    memcpy(id_str, line, id_len);
    f->id = (uint32_t)strtoul(id_str, NULL, 16);

    /* ID of Extended Frame */
    f->ext = (id_len == 8) ? 1 : 0;

    if (f->id > (f->ext ? CAN_EFF_ID_MAX : CAN_SFF_ID_MAX)) return -1;

    /* Frame Type */
    const char *payload = hash + 1;

    if (*payload == '#') {
        f->fd  = 1;
        f->rtr = 0;
        payload++;
        if (*payload == '*') {
            /* BRS */
            f->brs = 1;
            payload++;
        }
    } else if (*payload == 'R' || *payload == 'r') {
        /* RTR */
        f->rtr = 1;
        f->fd  = 0;
        f->len = 0;
        f->dlc = 0;
        return 0;
    }

    /* Data */
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
