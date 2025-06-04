/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_mvc.c
 * @brief MVC framework implementation for dynamic onion host applications
 **/

#include "core/or/or.h"
#include "feature/dynhost/dynhost_mvc.h"
#include "lib/log/log.h"
#include "lib/malloc/malloc.h"
#include "lib/string/util_string.h"
#include "lib/string/printf.h"
#include "lib/container/smartlist.h"
#include "lib/container/map.h"
#include "core/or/edge_connection_st.h"

#include <string.h>
#include <time.h>

/** Router structure */
struct mvc_router_t {
  smartlist_t *routes;
};

/** Route definition */
typedef struct mvc_route_t {
  char *method;
  char *pattern;
  mvc_controller_t *controller;
  char *action;
} mvc_route_t;

/* mvc_app_t is now defined in the header */

/** Global MVC app instance */
static mvc_app_t *global_mvc_app = NULL;

/** Global ID counter for model instances */
static int global_id_counter = 1;

/* Forward declarations for model methods */
static void *model_create(mvc_model_t *model, strmap_t *attributes);
static void *model_find(mvc_model_t *model, int id);
static smartlist_t *model_find_all(mvc_model_t *model);
static smartlist_t *model_where(mvc_model_t *model, const char *field, const void *value);
static int model_save(mvc_model_t *model, void *instance);
static int model_destroy(mvc_model_t *model, void *instance);
static int model_validate(mvc_model_t *model, void *instance, smartlist_t **errors);

/** Create a new model */
mvc_model_t *
mvc_model_new(const char *name)
{
  mvc_model_t *model = tor_malloc_zero(sizeof(mvc_model_t));
  model->name = tor_strdup(name);
  model->fields = smartlist_new();
  model->relationships = smartlist_new();
  model->instances = smartlist_new();
  model->callbacks = strmap_new();
  
  /* Set default model methods */
  model->create = model_create;
  model->find = model_find;
  model->find_all = model_find_all;
  model->where = model_where;
  model->save = model_save;
  model->destroy = model_destroy;
  model->validate = model_validate;
  
  return model;
}

/** Free a model */
void
mvc_model_free(mvc_model_t *model)
{
  if (!model)
    return;
    
  tor_free(model->name);
  
  /* Free fields */
  SMARTLIST_FOREACH_BEGIN(model->fields, mvc_field_t *, field) {
    tor_free(field->name);
    if (field->validations) {
      SMARTLIST_FOREACH(field->validations, mvc_validation_t *, val, {
        tor_free(val->message);
        if (val->type == MVC_VAL_PATTERN)
          tor_free(val->params.pattern);
        tor_free(val);
      });
      smartlist_free(field->validations);
    }
    tor_free(field);
  } SMARTLIST_FOREACH_END(field);
  smartlist_free(model->fields);
  
  /* Free relationships */
  SMARTLIST_FOREACH_BEGIN(model->relationships, mvc_relationship_t *, rel) {
    tor_free(rel->name);
    tor_free(rel->target_model);
    tor_free(rel->foreign_key);
    tor_free(rel->inverse_of);
    tor_free(rel);
  } SMARTLIST_FOREACH_END(rel);
  smartlist_free(model->relationships);
  
  /* Free instances */
  SMARTLIST_FOREACH_BEGIN(model->instances, mvc_instance_t *, inst) {
    strmap_free(inst->attributes, tor_free_);
    tor_free(inst);
  } SMARTLIST_FOREACH_END(inst);
  smartlist_free(model->instances);
  
  strmap_free(model->callbacks, NULL);
  tor_free(model);
}

/** Add a field to a model */
void
mvc_model_add_field(mvc_model_t *model, const char *name,
                   mvc_field_type_t type, const void *default_val)
{
  mvc_field_t *field = tor_malloc_zero(sizeof(mvc_field_t));
  field->name = tor_strdup(name);
  field->type = type;
  field->default_value = (void *)default_val;
  field->validations = smartlist_new();
  smartlist_add(model->fields, field);
}

/** Add a validation to a field */
void
mvc_model_add_validation(mvc_model_t *model, const char *field_name,
                        mvc_validation_t *validation)
{
  SMARTLIST_FOREACH_BEGIN(model->fields, mvc_field_t *, field) {
    if (strcmp(field->name, field_name) == 0) {
      smartlist_add(field->validations, validation);
      return;
    }
  } SMARTLIST_FOREACH_END(field);
}

/** Add a relationship to a model */
void
mvc_model_add_relationship(mvc_model_t *model, const char *name,
                          mvc_relationship_type_t type,
                          const char *target_model)
{
  mvc_relationship_t *rel = tor_malloc_zero(sizeof(mvc_relationship_t));
  rel->name = tor_strdup(name);
  rel->type = type;
  rel->target_model = tor_strdup(target_model);
  smartlist_add(model->relationships, rel);
}

/** Create a new model instance */
static void *
model_create(mvc_model_t *model, strmap_t *attributes)
{
  mvc_instance_t *inst = tor_malloc_zero(sizeof(mvc_instance_t));
  inst->id = global_id_counter++;
  inst->attributes = strmap_new();
  inst->created_at = time(NULL);
  inst->updated_at = inst->created_at;
  
  /* Copy attributes */
  if (attributes) {
    strmap_iter_t *iter;
    const char *key;
    void *val;
    for (iter = strmap_iter_init(attributes); !strmap_iter_done(iter);
         iter = strmap_iter_next(attributes, iter)) {
      strmap_iter_get(iter, &key, &val);
      strmap_set(inst->attributes, key, tor_strdup((char *)val));
    }
  }
  
  /* Add ID to attributes */
  char id_str[32];
  tor_snprintf(id_str, sizeof(id_str), "%d", inst->id);
  strmap_set(inst->attributes, "id", tor_strdup(id_str));
  
  /* Add to instances list */
  smartlist_add(model->instances, inst);
  
  return inst;
}

/** Find a model instance by ID */
static void *
model_find(mvc_model_t *model, int id)
{
  SMARTLIST_FOREACH_BEGIN(model->instances, mvc_instance_t *, inst) {
    if (inst->id == id)
      return inst;
  } SMARTLIST_FOREACH_END(inst);
  return NULL;
}

/** Find all model instances */
static smartlist_t *
model_find_all(mvc_model_t *model)
{
  /* Create a shallow copy of the instances list */
  smartlist_t *result = smartlist_new();
  SMARTLIST_FOREACH(model->instances, mvc_instance_t *, inst, {
    smartlist_add(result, inst);
  });
  return result;
}

/** Find instances matching a field value */
static smartlist_t *
model_where(mvc_model_t *model, const char *field, const void *value)
{
  smartlist_t *results = smartlist_new();
  
  SMARTLIST_FOREACH_BEGIN(model->instances, mvc_instance_t *, inst) {
    const char *attr_val = strmap_get(inst->attributes, field);
    if (attr_val && strcmp(attr_val, (const char *)value) == 0) {
      smartlist_add(results, inst);
    }
  } SMARTLIST_FOREACH_END(inst);
  
  return results;
}

/** Save a model instance */
static int
model_save(mvc_model_t *model, void *instance)
{
  mvc_instance_t *inst = (mvc_instance_t *)instance;
  
  /* Validate before saving */
  smartlist_t *errors = NULL;
  if (!model_validate(model, instance, &errors)) {
    if (errors) {
      SMARTLIST_FOREACH(errors, char *, err, {
        log_warn(LD_GENERAL, "Validation error: %s", err);
        tor_free(err);
      });
      smartlist_free(errors);
    }
    return -1;
  }
  
  inst->updated_at = time(NULL);
  return 0;
}

/** Destroy a model instance */
static int
model_destroy(mvc_model_t *model, void *instance)
{
  mvc_instance_t *inst = (mvc_instance_t *)instance;
  smartlist_remove(model->instances, inst);
  strmap_free(inst->attributes, tor_free_);
  tor_free(inst);
  return 0;
}

/** Validate a model instance */
static int
model_validate(mvc_model_t *model, void *instance, smartlist_t **errors)
{
  mvc_instance_t *inst = (mvc_instance_t *)instance;
  int valid = 1;
  
  if (errors)
    *errors = smartlist_new();
  
  SMARTLIST_FOREACH_BEGIN(model->fields, mvc_field_t *, field) {
    const char *value = strmap_get(inst->attributes, field->name);
    
    SMARTLIST_FOREACH_BEGIN(field->validations, mvc_validation_t *, val) {
      int field_valid = 1;
      char *error_msg = NULL;
      
      switch (val->type) {
        case MVC_VAL_REQUIRED:
          if (!value || strlen(value) == 0) {
            field_valid = 0;
            error_msg = tor_strdup(val->message ? val->message : 
                                 "Field is required");
          }
          break;
          
        case MVC_VAL_LENGTH:
          if (value) {
            size_t len = strlen(value);
            if (len < (size_t)val->params.length.min || 
                len > (size_t)val->params.length.max) {
              field_valid = 0;
              tor_asprintf(&error_msg, val->message ? val->message :
                          "Length must be between %d and %d",
                          val->params.length.min, val->params.length.max);
            }
          }
          break;
          
        default:
          break;
      }
      
      if (!field_valid) {
        valid = 0;
        if (errors && error_msg) {
          char *full_msg;
          tor_asprintf(&full_msg, "%s: %s", field->name, error_msg);
          smartlist_add(*errors, full_msg);
          tor_free(error_msg);
        }
      }
    } SMARTLIST_FOREACH_END(val);
  } SMARTLIST_FOREACH_END(field);
  
  return valid;
}

/** Create a new controller */
mvc_controller_t *
mvc_controller_new(const char *name, mvc_model_t *model)
{
  mvc_controller_t *ctrl = tor_malloc_zero(sizeof(mvc_controller_t));
  ctrl->name = tor_strdup(name);
  ctrl->actions = strmap_new();
  ctrl->model = model;
  return ctrl;
}

/** Free a controller */
void
mvc_controller_free(mvc_controller_t *controller)
{
  if (!controller)
    return;
    
  tor_free(controller->name);
  strmap_free(controller->actions, NULL);
  tor_free(controller);
}

/** Add an action to a controller */
void
mvc_controller_add_action(mvc_controller_t *controller, const char *name,
                         void (*handler)(mvc_controller_t *ctrl,
                                       mvc_request_t *req,
                                       mvc_response_t *resp))
{
  strmap_set(controller->actions, name, (void *)handler);
}

/** Simple template renderer */
static char *
view_render(mvc_view_t *view, strmap_t *data)
{
  /* For now, just return the template. In a full implementation,
   * this would do template variable substitution */
  (void)data;
  return tor_strdup(view->template);
}

/** Create a new view */
mvc_view_t *
mvc_view_new(const char *name, const char *template)
{
  mvc_view_t *view = tor_malloc_zero(sizeof(mvc_view_t));
  view->name = tor_strdup(name);
  view->template = tor_strdup(template);
  view->helpers = strmap_new();
  view->render = view_render;
  return view;
}

/** Free a view */
void
mvc_view_free(mvc_view_t *view)
{
  if (!view)
    return;
    
  tor_free(view->name);
  tor_free(view->template);
  strmap_free(view->helpers, NULL);
  tor_free(view);
}

/** Parse URL-encoded form data */
static strmap_t *
parse_form_data(const char *data)
{
  strmap_t *params = strmap_new();
  char *data_copy = tor_strdup(data);
  char *saveptr = NULL;
  char *pair = strtok_r(data_copy, "&", &saveptr);
  
  while (pair) {
    char *eq = strchr(pair, '=');
    if (eq) {
      *eq = '\0';
      strmap_set(params, pair, tor_strdup(eq + 1));
    }
    pair = strtok_r(NULL, "&", &saveptr);
  }
  
  tor_free(data_copy);
  return params;
}

/** Create request from HTTP data */
mvc_request_t *
mvc_request_from_http(const char *http_data, size_t len,
                     struct edge_connection_t *conn)
{
  mvc_request_t *req = tor_malloc_zero(sizeof(mvc_request_t));
  req->params = strmap_new();
  req->headers = strmap_new();
  req->conn = conn;
  
  /* Parse HTTP request line */
  const char *line_end = strstr(http_data, "\r\n");
  if (!line_end) {
    mvc_request_free(req);
    return NULL;
  }
  
  size_t line_len = line_end - http_data;
  char *request_line = tor_malloc(line_len + 1);
  memcpy(request_line, http_data, line_len);
  request_line[line_len] = '\0';
  
  /* Parse method, path, version */
  char *saveptr = NULL;
  char *method = strtok_r(request_line, " ", &saveptr);
  char *path = strtok_r(NULL, " ", &saveptr);
  
  if (method && path) {
    req->method = tor_strdup(method);
    
    /* Parse path and query string */
    char *query = strchr(path, '?');
    if (query) {
      *query = '\0';
      query++;
      strmap_t *query_params = parse_form_data(query);
      strmap_iter_t *iter;
      const char *key;
      void *val;
      for (iter = strmap_iter_init(query_params); !strmap_iter_done(iter);
           iter = strmap_iter_next(query_params, iter)) {
        strmap_iter_get(iter, &key, &val);
        strmap_set(req->params, key, tor_strdup((char *)val));
      }
      strmap_free(query_params, tor_free_);
    }
    req->path = tor_strdup(path);
  }
  
  tor_free(request_line);
  
  /* Parse headers and body */
  const char *headers_end = strstr(http_data, "\r\n\r\n");
  if (headers_end) {
    const char *body_start = headers_end + 4;
    size_t body_len = len - (body_start - http_data);
    if (body_len > 0) {
      req->body = tor_malloc(body_len + 1);
      memcpy(req->body, body_start, body_len);
      req->body[body_len] = '\0';
      
      /* Parse POST data if content-type is form-encoded */
      if (strcmp(req->method, "POST") == 0) {
        strmap_t *post_params = parse_form_data(req->body);
        strmap_iter_t *iter;
        const char *key;
        void *val;
        for (iter = strmap_iter_init(post_params); !strmap_iter_done(iter);
             iter = strmap_iter_next(post_params, iter)) {
          strmap_iter_get(iter, &key, &val);
          strmap_set(req->params, key, tor_strdup((char *)val));
        }
        strmap_free(post_params, tor_free_);
      }
    }
  }
  
  return req;
}

/** Free a request */
void
mvc_request_free(mvc_request_t *request)
{
  if (!request)
    return;
    
  tor_free(request->method);
  tor_free(request->path);
  tor_free(request->body);
  strmap_free(request->params, tor_free_);
  strmap_free(request->headers, tor_free_);
  tor_free(request);
}

/** Create a new response */
mvc_response_t *
mvc_response_new(int status)
{
  mvc_response_t *resp = tor_malloc_zero(sizeof(mvc_response_t));
  resp->status_code = status;
  resp->headers = strmap_new();
  strmap_set(resp->headers, "Content-Type", tor_strdup("text/html; charset=UTF-8"));
  strmap_set(resp->headers, "Connection", tor_strdup("close"));
  return resp;
}

/** Free a response */
void
mvc_response_free(mvc_response_t *response)
{
  if (!response)
    return;
    
  tor_free(response->body);
  strmap_free(response->headers, tor_free_);
  tor_free(response);
}

/** Set response body */
void
mvc_response_set_body(mvc_response_t *response, const char *body)
{
  tor_free(response->body);
  response->body = tor_strdup(body);
  response->body_len = strlen(body);
}

/** Convert response to HTTP */
char *
mvc_response_to_http(mvc_response_t *response)
{
  smartlist_t *parts = smartlist_new();
  
  /* Status line */
  char *status_line;
  tor_asprintf(&status_line, "HTTP/1.1 %d %s\r\n", 
              response->status_code,
              response->status_code == 200 ? "OK" :
              response->status_code == 404 ? "Not Found" :
              response->status_code == 500 ? "Internal Server Error" : "Unknown");
  smartlist_add(parts, status_line);
  
  /* Headers */
  strmap_iter_t *iter;
  const char *key;
  void *val;
  for (iter = strmap_iter_init(response->headers); !strmap_iter_done(iter);
       iter = strmap_iter_next(response->headers, iter)) {
    strmap_iter_get(iter, &key, &val);
    char *header;
    tor_asprintf(&header, "%s: %s\r\n", key, (char *)val);
    smartlist_add(parts, header);
  }
  
  /* Content-Length */
  char *content_length;
  tor_asprintf(&content_length, "Content-Length: %zu\r\n", response->body_len);
  smartlist_add(parts, content_length);
  
  /* End of headers */
  smartlist_add(parts, tor_strdup("\r\n"));
  
  /* Body */
  if (response->body)
    smartlist_add(parts, tor_strdup(response->body));
  
  /* Join all parts */
  char *http = smartlist_join_strings(parts, "", 0, NULL);
  
  SMARTLIST_FOREACH(parts, char *, part, tor_free(part));
  smartlist_free(parts);
  
  return http;
}

/** Create a new router */
mvc_router_t *
mvc_router_new(void)
{
  mvc_router_t *router = tor_malloc_zero(sizeof(mvc_router_t));
  router->routes = smartlist_new();
  return router;
}

/** Free a router */
void
mvc_router_free(mvc_router_t *router)
{
  if (!router)
    return;
    
  SMARTLIST_FOREACH_BEGIN(router->routes, mvc_route_t *, route) {
    tor_free(route->method);
    tor_free(route->pattern);
    tor_free(route->action);
    tor_free(route);
  } SMARTLIST_FOREACH_END(route);
  
  smartlist_free(router->routes);
  tor_free(router);
}

/** Add a route */
void
mvc_router_add_route(mvc_router_t *router, const char *method,
                    const char *pattern, mvc_controller_t *controller,
                    const char *action)
{
  mvc_route_t *route = tor_malloc_zero(sizeof(mvc_route_t));
  route->method = tor_strdup(method);
  route->pattern = tor_strdup(pattern);
  route->controller = controller;
  route->action = tor_strdup(action);
  smartlist_add(router->routes, route);
}

/** Dispatch a request */
int
mvc_router_dispatch(mvc_router_t *router, mvc_request_t *request,
                   mvc_response_t **response)
{
  SMARTLIST_FOREACH_BEGIN(router->routes, mvc_route_t *, route) {
    if (strcmp(route->method, request->method) == 0 &&
        strcmp(route->pattern, request->path) == 0) {
      /* Found matching route */
      void (*handler)(mvc_controller_t *, mvc_request_t *, mvc_response_t *);
      handler = strmap_get(route->controller->actions, route->action);
      
      if (handler) {
        *response = mvc_response_new(200);
        
        /* Call before_action if defined */
        if (route->controller->before_action)
          route->controller->before_action(route->controller, request);
        
        /* Call the action handler */
        handler(route->controller, request, *response);
        
        /* Call after_action if defined */
        if (route->controller->after_action)
          route->controller->after_action(route->controller, request, *response);
        
        return 0;
      }
    }
  } SMARTLIST_FOREACH_END(route);
  
  /* No matching route found */
  *response = mvc_response_new(404);
  mvc_response_set_body(*response, "<h1>404 Not Found</h1>");
  return -1;
}

/** Create a new application */
mvc_app_t *
mvc_app_new(const char *name)
{
  mvc_app_t *app = tor_malloc_zero(sizeof(mvc_app_t));
  app->name = tor_strdup(name);
  app->models = strmap_new();
  app->controllers = strmap_new();
  app->views = strmap_new();
  app->router = mvc_router_new();
  return app;
}

/** Free an application */
void
mvc_app_free(mvc_app_t *app)
{
  if (!app)
    return;
    
  tor_free(app->name);
  
  /* Note: We don't free the actual models/controllers/views here
   * as they might be used elsewhere. Just free the maps. */
  strmap_free(app->models, NULL);
  strmap_free(app->controllers, NULL);
  strmap_free(app->views, NULL);
  
  mvc_router_free(app->router);
  tor_free(app);
}

/** Register a model */
void
mvc_app_register_model(mvc_app_t *app, mvc_model_t *model)
{
  strmap_set(app->models, model->name, model);
}

/** Register a controller */
void
mvc_app_register_controller(mvc_app_t *app, mvc_controller_t *controller)
{
  strmap_set(app->controllers, controller->name, controller);
}

/** Register a view */
void
mvc_app_register_view(mvc_app_t *app, mvc_view_t *view)
{
  strmap_set(app->views, view->name, view);
}

/** Get the router */
mvc_router_t *
mvc_app_get_router(mvc_app_t *app)
{
  return app->router;
}

/** Get global app instance */
mvc_app_t *
mvc_app_get_global(void)
{
  return global_mvc_app;
}

/** Set global app instance */
void
mvc_app_set_global(mvc_app_t *app)
{
  global_mvc_app = app;
}