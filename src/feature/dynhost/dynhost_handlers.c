/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_handlers.c
 * @brief Dynamic onion host connection handlers
 **/

#include "core/or/or.h"
#include "feature/dynhost/dynhost.h"
#include "feature/dynhost/dynhost_handlers.h"
#include "feature/hs/hs_service.h"
#include "feature/hs/hs_common.h"
#include "core/or/connection_edge.h"
#include "core/or/edge_connection_st.h"
#include "core/or/circuit_st.h"
#include "core/or/origin_circuit_st.h"
#include "feature/hs/hs_ident.h"
#include "lib/log/log.h"
#include "lib/container/smartlist.h"
#include "core/mainloop/mainloop.h"
#include "core/mainloop/connection.h"
#include "core/mainloop/mainloop.h"
#include "core/or/connection_st.h"
#include "core/or/socks_request_st.h"
#include "core/or/reasons.h"
#include "core/or/relay.h"
#include "core/or/circuitlist.h"
#include "feature/dynhost/dynhost_webserver.h"
#include "lib/buf/buffers.h"

/* Use accessor function to get global dynhost service */

/**
 * Check if a service identity key belongs to our dynhost service.
 */
static int
dynhost_is_our_service(const ed25519_public_key_t *identity_pk)
{
  dynhost_service_t *dynhost = dynhost_get_global_service();
  if (!dynhost || !dynhost->hs_service) {
    return 0;
  }
  
  return ed25519_pubkey_eq(identity_pk, 
                          &dynhost->hs_service->keys.identity_pk);
}

/**
 * Check if connection meets isolation requirements.
 */
static int
dynhost_check_isolation(edge_connection_t *conn, uint32_t isolation_flags)
{
  /* For now, accept all connections. In the future, implement
   * circuit isolation based on the flags. */
  (void)conn;
  (void)isolation_flags;
  return 1;
}

/**
 * Handle new connection to dynhost service.
 */
static int
handle_dynhost_new_connection(edge_connection_t *edge_conn)
{
  if (BUG(!edge_conn->dynhost_port)) {
    return -1;
  }
  
  dynhost_port_t *port = edge_conn->dynhost_port;
  
  /* Validate connection against isolation flags */
  if (!dynhost_check_isolation(edge_conn, port->isolation_flags)) {
    log_info(LD_REND, "Connection failed isolation check");
    connection_mark_for_close(TO_CONN(edge_conn));
    return -1;
  }
  
  /* Set up connection for internal handling */
  edge_conn->base_.state = AP_CONN_STATE_OPEN;
  edge_conn->dynhost_active = 1;
  
  /* Clear the address since we're not connecting to a real port */
  tor_addr_make_unspec(&edge_conn->base_.addr);
  edge_conn->base_.port = 0;
  
  /* Initialize reassembly buffer */
  if (!edge_conn->dynhost_reassembly_buf) {
    edge_conn->dynhost_reassembly_buf = buf_new();
  }
  
  /* Register read handler */
  connection_start_reading(TO_CONN(edge_conn));
  
  log_info(LD_REND, "Dynhost connection established on virtual port %d",
           port->virtual_port);
  
  /* Send connected cell */
  connection_edge_send_command(edge_conn, RELAY_COMMAND_CONNECTED,
                              NULL, 0);
  
  return 0;
}

/**
 * Check if a hidden service is a dynhost service that needs special handling.
 */
int
dynhost_intercept_service_connection(const hs_service_t *service,
                                    edge_connection_t *conn)
{
  dynhost_service_t *dynhost = dynhost_get_global_service();
  
  log_info(LD_REND, "Checking dynhost interception for service %p", service);
  
  if (!dynhost || !dynhost->hs_service) {
    log_info(LD_REND, "No dynhost service configured");
    return 0;  /* Not a dynhost service */
  }
  
  log_info(LD_REND, "Comparing service keys: service=%p dynhost=%p", 
           service, dynhost->hs_service);
  
  /* Check if this is our ephemeral service by comparing identity keys */
  if (!ed25519_pubkey_eq(&service->keys.identity_pk,
                         &dynhost->hs_service->keys.identity_pk)) {
    log_info(LD_REND, "Service keys don't match");
    return 0;  /* Not our service */
  }
  
  log_notice(LD_REND, "This IS a dynhost service - intercepting!");
  
  /* This is a dynhost connection - intercept it */
  uint16_t virtual_port = conn->base_.port;
  
  /* Find matching virtual port in our configuration */
  SMARTLIST_FOREACH_BEGIN(dynhost->virtual_ports, 
                          dynhost_port_t *, port) {
    if (port->virtual_port == virtual_port) {
      /* Found matching port - set up for internal handling */
      conn->dynhost_port = port;
      conn->dynhost_active = 1;
      
      /* Set a localhost address to satisfy connection requirements */
      tor_addr_from_ipv4h(&conn->base_.addr, 0x7f000001);  /* 127.0.0.1 */
      conn->base_.port = 80;  /* Set to the virtual port */
      
      /* Initialize edge connection state */
      conn->base_.state = EXIT_CONN_STATE_CONNECTING;
      conn->edge_has_sent_end = 0;
      conn->end_reason = 0;
      conn->base_.s = TOR_INVALID_SOCKET;  /* No real socket */
      
      /* Initialize reassembly buffer */
      if (!conn->dynhost_reassembly_buf) {
        conn->dynhost_reassembly_buf = buf_new();
      }
      
      /* Set stream ID if not already set */
      if (conn->stream_id == 0) {
        conn->stream_id = conn->base_.global_identifier & 0xFFFF;
      }
      
      log_notice(LD_REND, "Dynhost intercepting connection to virtual port %d, "
                 "conn=%p, stream_id=%d, dynhost_active=%d",
                 virtual_port, conn, conn->stream_id, conn->dynhost_active);
      
      /* Note: CONNECTED cell will be sent by connection_exit_connect()
       * when it sees dynhost_active=1. We don't need to send it here. */
      
      log_notice(LD_REND, "Dynhost connection configured for port %d",
                 virtual_port);
      
      return 1;  /* We're handling this connection */
    }
  } SMARTLIST_FOREACH_END(port);
  
  log_info(LD_REND, "No dynhost handler for virtual port %d", virtual_port);
  return 0;  /* Let normal handling proceed */
}

/**
 * Handle completed message reassembly.
 */
static int
dynhost_handle_complete_message(edge_connection_t *conn, 
                               uint32_t msg_id,
                               uint8_t *data, 
                               size_t len)
{
  (void)msg_id;  /* Will be used for message tracking */
  
  log_info(LD_REND, "Dynhost received complete message of %zu bytes", len);
  
  /* Handle as HTTP request */
  int result = dynhost_webserver_handle_request(conn, data, len);
  
  /* Free the reassembled data */
  tor_free(data);
  
  return result;
}

/**
 * Handle incoming message chunk.
 */
int
dynhost_handle_chunk(edge_connection_t *conn,
                    uint32_t msg_id,
                    uint32_t total_chunks,
                    uint32_t chunk_seq,
                    const uint8_t *chunk_data,
                    uint16_t chunk_size)
{
  /* For now, implement simple single-chunk handling.
   * TODO: Implement full reassembly with message tracking */
  
  if (total_chunks == 1 && chunk_seq == 0) {
    /* Single chunk message - process immediately */
    uint8_t *data = tor_memdup(chunk_data, chunk_size);
    return dynhost_handle_complete_message(conn, msg_id, data, chunk_size);
  }
  
  /* Multi-chunk messages need reassembly buffer implementation */
  log_warn(LD_REND, "Multi-chunk messages not yet implemented");
  return -1;
}

/**
 * Override connection_handle_read for dynhost connections.
 */
int
dynhost_connection_handle_read(edge_connection_t *edge_conn)
{
  if (!edge_conn->dynhost_active) {
    return -1;  /* Not our connection */
  }
  
  /* For simple HTTP handling, accumulate data until we have a complete request */
  connection_t *conn = TO_CONN(edge_conn);
  size_t available = connection_get_inbuf_len(conn);
  
  log_notice(LD_REND, "Dynhost read handler called, %zu bytes available", available);
  
  if (available == 0) {
    return 0;
  }
  
  /* Read all available data into reassembly buffer */
  char tmp_buf[4096];
  while (available > 0) {
    size_t to_read = MIN(available, sizeof(tmp_buf));
    connection_buf_get_bytes(tmp_buf, to_read, conn);
    buf_add(edge_conn->dynhost_reassembly_buf, tmp_buf, to_read);
    available = connection_get_inbuf_len(conn);
  }
  
  /* Check if we have a complete HTTP request */
  size_t buf_len = buf_datalen(edge_conn->dynhost_reassembly_buf);
  log_notice(LD_REND, "Dynhost reassembly buffer has %zu bytes", buf_len);
  
  if (buf_len > 0) {
    char *data = tor_malloc(buf_len + 1);
    buf_get_bytes(edge_conn->dynhost_reassembly_buf, data, buf_len);
    data[buf_len] = '\0';
    
    log_notice(LD_REND, "Checking for complete HTTP request, first 100 chars: %.100s", data);
    
    /* Check for complete HTTP request */
    if (dynhost_webserver_has_complete_request((uint8_t *)data, buf_len)) {
      log_notice(LD_REND, "Received complete HTTP request (%zu bytes)", buf_len);
      
      /* Clear the buffer */
      buf_clear(edge_conn->dynhost_reassembly_buf);
      
      /* Handle the request */
      dynhost_webserver_handle_request(edge_conn, (uint8_t *)data, buf_len);
      
      /* Don't close connection yet - let response be sent first */
      /* connection_mark_for_close(conn); */
    } else {
      log_notice(LD_REND, "HTTP request not complete yet, waiting for more data");
      /* Put data back - not complete yet */
      buf_clear(edge_conn->dynhost_reassembly_buf);
      buf_add(edge_conn->dynhost_reassembly_buf, data, buf_len);
    }
    
    tor_free(data);
  }
  
  return 0;
}

/**
 * Check if we should intercept this service's connections.
 */
int
dynhost_should_intercept_service(const hs_service_t *service)
{
  dynhost_service_t *dynhost = dynhost_get_global_service();
  if (!dynhost || !dynhost->hs_service) {
    return 0;
  }
  
  return service == dynhost->hs_service;
}