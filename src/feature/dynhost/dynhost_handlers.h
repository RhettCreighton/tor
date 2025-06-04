/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_handlers.h
 * @brief Header for dynamic onion host connection handlers
 **/

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_HANDLERS_H
#define TOR_FEATURE_DYNHOST_DYNHOST_HANDLERS_H

#include "core/or/or.h"

struct edge_connection_t;
struct hs_service_t;

/* Connection interception */
int dynhost_intercept_service_connection(const struct hs_service_t *service,
                                        struct edge_connection_t *conn);
int dynhost_connection_handle_read(struct edge_connection_t *edge_conn);
int dynhost_should_intercept_service(const struct hs_service_t *service);

/* Message handling */
int dynhost_handle_chunk(struct edge_connection_t *conn,
                        uint32_t msg_id,
                        uint32_t total_chunks,
                        uint32_t chunk_seq,
                        const uint8_t *chunk_data,
                        uint16_t chunk_size);

#endif /* !defined(TOR_FEATURE_DYNHOST_DYNHOST_HANDLERS_H) */