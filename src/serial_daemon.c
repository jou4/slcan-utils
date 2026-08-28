/*
 * serial_daemon.c
 *
 * Support:
 *   Classic CAN : t T r R  (Lawicel)
 *   CAN FD      : d D b B  (CANable 2.0)
 *
 * Named Pipe (per channel):
 *   \\.\pipe\serial_tx\<channel>   writer CLI -> daemon  (CanFrame)
 *   \\.\pipe\serial_rx\<channel>   daemon -> reader CLI  (CanFrame)
 *
 * Usage:
 *   serial_daemon.exe [COM] [channel] [arb_code] [data_code]
 *   serial_daemon.exe -h | --help
 *       serial_daemon.exe COM1 can0 6 5     (Arbitration Rate 500kbps, Data Rate 1Mbps)
 *       serial_daemon.exe COM1 can0 6       (Classic CAN 500kbps)
 *
 * BaudRate Code (Sn):
 *   0=10k 1=20k 2=50k 3=100k 4=125k 5=250k 6=500k 7=800k 8=1M
 *
 * FD DataRate Code (Yn / CANable 2.0):
 *   1=1M 2=2M 4=4M 5=5M
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "slcan.h"


#define DEFAULT_COM          "\\\\.\\COM1"
#define DEFAULT_CHANNEL      "can0"
#define DEFAULT_ARB_CODE     6      /* 500 kbps */
#define DEFAULT_DATA_CODE    (-1)   /* -1 = Classic CAN */
#define SERIAL_BAUD          CBR_115200

#define PIPE_TX_FORMAT       "\\\\.\\pipe\\serial_tx\\%s"
#define PIPE_RX_FORMAT       "\\\\.\\pipe\\serial_rx\\%s"

#define RING_SLOTS           256
#define QUEUE_SLOTS          128
#define RX_RAW_BUF           256


/* Single-producer/single-consumer ring buffer, one instance per RX
 * client (see RxClient below): rx_thread() is the only producer (via
 * rx_broadcast -> ring_push, one push per connected client), the
 * client's own rx_client_worker() is the only consumer (ring_pop).
 * head/tail are free-running counters (not masked on their own), so
 * RING_SLOTS must be a power of two for "% RING_SLOTS" to behave as
 * a proper wrap. event is signaled while head != tail (frames
 * pending) and reset when the ring drains, so a consumer can
 * WaitForSingleObject() on it instead of polling. */
typedef struct {
    CanFrame frames[RING_SLOTS];
    DWORD    head, tail;
    HANDLE   mutex;
    HANDLE   event;
} RingBuf;

/* Zero-initialize a RingBuf and create its mutex/event. Must be
 * called once before any ring_push()/ring_pop() on r. */
static void ring_init(RingBuf *r) {
    memset(r, 0, sizeof(*r));
    r->mutex = CreateMutex(NULL, FALSE, NULL);
    r->event = CreateEvent(NULL, TRUE, FALSE, NULL);
}

/* Append f to the ring. If the ring is already full (the consumer
 * hasn't kept up), the oldest unread frame is dropped to make room
 * rather than silently overwriting frames[tail % RING_SLOTS] in
 * place - with head left unmoved, that in-place overwrite would
 * corrupt whatever the next ring_pop() reads (it can land exactly on
 * the slot ring_pop() is about to consume), not just lose data. */
static void ring_push(RingBuf *r, const CanFrame *f) {
    WaitForSingleObject(r->mutex, INFINITE);
    if (r->tail - r->head >= RING_SLOTS) {
        r->head++;   /* drop oldest, keep the ring's ordering intact */
    }
    r->frames[r->tail % RING_SLOTS] = *f;
    r->tail++;
    ReleaseMutex(r->mutex);
    SetEvent(r->event);
}

/* Pop the oldest frame into *out. Returns FALSE (out left untouched)
 * if the ring is currently empty. */
static BOOL ring_pop(RingBuf *r, CanFrame *out) {
    WaitForSingleObject(r->mutex, INFINITE);
    if (r->head == r->tail) {
        ResetEvent(r->event);
        ReleaseMutex(r->mutex);
        return FALSE;
    }
    *out = r->frames[r->head % RING_SLOTS];
    r->head++;
    if (r->head == r->tail) ResetEvent(r->event);
    ReleaseMutex(r->mutex);
    return TRUE;
}


/* Single shared queue for frames arriving from any connected
 * serial_writer.c, drained by tx_thread() and written to the serial
 * port. Same free-running head/tail + mutex/event design as RingBuf
 * above, just auto-reset (event only needs to wake tx_thread once
 * per burst, not stay signaled while non-empty). */
typedef struct {
    CanFrame frames[QUEUE_SLOTS];
    DWORD    head, tail;
    HANDLE   mutex;
    HANDLE   event;
} TxQueue;

/* Zero-initialize a TxQueue and create its mutex/event. Must be
 * called once before any txq_push()/txq_pop() on q. */
static void txq_init(TxQueue *q) {
    memset(q, 0, sizeof(*q));
    q->mutex = CreateMutex(NULL, FALSE, NULL);
    q->event = CreateEvent(NULL, FALSE, FALSE, NULL);
}

/* Append f to the queue, dropping the oldest unsent frame first if
 * the queue is full (see ring_push()'s comment above - same reason:
 * overwriting frames[tail % QUEUE_SLOTS] in place without moving
 * head would corrupt, not just lose, an unsent frame). */
static void txq_push(TxQueue *q, const CanFrame *f) {
    WaitForSingleObject(q->mutex, INFINITE);
    if (q->tail - q->head >= QUEUE_SLOTS) {
        q->head++;
    }
    q->frames[q->tail % QUEUE_SLOTS] = *f;
    q->tail++;
    ReleaseMutex(q->mutex);
    SetEvent(q->event);
}

/* Pop the oldest queued frame into *out. Returns FALSE (out left
 * untouched) if the queue is currently empty. */
static BOOL txq_pop(TxQueue *q, CanFrame *out) {
    WaitForSingleObject(q->mutex, INFINITE);
    if (q->head == q->tail) {
        ReleaseMutex(q->mutex);
        return FALSE;
    }
    *out = q->frames[q->head % QUEUE_SLOTS];
    q->head++;
    ReleaseMutex(q->mutex);
    return TRUE;
}


static const char *g_pipe_tx, *g_pipe_rx;
static HANDLE  g_hSerial  = INVALID_HANDLE_VALUE;
static TxQueue g_txQueue;
static volatile BOOL g_running = TRUE;
static int     g_fd_mode  = 0;


/*
 * RX client registry
 *
 * Each connected serial_reader.c gets its own RingBuf so that every
 * connected reader receives a full copy of every frame (broadcast),
 * instead of frames being split across readers.  The list is guarded
 * by g_rxClientsMutex since readers can connect/disconnect at any time
 * from their own worker threads while rx_thread is broadcasting.
 */
typedef struct RxClient {
    HANDLE            hPipe;
    RingBuf           ring;
    volatile BOOL     active;
    struct RxClient  *next;
} RxClient;

static RxClient *g_rxClients      = NULL;
static HANDLE    g_rxClientsMutex = NULL;

/* Link a newly-connected RxClient into the head of g_rxClients.
 * Called once by rx_client_worker() right after it accepts a reader
 * connection, before that client can receive any broadcast frames. */
static void rx_client_add(RxClient *c)
{
    WaitForSingleObject(g_rxClientsMutex, INFINITE);
    c->next = g_rxClients;
    g_rxClients = c;
    ReleaseMutex(g_rxClientsMutex);
}

/* Unlink c from g_rxClients. Called once by rx_client_worker() right
 * before it tears the client down (closes its pipe/ring), so that no
 * later rx_broadcast() call can touch memory that's about to be
 * freed/invalidated. A no-op if c is already unlinked. */
static void rx_client_remove(RxClient *c)
{
    WaitForSingleObject(g_rxClientsMutex, INFINITE);
    RxClient **pp = &g_rxClients;
    while (*pp) {
        if (*pp == c) { *pp = c->next; break; }
        pp = &(*pp)->next;
    }
    ReleaseMutex(g_rxClientsMutex);
}

/* Push one decoded CAN frame into every currently-connected reader's
 * own RingBuf, so every reader sees the same stream independently
 * (see the RX client registry comment above RxClient). Called from
 * rx_thread() once per frame decoded off the serial port. */
static void rx_broadcast(const CanFrame *f)
{
    WaitForSingleObject(g_rxClientsMutex, INFINITE);
    for (RxClient *c = g_rxClients; c; c = c->next) {
        if (c->active) ring_push(&c->ring, f);
    }
    ReleaseMutex(g_rxClientsMutex);
}


/* Open the serial port for overlapped (async) I/O and configure it
 * for the SLCAN adapter's fixed line settings (115200-8N1, DTR
 * asserted so USB-CDC adapters that gate on DTR start passing data).
 * ReadIntervalTimeout=MAXDWORD with the other COMMTIMEOUTS fields
 * left at 0 makes ReadFile return immediately with whatever bytes
 * are already buffered instead of waiting to fill the caller's
 * buffer, which is what rx_thread()'s overlapped read loop expects.
 * Returns INVALID_HANDLE_VALUE on failure (check GetLastError()). */
static HANDLE open_serial(const char *port)
{
    HANDLE h = CreateFileA(port,
                           GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(h, &dcb);
    dcb.BaudRate    = SERIAL_BAUD;
    dcb.ByteSize    = 8;
    dcb.Parity      = NOPARITY;
    dcb.StopBits    = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    SetCommState(h, &dcb);

    COMMTIMEOUTS to = {0};
    to.ReadIntervalTimeout = MAXDWORD;
    SetCommTimeouts(h, &to);

    return h;
}


/* Write exactly len bytes of buf to h and block (up to 3s) until the
 * write completes, using ov for the overlapped WriteFile. Intended
 * for the short one-off SLCAN config commands sent by slcan_open()/
 * slcan_close(), not for the bulk data path (tx_thread has its own
 * overlapped write loop with its own timeout handling). ov must
 * already have a valid hEvent; this function resets and reuses it.
 * Returns TRUE only if the write both completed and wrote all len
 * bytes; FALSE on any error or a 3s timeout (the caller decides
 * whether that's fatal). */
static BOOL serial_write_sync(HANDLE h, const void *buf, DWORD len,
                               OVERLAPPED *ov)
{
    DWORD written = 0;
    ResetEvent(ov->hEvent);
    BOOL ok = WriteFile(h, buf, len, &written, ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov->hEvent, 3000);
        ok = GetOverlappedResult(h, ov, &written, FALSE);
    }
    return ok && (written == len);
}


/* Run the SLCAN adapter through its Lawicel/CANable 2.0 open
 * sequence: close any session already left open by a previous run
 * ("C"), drain whatever stale response it sends back for that, set
 * the arbitration bitrate ("Sn"), optionally set the CAN FD data
 * bitrate ("Yn") and flip on FD framing, then open the channel
 * ("O"). Each serial_write_sync() failure is logged but does not
 * abort the sequence - the adapter is still probed with the
 * remaining commands, since a single dropped write on an otherwise
 * healthy link is more likely than the link being fully dead, and
 * the following steps double as a retry signal in the logs if
 * something is actually wrong. Sets g_fd_mode when data_code >= 0. */
static void slcan_open(HANDLE h, int arb_code, int data_code)
{
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    uint8_t ack[8];
    DWORD   n;
    char    cmd[16];

	// close existing session
    if (!serial_write_sync(h, "C\r", 2, &ov))
        fprintf(stderr, "[daemon] WARNING: failed to send close command (C)\n");
    Sleep(100);

    /* drain whatever the adapter sent back for the close above; the
     * response itself is not meaningful, this just clears the input
     * buffer before we start sending the real config commands */
    ResetEvent(ov.hEvent);
    ReadFile(h, ack, sizeof(ack), &n, &ov);
    WaitForSingleObject(ov.hEvent, 200);
    GetOverlappedResult(h, &ov, &n, FALSE);

    snprintf(cmd, sizeof(cmd), "S%d\r", arb_code);
    if (!serial_write_sync(h, cmd, (DWORD)strlen(cmd), &ov))
        fprintf(stderr,
                "[daemon] WARNING: failed to set arbitration rate (S%d)\n",
                arb_code);
    Sleep(50);

    if (data_code >= 0) {
        snprintf(cmd, sizeof(cmd), "Y%d\r", data_code);
        if (!serial_write_sync(h, cmd, (DWORD)strlen(cmd), &ov))
            fprintf(stderr,
                    "[daemon] WARNING: failed to set data rate (Y%d)\n",
                    data_code);
        Sleep(50);
        g_fd_mode = 1;
        fprintf(stderr, "[daemon] CAN FD mode: arb=S%d data=Y%d\n",
                arb_code, data_code);
    } else {
        fprintf(stderr, "[daemon] Classic CAN mode: S%d\n", arb_code);
    }

    if (!serial_write_sync(h, "O\r", 2, &ov))
        fprintf(stderr, "[daemon] WARNING: failed to open SLCAN channel (O)\n");
    Sleep(100);

    CloseHandle(ov.hEvent);
    fprintf(stderr, "[daemon] SLCAN channel opened\n");
}

/* Send the SLCAN "close channel" command ("C") during shutdown, best
 * effort - by this point g_running is already FALSE and the daemon
 * is exiting regardless of whether the adapter acknowledges it. */
static void slcan_close(HANDLE h)
{
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!serial_write_sync(h, "C\r", 2, &ov))
        fprintf(stderr, "[daemon] WARNING: failed to send close command (C)\n");
    CloseHandle(ov.hEvent);
    fprintf(stderr, "[daemon] SLCAN channel closed\n");
}


/* Serial -> reader(s) pump. Overlapped-reads raw bytes off g_hSerial,
 * splits them into '\r'-terminated SLCAN lines (LF is ignored, an
 * empty line or a lone BEL '\a' - the adapter's error indicator -
 * is skipped), strips a trailing 'S' (ESI) suffix if present, then
 * decodes each line with slcan_decode() and broadcasts the resulting
 * frame to every connected reader via rx_broadcast(). Runs until
 * g_running is cleared or the serial handle errors out. A line
 * longer than SLCAN_LINE_MAX-1 is dropped (linepos reset) rather
 * than overflowing linebuf. */
static DWORD WINAPI rx_thread(LPVOID arg)
{
    (void)arg;
    uint8_t    raw[RX_RAW_BUF];
    OVERLAPPED ov  = {0};
    DWORD      n   = 0;
    char       linebuf[SLCAN_LINE_MAX];
    int        linepos = 0;

    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    while (g_running) {
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(g_hSerial, raw, sizeof(raw), &n, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                fprintf(stderr, "[rx] ReadFile error: %lu\n", err);
                break;
            }
            DWORD wait = WaitForSingleObject(ov.hEvent, 200);
            if (wait == WAIT_TIMEOUT)  continue;
            if (wait != WAIT_OBJECT_0) break;
            if (!GetOverlappedResult(g_hSerial, &ov, &n, FALSE)) break;
        }

        for (DWORD i = 0; i < n; i++) {
            char c = (char)raw[i];

            if (c == '\r') {
                linebuf[linepos] = '\0';
                linepos = 0;

                if (linebuf[0] == '\0') continue;
                if (linebuf[0] == '\a') continue;

				// ESI suffix
                int slen = (int)strlen(linebuf);
                uint8_t esi = 0;
                if (slen > 0 && linebuf[slen - 1] == 'S') {
                    esi = 1;
                    linebuf[slen - 1] = '\0';   /* remove 'S' for decode */
                }

                CanFrame frame;
                if (slcan_decode(linebuf, &frame) == 0) {
                    frame.esi = esi;
					rx_broadcast(&frame);
                }

            } else if (c != '\n') {
                if (linepos < SLCAN_LINE_MAX - 1)
                    linebuf[linepos++] = c;
                else
                    linepos = 0;    /* overflow */
            }
        }
    }

    CloseHandle(ov.hEvent);
    return 0;
}


/* Writer(s) -> serial pump. Drains g_txQueue (fed by every connected
 * serial_writer.c via tx_client_worker) and, for each frame, encodes
 * it to an SLCAN line with slcan_encode() and overlapped-writes it to
 * g_hSerial, one frame fully written before the next is attempted. An
 * FD frame arriving while the adapter wasn't opened in FD mode is
 * dropped with a warning rather than sent (the adapter wasn't told to
 * expect 'd'/'D'/'b'/'B' commands). A write that doesn't complete
 * within 5s is cancelled (CancelIo) and the frame is dropped so one
 * stuck write can't stall the whole queue indefinitely. Runs until
 * g_running is cleared or a non-timeout WriteFile error occurs. */
static DWORD WINAPI tx_thread(LPVOID arg)
{
    (void)arg;
    CanFrame   frame;
    char       slcan_line[SLCAN_LINE_MAX];
    OVERLAPPED ov = {0};
    DWORD      written = 0;

    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    while (g_running) {
        WaitForSingleObject(g_txQueue.event, 200);

        while (txq_pop(&g_txQueue, &frame)) {

			// received FD frame not in FD mode
            if (frame.fd && !g_fd_mode) {
                fprintf(stderr,
                        "[tx] WARNING: FD frame dropped (not in FD mode)\n");
                continue;
            }

            int len = slcan_encode(&frame, slcan_line, sizeof(slcan_line));
            if (len <= 0) {
                fprintf(stderr, "[tx] encode error\n");
                continue;
            }

            ResetEvent(ov.hEvent);
            BOOL ok = WriteFile(g_hSerial, slcan_line, (DWORD)len,
                                &written, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    fprintf(stderr, "[tx] WriteFile error: %lu\n", err);
                    goto cleanup;
                }
                DWORD wait = WaitForSingleObject(ov.hEvent, 5000);
                if (wait != WAIT_OBJECT_0) {
                    fprintf(stderr, "[tx] timeout\n");
                    CancelIo(g_hSerial);
                    continue;
                }
                GetOverlappedResult(g_hSerial, &ov, &written, FALSE);
            }
        }
    }

cleanup:
    CloseHandle(ov.hEvent);
    return 0;
}


/*
 * tx pipe: writer(s) -> daemon
 *
 * One worker thread per connected serial_writer.c. All workers push
 * into the single shared g_txQueue (already mutex-protected), so any
 * number of writers can be connected and sending frames concurrently.
 *
 * Both the pipe instance and this read loop use overlapped I/O (not
 * because the read itself needs to be async, but so it can be woken
 * on a timeout to re-check g_running - otherwise a connected-but-idle
 * writer would leave this thread blocked in a plain ReadFile forever,
 * with no way to notice a shutdown request).
 */
static DWORD WINAPI tx_client_worker(LPVOID arg)
{
    HANDLE     hPipe = (HANDLE)arg;
    CanFrame   frame;
    DWORD      n;
    OVERLAPPED ov = {0};

    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    fprintf(stderr, "[tx_pipe] writer connected\n");

    while (g_running) {
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(hPipe, &frame, sizeof(frame), &n, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) break;   /* real error or pipe closed */

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
        if (n != sizeof(frame)) break;   /* short/misaligned read: give up on this client */
        txq_push(&g_txQueue, &frame);
    }

    fprintf(stderr, "[tx_pipe] writer disconnected\n");
    CloseHandle(ov.hEvent);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    return 0;
}

/* Accept-loop for PIPE_TX: create one pipe instance, wait (with a
 * bounded poll rather than an indefinite block, again so shutdown
 * isn't stuck waiting for a writer that may never connect) for a
 * writer to connect, hand the connected instance off to its own
 * tx_client_worker thread, then immediately create a fresh instance
 * so the next writer can connect without waiting for this one. */
static DWORD WINAPI tx_pipe_listener(LPVOID arg)
{
    (void)arg;

    while (g_running) {
        HANDLE hPipe = CreateNamedPipeA(
			g_pipe_tx,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            0, sizeof(CanFrame) * 16, 0, NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "[tx_pipe] CreateNamedPipe error: %lu\n",
                    GetLastError());
            Sleep(500);
            continue;
        }

        //fprintf(stderr, "[tx_pipe] waiting for writer...\n");

        OVERLAPPED covl = {0};
        covl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        BOOL connected = ConnectNamedPipe(hPipe, &covl);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                DWORD wait;
                do {
                    wait = WaitForSingleObject(covl.hEvent, 200);
                } while (wait == WAIT_TIMEOUT && g_running);
                if (wait == WAIT_OBJECT_0) {
                    DWORD dummy;
                    connected = GetOverlappedResult(hPipe, &covl, &dummy, FALSE);
                } else {
                    CancelIo(hPipe);   /* shutting down before anyone connected */
                }
            }
        }
        CloseHandle(covl.hEvent);

        if (!connected) {
            if (g_running)
                fprintf(stderr, "[tx_pipe] ConnectNamedPipe error: %lu\n",
                        GetLastError());
            CloseHandle(hPipe);
            continue;
        }

        /* hand the connected instance to its own worker thread and
         * immediately loop back to create a fresh instance so the
         * next writer can connect without waiting for this one */
        HANDLE hThread = CreateThread(NULL, 0, tx_client_worker,
                                      (LPVOID)hPipe, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            fprintf(stderr, "[tx_pipe] CreateThread error: %lu\n",
                    GetLastError());
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
    }
    return 0;
}


/*
 * rx pipe: daemon -> reader(s)
 *
 * One worker thread per connected serial_reader.c, each with its own
 * RxClient/RingBuf registered in g_rxClients. rx_thread() broadcasts
 * every decoded frame to all registered clients (see rx_broadcast).
 */
static DWORD WINAPI rx_client_worker(LPVOID arg)
{
    HANDLE     hPipe = (HANDLE)arg;
    RxClient   client;
    OVERLAPPED ov = {0};
    CanFrame   frame;
    DWORD      written;

    memset(&client, 0, sizeof(client));
    ring_init(&client.ring);
    client.hPipe  = hPipe;
    client.active = TRUE;
    rx_client_add(&client);

    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    fprintf(stderr, "[rx_pipe] reader connected\n");

    while (g_running) {
        WaitForSingleObject(client.ring.event, 100);

        while (ring_pop(&client.ring, &frame)) {
            ResetEvent(ov.hEvent);
            BOOL ok = WriteFile(hPipe, &frame, sizeof(frame),
                                &written, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(ov.hEvent, 5000) != WAIT_OBJECT_0)
                        goto reader_disconnected;
                    if (!GetOverlappedResult(hPipe, &ov, &written, FALSE))
                        goto reader_disconnected;
                } else {
                    goto reader_disconnected;
                }
            }
        }
    }

reader_disconnected:
    fprintf(stderr, "[rx_pipe] reader disconnected\n");
    client.active = FALSE;
    rx_client_remove(&client);

    CloseHandle(ov.hEvent);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    CloseHandle(client.ring.mutex);
    CloseHandle(client.ring.event);
    return 0;
}

/* Accept-loop for PIPE_RX: same shape as tx_pipe_listener above -
 * create an instance, wait (bounded poll, not an indefinite block)
 * for a reader to connect, hand it off to rx_client_worker, and
 * immediately create the next instance. */
static DWORD WINAPI rx_pipe_listener(LPVOID arg)
{
    (void)arg;

    while (g_running) {
        HANDLE hPipe = CreateNamedPipeA(
			g_pipe_rx,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(CanFrame) * 16, 0, 0, NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "[rx_pipe] CreateNamedPipe error: %lu\n",
                    GetLastError());
            Sleep(500);
            continue;
        }

        //fprintf(stderr, "[rx_pipe] waiting for reader...\n");

        OVERLAPPED covl = {0};
        covl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        BOOL connected = ConnectNamedPipe(hPipe, &covl);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                DWORD wait;
                do {
                    wait = WaitForSingleObject(covl.hEvent, 200);
                } while (wait == WAIT_TIMEOUT && g_running);
                if (wait == WAIT_OBJECT_0) {
                    DWORD dummy;
                    connected = GetOverlappedResult(hPipe, &covl, &dummy, FALSE);
                } else {
                    CancelIo(hPipe);   /* shutting down before anyone connected */
                }
            }
        }
        CloseHandle(covl.hEvent);

        if (!connected) {
            if (g_running)
                fprintf(stderr, "[rx_pipe] ConnectNamedPipe error: %lu\n",
                        GetLastError());
            CloseHandle(hPipe);
            continue;
        }

        /* hand the connected instance to its own worker thread and
         * immediately loop back to create a fresh instance so the
         * next reader can connect without waiting for this one */
        HANDLE hThread = CreateThread(NULL, 0, rx_client_worker,
                                      (LPVOID)hPipe, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            fprintf(stderr, "[rx_pipe] CreateThread error: %lu\n",
                    GetLastError());
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
    }
    return 0;
}


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

/* Parse a decimal integer strictly (no partial matches like atoi's "6x"->6)
 * and check it falls within [min, max]. Returns 0 on success. */
static int parse_int_range(const char *s, int min, int max, int *out)
{
    if (!s || *s == '\0') return -1;

    char *end = NULL;
    long  v   = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;   /* not a clean integer */
    if (v < (long)min || v > (long)max) return -1;

    *out = (int)v;
    return 0;
}

/* data_code isn't a contiguous range (1=1M 2=2M 4=4M 5=5M), so it needs
 * set-membership checking rather than a min/max range. */
static int parse_data_code(const char *s, int *out)
{
    char *end = NULL;
    long  v;

    if (!s || *s == '\0') return -1;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    if (v != 1 && v != 2 && v != 4 && v != 5) return -1;

    *out = (int)v;
    return 0;
}

/* Print full --help text (usage, argument reference, rate-code
 * tables, resolved pipe-name examples for DEFAULT_CHANNEL) to
 * stderr. prog is the name to show in the usage line - pass argv[0],
 * or a hardcoded fallback if argv[0] is unavailable/empty. */
static void print_usage(const char *prog)
{
    char pipe_tx_ex[48], pipe_rx_ex[48];
    snprintf(pipe_tx_ex, sizeof(pipe_tx_ex), PIPE_TX_FORMAT, DEFAULT_CHANNEL);
    snprintf(pipe_rx_ex, sizeof(pipe_rx_ex), PIPE_RX_FORMAT, DEFAULT_CHANNEL);

    fprintf(stderr,
        "Usage: %s [COM] [channel] [arb_code] [data_code]\n"
        "       %s -h | --help\n"
        "\n"
        "Arguments (all optional, positional):\n"
        "  COM         Serial port name, e.g. COM1 (default: %s)\n"
        "  channel     CAN channel name, used to namespace the named pipes\n"
        "              so multiple daemons/ports can run side by side\n"
        "              (letters, digits, '_' or '-' only; default: %s)\n"
        "  arb_code    Arbitration bitrate code, 0-8 (default: %d = 500 kbps)\n"
        "  data_code   CAN FD data bitrate code: 1, 2, 4 or 5.\n"
        "              Omit for Classic CAN.\n"
        "\n"
        "Arbitration Rate Code (Sn):\n"
        "  0=10k 1=20k 2=50k 3=100k 4=125k 5=250k 6=500k 7=800k 8=1M\n"
        "\n"
        "FD Data Rate Code (Yn / CANable 2.0):\n"
        "  1=1M  2=2M  4=4M  5=5M\n"
        "\n"
        "Named Pipe (per channel, e.g. channel=%s):\n"
        "  %s   writer CLI -> daemon\n"
        "  %s   daemon -> reader CLI\n"
        "\n"
        "Examples:\n"
        "  %s COM1 can0 6 5   CAN FD  (Arbitration 500kbps, Data 1Mbps)\n"
        "  %s COM1 can0 6     Classic CAN 500kbps\n"
        "  %s                 Use defaults (%s, %s, code %d, Classic CAN)\n"
        "\n"
        "Options:\n"
        "  -h, --help      Show this help message and exit\n",
        prog, prog, DEFAULT_COM, DEFAULT_CHANNEL, DEFAULT_ARB_CODE,
        DEFAULT_CHANNEL, pipe_tx_ex, pipe_rx_ex,
        prog, prog, prog, DEFAULT_COM, DEFAULT_CHANNEL, DEFAULT_ARB_CODE);
}

/* Console control handler: on Ctrl+C, Ctrl+Break, or the console
 * window closing, clear g_running instead of letting the default
 * handler terminate the process immediately. Every long-running loop
 * in this file (rx_thread, tx_thread, and the pipe listeners/workers
 * via their bounded 200ms polls) checks g_running, so this is what
 * actually makes "Press Ctrl+C to stop" (see the message below) reach
 * main()'s WaitForMultipleObjects and the slcan_close()/CloseHandle
 * cleanup that follows it, instead of the process just vanishing
 * mid-write with the SLCAN channel left open on the adapter. */
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

/* Parse [COM] [channel] [arb_code] [data_code] / -h|--help, open and
 * configure the serial port, then start the four long-running
 * threads (rx_thread, tx_thread, tx_pipe_listener, rx_pipe_listener)
 * and block until all of them exit - normally triggered by Ctrl+C
 * via console_ctrl_handler() above, or by a fatal serial error. */
int main(int argc, char *argv[])
{
    const char *prog = (argc > 0 && argv[0][0] != '\0') ? argv[0]
                                                          : "slcd.exe";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "/?") == 0) {
            print_usage(prog);
            return 0;
        }
    }

    if (argc > 5) {
        fprintf(stderr, "[daemon] Error: too many arguments\n\n");
        print_usage(prog);
        return 1;
    }

    const char *port      = DEFAULT_COM;
    const char *ch        = DEFAULT_CHANNEL;
    int         arb_code  = DEFAULT_ARB_CODE;
    int         data_code = DEFAULT_DATA_CODE;

    char portbuf[16];

    if (argc >= 2) {
        if (argv[1][0] == '\0') {
            fprintf(stderr, "[daemon] Error: empty COM port name\n\n");
            print_usage(prog);
            return 1;
        }
        if (argv[1][0] != '\\') {
            int need = snprintf(portbuf, sizeof(portbuf), "\\\\.\\%s", argv[1]);
            if (need < 0 || need >= (int)sizeof(portbuf)) {
                fprintf(stderr, "[daemon] Error: COM port name too long: %s\n",
                        argv[1]);
                return 1;
            }
            port = portbuf;
        } else {
            port = argv[1];
        }
    }

	if (argc >= 3) {
        if (!is_valid_channel(argv[2])) {
            fprintf(stderr,
                    "[daemon] Error: invalid channel name '%s' "
                    "(use letters, digits, '_' or '-', non-empty)\n\n",
                    argv[2]);
            print_usage(prog);
            return 1;
        }
		ch = argv[2];
	}

    if (argc >= 4 && parse_int_range(argv[3], 0, 8, &arb_code) != 0) {
        fprintf(stderr,
                "[daemon] Error: invalid arb_code '%s' (must be an integer 0-8)\n\n",
                argv[3]);
        print_usage(prog);
        return 1;
    }

    if (argc >= 5 && parse_data_code(argv[4], &data_code) != 0) {
        fprintf(stderr,
                "[daemon] Error: invalid data_code '%s' (must be 1, 2, 4 or 5)\n\n",
                argv[4]);
        print_usage(prog);
        return 1;
    }

	char pipetxbuf[32], piperxbuf[32];
    int  need_tx = snprintf(pipetxbuf, sizeof(pipetxbuf), PIPE_TX_FORMAT, ch);
    int  need_rx = snprintf(piperxbuf, sizeof(piperxbuf), PIPE_RX_FORMAT, ch);
    if (need_tx < 0 || need_tx >= (int)sizeof(pipetxbuf) ||
        need_rx < 0 || need_rx >= (int)sizeof(piperxbuf)) {
        fprintf(stderr, "[daemon] Error: channel name too long: %s\n", ch);
        return 1;
    }
    g_pipe_tx = pipetxbuf;
    g_pipe_rx = piperxbuf;

    g_hSerial = open_serial(port);
    if (g_hSerial == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[daemon] Cannot open %s: %lu\n",
                port, GetLastError());
        return 1;
    }
    fprintf(stderr, "[daemon] Opened %s as %s\n", port, ch);

    txq_init(&g_txQueue);
	g_rxClientsMutex = CreateMutex(NULL, FALSE, NULL);

    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE))
        fprintf(stderr,
                "[daemon] WARNING: could not install Ctrl+C handler (%lu); "
                "Ctrl+C will terminate without cleanup\n", GetLastError());

    slcan_open(g_hSerial, arb_code, data_code);

    HANDLE threads[4];
    threads[0] = CreateThread(NULL, 0, rx_thread,      NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, tx_thread,      NULL, 0, NULL);
    threads[2] = CreateThread(NULL, 0, tx_pipe_listener, NULL, 0, NULL);
    threads[3] = CreateThread(NULL, 0, rx_pipe_listener, NULL, 0, NULL);

    fprintf(stderr, "[daemon] Running. Press Ctrl+C to stop.\n");
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);

    g_running = FALSE;
    slcan_close(g_hSerial);
    CloseHandle(g_hSerial);
	CloseHandle(g_rxClientsMutex);
    return 0;
}
