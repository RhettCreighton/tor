/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_mvc.h
 * @brief MVC framework for dynamic onion host applications
 *
 * This file provides a Rails-like MVC framework for building web applications
 * that run entirely within the Tor process. It includes:
 * - Models with fields, validations, and relationships
 * - Controllers with action-based request handling
 * - Views with template rendering
 * - Router for URL pattern matching
 * - In-memory data storage
 *
 * Example usage:
 * @code
 * // Create a model
 * mvc_model_t *post = mvc_model_new("Post");
 * mvc_model_add_field(post, "title", MVC_FIELD_STRING, NULL);
 * mvc_model_add_field(post, "content", MVC_FIELD_TEXT, NULL);
 * 
 * // Add validation
 * mvc_validation_t *val = tor_malloc_zero(sizeof(mvc_validation_t));
 * val->type = MVC_VAL_REQUIRED;
 * val->message = tor_strdup("Title is required");
 * mvc_model_add_validation(post, "title", val);
 * 
 * // Create controller
 * mvc_controller_t *ctrl = mvc_controller_new("PostsController", post);
 * mvc_controller_add_action(ctrl, "index", posts_index_action);
 * 
 * // Set up routing
 * mvc_router_t *router = mvc_router_new();
 * mvc_router_add_route(router, "GET", "/posts", ctrl, "index");
 * @endcode
 **/

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_MVC_H
#define TOR_FEATURE_DYNHOST_DYNHOST_MVC_H

#include "lib/container/smartlist.h"
#include "lib/container/map.h"

#include <time.h>

/* Forward declarations */
typedef struct mvc_model_t mvc_model_t;
typedef struct mvc_controller_t mvc_controller_t;
typedef struct mvc_view_t mvc_view_t;
typedef struct mvc_field_t mvc_field_t;
typedef struct mvc_validation_t mvc_validation_t;
typedef struct mvc_relationship_t mvc_relationship_t;
typedef struct mvc_request_t mvc_request_t;
typedef struct mvc_response_t mvc_response_t;
typedef struct mvc_instance_t mvc_instance_t;

/** Instance storage for models */
struct mvc_instance_t {
  int id;
  strmap_t *attributes;
  time_t created_at;
  time_t updated_at;
};

/* Field types for models */
typedef enum {
  MVC_FIELD_STRING,
  MVC_FIELD_INTEGER,
  MVC_FIELD_DATETIME,
  MVC_FIELD_TEXT,
  MVC_FIELD_BOOLEAN
} mvc_field_type_t;

/* Relationship types */
typedef enum {
  MVC_REL_HAS_MANY,
  MVC_REL_BELONGS_TO,
  MVC_REL_HAS_ONE
} mvc_relationship_type_t;

/* Validation types */
typedef enum {
  MVC_VAL_REQUIRED,
  MVC_VAL_LENGTH,
  MVC_VAL_PATTERN,
  MVC_VAL_RANGE,
  MVC_VAL_CUSTOM
} mvc_validation_type_t;

/** Field definition for models */
struct mvc_field_t {
  char *name;
  mvc_field_type_t type;
  void *default_value;
  smartlist_t *validations; /* List of mvc_validation_t */
};

/** Validation rule */
struct mvc_validation_t {
  mvc_validation_type_t type;
  char *message;
  union {
    struct { int min; int max; } length;
    struct { int min; int max; } range;
    char *pattern; /* Regex pattern */
    int (*custom_validator)(const void *value, char **error);
  } params;
};

/** Relationship definition */
struct mvc_relationship_t {
  char *name;
  mvc_relationship_type_t type;
  char *target_model;
  char *foreign_key;
  char *inverse_of;
};

/** Model base structure */
struct mvc_model_t {
  char *name;
  smartlist_t *fields;        /* List of mvc_field_t */
  smartlist_t *relationships; /* List of mvc_relationship_t */
  smartlist_t *instances;     /* In-memory storage of model instances */
  strmap_t *callbacks;        /* Lifecycle callbacks */
  
  /* Model methods */
  void *(*create)(mvc_model_t *model, strmap_t *attributes);
  void *(*find)(mvc_model_t *model, int id);
  smartlist_t *(*find_all)(mvc_model_t *model);
  smartlist_t *(*where)(mvc_model_t *model, const char *field, const void *value);
  int (*save)(mvc_model_t *model, void *instance);
  int (*destroy)(mvc_model_t *model, void *instance);
  int (*validate)(mvc_model_t *model, void *instance, smartlist_t **errors);
};

/** Controller base structure */
struct mvc_controller_t {
  char *name;
  strmap_t *actions; /* Map of action names to handler functions */
  mvc_model_t *model; /* Associated model */
  
  /* Controller lifecycle hooks */
  void (*before_action)(mvc_controller_t *ctrl, mvc_request_t *req);
  void (*after_action)(mvc_controller_t *ctrl, mvc_request_t *req, mvc_response_t *resp);
};

/** View structure */
struct mvc_view_t {
  char *name;
  char *template;
  strmap_t *helpers; /* View helper functions */
  
  /* Rendering method */
  char *(*render)(mvc_view_t *view, strmap_t *data);
};

/** HTTP Request wrapper */
struct mvc_request_t {
  char *method;
  char *path;
  strmap_t *params;
  strmap_t *headers;
  char *body;
  struct edge_connection_t *conn;
};

/** HTTP Response wrapper */
struct mvc_response_t {
  int status_code;
  strmap_t *headers;
  char *body;
  size_t body_len;
};

/* MVC Framework API */

/* Model functions */
mvc_model_t *mvc_model_new(const char *name);
void mvc_model_free(mvc_model_t *model);
void mvc_model_add_field(mvc_model_t *model, const char *name, 
                        mvc_field_type_t type, const void *default_val);
void mvc_model_add_validation(mvc_model_t *model, const char *field,
                             mvc_validation_t *validation);
void mvc_model_add_relationship(mvc_model_t *model, const char *name,
                               mvc_relationship_type_t type,
                               const char *target_model);

/* Controller functions */
mvc_controller_t *mvc_controller_new(const char *name, mvc_model_t *model);
void mvc_controller_free(mvc_controller_t *controller);
void mvc_controller_add_action(mvc_controller_t *controller, const char *name,
                              void (*handler)(mvc_controller_t *ctrl, 
                                            mvc_request_t *req,
                                            mvc_response_t *resp));

/* View functions */
mvc_view_t *mvc_view_new(const char *name, const char *template);
void mvc_view_free(mvc_view_t *view);
void mvc_view_add_helper(mvc_view_t *view, const char *name,
                        char *(*helper)(strmap_t *data));

/* Request/Response functions */
mvc_request_t *mvc_request_from_http(const char *http_data, size_t len,
                                    struct edge_connection_t *conn);
void mvc_request_free(mvc_request_t *request);
mvc_response_t *mvc_response_new(int status);
void mvc_response_free(mvc_response_t *response);
void mvc_response_set_body(mvc_response_t *response, const char *body);
char *mvc_response_to_http(mvc_response_t *response);

/* Router functions */
typedef struct mvc_router_t mvc_router_t;
mvc_router_t *mvc_router_new(void);
void mvc_router_free(mvc_router_t *router);
void mvc_router_add_route(mvc_router_t *router, const char *method,
                         const char *pattern, mvc_controller_t *controller,
                         const char *action);
int mvc_router_dispatch(mvc_router_t *router, mvc_request_t *request,
                       mvc_response_t **response);

/* Application container */
typedef struct mvc_app_t {
  char *name;
  strmap_t *models;
  strmap_t *controllers;
  strmap_t *views;
  mvc_router_t *router;
} mvc_app_t;

mvc_app_t *mvc_app_new(const char *name);
void mvc_app_free(mvc_app_t *app);
void mvc_app_register_model(mvc_app_t *app, mvc_model_t *model);
void mvc_app_register_controller(mvc_app_t *app, mvc_controller_t *controller);
void mvc_app_register_view(mvc_app_t *app, mvc_view_t *view);
mvc_router_t *mvc_app_get_router(mvc_app_t *app);

/* Global MVC app instance */
mvc_app_t *mvc_app_get_global(void);
void mvc_app_set_global(mvc_app_t *app);

#endif /* !defined(TOR_FEATURE_DYNHOST_DYNHOST_MVC_H) */