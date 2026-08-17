/* SPDX-FileCopyrightText: 2026 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_client.h
 * @brief Outbound HTTP client for .onion services via embedded Tor.
 *
 * Provides dynhost_client_fetch() — a thread-safe API for making HTTP
 * GET requests to .onion addresses using Tor's internal circuit machinery.
 * No SOCKS proxy. No external ports. Pure C function calls.
 *
 * Usage from embedding application (e.g., zclassic23):
 *
 *   dynhost_client_fetch("abc...xyz.onion", 80, "/directory.json",
 *                        my_callback, my_ctx, 60);
 *
 * The callback is invoked from Tor's event loop thread. The caller
 * must handle thread safety (e.g., atomic flags, mutexes).
 */

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_CLIENT_H
#define TOR_FEATURE_DYNHOST_DYNHOST_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/**
 * Callback invoked when a fetch completes (success or failure).
 *
 * @param status    HTTP status code (200, 404, etc.) or -1 on error
 * @param body      Response body (NULL on error). Caller must copy if needed —
 *                  the buffer is freed after the callback returns.
 * @param body_len  Length of response body
 * @param ctx       Caller's context pointer
 */
typedef void (*dynhost_client_callback_fn)(int status,
                                           const uint8_t *body,
                                           size_t body_len,
                                           void *ctx);

/**
 * Queue an HTTP GET request to a .onion address.
 *
 * Thread-safe: may be called from any thread. The request is queued
 * and processed by Tor's event loop via dynhost_client_process_pending().
 *
 * @param onion_address  Target .onion hostname (with or without .onion suffix)
 * @param port           Target port (typically 80)
 * @param path           HTTP path (e.g., "/directory.json")
 * @param callback       Called when response arrives or timeout
 * @param ctx            Caller's context
 * @param timeout_secs   Timeout in seconds (0 = default 60s)
 * @return 0 on success (queued), -1 if Tor not initialized
 */
int dynhost_client_fetch(const char *onion_address,
                         uint16_t port,
                         const char *path,
                         dynhost_client_callback_fn callback,
                         void *ctx,
                         int timeout_secs);

/**
 * Process pending fetch requests. Called from Tor's event loop
 * (dynhost_run_scheduled_events). NOT thread-safe — must only
 * be called from Tor's main thread.
 */
void dynhost_client_process_pending(void);

/**
 * Clean up all pending and active fetches. Called during shutdown.
 */
void dynhost_client_cleanup(void);

struct dir_connection_t;
/**
 * Deliver a completed HTTP response to the matching active fetch.
 * Called from dirclient.c's response dispatch for connections whose
 * purpose is DIR_PURPOSE_DYNHOST_FETCH. Tor main thread only.
 *
 * @param conn       The dynhost fetch's dir_connection (EOF reached)
 * @param status     Parsed HTTP status code
 * @param body       Decompressed response body (owned by caller)
 * @param body_len   Length of response body
 * @return 0 always (the connection is closed by the caller regardless)
 */
int dynhost_client_handle_response(struct dir_connection_t *conn,
                                   int status,
                                   const char *body,
                                   size_t body_len);

#endif /* TOR_FEATURE_DYNHOST_DYNHOST_CLIENT_H */
