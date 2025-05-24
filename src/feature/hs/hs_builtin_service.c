/**
 * \file hs_builtin_service.c
 * \brief Implementation for the hidden service built-in content system.
 *
 * Modifications Copyright 2025 Rhett Creighton
 * Licensed under the Apache License, Version 2.0
 *
 * This file contains modifications to support built-in HTTP handlers
 * for serving static files and dynamic content directly through Tor's
 * internal systems without opening local TCP ports.
 *
 * Original Tor code is Copyright (c) The Tor Project, Inc.
 * and licensed under the Tor license. See LICENSE in the tor directory.
 */

#define HS_BUILTIN_SERVICE_PRIVATE

#include "core/or/or.h"
#include "feature/hs/hs_builtin_service.h"
#include "lib/container/smartlist.h"
#include "lib/log/log.h"
#include "lib/container/map.h"
#include "lib/fs/files.h"
#include "core/mainloop/connection.h"
#include "core/or/relay.h"  /* For connection_edge_send_command */
#include "lib/crypt_ops/crypto_rand.h"
#include "lib/crypt_ops/crypto_rsa.h"
#include "lib/crypt_ops/crypto_util.h"
#include "feature/nodelist/torcert.h"
#include <sys/stat.h>
#include <stdlib.h>  /* For getenv */
#include <time.h>    /* For time functions */
#include <string.h>  /* For strcmp */

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

/** ID for the Hello World handler (HTTP) */
#define BUILTIN_HANDLER_HELLO_WORLD 1

/** ID for the Time Server handler */
#define BUILTIN_HANDLER_TIME_SERVER 2

/**
 * Static site handler for built-in services.
 *
 * Serves static HTML files from a directory.
 * Used for plain HTTP on port 80.
 *
 * @param conn The edge connection to handle
 * @return 0 on success, -1 on error
 */
static hs_builtin_service_status_t
hello_world_handler(edge_connection_t *conn)
{
  char request_data[1024] = {0};
  size_t request_len = 0;
  char requested_path[256] = {0};
  
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
        return HS_SERVICE_HANDLER_ERROR;
      }
      memcpy(request_data, head, to_read);
      log_notice(LD_REND, "HTTP handler received request (%d bytes): %.100s", 
                (int)request_len, request_data);
                
      /* Parse the request to get the requested path */
      if (sscanf(request_data, "GET %255s", requested_path) != 1) {
        strcpy(requested_path, "/");
      }
    } else {
      strcpy(requested_path, "/");
    }
  } else {
    strcpy(requested_path, "/");
  }
  
  /* Default to index.html if the path is / */
  if (strcmp(requested_path, "/") == 0) {
    strcpy(requested_path, "/index.html");
  }
  
  /* Construct the file path */
  char file_path[512];
  const char *static_root = getenv("CHEESEBURGER_STATIC_ROOT");
  if (!static_root) {
    static_root = "./static-site";
  }
  snprintf(file_path, sizeof(file_path), "%s%s", static_root, requested_path);
  
  log_notice(LD_REND, "Attempting to serve file: %s", file_path);
  
  /* Try to read the requested file */
  char *file_content = NULL;
  struct stat st;
  memset(&st, 0, sizeof(st));
  
  /* Check if it's a directory first */
  if (file_status(file_path) == FN_DIR) {
    /* If it's a directory, try to serve index.html in that directory */
    char index_path[1024]; /* Increase buffer size to avoid truncation warning */
    tor_snprintf(index_path, sizeof(index_path), "%s/index.html", file_path);
    
    if (file_status(index_path) == FN_FILE) {
      /* Use the index.html file path instead */
      strlcpy(file_path, index_path, sizeof(file_path));
      log_notice(LD_REND, "Directory access, serving index: %s", file_path);
    }
  }
  
  /* Now try to read the file */
  if (file_status(file_path) == FN_FILE) {
    file_content = read_file_to_str(file_path, RFTS_BIN, &st);
    log_notice(LD_REND, "Read file of size: %zu bytes", (size_t)st.st_size);
  }
  
  size_t file_size = file_content ? (size_t)st.st_size : 0;
  
  if (!file_content) {
    /* File not found, serve a 404 page */
    const char *not_found = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Type: text/html\r\n"
                           "Content-Length: 172\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "<html>\n"
                           "<head><title>404 Not Found</title></head>\n"
                           "<body>\n"
                           "<h1>404 Not Found</h1>\n"
                           "<p>The requested page could not be found.</p>\n"
                           "<p><a href=\"/\">Go to homepage</a></p>\n"
                           "</body>\n"
                           "</html>";
    
    log_notice(LD_REND, "File not found, sending 404 response");
    
    if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                   not_found, strlen(not_found)) < 0) {
      log_warn(LD_REND, "Failed to send 404 response");
      return HS_SERVICE_HANDLER_ERROR;
    }
  } else {
    /* File found, serve it */
    /* Determine content type based on file extension */
    const char *content_type = "text/plain";
    if (strcasestr(file_path, ".html") || strcasestr(file_path, ".htm")) {
      content_type = "text/html";
    } else if (strcasestr(file_path, ".css")) {
      content_type = "text/css";
    } else if (strcasestr(file_path, ".js")) {
      content_type = "application/javascript";
    } else if (strcasestr(file_path, ".jpg") || strcasestr(file_path, ".jpeg")) {
      content_type = "image/jpeg";
    } else if (strcasestr(file_path, ".png")) {
      content_type = "image/png";
    } else if (strcasestr(file_path, ".gif")) {
      content_type = "image/gif";
    }
    
    /* Create the HTTP header */
    char header[1024];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n", content_type, file_size);
    
    log_notice(LD_REND, "Sending file response (%zu bytes, type: %s)", 
               file_size, content_type);
    
    /* Send the header first */
    if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                   header, strlen(header)) < 0) {
      log_warn(LD_REND, "Failed to send response header");
      tor_free(file_content);
      return HS_SERVICE_HANDLER_ERROR;
    }
    
    /* Send the file content in chunks to avoid exceeding relay cell payload size limit */
    const size_t CHUNK_SIZE = 400; /* Stay well under the 509-(1+2+2+4+2) limit */
    size_t bytes_sent = 0;
    
    while (bytes_sent < file_size) {
      /* Calculate the size of the next chunk */
      size_t chunk_size = file_size - bytes_sent;
      if (chunk_size > CHUNK_SIZE)
        chunk_size = CHUNK_SIZE;
      
      /* Send this chunk */
      if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                    file_content + bytes_sent, chunk_size) < 0) {
        log_warn(LD_REND, "Failed to send file content chunk at offset %zu", bytes_sent);
        tor_free(file_content);
        return HS_SERVICE_HANDLER_ERROR;
      }
      
      bytes_sent += chunk_size;
      log_debug(LD_REND, "Sent chunk of %zu bytes, total sent: %zu/%zu", 
               chunk_size, bytes_sent, file_size);
    }
    
    tor_free(file_content);
  }
  
  /* Consume all input data to avoid warnings about unprocessed data */
  buf_clear(TO_CONN(conn)->inbuf);
  
  log_info(LD_REND, "Static file handler processed request successfully");
  return HS_SERVICE_HANDLER_DONE;
}

/**
 * Time server handler for built-in services.
 *
 * Serves current local time as HTML page.
 * Used for dynamic HTTP content on port 80.
 *
 * @param conn The edge connection to handle
 * @return HS_SERVICE_HANDLER_DONE on success, HS_SERVICE_HANDLER_ERROR on error
 */
static hs_builtin_service_status_t
time_server_handler(edge_connection_t *conn)
{
  log_notice(LD_REND, "Processing time server request");
  
  /* Get current time */
  time_t now = time(NULL);
  struct tm *local_time = localtime(&now);
  
  /* Format time string */
  char time_str[64];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", local_time);
  
  /* Create HTML response */
  char html_content[1024];
  snprintf(html_content, sizeof(html_content),
           "<!DOCTYPE html>\n"
           "<html>\n"
           "<head>\n"
           "  <title>Cheeseburger Time Server</title>\n"
           "  <meta http-equiv=\"refresh\" content=\"30\">\n"
           "  <style>\n"
           "    body { font-family: Arial, sans-serif; text-align: center; margin-top: 100px; }\n"
           "    .time { font-size: 48px; color: #333; margin: 20px; }\n"
           "    .info { font-size: 18px; color: #666; }\n"
           "  </style>\n"
           "</head>\n"
           "<body>\n"
           "  <h1>🍔 Cheeseburger Time Server</h1>\n"
           "  <div class=\"time\">%s</div>\n"
           "  <div class=\"info\">Local time on the server</div>\n"
           "  <div class=\"info\">Updates every 30 seconds</div>\n"
           "</body>\n"
           "</html>",
           time_str);
  
  /* Create the HTTP header */
  char header[1024];
  snprintf(header, sizeof(header),
           "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/html; charset=UTF-8\r\n"
           "Content-Length: %zu\r\n"
           "Cache-Control: no-cache\r\n"
           "Connection: close\r\n"
           "\r\n", strlen(html_content));
  
  log_notice(LD_REND, "Sending time server response for time: %s", time_str);
  
  /* Send the header first */
  if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                 header, strlen(header)) < 0) {
    log_warn(LD_REND, "Failed to send time server response header");
    return HS_SERVICE_HANDLER_ERROR;
  }
  
  /* Send the HTML content in chunks to avoid exceeding relay cell payload size limit */
  const size_t CHUNK_SIZE = 400; /* Stay well under the 509-(1+2+2+4+2) limit */
  size_t content_size = strlen(html_content);
  size_t bytes_sent = 0;
  
  while (bytes_sent < content_size) {
    /* Calculate the size of the next chunk */
    size_t chunk_size = content_size - bytes_sent;
    if (chunk_size > CHUNK_SIZE)
      chunk_size = CHUNK_SIZE;
    
    /* Send this chunk */
    if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                  html_content + bytes_sent, chunk_size) < 0) {
      log_warn(LD_REND, "Failed to send time server content chunk at offset %zu", bytes_sent);
      return HS_SERVICE_HANDLER_ERROR;
    }
    
    bytes_sent += chunk_size;
    log_debug(LD_REND, "Sent time server chunk of %zu bytes, total sent: %zu/%zu", 
             chunk_size, bytes_sent, content_size);
  }
  
  /* Consume all input data */
  buf_clear(TO_CONN(conn)->inbuf);
  
  log_info(LD_REND, "Time server handler processed request successfully");
  return HS_SERVICE_HANDLER_DONE;
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
 * 
 * Finds and calls the appropriate handler for the given virtual port.
 * 
 * @param conn The edge connection to handle
 * @param virtual_port The virtual port from the client connection
 * @return HS_SERVICE_HANDLER_ERROR on error (should close connection)
 *         HS_SERVICE_HANDLER_DONE if handler is done (connection can be closed)
 *         HS_SERVICE_HANDLER_WAIT if handler needs more data (keep connection open)
 */
hs_builtin_service_status_t
hs_handle_builtin_service(edge_connection_t *conn, uint16_t virtual_port)
{
  hs_builtin_service_handler_t handler;
  
  /* Look up the handler for this port */
  handler = get_builtin_service_handler(virtual_port);
  if (!handler) {
    log_warn(LD_REND, "No handler found for virtual port %d", virtual_port);
    connection_mark_for_close(TO_CONN(conn));
    return HS_SERVICE_HANDLER_ERROR;
  }
  
  /* Call the handler */
  log_notice(LD_REND, "Processing built-in service request for port %d", 
           virtual_port);
  hs_builtin_service_status_t result = handler(conn);
  
  /* Log the handler's status */
  if (result == HS_SERVICE_HANDLER_ERROR) {
    log_warn(LD_REND, "Handler for port %d returned ERROR status", virtual_port);
    connection_mark_for_close(TO_CONN(conn));
  } else if (result == HS_SERVICE_HANDLER_DONE) {
    log_info(LD_REND, "Handler for port %d returned DONE status", virtual_port);
  } else if (result == HS_SERVICE_HANDLER_WAIT) {
    log_info(LD_REND, "Handler for port %d returned WAIT status (needs more data)", virtual_port);
  }
  
  return result;
}

/**
 * Register default built-in service handlers.
 */
void
hs_builtin_service_add_default_handlers(void)
{
  const char *service_type = getenv("CHEESEBURGER_SERVICE_TYPE");
  
  /* Register our handlers */
  hs_register_builtin_service_handler(BUILTIN_HANDLER_HELLO_WORLD, 
                                    hello_world_handler);
  hs_register_builtin_service_handler(BUILTIN_HANDLER_TIME_SERVER,
                                    time_server_handler);
  
  /* Map ports to handlers based on service type */
  if (service_type && strcmp(service_type, "time") == 0) {
    hs_register_builtin_service_port(80, BUILTIN_HANDLER_TIME_SERVER);
    log_notice(LD_REND, "Registered TIME SERVER handler for port 80");
  } else {
    hs_register_builtin_service_port(80, BUILTIN_HANDLER_HELLO_WORLD);
    log_notice(LD_REND, "Registered STATIC FILE handler for port 80");
  }
}