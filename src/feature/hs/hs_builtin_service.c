/**
 * \file hs_builtin_service.c
 * \brief Implementation for the hidden service built-in content system.
 */

#define HS_BUILTIN_SERVICE_PRIVATE

#include "core/or/or.h"
#include "feature/hs/hs_builtin_service.h"
#include "lib/container/smartlist.h"
#include "lib/log/log.h"
#include "lib/container/map.h"
#include "core/mainloop/connection.h"
#include "core/or/relay.h"  /* For connection_edge_send_command */

/* ================================================================== */
/* Data structures and static variables */

/** Structure to map virtual ports to handler IDs */
typedef struct {
  uint16_t virtual_port;  /**< Port number on the onion service */
  uint16_t handler_id;    /**< Internal ID for the handler (not a real port) */
} builtin_service_port_t;

/** List of virtual port mappings */
static smartlist_t *builtin_service_ports = NULL;

/** Map of handler IDs to handler functions */
static digestmap_t *builtin_service_handlers = NULL;

/** ID for the Hello World handler */
#define BUILTIN_HANDLER_HELLO_WORLD 1

/* ================================================================== */
/* Private functions */

/**
 * Simple HTTP Hello World handler for built-in services.
 *
 * Responds to any HTTP request with a simple Hello World page.
 *
 * @param conn The edge connection to handle
 * @return 0 on success, -1 on error
 */
static int
hello_world_handler(edge_connection_t *conn)
{
  char request_data[1024] = {0};
  size_t request_len = 0;
  
  /* Read the request data if any */
  if (TO_CONN(conn)->inbuf) {
    request_len = connection_get_inbuf_len(TO_CONN(conn));
    if (request_len > 0) {
      size_t to_read = request_len > sizeof(request_data)-1 ? sizeof(request_data)-1 : request_len;
      const char *head;
      size_t len_out;
      buf_pullup(TO_CONN(conn)->inbuf, to_read, &head, &len_out);
      if (len_out < to_read) {
        log_warn(LD_REND, "Unable to read enough request data from buffer");
        return -1;
      }
      memcpy(request_data, head, to_read);
      log_notice(LD_REND, "Built-in service received request (%d bytes): %.100s", 
                (int)request_len, request_data);
    }
  }
  
  /* Prepare a simple HTTP response with Hello World */
  const char *response = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: 185\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<html>\n"
    "<head><title>Hello from Tor</title></head>\n"
    "<body>\n"
    "<h1>Hello, Onion World!</h1>\n"
    "<p>This page is served directly from your Tor process.</p>\n"
    "<p>No external web server is involved.</p>\n"
    "</body>\n"
    "</html>";
  
  log_notice(LD_REND, "Built-in service sending response (%d bytes)", 
             (int)strlen(response));
             
  /* Send the response as relay data through the circuit */
  if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                  response, strlen(response)) < 0) {
    log_warn(LD_REND, "Failed to send response through circuit");
    return -1;
  }
  
  /* Consume all input data to avoid warnings about unprocessed data */
  buf_clear(TO_CONN(conn)->inbuf);
  
  log_info(LD_REND, "Hello World handler processed request successfully");
  return 0;
}

/**
 * Get the handler function for a specific virtual port.
 *
 * @param virtual_port The virtual port to look up
 * @return The handler function, or NULL if none is registered
 */
static hs_builtin_service_handler_t
get_builtin_service_handler(uint16_t virtual_port)
{
  /* Check if we have initialized the system */
  if (!builtin_service_ports || !builtin_service_handlers)
    return NULL;
    
  /* Find the port mapping entry */
  SMARTLIST_FOREACH_BEGIN(builtin_service_ports, builtin_service_port_t *, port) {
    if (port->virtual_port == virtual_port) {
      /* Found a matching port, look up the handler */
      char digest[DIGEST_LEN];
      memset(digest, 0, sizeof(digest));
      memcpy(digest, &port->handler_id, sizeof(uint16_t));
      return (hs_builtin_service_handler_t)digestmap_get(builtin_service_handlers, digest);
    }
  } SMARTLIST_FOREACH_END(port);
  
  return NULL;
}

/* ================================================================== */
/* Public functions */

/**
 * Initialize the built-in service subsystem.
 */
void
hs_builtin_service_init(void)
{
  if (!builtin_service_ports)
    builtin_service_ports = smartlist_new();
    
  if (!builtin_service_handlers)
    builtin_service_handlers = digestmap_new();
    
  log_info(LD_REND, "Built-in service subsystem initialized");
}

/**
 * Register a built-in service handler for a specific virtual port.
 */
void
hs_register_builtin_service_port(uint16_t virtual_port, uint16_t handler_id)
{
  tor_assert(builtin_service_ports);
  
  /* Check if this port is already registered */
  SMARTLIST_FOREACH_BEGIN(builtin_service_ports, builtin_service_port_t *, port) {
    if (port->virtual_port == virtual_port) {
      /* Update the existing entry */
      port->handler_id = handler_id;
      log_info(LD_REND, "Updated built-in service mapping for port %d to handler %d",
               virtual_port, handler_id);
      return;
    }
  } SMARTLIST_FOREACH_END(port);
  
  /* Create a new mapping */
  builtin_service_port_t *port = tor_malloc_zero(sizeof(builtin_service_port_t));
  port->virtual_port = virtual_port;
  port->handler_id = handler_id;
  smartlist_add(builtin_service_ports, port);
  
  log_info(LD_REND, "Registered built-in service for port %d using handler %d",
           virtual_port, handler_id);
}

/**
 * Register a handler function with a specific handler ID.
 */
void
hs_register_builtin_service_handler(uint16_t handler_id, 
                                  hs_builtin_service_handler_t handler)
{
  char digest[DIGEST_LEN];
  
  tor_assert(builtin_service_handlers);
  tor_assert(handler);
  
  /* Use the handler ID as key in the digestmap */
  memset(digest, 0, sizeof(digest));
  memcpy(digest, &handler_id, sizeof(uint16_t));
  
  digestmap_set(builtin_service_handlers, digest, (void*)handler);
  
  log_info(LD_REND, "Registered handler for built-in service ID %d", handler_id);
}

/**
 * Check if a virtual port is configured to use a built-in service handler.
 */
int
hs_has_builtin_service_for_port(uint16_t virtual_port)
{
  return get_builtin_service_handler(virtual_port) != NULL;
}

/**
 * Process a connection with the appropriate built-in service handler.
 */
int
hs_handle_builtin_service(edge_connection_t *conn, uint16_t virtual_port)
{
  hs_builtin_service_handler_t handler;
  
  /* Look up the handler for this port */
  handler = get_builtin_service_handler(virtual_port);
  if (!handler) {
    log_warn(LD_REND, "No handler found for virtual port %d", virtual_port);
    connection_mark_for_close(TO_CONN(conn));
    return -1;
  }
  
  /* Call the handler */
  log_notice(LD_REND, "Processing built-in service request for port %d", 
           virtual_port);
  int result = handler(conn);
  
  /* If handler returned an error, mark connection for close */
  if (result < 0) {
    log_warn(LD_REND, "Handler for port %d returned error %d", virtual_port, result);
    connection_mark_for_close(TO_CONN(conn));
  }
  
  return result;
}

/**
 * Register default built-in service handlers.
 */
void
hs_builtin_service_add_default_handlers(void)
{
  /* Register the Hello World handler */
  hs_register_builtin_service_handler(BUILTIN_HANDLER_HELLO_WORLD, 
                                    hello_world_handler);
  
  /* Map HTTP and HTTPS ports to the Hello World handler */
  hs_register_builtin_service_port(80, BUILTIN_HANDLER_HELLO_WORLD);
  hs_register_builtin_service_port(443, BUILTIN_HANDLER_HELLO_WORLD);
  
  log_notice(LD_REND, "Default built-in service handlers registered "
                      "for ports 80 and 443");
}