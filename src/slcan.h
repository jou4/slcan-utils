#pragma once
#include <stdint.h>


#define SLCAN_MAX_DLEN      8
#define SLCAN_FD_MAX_DLEN   64
#define SLCAN_LINE_MAX      150

#define CANFD_MAX_DLC       0x0F

/* CAN ID range limits, see ISO 11898-1: 11-bit standard, 29-bit
 * extended. Shared by slcan_encode()'s and slcan_parse_frame()'s
 * range checks. */
#define CAN_SFF_ID_MAX      0x7FFu
#define CAN_EFF_ID_MAX      0x1FFFFFFFu

/* id is only meaningful in the low 11 bits (ext=0) or low 29 bits
 * (ext=1); slcan_encode() rejects a frame whose id doesn't fit. */
typedef struct {
    uint32_t id;
    uint8_t  ext;   /* 0=Standard(11bit)  1=Extended(29bit) */
    uint8_t  rtr;   /* 0=data  1=remote (always 0 when FD) */
    uint8_t  fd;    /* 0=Classic CAN  1=CAN FD */
    uint8_t  brs;   /* Bit Rate Switch (For FD only) */
    uint8_t  esi;   /* Error State Indicator (For RX only) */
    uint8_t  dlc;   /* DLC Code 0x00-0x0F */
    uint8_t  len;   /* Data Length (convert from/to dlc) */
    uint8_t  data[SLCAN_FD_MAX_DLEN];
} CanFrame;

/* Convert a CAN FD DLC code (0x0-0xF) to its data length in bytes,
 * per the ISO 11898-1 FD length table (9=12B 10=16B 11=20B 12=24B
 * 13=32B 14=48B 15=64B; 0-8 map 1:1). Returns 0 for a dlc value
 * outside 0x0-0xF (cannot happen for a dlc produced by slcan_decode,
 * since it is always parsed from a single hex digit). */
static inline uint8_t canfd_dlc2len(uint8_t dlc)
{
    static const uint8_t table[16] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 12, 16, 20, 24, 32, 48, 64
    };
    return (dlc <= CANFD_MAX_DLC) ? table[dlc] : 0;
}

/* Inverse of canfd_dlc2len(): round a data length in bytes up to the
 * nearest CAN FD DLC code. Lengths above 64 are clamped to DLC 15
 * (64 bytes) rather than treated as an error, since callers already
 * bound f->len to SLCAN_FD_MAX_DLEN before reaching here. */
static inline uint8_t canfd_len2dlc(uint8_t len)
{
    if (len <= 8)  return len;
    if (len <= 12) return 9;
    if (len <= 16) return 10;
    if (len <= 20) return 11;
    if (len <= 24) return 12;
    if (len <= 32) return 13;
    if (len <= 48) return 14;
    return 15;
}

/* Encode one CanFrame as a Lawicel/CANable SLCAN command line
 * (see the format table at the top of slcan.c), terminated by '\r'
 * and NUL.
 *
 * f      - frame to encode; f->id must fit in 11 bits (ext=0) or
 *          29 bits (ext=1), and f->len must not exceed 8 (classic)
 *          or SLCAN_FD_MAX_DLEN (fd).
 * out    - destination buffer, at least SLCAN_LINE_MAX bytes
 *          recommended.
 * maxlen - size of out in bytes.
 *
 * Returns the encoded line length (excluding the terminating NUL,
 * including the trailing '\r') on success, or -1 if f/out are NULL,
 * f->id/f->len are out of range, or the encoded line would not fit
 * in maxlen bytes. */
int slcan_encode(const CanFrame *f, char *out, int maxlen);

/* Decode one SLCAN command line (as produced by slcan_encode(), or
 * received verbatim from a CANable/Lawicel adapter) into a CanFrame.
 * line must be NUL-terminated and must NOT include the trailing
 * '\r' (strip it first, as serial_daemon.c's rx_thread does when
 * splitting the raw serial stream into lines).
 *
 * Returns 0 on success. Returns -1 if line/out are NULL, the type
 * character is unrecognized, the header fields don't parse, or the
 * line is shorter than the data length its own DLC declares (a
 * truncated/corrupted line)  -  decoding never reads past the NUL
 * terminator of line. */
int slcan_decode(const char *line, CanFrame *out);

/* Check that s is safe to embed verbatim in a named pipe path: non-empty,
 * and restricted to letters, digits, '_' and '-' (no backslashes or
 * other control/path-special characters). Every slcan-utils CLI
 * (slcd/slcr/slcw/slcplay) validates its channel argument with this
 * before building \\.\pipe\serial_tx\<channel> / \\.\pipe\serial_rx\
 * <channel>. Returns 1 if valid, 0 otherwise (including s == NULL). */
int slcan_valid_channel(const char *s);

/* Parse one candump/SLCAN-CLI style frame text line into a CanFrame:
 *
 *   123#DEADBEEF          Classic CAN, standard (11-bit) ID
 *   00000123#DEADBEEF     Classic CAN, extended (29-bit) ID
 *   123#R                 Classic CAN, RTR
 *   123##DEADBEEF...      CAN FD, standard ID
 *   123##*DEADBEEF...     CAN FD, standard ID, BRS
 *   00000123##DEADBEEF    CAN FD, extended ID
 *   00000123##*DEADBEEF   CAN FD, extended ID, BRS
 *
 * line must contain a '#' separating the hex CAN ID from the
 * frame-type/data suffix; id and data digits are validated as hex (a
 * typo like "12G#.." is rejected rather than silently truncated by
 * strtoul()'s lenient parsing), and id must fit the 11-bit/29-bit
 * range implied by its digit count (8 hex digits selects extended).
 * Parsing stops at '\0', '\r', or '\n', so line need not be
 * newline-terminated. Shared by serial_writer.c (one frame per stdin
 * line) and slcan_player.c (the frame column of a replayed log line).
 *
 * Returns 0 on success, -1 on any parse error (f's contents are then
 * unspecified - the caller should not use it). */
int slcan_parse_frame(const char *line, CanFrame *f);
