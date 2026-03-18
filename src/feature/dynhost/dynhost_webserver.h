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

/* External handler registration — embedding applications call this to
 * receive all .onion HTTP requests instead of the built-in demos. */
typedef size_t (*dynhost_external_handler_fn)(const char *method, const char *path,
                                              const uint8_t *body, size_t body_len,
                                              uint8_t *response, size_t response_max,
                                              void *ctx);
void dynhost_webserver_set_external_handler(dynhost_external_handler_fn handler,
                                            void *ctx);

#endif /* !defined(TOR_FEATURE_DYNHOST_DYNHOST_WEBSERVER_H) */