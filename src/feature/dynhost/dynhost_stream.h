/* SPDX-FileCopyrightText: 2026 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_stream.h
 * @brief Raw bidirectional byte streams to .onion services via embedded Tor.
 *
 * Provides dynhost_stream_open() — a thread-safe API for opening a raw
 * TCP-like byte stream to <onion>:<port> using Tor's internal circuit
 * machinery (connection_ap_make_link). No SOCKS proxy. No HTTP. Bytes
 * written with dynhost_stream_write() flow through the circuit as-is;
 * inbound bytes are delivered to the read callback as they arrive.
 *
 * Usage from embedding application (e.g., zclassic23):
 *
 *   dynhost_stream_t *s = dynhost_stream_open("abc...xyz.onion", 8233,
 *                            my_read_cb, my_event_cb, my_ctx, 300);
 *
 * Threading contract:
 *  - dynhost_stream_open/write/close may be called from ANY thread.
 *    Writes are queued under a mutex and flushed onto the circuit by
 *    Tor's event loop on its next scheduled tick (1s cadence).
 *  - Both callbacks fire ONLY on Tor's event loop thread.
 *  - Exactly one terminal event (DYNHOST_STREAM_EVENT_CLOSED or
 *    DYNHOST_STREAM_EVENT_TIMEOUT) fires per stream. After it fires,
 *    no further callbacks occur.
 *  - The handle is owned by the caller and released ONLY by
 *    dynhost_stream_close(). Call close() exactly once — before or
 *    after the terminal event — and never touch the handle after
 *    close() returns. After the terminal event, write() fails with -1
 *    and close() merely releases the handle.
 */

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_STREAM_H
#define TOR_FEATURE_DYNHOST_DYNHOST_STREAM_H

#include <stddef.h>
#include <stdint.h>

/** Opaque stream handle returned by dynhost_stream_open(). */
typedef struct dynhost_stream dynhost_stream_t;

/** The connection objects were created and the circuit is pending. */
#define DYNHOST_STREAM_EVENT_OPEN 0
/** The stream is established end-to-end: the remote side accepted the
 * connection and bytes may now flow in both directions. */
#define DYNHOST_STREAM_EVENT_CONNECTED 1
/** The stream closed: peer EOF, circuit/stream error, or local close.
 * Terminal event. */
#define DYNHOST_STREAM_EVENT_CLOSED 2
/** The stream exceeded its timeout. Terminal event; the connection is
 * torn down. */
#define DYNHOST_STREAM_EVENT_TIMEOUT 3

/**
 * Callback invoked on Tor's event loop thread when inbound bytes arrive.
 *
 * @param stream  The stream handle
 * @param data    Inbound bytes (freed after the callback returns; copy
 *                if needed)
 * @param len     Number of bytes (never 0)
 * @param ctx     Caller's context pointer
 */
typedef void (*dynhost_stream_read_fn)(dynhost_stream_t *stream,
                                       const uint8_t *data,
                                       size_t len,
                                       void *ctx);

/**
 * Callback invoked on Tor's event loop thread on stream lifecycle events
 * (DYNHOST_STREAM_EVENT_*).
 *
 * @param stream  The stream handle
 * @param event   One of DYNHOST_STREAM_EVENT_*
 * @param ctx     Caller's context pointer
 */
typedef void (*dynhost_stream_event_fn)(dynhost_stream_t *stream,
                                        int event,
                                        void *ctx);

/**
 * Queue opening a raw byte stream to a .onion address.
 *
 * Thread-safe: may be called from any thread. The open is processed by
 * Tor's event loop via dynhost_stream_process_pending().
 *
 * @param onion_address  Target .onion hostname (with or without suffix)
 * @param port           Target port
 * @param read_cb        Called as inbound bytes arrive
 * @param event_cb       Called on OPEN/CONNECTED/CLOSED/TIMEOUT
 * @param ctx            Caller's context
 * @param timeout_secs   Lifetime timeout in seconds (0 = default 60s).
 *                       The deadline applies to the whole stream, not
 *                       just connection setup; pass a large value for
 *                       long-lived streams.
 * @return stream handle, or NULL on invalid arguments
 */
dynhost_stream_t *dynhost_stream_open(const char *onion_address,
                                      uint16_t port,
                                      dynhost_stream_read_fn read_cb,
                                      dynhost_stream_event_fn event_cb,
                                      void *ctx,
                                      int timeout_secs);

/**
 * Queue bytes for transmission on the stream.
 *
 * Thread-safe: may be called from any thread. Bytes are queued under a
 * mutex and written to the circuit by Tor's event loop on its next tick.
 *
 * @return 0 if queued, -1 if the stream is closing/closed, the handle
 *         is invalid, or the queue cap (4 MiB) would be exceeded.
 */
int dynhost_stream_write(dynhost_stream_t *stream,
                         const uint8_t *data,
                         size_t len);

/**
 * Close the stream and release the handle.
 *
 * Thread-safe: may be called from any thread, at most once per stream.
 * If no terminal event has fired yet, the connection is torn down and
 * DYNHOST_STREAM_EVENT_CLOSED fires on Tor's event loop thread before
 * the handle is released. If a terminal event already fired, the handle
 * is released immediately with no further callbacks.
 */
void dynhost_stream_close(dynhost_stream_t *stream);

/**
 * Process pending opens, queued writes, and stream liveness. Called
 * from Tor's event loop (dynhost_run_scheduled_events). NOT thread-safe
 * — Tor main thread only.
 */
void dynhost_stream_process_pending(void);

/**
 * Close all streams, fire CLOSED for each, and release all state.
 * Called during shutdown. Tor main thread only.
 */
void dynhost_stream_cleanup(void);

struct dir_connection_t;
/**
 * Drain inbound bytes from a DIR_PURPOSE_DYNHOST_STREAM connection's
 * inbuf and deliver them to the stream's read callback. Called from
 * connection_dir_process_inbuf() before any HTTP/size-limit handling.
 * Tor main thread only.
 */
void dynhost_stream_deliver_inbuf(struct dir_connection_t *conn);

/**
 * Handle EOF on a DIR_PURPOSE_DYNHOST_STREAM connection: deliver any
 * remaining bytes and fire the terminal CLOSED event. Called from
 * connection_dir_reached_eof(). Tor main thread only.
 *
 * @return 0 always (the caller closes the connection regardless)
 */
int dynhost_stream_handle_eof(struct dir_connection_t *conn);

#endif /* TOR_FEATURE_DYNHOST_DYNHOST_STREAM_H */
