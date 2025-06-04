/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_message.c
 * @brief 488-byte message protocol implementation for dynamic onion host
 **/

#include "core/or/or.h"
#include "feature/dynhost/dynhost.h"
#include "feature/dynhost/dynhost_handlers.h"
#include "feature/dynhost/dynhost_message.h"
#include "core/or/connection_edge.h"
#include "core/or/edge_connection_st.h"
#include "core/or/connection_st.h"
#include "core/mainloop/connection.h"
#include "core/mainloop/mainloop.h"
#include "lib/log/log.h"
#include "lib/crypt_ops/crypto_util.h"
#include "lib/malloc/malloc.h"
#include "lib/container/smartlist.h"
#include "lib/buf/buffers.h"

/* Use accessor function to get global dynhost service */

/* CRC32 table for checksum calculation */
static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

/**
 * Initialize CRC32 table.
 */
static void
init_crc32_table(void)
{
  if (crc32_table_initialized) {
    return;
  }
  
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      if (c & 1) {
        c = 0xEDB88320 ^ (c >> 1);
      } else {
        c = c >> 1;
      }
    }
    crc32_table[i] = c;
  }
  crc32_table_initialized = 1;
}

/**
 * Calculate CRC32 checksum.
 */
uint32_t
dynhost_crc32(const uint8_t *data, size_t len)
{
  init_crc32_table();
  
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

/**
 * Generate unique message ID.
 */
uint32_t
dynhost_generate_msg_id(void)
{
  dynhost_service_t *dynhost = dynhost_get_global_service();
  if (!dynhost) {
    return 0;
  }
  
  uint32_t msg_id;
  
  tor_mutex_acquire(&dynhost->handler_mutex);
  msg_id = dynhost->next_msg_id++;
  tor_mutex_release(&dynhost->handler_mutex);
  
  return msg_id;
}

/**
 * Send fragmented message.
 */
int
dynhost_send_message(edge_connection_t *conn, const uint8_t *data, size_t len)
{
  if (!conn || !data || len == 0) {
    return -1;
  }
  
  if (!conn->dynhost_active) {
    log_warn(LD_REND, "Attempted to send message on non-dynhost connection");
    return -1;
  }
  
  uint32_t msg_id = dynhost_generate_msg_id();
  uint32_t total_chunks = (len + DYNHOST_MAX_CHUNK_DATA - 1) / DYNHOST_MAX_CHUNK_DATA;
  
  log_info(LD_REND, "Sending message %u: %zu bytes in %u chunks", 
           msg_id, len, total_chunks);
  
  for (uint32_t chunk_seq = 0; chunk_seq < total_chunks; chunk_seq++) {
    size_t chunk_offset = chunk_seq * DYNHOST_MAX_CHUNK_DATA;
    size_t chunk_size = MIN(DYNHOST_MAX_CHUNK_DATA, len - chunk_offset);
    
    /* Build chunk */
    uint8_t chunk_buffer[488];
    dynhost_msg_header_t *header = (dynhost_msg_header_t *)chunk_buffer;
    
    header->msg_id = htonl(msg_id);
    header->total_chunks = htonl(total_chunks);
    header->chunk_seq = htonl(chunk_seq);
    header->chunk_size = htons((uint16_t)chunk_size);
    header->flags = 0;
    
    /* Copy chunk data */
    memcpy(chunk_buffer + DYNHOST_MSG_HEADER_SIZE, 
           data + chunk_offset, chunk_size);
    
    /* Calculate checksum */
    header->checksum = htonl(dynhost_crc32(data + chunk_offset, chunk_size));
    
    /* Send chunk through connection buffer */
    connection_buf_add((char *)chunk_buffer, 
                      DYNHOST_MSG_HEADER_SIZE + chunk_size,
                      TO_CONN(conn));
    
    log_debug(LD_REND, "Sent chunk %u/%u of message %u (%zu bytes)",
              chunk_seq + 1, total_chunks, msg_id, chunk_size);
  }
  
  /* Ensure data is written */
  connection_start_writing(TO_CONN(conn));
  
  return 0;
}

/**
 * Receive and reassemble fragmented message.
 */
int
dynhost_receive_message(edge_connection_t *conn)
{
  if (!conn || !conn->dynhost_active) {
    return -1;
  }
  
  connection_t *base_conn = TO_CONN(conn);
  
  /* Check if we have enough data for header */
  if (connection_get_inbuf_len(base_conn) < DYNHOST_MSG_HEADER_SIZE) {
    return 0;  /* Need more data */
  }
  
  /* Peek at header to determine full chunk size */
  uint8_t header_buf[DYNHOST_MSG_HEADER_SIZE];
  connection_buf_get_bytes((char *)header_buf, DYNHOST_MSG_HEADER_SIZE, base_conn);
  
  dynhost_msg_header_t *header = (dynhost_msg_header_t *)header_buf;
  uint16_t chunk_size = ntohs(header->chunk_size);
  
  /* Validate chunk size */
  if (chunk_size > DYNHOST_MAX_CHUNK_DATA) {
    log_warn(LD_REND, "Invalid chunk size %u in dynhost message", chunk_size);
    connection_mark_for_close(base_conn);
    return -1;
  }
  
  /* Check if we have the full chunk */
  size_t total_needed = DYNHOST_MSG_HEADER_SIZE + chunk_size;
  if (connection_get_inbuf_len(base_conn) < total_needed) {
    return 0;  /* Need more data */
  }
  
  /* Read the full chunk */
  uint8_t chunk_buffer[488];
  connection_buf_get_bytes((char *)chunk_buffer, total_needed, base_conn);
  
  /* Parse header */
  header = (dynhost_msg_header_t *)chunk_buffer;
  uint32_t msg_id = ntohl(header->msg_id);
  uint32_t total_chunks = ntohl(header->total_chunks);
  uint32_t chunk_seq = ntohl(header->chunk_seq);
  uint32_t checksum = ntohl(header->checksum);
  
  /* Validate chunk sequence */
  if (chunk_seq >= total_chunks) {
    log_warn(LD_REND, "Invalid chunk sequence %u/%u in message %u",
             chunk_seq, total_chunks, msg_id);
    return -1;
  }
  
  /* Verify checksum */
  uint32_t calc_checksum = dynhost_crc32(chunk_buffer + DYNHOST_MSG_HEADER_SIZE,
                                        chunk_size);
  if (calc_checksum != checksum) {
    log_warn(LD_REND, "Checksum mismatch in dynhost chunk: expected %08x, got %08x",
             checksum, calc_checksum);
    return -1;
  }
  
  log_debug(LD_REND, "Received chunk %u/%u of message %u (%u bytes)",
            chunk_seq + 1, total_chunks, msg_id, chunk_size);
  
  /* Handle message reassembly */
  return dynhost_handle_chunk(conn, msg_id, total_chunks, chunk_seq,
                             chunk_buffer + DYNHOST_MSG_HEADER_SIZE,
                             chunk_size);
}

/**
 * Initialize message subsystem.
 */
void
dynhost_message_init(void)
{
  init_crc32_table();
}