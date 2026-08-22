/* SPDX-FileCopyrightText: 2026 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_stream.c
 * @brief Raw bidirectional byte streams to .onion services.
 *
 * Modeled directly on dynhost_client.c: open requests and outbound bytes
 * are enqueued under a mutex from any thread; Tor's event loop initiates
 * the connection with connection_ap_make_link(), flushes queued writes,
 * and fires all callbacks. The difference from the fetch client is the
 * absence of HTTP: inbound bytes are drained from the dir_connection's
 * inbuf and delivered as they arrive (see dynhost_stream_deliver_inbuf,
 * hooked into connection_dir_process_inbuf for DIR_PURPOSE_DYNHOST_STREAM
 * before any HTTP parsing).
 */

#include "core/or/or.h"
#include "feature/dynhost/dynhost_stream.h"
#include "core/or/connection_edge.h"
#include "core/or/entry_connection_st.h"
#include "core/or/edge_connection_st.h"
#include "core/mainloop/connection.h"
#include "core/mainloop/mainloop.h"
#include "core/or/connection_st.h"
#include "feature/dircommon/directory.h"
#include "feature/dircommon/dir_connection_st.h"
#include "lib/log/log.h"
#include "lib/malloc/malloc.h"
#include "lib/lock/compat_mutex.h"
#include "lib/buf/buffers.h"

#include <string.h>
#include <time.h>

/** Cap on outbound bytes queued but not yet flushed by the Tor thread.
 * Prevents unbounded memory growth when the caller outpaces the circuit. */
#define DYNHOST_STREAM_MAX_QUEUE_BYTES (4u*1024u*1024u)

/* ── Stream handle ─────────────────────────────────────────── */

typedef struct dynhost_stream_chunk_t {
  uint8_t *data;
  size_t len;
  struct dynhost_stream_chunk_t *next;
} dynhost_stream_chunk_t;

struct dynhost_stream {
  /* Immutable after open(). */
  char onion_address[128];
  uint16_t port;
  dynhost_stream_read_fn read_cb;
  dynhost_stream_event_fn event_cb;
  void *ctx;
  time_t deadline;

  /* Shared with other threads: guarded by g_queue_mutex. */
  uint64_t conn_gid;     /* weak ref to our side of the linked pair;
                          * 0 until initiated (Tor thread writes) */
  int close_called;      /* caller ran dynhost_stream_close() */
  int terminal_fired;    /* terminal event callback has been fired */
  dynhost_stream_chunk_t *write_head;
  dynhost_stream_chunk_t *write_tail;
  size_t write_queued_bytes;

  /* Tor main thread only. */
  int connected_fired;
  struct dynhost_stream *next;
};

/* Active streams hold a WEAK reference to the connection via its
 * global_identifier, never a raw pointer: Tor owns the connection's
 * lifetime and may close+free it from its own event loop between our
 * ticks. Resolve with connection_get_by_global_id() before every use —
 * NULL means gone. Same discipline as dynhost_client.c. */
static tor_mutex_t g_queue_mutex;
static int g_mutex_initialized = 0;
static dynhost_stream_t *g_pending_head = NULL;
static dynhost_stream_t *g_active_head = NULL;

static void
ensure_mutex(void)
{
  if (!g_mutex_initialized) {
    tor_mutex_init(&g_queue_mutex);
    g_mutex_initialized = 1;
  }
}

static void
free_chunk_list(dynhost_stream_chunk_t *chunk)
{
  while (chunk) {
    dynhost_stream_chunk_t *next = chunk->next;
    tor_free(chunk->data);
    tor_free(chunk);
    chunk = next;
  }
}

/* ── Terminal-event plumbing (Tor main thread only) ────────── */

/**
 * Fire the terminal event for a stream that is not on any list: mark it
 * terminal under the mutex (so concurrent write()/close() calls fail
 * cleanly), drop any queued writes, invoke the event callback, and free
 * the handle if the caller already ran dynhost_stream_close().
 *
 * The callback runs outside the mutex: the callback (or another thread)
 * may legally call dynhost_stream_close() while it runs. close() frees
 * the handle itself when terminal_fired is set, so afterwards we must
 * consult only the local free_now, never the handle.
 */
static void
stream_terminal_detached(dynhost_stream_t *s, int event)
{
  int free_now;
  dynhost_stream_chunk_t *chunks;

  tor_mutex_acquire(&g_queue_mutex);
  s->terminal_fired = 1;
  free_now = s->close_called;
  chunks = s->write_head;
  s->write_head = s->write_tail = NULL;
  s->write_queued_bytes = 0;
  tor_mutex_release(&g_queue_mutex);

  free_chunk_list(chunks);
  s->event_cb(s, event, s->ctx);
  if (free_now)
    tor_free(s);
}

/** Unlink a stream from the active list and fire its terminal event. */
static void
stream_terminal(dynhost_stream_t **pp, dynhost_stream_t *s, int event)
{
  *pp = s->next;
  s->next = NULL;
  stream_terminal_detached(s, event);
}

/** Find an active stream by its connection's global identifier.
 * Tor main thread only — the active list is only mutated there. */
static dynhost_stream_t *
find_active_stream(uint64_t gid)
{
  dynhost_stream_t *s;
  for (s = g_active_head; s; s = s->next) {
    if (s->conn_gid == gid)
      return s;
  }
  return NULL;
}

/* ── Public API (any thread) ───────────────────────────────── */

dynhost_stream_t *
dynhost_stream_open(const char *onion_address,
                    uint16_t port,
                    dynhost_stream_read_fn read_cb,
                    dynhost_stream_event_fn event_cb,
                    void *ctx,
                    int timeout_secs)
{
  if (!onion_address || !read_cb || !event_cb)
    return NULL;

  ensure_mutex();

  dynhost_stream_t *s = tor_malloc_zero(sizeof(*s));
  strlcpy(s->onion_address, onion_address, sizeof(s->onion_address));
  s->port = port ? port : 80;
  s->read_cb = read_cb;
  s->event_cb = event_cb;
  s->ctx = ctx;
  s->deadline = time(NULL) + (timeout_secs > 0 ? timeout_secs : 60);

  /* Ensure .onion suffix */
  if (!strstr(s->onion_address, ".onion")) {
    size_t len = strlen(s->onion_address);
    if (len + 7 < sizeof(s->onion_address))
      strlcat(s->onion_address, ".onion", sizeof(s->onion_address));
  }

  tor_mutex_acquire(&g_queue_mutex);
  s->next = g_pending_head;
  g_pending_head = s;
  tor_mutex_release(&g_queue_mutex);

  log_notice(LD_REND, "Dynhost stream: queued open to %s:%u",
             s->onion_address, (unsigned)s->port);
  return s;
}

int
dynhost_stream_write(dynhost_stream_t *s, const uint8_t *data, size_t len)
{
  if (!s || (!data && len) || !g_mutex_initialized)
    return -1;
  if (len == 0)
    return 0;

  dynhost_stream_chunk_t *chunk = tor_malloc(sizeof(*chunk));
  chunk->data = tor_memdup(data, len);
  chunk->len = len;
  chunk->next = NULL;

  tor_mutex_acquire(&g_queue_mutex);
  if (s->terminal_fired || s->close_called ||
      s->write_queued_bytes + len > DYNHOST_STREAM_MAX_QUEUE_BYTES) {
    tor_mutex_release(&g_queue_mutex);
    tor_free(chunk->data);
    tor_free(chunk);
    return -1;
  }
  if (s->write_tail)
    s->write_tail->next = chunk;
  else
    s->write_head = chunk;
  s->write_tail = chunk;
  s->write_queued_bytes += len;
  tor_mutex_release(&g_queue_mutex);
  return 0;
}

void
dynhost_stream_close(dynhost_stream_t *s)
{
  if (!s || !g_mutex_initialized)
    return;

  int free_now = 0;
  tor_mutex_acquire(&g_queue_mutex);
  if (s->close_called) {
    tor_mutex_release(&g_queue_mutex);
    return;
  }
  s->close_called = 1;
  free_now = s->terminal_fired;
  tor_mutex_release(&g_queue_mutex);

  /* If the terminal event already fired, the module no longer
   * references the handle: release it now. Otherwise the event loop
   * fires CLOSED and releases it there. */
  if (free_now)
    tor_free(s);
}

/* ── Initiate a stream (Tor main thread only) ──────────────── */

/**
 * Start a single stream: create the dir_connection partner, link it to
 * an AP connection through Tor's circuit machinery, and run the onion
 * rewrite-and-attach. Identical to dynhost_client.c's initiate_fetch()
 * except for the purpose and the absence of an HTTP request.
 */
static int
initiate_stream(dynhost_stream_t *s)
{
  dir_connection_t *dir_conn = dir_connection_new(AF_INET);
  if (!dir_conn) {
    log_warn(LD_REND, "Dynhost stream: dir_connection_new failed");
    return -1;
  }

  tor_addr_from_ipv4h(&dir_conn->base_.addr, 0x7f000001); /* dummy */
  dir_conn->base_.port = s->port;
  dir_conn->base_.address = tor_strdup(s->onion_address);
  /* A raw application byte stream, not directory data: it needs its own
   * purpose so connection_dir_process_inbuf/reached_eof bypass the HTTP
   * machinery and hand bytes to dynhost_stream (see directory.c and
   * dirclient.c). */
  dir_conn->base_.purpose = DIR_PURPOSE_DYNHOST_STREAM;
  dir_conn->base_.state = DIR_CONN_STATE_CONNECTING;

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
    log_warn(LD_REND, "Dynhost stream: connection_ap_make_link failed for %s",
             s->onion_address);
    connection_free_(TO_CONN(dir_conn));
    return -1;
  }

  if (connection_add(TO_CONN(dir_conn)) < 0) {
    log_warn(LD_REND, "Dynhost stream: connection_add failed");
    connection_free_(TO_CONN(dir_conn));
    return -1;
  }

  /* An internal linked conn never passes through a SOCKS listener, so
   * opt in to onion traffic explicitly and run the same
   * rewrite-and-attach the SOCKS handshake runs; see the long comment
   * in dynhost_client.c's initiate_fetch(). */
  linked_conn->entry_cfg.onion_traffic = 1;
  connection_ap_mark_as_non_pending_circuit(linked_conn);
  if (connection_ap_rewrite_and_attach_if_allowed(linked_conn, NULL,
                                                  NULL) < 0) {
    log_warn(LD_REND, "Dynhost stream: onion rewrite/attach refused for %s",
             s->onion_address);
    connection_mark_for_close(TO_CONN(dir_conn));
    return -1;
  }

  dir_conn->base_.state = DIR_CONN_STATE_CLIENT_SENDING;
  connection_watch_events(TO_CONN(dir_conn), READ_EVENT | WRITE_EVENT);
  connection_start_reading(ENTRY_TO_CONN(linked_conn));

  tor_mutex_acquire(&g_queue_mutex);
  s->conn_gid = TO_CONN(dir_conn)->global_identifier;
  tor_mutex_release(&g_queue_mutex);

  log_notice(LD_REND, "Dynhost stream: initiated stream to %s:%u",
             s->onion_address, (unsigned)s->port);
  return 0;
}

/* ── Inbound data (Tor main thread, via process_inbuf hook) ── */

void
dynhost_stream_deliver_inbuf(struct dir_connection_t *dir_conn)
{
  if (!g_mutex_initialized)
    return;

  connection_t *conn = TO_CONN(dir_conn);
  size_t available = connection_get_inbuf_len(conn);
  if (available == 0)
    return;

  uint8_t *data = tor_malloc(available);
  connection_buf_get_bytes((char *)data, available, conn);

  dynhost_stream_t *s = find_active_stream(conn->global_identifier);
  if (s) {
    s->read_cb(s, data, available, s->ctx);
  } else {
    /* Stream already terminated (timeout/close) but the connection is
     * still draining: drop the bytes. */
    log_debug(LD_REND, "Dynhost stream: dropping %"TOR_PRIuSZ" bytes for "
              "unknown stream (gid %llu)", available,
              (unsigned long long)conn->global_identifier);
  }
  tor_free(data);
}

int
dynhost_stream_handle_eof(struct dir_connection_t *dir_conn)
{
  if (!g_mutex_initialized)
    return 0;

  /* Deliver anything still buffered, then terminate. */
  dynhost_stream_deliver_inbuf(dir_conn);

  uint64_t gid = TO_CONN(dir_conn)->global_identifier;
  dynhost_stream_t **pp = &g_active_head;
  while (*pp) {
    dynhost_stream_t *s = *pp;
    if (s->conn_gid == gid) {
      log_notice(LD_REND, "Dynhost stream: EOF, closing stream (gid %llu)",
                 (unsigned long long)gid);
      stream_terminal(pp, s, DYNHOST_STREAM_EVENT_CLOSED);
      return 0;
    }
    pp = &(*pp)->next;
  }
  return 0;
}

/* ── Check active streams (Tor main thread only) ───────────── */

/** Move queued outbound chunks onto the connection's outbuf. */
static void
flush_pending_writes(dynhost_stream_t *s, connection_t *conn)
{
  dynhost_stream_chunk_t *chunks;

  tor_mutex_acquire(&g_queue_mutex);
  chunks = s->write_head;
  s->write_head = s->write_tail = NULL;
  s->write_queued_bytes = 0;
  tor_mutex_release(&g_queue_mutex);

  while (chunks) {
    dynhost_stream_chunk_t *next = chunks->next;
    connection_buf_add((const char *)chunks->data, chunks->len, conn);
    tor_free(chunks->data);
    tor_free(chunks);
    chunks = next;
  }
}

static void
check_active_streams(time_t now)
{
  dynhost_stream_t **pp = &g_active_head;

  while (*pp) {
    dynhost_stream_t *s = *pp;
    /* Resolve the connection by global id every tick; Tor owns its
     * lifetime and may have closed+freed it since the previous tick. */
    connection_t *conn = connection_get_by_global_id(s->conn_gid);
    if (!conn) {
      stream_terminal(pp, s, DYNHOST_STREAM_EVENT_CLOSED);
      continue;
    }

    int close_req;
    tor_mutex_acquire(&g_queue_mutex);
    close_req = s->close_called;
    tor_mutex_release(&g_queue_mutex);

    if (close_req) {
      connection_mark_for_close(conn);
      stream_terminal(pp, s, DYNHOST_STREAM_EVENT_CLOSED);
      continue;
    }

    if (now >= s->deadline) {
      log_warn(LD_REND, "Dynhost stream: stream timed out");
      connection_mark_for_close(conn);
      stream_terminal(pp, s, DYNHOST_STREAM_EVENT_TIMEOUT);
      continue;
    }

    if (conn->marked_for_close) {
      /* Drain whatever is left, then terminate. */
      dynhost_stream_deliver_inbuf(TO_DIR_CONN(conn));
      stream_terminal(pp, s, DYNHOST_STREAM_EVENT_CLOSED);
      continue;
    }

    /* CONNECTED: the linked AP conn reaches AP_CONN_STATE_OPEN when the
     * relay CONNECTED cell arrives (relay.c). Fire once. */
    if (!s->connected_fired &&
        conn->linked_conn &&
        !conn->linked_conn->marked_for_close &&
        conn->linked_conn->state == AP_CONN_STATE_OPEN) {
      s->connected_fired = 1;
      s->event_cb(s, DYNHOST_STREAM_EVENT_CONNECTED, s->ctx);
    }

    flush_pending_writes(s, conn);

    pp = &(*pp)->next;
  }
}

/* ── Process pending requests (Tor event loop) ─────────────── */

void
dynhost_stream_process_pending(void)
{
  if (!g_mutex_initialized)
    return;

  time_t now = time(NULL);

  /* Drain pending open queue */
  tor_mutex_acquire(&g_queue_mutex);
  dynhost_stream_t *pending = g_pending_head;
  g_pending_head = NULL;
  tor_mutex_release(&g_queue_mutex);

  while (pending) {
    dynhost_stream_t *s = pending;
    pending = s->next;
    s->next = NULL;

    int close_req;
    tor_mutex_acquire(&g_queue_mutex);
    close_req = s->close_called;
    tor_mutex_release(&g_queue_mutex);

    if (close_req) {
      /* Caller closed before we ever initiated: fire CLOSED and free
       * (close_called is set, so stream_terminal_detached frees). */
      stream_terminal_detached(s, DYNHOST_STREAM_EVENT_CLOSED);
      continue;
    }

    if (initiate_stream(s) < 0) {
      stream_terminal_detached(s, DYNHOST_STREAM_EVENT_CLOSED);
      continue;
    }

    s->next = g_active_head;
    g_active_head = s;
    s->event_cb(s, DYNHOST_STREAM_EVENT_OPEN, s->ctx);
  }

  check_active_streams(now);
}

/* ── Cleanup ───────────────────────────────────────────────── */

void
dynhost_stream_cleanup(void)
{
  if (!g_mutex_initialized)
    return;

  /* Cancel pending opens */
  tor_mutex_acquire(&g_queue_mutex);
  dynhost_stream_t *p = g_pending_head;
  g_pending_head = NULL;
  tor_mutex_release(&g_queue_mutex);

  while (p) {
    dynhost_stream_t *next = p->next;
    p->next = NULL;
    stream_terminal_detached(p, DYNHOST_STREAM_EVENT_CLOSED);
    p = next;
  }

  /* Cancel active streams */
  while (g_active_head) {
    dynhost_stream_t *s = g_active_head;
    /* Resolve by global id — Tor may already have freed the conn. */
    connection_t *conn = connection_get_by_global_id(s->conn_gid);
    if (conn && !conn->marked_for_close)
      connection_mark_for_close(conn);
    stream_terminal(&g_active_head, s, DYNHOST_STREAM_EVENT_CLOSED);
  }
}
