/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost.h
 * @brief Header for dynamic onion host implementation
 **/

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_H
#define TOR_FEATURE_DYNHOST_DYNHOST_H

#include "core/or/or.h"
#include "feature/hs/hs_service.h"
#include "feature/hs/hs_common.h"
#include "lib/container/smartlist.h"
#include "lib/lock/compat_mutex.h"
#include "lib/net/address.h"

/** Message header size for 488-byte protocol */
#define DYNHOST_MSG_HEADER_SIZE 20

/** Maximum data per chunk (488 - header) */
#define DYNHOST_MAX_CHUNK_DATA (488 - DYNHOST_MSG_HEADER_SIZE)

/** Dynamic onion host port configuration */
typedef struct dynhost_port_t {
  uint16_t virtual_port;        /**< External-facing port */
  uint32_t isolation_flags;     /**< Circuit isolation flags */
  void *handler_data;           /**< Custom handler data */
} dynhost_port_t;

/** Message header for fragmented data */
typedef struct dynhost_msg_header_t {
  uint32_t msg_id;              /**< Unique message ID */
  uint32_t total_chunks;        /**< Total number of chunks */
  uint32_t chunk_seq;           /**< Current chunk sequence (0-based) */
  uint16_t chunk_size;          /**< Size of this chunk */
  uint16_t flags;               /**< Message flags */
  uint32_t checksum;            /**< CRC32 of chunk data */
} dynhost_msg_header_t;

/** Global dynhost service state */
typedef struct dynhost_service_t {
  hs_service_t *hs_service;     /**< Underlying onion service */
  smartlist_t *virtual_ports;   /**< List of dynhost_port_t */
  tor_mutex_t handler_mutex;    /**< Thread safety for handlers */
  uint32_t next_msg_id;         /**< Next message ID counter */
  char *onion_address;          /**< The .onion address (without .onion suffix) */
} dynhost_service_t;

/* Core API functions */
int dynhost_init_global_state(void);
void dynhost_cleanup_global_state(void);
int dynhost_configure(const struct or_options_t *options);
int dynhost_activate_service(void);
void dynhost_check_and_activate(void);
void dynhost_run_scheduled_events(time_t now);

/* Service management */
hs_service_t *dynhost_create_service(void);
int dynhost_add_virtual_port(uint16_t virtual_port, uint32_t isolation_flags);

/* Message protocol */
int dynhost_send_message(struct edge_connection_t *conn, 
                        const uint8_t *data, size_t len);
int dynhost_receive_message(struct edge_connection_t *conn);

/* Connection handling - defined in dynhost_handlers.h */

/* Utility functions */
uint32_t dynhost_generate_msg_id(void);
uint32_t dynhost_crc32(const uint8_t *data, size_t len);
dynhost_service_t *dynhost_get_global_service(void);

#endif /* !defined(TOR_FEATURE_DYNHOST_DYNHOST_H) */