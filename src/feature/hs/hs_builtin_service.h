/**
 * \file hs_builtin_service.h
 * \brief Header file for the hidden service built-in content system.
 *
 * This subsystem allows Tor to serve content directly from within the
 * process without connecting to actual local ports, making it simpler
 * and more secure to host simple web content through onion services.
 */

#ifndef TOR_HS_BUILTIN_SERVICE_H
#define TOR_HS_BUILTIN_SERVICE_H

#include "core/or/or.h"
#include "core/or/edge_connection_st.h"

/** 
 * Status codes for built-in service handlers
 */
typedef enum {
  HS_SERVICE_HANDLER_ERROR = -1,  /**< An error occurred, connection should be closed */
  HS_SERVICE_HANDLER_DONE = 0,    /**< Handler is done, connection can be closed */
  HS_SERVICE_HANDLER_WAIT = 1     /**< Handler needs more data, don't close connection */
} hs_builtin_service_status_t;

/** 
 * Function signature for built-in service content handlers.
 *
 * A handler receives the edge connection and should process any data in the
 * input buffer, generate a response, and write it to the output buffer.
 *
 * @param conn The edge connection to handle
 * @return HS_SERVICE_HANDLER_ERROR on error (should close connection)
 *         HS_SERVICE_HANDLER_DONE if handler is done (connection can be closed)
 *         HS_SERVICE_HANDLER_WAIT if handler needs more data (keep connection open)
 */
typedef hs_builtin_service_status_t (*hs_builtin_service_handler_t)(edge_connection_t *conn);

/**
 * Initialize the built-in service subsystem.
 * 
 * This must be called during Tor initialization before any other
 * built-in service functions.
 */
void hs_builtin_service_init(void);

/**
 * Register a built-in service handler for a specific virtual port.
 *
 * @param virtual_port The virtual port on the hidden service (e.g. 80, 443)
 * @param handler_id An internal identifier for the handler (not a real port)
 */
void hs_register_builtin_service_port(uint16_t virtual_port, uint16_t handler_id);

/**
 * Register a handler function with a specific handler ID.
 *
 * @param handler_id The internal identifier for this handler
 * @param handler The handler function to register
 */
void hs_register_builtin_service_handler(uint16_t handler_id, 
                                       hs_builtin_service_handler_t handler);

/**
 * Check if a virtual port is configured to use a built-in service handler.
 *
 * @param virtual_port The virtual port to check
 * @return 1 if a handler exists, 0 otherwise
 */
int hs_has_builtin_service_for_port(uint16_t virtual_port);

/**
 * Process a connection with the appropriate built-in service handler.
 * 
 * Finds and calls the appropriate handler for the given virtual port.
 * 
 * @param conn The edge connection to handle
 * @param virtual_port The virtual port from the client connection
 * @return HS_SERVICE_HANDLER_ERROR on error (should close connection)
 *         HS_SERVICE_HANDLER_DONE if handler is done (connection can be closed)
 *         HS_SERVICE_HANDLER_WAIT if handler needs more data (keep connection open)
 */
hs_builtin_service_status_t hs_handle_builtin_service(edge_connection_t *conn, 
                                                   uint16_t virtual_port);

/**
 * Register default built-in service handlers.
 *
 * This registers the Hello World handler for ports 80 by default.
 */
void hs_builtin_service_add_default_handlers(void);

#endif /* !defined(TOR_HS_BUILTIN_SERVICE_H) */