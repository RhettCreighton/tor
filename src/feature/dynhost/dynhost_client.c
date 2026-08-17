/* SPDX-FileCopyrightText: 2026 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_client.c
 * @brief Outbound .onion HTTP client using Tor's internal circuit API.
 *
 * Uses connection_ap_make_link() — the same mechanism Tor's directory
 * client uses to fetch descriptors over onion services. No SOCKS.
 */

#include "core/or/or.h"
#include "feature/dynhost/dynhost_client.h"
#include "core/or/connection_edge.h"
#include "core/or/entry_connection_st.h"
#include "core/or/edge_connection_st.h"
#include "core/mainloop/connection.h"
#include "core/or/connection_st.h"
#include "feature/dirclient/dirclient.h"
#include "feature/dircommon/directory.h"
#include "feature/dircommon/dir_connection_st.h"
#include "lib/log/log.h"
#include "lib/malloc/malloc.h"
#include "lib/lock/compat_mutex.h"
#include "lib/buf/buffers.h"
#include "core/mainloop/mainloop.h"

#include <string.h>
#include <time.h>

/* ── Request queue (thread-safe) ───────────────────────────── */

typedef struct dynhost_fetch_request_t {
  char onion_address[128];
  uint16_t port;
  /* Must hold the longest application path: the zclassic23 market chunk
   * path is /market/chunk/<412 hex chars>?slice=N — 434 bytes. */
  char path[512];
  dynhost_client_callback_fn callback;
  void *ctx;
  time_t deadline;
  struct dynhost_fetch_request_t *next;
} dynhost_fetch_request_t;

/* Active fetch: a request that has been initiated (connection created).
 *
 * We hold a WEAK reference to our side of the linked pair via its
 * global_identifier, never a raw pointer. Tor owns the connection's
 * lifetime and may close+free it from its own event loop between our
 * ticks; a raw dir_connection_t* would dangle and a later deref
 * (buf_datalen on a freed inbuf) would crash. Resolve the id with
 * connection_get_by_global_id() before every use — NULL means gone. */
typedef struct dynhost_active_fetch_t {
  uint64_t conn_gid;              /* weak ref to our side of the linked pair */
  dynhost_client_callback_fn callback;
  void *ctx;
  time_t deadline;
  int response_started;           /* have we seen any response data? */
  struct dynhost_active_fetch_t *next;
} dynhost_active_fetch_t;

static tor_mutex_t g_queue_mutex;
static int g_mutex_initialized = 0;
static dynhost_fetch_request_t *g_pending_head = NULL;
static dynhost_active_fetch_t *g_active_head = NULL;

static void
ensure_mutex(void)
{
  if (!g_mutex_initialized) {
    tor_mutex_init(&g_queue_mutex);
    g_mutex_initialized = 1;
  }
}

/* ── Public API: queue a fetch request ─────────────────────── */

int
dynhost_client_fetch(const char *onion_address,
                     uint16_t port,
                     const char *path,
                     dynhost_client_callback_fn callback,
                     void *ctx,
                     int timeout_secs)
{
  if (!onion_address || !path || !callback)
    return -1;

  ensure_mutex();

  dynhost_fetch_request_t *req = tor_malloc_zero(sizeof(*req));
  strlcpy(req->onion_address, onion_address, sizeof(req->onion_address));
  req->port = port ? port : 80;
  strlcpy(req->path, path, sizeof(req->path));
  req->callback = callback;
  req->ctx = ctx;
  req->deadline = time(NULL) + (timeout_secs > 0 ? timeout_secs : 60);

  /* Ensure .onion suffix */
  if (!strstr(req->onion_address, ".onion")) {
    size_t len = strlen(req->onion_address);
    if (len + 7 < sizeof(req->onion_address))
      strlcat(req->onion_address, ".onion", sizeof(req->onion_address));
  }

  tor_mutex_acquire(&g_queue_mutex);
  req->next = g_pending_head;
  g_pending_head = req;
  tor_mutex_release(&g_queue_mutex);

  log_notice(LD_REND, "Dynhost client: queued fetch for %s%s",
             onion_address, path);
  return 0;
}

/* ── Initiate a fetch (Tor thread only) ────────────────────── */

/**
 * Write a minimal HTTP/1.0 GET request to the dir_connection's outbuf.
 */
static void
write_http_get(dir_connection_t *conn, const char *host, const char *path)
{
  char request[1024];
  int n = tor_snprintf(request, sizeof(request),
      "GET %s HTTP/1.0\r\n"
      "Host: %s\r\n"
      "Connection: close\r\n"
      "\r\n",
      path, host);
  if (n > 0) {
    connection_buf_add(request, (size_t)n, TO_CONN(conn));
  }
}

/**
 * Start a single fetch: create dir_connection, call connection_ap_make_link,
 * write HTTP request.
 */
static int
initiate_fetch(dynhost_fetch_request_t *req)
{
  tor_addr_t addr;
  tor_addr_make_unspec(&addr);

  /* Create a dir_connection as the "partner" — this is the client side.
   * We write our HTTP request to its outbuf, and the response comes
   * back to its inbuf via the Tor circuit. */
  dir_connection_t *dir_conn = dir_connection_new(AF_INET);
  if (!dir_conn) {
    log_warn(LD_REND, "Dynhost client: dir_connection_new failed");
    return -1;
  }

  /* Set address to the .onion hostname */
  tor_addr_from_ipv4h(&dir_conn->base_.addr, 0x7f000001); /* dummy */
  dir_conn->base_.port = req->port;
  dir_conn->base_.address = tor_strdup(req->onion_address);
  /* The response carries application data, not directory data. It MUST
   * have its own purpose: connection_dir_client_reached_eof() dispatches
   * on base_.purpose, and masquerading as DIR_PURPOSE_FETCH_CONSENSUS
   * routes the reply into handle_response_fetch_consensus(), which calls
   * networkstatus_consensus_download_failed(status, flavname=NULL) on any
   * non-200 — a NULL-deref crash — and would try to install a 200 reply
   * as the current consensus. */
  dir_conn->base_.purpose = DIR_PURPOSE_DYNHOST_FETCH;
  dir_conn->base_.state = DIR_CONN_STATE_CONNECTING;

  /* Create the linked AP connection through Tor's circuit machinery.
   * This is the same call dirclient.c uses for directory fetches. */
  entry_connection_t *linked_conn =
    connection_ap_make_link(TO_CONN(dir_conn),
                            dir_conn->base_.address,
                            dir_conn->base_.port,
                            NULL,  /* no digest */
                            SESSION_GROUP_DIRCONN,
                            ISO_STREAM,
                            0,     /* not begindir */
                            0);    /* anonymized (3-hop) */

  if (!linked_conn) {
    log_warn(LD_REND, "Dynhost client: connection_ap_make_link failed for %s",
             req->onion_address);
    connection_free_(TO_CONN(dir_conn));
    return -1;
  }

  if (connection_add(TO_CONN(dir_conn)) < 0) {
    log_warn(LD_REND, "Dynhost client: connection_add failed");
    connection_free_(TO_CONN(dir_conn));
    return -1;
  }

  /* A linked AP conn made this way never passes through a SOCKS listener,
   * so nothing else parses its ".onion" hostname into the rendezvous
   * machinery: left alone, hs_ident stays NULL and
   * connection_ap_handshake_attach_circuit() treats the stream as a
   * GENERAL exit stream — the exit then fails to DNS-resolve the .onion
   * and the stream dies after MAX_RESOLVE_FAILURES (the RESOLVEFAILED
   * "giving up" signature). Real SOCKS listeners get onion_traffic=1 from
   * their port defaults; an internal conn must opt in explicitly, then run
   * the same rewrite-and-attach the SOCKS handshake runs: it parses the
   * v3 hostname, sets hs_ident, and parks the stream in RENDDESC_WAIT
   * behind the descriptor fetch (or attaches immediately when the
   * descriptor is cached). */
  linked_conn->entry_cfg.onion_traffic = 1;
  /* connection_ap_make_link() has ALREADY marked this conn pending
   * (connection_edge.c:3518), but the rewrite-and-attach path owns the
   * pending bookkeeping from here exactly as it does for a SOCKS conn
   * that was never marked: the no-cached-descriptor branch unmarks and
   * parks in RENDDESC_WAIT, the cached-descriptor branch re-marks before
   * attaching. Leave the make_link mark in place and the cached branch
   * marks a second time ("Bug: pending_entry_connections already
   * contains ..." on every warm fetch). Undo it first. */
  connection_ap_mark_as_non_pending_circuit(linked_conn);
  if (connection_ap_rewrite_and_attach_if_allowed(linked_conn, NULL,
                                                  NULL) < 0) {
    log_warn(LD_REND, "Dynhost client: onion rewrite/attach refused for %s",
             req->onion_address);
    connection_mark_for_close(TO_CONN(dir_conn));
    return -1;
  }

  /* Set state and write the HTTP request */
  dir_conn->base_.state = DIR_CONN_STATE_CLIENT_SENDING;
  write_http_get(dir_conn, req->onion_address, req->path);

  connection_watch_events(TO_CONN(dir_conn), READ_EVENT | WRITE_EVENT);
  connection_start_reading(ENTRY_TO_CONN(linked_conn));

  /* Track as active fetch */
  dynhost_active_fetch_t *active = tor_malloc_zero(sizeof(*active));
  active->conn_gid = TO_CONN(dir_conn)->global_identifier;
  active->callback = req->callback;
  active->ctx = req->ctx;
  active->deadline = req->deadline;
  active->next = g_active_head;
  g_active_head = active;

  log_notice(LD_REND, "Dynhost client: initiated fetch to %s%s",
             req->onion_address, req->path);
  return 0;
}

/* ── Check active fetches for completion ───────────────────── */

/**
 * Parse HTTP status code from the first line of a response.
 * Returns status code (e.g., 200) or -1 on parse failure.
 */
static int
parse_http_status(const char *data, size_t len)
{
  if (len < 12) return -1;
  /* HTTP/1.x NNN */
  if (memcmp(data, "HTTP/", 5) != 0) return -1;
  const char *space = memchr(data, ' ', len);
  if (!space || (size_t)(space - data) >= len - 3) return -1;
  return atoi(space + 1);
}

/**
 * Find the end of HTTP headers (\r\n\r\n) and return pointer to body.
 * Returns NULL if headers aren't complete yet.
 */
static const char *
find_http_body(const char *data, size_t len, size_t *header_len)
{
  const char *sep = tor_memmem(data, len, "\r\n\r\n", 4);
  if (!sep) return NULL;
  *header_len = (size_t)(sep - data) + 4;
  return sep + 4;
}

static void
check_active_fetches(time_t now)
{
  dynhost_active_fetch_t **pp = &g_active_head;

  while (*pp) {
    dynhost_active_fetch_t *af = *pp;
    /* Resolve the connection by global id every tick. Tor owns its
     * lifetime and may have closed+freed it from its own event loop
     * since the previous tick; a raw pointer would dangle. NULL means
     * it is already gone — report failure and drop the fetch. */
    connection_t *conn = connection_get_by_global_id(af->conn_gid);
    if (!conn) {
      af->callback(-1, NULL, 0, af->ctx);
      *pp = af->next;
      tor_free(af);
      continue;
    }

    /* Check timeout */
    if (now >= af->deadline) {
      log_warn(LD_REND, "Dynhost client: fetch timed out");
      af->callback(-1, NULL, 0, af->ctx);
      connection_mark_for_close(conn);
      *pp = af->next;
      tor_free(af);
      continue;
    }

    /* Check if connection is marked for close (error or EOF) */
    if (conn->marked_for_close) {
      /* Try to read any remaining data */
      size_t available = connection_get_inbuf_len(conn);
      if (available > 0) {
        char *data = tor_malloc(available + 1);
        connection_buf_get_bytes(data, available, conn);
        data[available] = '\0';

        size_t hdr_len = 0;
        const char *body = find_http_body(data, available, &hdr_len);
        if (body) {
          int status = parse_http_status(data, hdr_len);
          size_t body_len = available - hdr_len;
          af->callback(status, (const uint8_t *)body, body_len, af->ctx);
        } else {
          af->callback(-1, (const uint8_t *)data, available, af->ctx);
        }
        tor_free(data);
      } else {
        af->callback(-1, NULL, 0, af->ctx);
      }

      *pp = af->next;
      tor_free(af);
      continue;
    }

    /* Check for response data on the inbuf */
    size_t available = connection_get_inbuf_len(conn);
    if (available > 0) {
      af->response_started = 1;

      /* Peek at data to see if response is complete.
       * For HTTP/1.0 with Connection: close, the response is complete
       * when the connection closes. We'll process it then. */
    }

    pp = &(*pp)->next;
  }
}

/* ── Response delivery (Tor event loop, via dirclient EOF) ─── */

/**
 * Called from connection_dir_client_reached_eof() when a
 * DIR_PURPOSE_DYNHOST_FETCH connection reaches EOF with a fully-parsed
 * HTTP response. Match the connection to its active fetch by global id,
 * fire the fetch's callback exactly once, and drop the fetch from the
 * active list. The unlink is the double-callback guard: afterwards
 * check_active_fetches() never sees the fetch, so its marked-for-close /
 * conn-gone failure paths cannot fire the callback a second time.
 */
int
dynhost_client_handle_response(struct dir_connection_t *dir_conn,
                               int status,
                               const char *body,
                               size_t body_len)
{
  if (!g_mutex_initialized)
    return 0;

  uint64_t gid = TO_CONN(dir_conn)->global_identifier;
  dynhost_active_fetch_t **pp = &g_active_head;
  while (*pp) {
    dynhost_active_fetch_t *af = *pp;
    if (af->conn_gid == gid) {
      log_notice(LD_REND,
                 "Dynhost client: fetch completed, status %d, %"
                 TOR_PRIuSZ " body bytes", status, body_len);
      af->callback(status, (const uint8_t *)body, body_len, af->ctx);
      *pp = af->next;
      tor_free(af);
      return 0;
    }
    pp = &(*pp)->next;
  }

  /* No active fetch: the fetch already timed out and its callback was
   * fired with -1. Nothing to do — the caller closes the connection. */
  log_info(LD_REND, "Dynhost client: response for unknown/timed-out "
           "connection (gid %llu), ignoring", (unsigned long long)gid);
  return 0;
}

/* ── Process pending requests (Tor event loop) ─────────────── */

void
dynhost_client_process_pending(void)
{
  if (!g_mutex_initialized) return;

  time_t now = time(NULL);

  /* Drain pending queue */
  tor_mutex_acquire(&g_queue_mutex);
  dynhost_fetch_request_t *pending = g_pending_head;
  g_pending_head = NULL;
  tor_mutex_release(&g_queue_mutex);

  while (pending) {
    dynhost_fetch_request_t *req = pending;
    pending = req->next;

    if (initiate_fetch(req) < 0) {
      /* Failed to initiate — invoke callback with error */
      req->callback(-1, NULL, 0, req->ctx);
    }
    tor_free(req);
  }

  /* Check active fetches */
  check_active_fetches(now);
}

/* ── Cleanup ───────────────────────────────────────────────── */

void
dynhost_client_cleanup(void)
{
  if (!g_mutex_initialized) return;

  /* Cancel pending */
  tor_mutex_acquire(&g_queue_mutex);
  dynhost_fetch_request_t *p = g_pending_head;
  g_pending_head = NULL;
  tor_mutex_release(&g_queue_mutex);

  while (p) {
    dynhost_fetch_request_t *next = p->next;
    p->callback(-1, NULL, 0, p->ctx);
    tor_free(p);
    p = next;
  }

  /* Cancel active */
  dynhost_active_fetch_t *a = g_active_head;
  g_active_head = NULL;
  while (a) {
    dynhost_active_fetch_t *next = a->next;
    a->callback(-1, NULL, 0, a->ctx);
    /* Resolve by global id — Tor may already have freed the conn. */
    connection_t *conn = connection_get_by_global_id(a->conn_gid);
    if (conn && !conn->marked_for_close)
      connection_mark_for_close(conn);
    tor_free(a);
    a = next;
  }
}
