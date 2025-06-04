/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_webserver.h
 * @brief Header for timestamp web server
 **/

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_WEBSERVER_H
#define TOR_FEATURE_DYNHOST_DYNHOST_WEBSERVER_H

struct edge_connection_t;

int dynhost_webserver_handle_request(struct edge_connection_t *conn,
                                    const uint8_t *data, size_t len);
int dynhost_webserver_has_complete_request(const uint8_t *data, size_t len);

#endif /* !defined(TOR_FEATURE_DYNHOST_DYNHOST_WEBSERVER_H) */