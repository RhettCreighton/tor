/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_webserver.c
 * @brief Calculator web server for dynamic onion host
 **/

#include "core/or/or.h"
#include "feature/dynhost/dynhost.h"
#include "feature/dynhost/dynhost_handlers.h"
#include "feature/dynhost/dynhost_webserver.h"
#include "core/or/edge_connection_st.h"
#include "core/or/relay.h"
#include "lib/log/log.h"
#include "lib/malloc/malloc.h"
#include "lib/string/printf.h"
#include "lib/string/util_string.h"
#include "feature/dynhost/dynhost_mvc.h"
#include "feature/dynhost/dynhost_blog.h"

#include <time.h>

/** HTTP response template */
static const char HTTP_RESPONSE_TEMPLATE[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: text/html; charset=UTF-8\r\n"
  "Content-Length: %zu\r\n"
  "Connection: close\r\n"
  "Cache-Control: no-cache\r\n"
  "\r\n"
  "%s";

/** Main menu HTML template */
static const char MAIN_MENU_HTML[] =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>Tor Dynhost Demo Server</title>\n"
  "  <meta charset=\"UTF-8\">\n"
  "  <style>\n"
  "    body { font-family: Arial, sans-serif; max-width: 800px; "
  "margin: 50px auto; padding: 20px; background: #f0f0f0; }\n"
  "    h1 { color: #333; text-align: center; }\n"
  "    .demo-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); "
  "gap: 20px; margin-top: 30px; }\n"
  "    .demo-card { background: white; padding: 30px; border-radius: 10px; "
  "box-shadow: 0 2px 10px rgba(0,0,0,0.1); text-align: center; "
  "transition: transform 0.2s; }\n"
  "    .demo-card:hover { transform: translateY(-5px); }\n"
  "    .demo-card h2 { color: #4CAF50; margin-bottom: 15px; }\n"
  "    .demo-card p { color: #666; margin-bottom: 20px; }\n"
  "    .demo-card a { display: inline-block; padding: 12px 30px; "
  "background: #4CAF50; color: white; text-decoration: none; "
  "border-radius: 5px; transition: background 0.2s; }\n"
  "    .demo-card a:hover { background: #45a049; }\n"
  "    .info { margin-top: 40px; padding: 20px; background: #e3f2fd; "
  "border-radius: 10px; color: #1976d2; text-align: center; }\n"
  "  </style>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>Tor Dynamic Onion Host - Demo Server</h1>\n"
  "  <div class=\"demo-grid\">\n"
  "    <div class=\"demo-card\">\n"
  "      <h2>⏰ Time Server</h2>\n"
  "      <p>Display the current server time with automatic updates</p>\n"
  "      <a href=\"/time\">View Time</a>\n"
  "    </div>\n"
  "    <div class=\"demo-card\">\n"
  "      <h2>🧮 Calculator</h2>\n"
  "      <p>Add 100 to any number you enter</p>\n"
  "      <a href=\"/calculator\">Try Calculator</a>\n"
  "    </div>\n"
  "    <div class=\"demo-card\">\n"
  "      <h2>📝 MVC Blog</h2>\n"
  "      <p>Full-featured RESTful blog with posts and comments</p>\n"
  "      <a href=\"/blog\">Visit Blog</a>\n"
  "    </div>\n"
  "  </div>\n"
  "  <div class=\"info\">\n"
  "    <strong>About this server:</strong><br>\n"
  "    This web server is running entirely inside the Tor binary itself!<br>\n"
  "    No external ports, no separate process - just pure Tor magic.\n"
  "  </div>\n"
  "</body>\n"
  "</html>\n";

/** Time server HTML template */
static const char TIME_HTML_TEMPLATE[] =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>Tor Dynhost Timestamp Server</title>\n"
  "  <meta charset=\"UTF-8\">\n"
  "  <style>\n"
  "    body { font-family: monospace; background: #1a1a1a; "
  "color: #00ff00; padding: 20px; text-align: center; }\n"
  "    h1 { font-size: 24px; margin-bottom: 30px; }\n"
  "    .time { font-size: 48px; margin: 20px 0; }\n"
  "    .info { font-size: 16px; color: #888; margin: 10px 0; }\n"
  "    .nav { margin-top: 40px; }\n"
  "    .nav a { color: #00ff00; margin: 0 10px; }\n"
  "  </style>\n"
  "  <script>\n"
  "    setTimeout(function() { location.reload(); }, 1000);\n"
  "  </script>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>Tor Dynamic Onion Host - Time Server</h1>\n"
  "  <div class=\"time\">%s</div>\n"
  "  <div class=\"info\">Current Unix Timestamp: %ld</div>\n"
  "  <div class=\"info\">Running inside Tor binary - No external ports!</div>\n"
  "  <div class=\"nav\">\n"
  "    <a href=\"/\">Back to Menu</a> | <a href=\"/time\">Refresh</a>\n"
  "  </div>\n"
  "</body>\n"
  "</html>\n";

/** Calculator form HTML template */
static const char FORM_HTML_TEMPLATE[] =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>Tor Dynhost Calculator</title>\n"
  "  <meta charset=\"UTF-8\">\n"
  "  <style>\n"
  "    body { font-family: Arial, sans-serif; max-width: 600px; "
  "margin: 50px auto; padding: 20px; background: #f0f0f0; }\n"
  "    h1 { color: #333; }\n"
  "    form { background: white; padding: 30px; border-radius: 10px; "
  "box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n"
  "    label { display: block; margin-bottom: 10px; font-weight: bold; }\n"
  "    input[type=\"number\"] { width: 100%%; padding: 10px; "
  "font-size: 18px; border: 2px solid #ddd; border-radius: 5px; "
  "box-sizing: border-box; }\n"
  "    input[type=\"submit\"] { width: 100%%; padding: 12px; "
  "margin-top: 20px; font-size: 18px; background: #4CAF50; "
  "color: white; border: none; border-radius: 5px; cursor: pointer; }\n"
  "    input[type=\"submit\"]:hover { background: #45a049; }\n"
  "    .info { margin-top: 20px; padding: 15px; background: #e3f2fd; "
  "border-radius: 5px; color: #1976d2; }\n"
  "    .nav { text-align: center; margin-bottom: 20px; }\n"
  "    .nav a { color: #4CAF50; text-decoration: none; }\n"
  "  </style>\n"
  "</head>\n"
  "<body>\n"
  "  <div class=\"nav\"><a href=\"/\">← Back to Menu</a></div>\n"
  "  <h1>Tor Dynamic Host Calculator</h1>\n"
  "  <form method=\"POST\" action=\"/calculator\">\n"
  "    <label for=\"number\">Enter a number:</label>\n"
  "    <input type=\"number\" id=\"number\" name=\"number\" "
  "required autofocus placeholder=\"Enter any number\">\n"
  "    <input type=\"submit\" value=\"Add 100\">\n"
  "  </form>\n"
  "  <div class=\"info\">\n"
  "    <strong>How it works:</strong><br>\n"
  "    This form is served directly from inside the Tor binary. "
  "When you submit a number, the embedded server adds 100 to it "
  "and returns the result. No external web server required!\n"
  "  </div>\n"
  "</body>\n"
  "</html>\n";

/** Result page HTML template */
static const char RESULT_HTML_TEMPLATE[] =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>Tor Dynhost Calculator - Result</title>\n"
  "  <meta charset=\"UTF-8\">\n"
  "  <style>\n"
  "    body { font-family: Arial, sans-serif; max-width: 600px; "
  "margin: 50px auto; padding: 20px; background: #f0f0f0; }\n"
  "    .result { background: #e0ffe0; padding: 20px; "
  "border-radius: 10px; margin: 20px 0; text-align: center; }\n"
  "    .result-number { font-size: 48px; color: #008000; }\n"
  "    .calculation { font-size: 20px; color: #666; }\n"
  "    a { display: inline-block; margin-top: 20px; padding: 10px 20px; "
  "background: #4CAF50; color: white; text-decoration: none; "
  "border-radius: 5px; }\n"
  "    a:hover { background: #45a049; }\n"
  "  </style>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>Calculation Result</h1>\n"
  "  <div class=\"result\">\n"
  "    <div class=\"calculation\">100 + %d =</div>\n"
  "    <div class=\"result-number\">%d</div>\n"
  "  </div>\n"
  "  <p>This calculation was performed inside the Tor binary!</p>\n"
  "  <a href=\"/calculator\">Calculate Another Number</a>\n"
  "  <a href=\"/\" style=\"margin-left: 20px;\">Back to Menu</a>\n"
  "</body>\n"
  "</html>\n";

/** Parse HTTP request and extract method and path */
static int
parse_http_request(const char *request, size_t len,
                   char **method_out, char **path_out)
{
  const char *end_of_line = memchr(request, '\n', len);
  if (!end_of_line) {
    return -1;
  }
  
  size_t line_len = end_of_line - request;
  if (line_len > 0 && request[line_len - 1] == '\r') {
    line_len--;
  }
  
  // Find method
  const char *space1 = memchr(request, ' ', line_len);
  if (!space1) {
    return -1;
  }
  
  *method_out = tor_strndup(request, space1 - request);
  
  // Find path
  const char *path_start = space1 + 1;
  const char *space2 = memchr(path_start, ' ', 
                              line_len - (path_start - request));
  if (!space2) {
    tor_free(*method_out);
    return -1;
  }
  
  *path_out = tor_strndup(path_start, space2 - path_start);
  
  return 0;
}

/** Parse URL-encoded form data to extract a field value */
static int
parse_form_field(const char *data, const char *field_name, char *out, size_t out_len)
{
  char search_pattern[128];
  tor_snprintf(search_pattern, sizeof(search_pattern), "%s=", field_name);
  
  const char *field_start = strstr(data, search_pattern);
  if (!field_start) {
    return -1;
  }
  
  field_start += strlen(search_pattern);
  
  const char *field_end = strchr(field_start, '&');
  size_t field_len;
  if (field_end) {
    field_len = field_end - field_start;
  } else {
    field_len = strlen(field_start);
  }
  
  if (field_len >= out_len) {
    field_len = out_len - 1;
  }
  
  memcpy(out, field_start, field_len);
  out[field_len] = '\0';
  
  /* Simple URL decode - convert + to space */
  char *p = out;
  while (*p) {
    if (*p == '+') *p = ' ';
    p++;
  }
  
  return 0;
}

/** Handle HTTP request and generate response */
int
dynhost_webserver_handle_request(edge_connection_t *conn,
                                const uint8_t *data, size_t len)
{
  char *method = NULL;
  char *path = NULL;
  char *response = NULL;
  int result = -1;
  
  log_notice(LD_REND, "Webserver received request of %zu bytes", len);
  log_notice(LD_REND, "First 100 chars: %.100s", (const char *)data);
  
  // Parse HTTP request
  if (parse_http_request((const char *)data, len, &method, &path) < 0) {
    log_warn(LD_REND, "Failed to parse HTTP request");
    goto done;
  }
  
  log_notice(LD_REND, "HTTP %s %s", method, path);
  
  // Route based on path
  if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0) {
    // Show main menu
    size_t html_len = strlen(MAIN_MENU_HTML);
    tor_asprintf(&response, HTTP_RESPONSE_TEMPLATE, html_len, MAIN_MENU_HTML);
  } else if (strcmp(path, "/time") == 0 && strcmp(method, "GET") == 0) {
    // Show time server
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    char *html;
    tor_asprintf(&html, TIME_HTML_TEMPLATE, time_str, (long)now);
    size_t html_len = strlen(html);
    
    tor_asprintf(&response, HTTP_RESPONSE_TEMPLATE, html_len, html);
    tor_free(html);
  } else if (strcmp(path, "/calculator") == 0) {
    if (strcmp(method, "GET") == 0) {
      // Show calculator form
      size_t html_len = strlen(FORM_HTML_TEMPLATE);
      tor_asprintf(&response, HTTP_RESPONSE_TEMPLATE, html_len, FORM_HTML_TEMPLATE);
    } else if (strcmp(method, "POST") == 0) {
      // Handle calculator POST
      const char *body_start = tor_memmem(data, len, "\r\n\r\n", 4);
      if (body_start) {
        body_start += 4;  // Skip past \r\n\r\n
        
        char number_str[32] = {0};
        if (parse_form_field(body_start, "number", number_str, sizeof(number_str)) == 0) {
          // Parse the number and calculate result
          int number = atoi(number_str);
          int result_value = 100 + number;
          
          // Generate result page
          char *html;
          tor_asprintf(&html, RESULT_HTML_TEMPLATE, number, result_value);
          size_t html_len = strlen(html);
          
          // Build HTTP response
          tor_asprintf(&response, HTTP_RESPONSE_TEMPLATE, html_len, html);
          tor_free(html);
          
          log_notice(LD_REND, "Calculated: 100 + %d = %d", number, result_value);
        } else {
          // Error parsing form data
          const char *error_html = "<html><body><h1>Error parsing form data</h1>"
                                   "<p><a href=\"/calculator\">Try again</a></p></body></html>";
          tor_asprintf(&response, HTTP_RESPONSE_TEMPLATE, strlen(error_html), error_html);
        }
      } else {
        // No body found
        const char *error_html = "<html><body><h1>No form data received</h1>"
                                 "<p><a href=\"/calculator\">Try again</a></p></body></html>";
        tor_asprintf(&response, HTTP_RESPONSE_TEMPLATE, strlen(error_html), error_html);
      }
    } else {
      // Method not allowed for calculator
      const char *error_response = 
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
      connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                  error_response, strlen(error_response));
      result = 0;
      goto done;
    }
  } else if (strstr(path, "/blog") == path) {
    // Handle blog routes through MVC framework
    mvc_app_t *blog_app = dynhost_blog_get_app();
    if (!blog_app) {
      // Initialize blog app if not already done
      dynhost_blog_init();
      blog_app = dynhost_blog_get_app();
    }
    
    if (blog_app) {
      // Create MVC request from HTTP data
      mvc_request_t *mvc_req = mvc_request_from_http((const char *)data, len, conn);
      if (mvc_req) {
        mvc_response_t *mvc_resp = NULL;
        
        // Handle dynamic routes for blog posts and comments
        if (strstr(path, "/blog/post/") == path && strcmp(method, "GET") == 0) {
          // Extract post ID from path like /blog/post/123
          const char *id_start = path + strlen("/blog/post/");
          char *id_str = tor_strdup(id_start);
          strmap_set(mvc_req->params, "id", id_str);
          
          // Get the posts controller and call show action
          mvc_controller_t *posts_ctrl = strmap_get(blog_app->controllers, "PostsController");
          if (posts_ctrl) {
            mvc_resp = mvc_response_new(200);
            void (*show_action)(mvc_controller_t *, mvc_request_t *, mvc_response_t *);
            show_action = strmap_get(posts_ctrl->actions, "show");
            if (show_action) {
              show_action(posts_ctrl, mvc_req, mvc_resp);
            }
          }
        } else if (strstr(path, "/blog/post/") == path && 
                   strstr(path, "/comment") && strcmp(method, "POST") == 0) {
          // Extract post ID for comment creation
          const char *id_start = path + strlen("/blog/post/");
          const char *id_end = strstr(id_start, "/comment");
          if (id_end) {
            size_t id_len = id_end - id_start;
            char *post_id = tor_malloc(id_len + 1);
            memcpy(post_id, id_start, id_len);
            post_id[id_len] = '\0';
            strmap_set(mvc_req->params, "post_id", post_id);
            
            // Get the comments controller and call create action
            mvc_controller_t *comments_ctrl = strmap_get(blog_app->controllers, "CommentsController");
            if (comments_ctrl) {
              mvc_resp = mvc_response_new(200);
              void (*create_action)(mvc_controller_t *, mvc_request_t *, mvc_response_t *);
              create_action = strmap_get(comments_ctrl->actions, "create");
              if (create_action) {
                create_action(comments_ctrl, mvc_req, mvc_resp);
              }
            }
          }
        } else {
          // Use the router for standard routes
          mvc_router_t *router = mvc_app_get_router(blog_app);
          if (router) {
            mvc_router_dispatch(router, mvc_req, &mvc_resp);
          }
        }
        
        // Convert MVC response to HTTP
        if (mvc_resp) {
          char *http_response = mvc_response_to_http(mvc_resp);
          
          // IMPORTANT: Send response in chunks (relay cells have 498 byte limit)
          // Without chunking, large responses will trigger:
          // "Tried to send a command 2 of length XXXX in a v0 cell"
          size_t resp_len = strlen(http_response);
          const char *ptr = http_response;
          size_t remaining = resp_len;
          
          while (remaining > 0) {
            size_t chunk_size = MIN(remaining, 498);
            if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                           ptr, chunk_size) < 0) {
              log_warn(LD_REND, "Failed to send blog response data");
              tor_free(http_response);
              mvc_response_free(mvc_resp);
              result = -1;
              goto done;
            }
            ptr += chunk_size;
            remaining -= chunk_size;
          }
          
          tor_free(http_response);
          mvc_response_free(mvc_resp);
          
          /* Send END cell to close the connection after response */
          connection_edge_send_command(conn, RELAY_COMMAND_END,
                                      (const char*)&(uint8_t){END_STREAM_REASON_DONE},
                                      1);
          result = 0;
        }
        
        mvc_request_free(mvc_req);
      }
    }
    
    if (!response && result != 0) {
      // If blog handling failed, return 500 error
      const char *error_response = 
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 38\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<h1>500 Internal Server Error</h1>\n";
      
      // Send error response in chunks if needed
      size_t err_len = strlen(error_response);
      const char *ptr = error_response;
      size_t remaining = err_len;
      
      while (remaining > 0) {
        size_t chunk_size = MIN(remaining, 498);
        if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                       ptr, chunk_size) < 0) {
          log_warn(LD_REND, "Failed to send error response");
          result = -1;
          goto done;
        }
        ptr += chunk_size;
        remaining -= chunk_size;
      }
      
      /* Send END cell to close the connection */
      connection_edge_send_command(conn, RELAY_COMMAND_END,
                                  (const char*)&(uint8_t){END_STREAM_REASON_DONE},
                                  1);
      result = 0;
    }
    goto done;
  } else {
    // 404 Not Found
    const char *not_found_html = 
      "<html><body><h1>404 Not Found</h1>"
      "<p>The requested page was not found.</p>"
      "<p><a href=\"/\">Go to Home</a></p></body></html>";
    const char *not_found_response = 
      "HTTP/1.1 404 Not Found\r\n"
      "Content-Type: text/html; charset=UTF-8\r\n"
      "Content-Length: %zu\r\n"
      "Connection: close\r\n"
      "\r\n"
      "%s";
    tor_asprintf(&response, not_found_response, strlen(not_found_html), not_found_html);
  }
  
  if (!response) {
    // Something went wrong, send 500 error
    const char *error_html = "<html><body><h1>500 Internal Server Error</h1></body></html>";
    const char *error_response = 
      "HTTP/1.1 500 Internal Server Error\r\n"
      "Content-Type: text/html; charset=UTF-8\r\n"
      "Content-Length: %zu\r\n"
      "Connection: close\r\n"
      "\r\n"
      "%s";
    tor_asprintf(&response, error_response, strlen(error_html), error_html);
  }
  
  // Send response in chunks if needed (relay cells have size limits)
  size_t response_len = strlen(response);
  const char *ptr = response;
  size_t remaining = response_len;
  
  while (remaining > 0) {
    size_t chunk_size = MIN(remaining, 498);  /* RELAY_PAYLOAD_SIZE */
    if (connection_edge_send_command(conn, RELAY_COMMAND_DATA,
                                     ptr, chunk_size) < 0) {
      log_warn(LD_REND, "Failed to send response data");
      result = -1;
      goto done;
    }
    ptr += chunk_size;
    remaining -= chunk_size;
  }
  
  log_notice(LD_REND, "Sent HTTP response (%zu bytes)", response_len);
  
  /* Send END cell to close the connection after response */
  connection_edge_send_command(conn, RELAY_COMMAND_END,
                              (const char*)&(uint8_t){END_STREAM_REASON_DONE},
                              1);
  result = 0;
  
done:
  tor_free(method);
  tor_free(path);
  tor_free(response);
  return result;
}

/** Check if data contains a complete HTTP request */
int
dynhost_webserver_has_complete_request(const uint8_t *data, size_t len)
{
  // Look for end of HTTP headers (double CRLF)
  const char *end_marker = "\r\n\r\n";
  const char *headers_end = tor_memmem(data, len, end_marker, 4);
  if (!headers_end) {
    return 0;  // Headers not complete
  }
  
  // For POST requests, check if we have Content-Length and full body
  const char *content_length_str = tor_memmem(data, len, "Content-Length:", 15);
  if (content_length_str) {
    content_length_str += 15;
    while (*content_length_str == ' ') content_length_str++;
    int content_length = atoi(content_length_str);
    
    // Check if we have the full body
    size_t headers_size = (headers_end + 4) - (const char *)data;
    size_t body_size = len - headers_size;
    
    return (body_size >= (size_t)content_length) ? 1 : 0;
  }
  
  // For GET requests or requests without Content-Length, headers end is enough
  return 1;
}