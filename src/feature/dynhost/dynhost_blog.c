/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_blog.c
 * @brief Blog application implementation using dynhost MVC framework
 **/

#include "core/or/or.h"
#include "feature/dynhost/dynhost_blog.h"
#include "feature/dynhost/dynhost_mvc.h"
#include "lib/log/log.h"
#include "lib/malloc/malloc.h"
#include "lib/string/printf.h"
#include "lib/string/util_string.h"
#include "lib/container/smartlist.h"

#include <time.h>

/** HTML templates */
static const char BLOG_LAYOUT[] =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "  <title>%s - Tor Dynhost Blog</title>\n"
  "  <meta charset=\"UTF-8\">\n"
  "  <style>\n"
  "    body { font-family: Georgia, serif; max-width: 800px; "
  "margin: 40px auto; padding: 20px; background: #f9f9f9; "
  "color: #333; line-height: 1.6; }\n"
  "    header { background: #2c3e50; color: white; padding: 30px; "
  "margin: -20px -20px 30px; text-align: center; }\n"
  "    h1 { margin: 0; font-size: 2.5em; }\n"
  "    .tagline { margin-top: 10px; font-style: italic; opacity: 0.8; }\n"
  "    nav { background: #34495e; margin: -30px -20px 30px; padding: 15px 20px; }\n"
  "    nav a { color: white; margin-right: 20px; text-decoration: none; }\n"
  "    nav a:hover { text-decoration: underline; }\n"
  "    .post { background: white; padding: 30px; margin-bottom: 30px; "
  "border-radius: 5px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }\n"
  "    .post h2 { color: #2c3e50; margin-top: 0; }\n"
  "    .post-meta { color: #7f8c8d; font-size: 0.9em; margin-bottom: 15px; }\n"
  "    .post-content { margin: 20px 0; }\n"
  "    .comments { margin-top: 30px; padding-top: 20px; "
  "border-top: 2px solid #ecf0f1; }\n"
  "    .comment { background: #ecf0f1; padding: 15px; margin-bottom: 15px; "
  "border-radius: 3px; }\n"
  "    .comment-author { font-weight: bold; color: #2c3e50; }\n"
  "    .comment-time { color: #7f8c8d; font-size: 0.85em; }\n"
  "    form { background: white; padding: 25px; border-radius: 5px; "
  "box-shadow: 0 2px 5px rgba(0,0,0,0.1); }\n"
  "    input[type='text'], textarea { width: 100%%; padding: 10px; "
  "margin-bottom: 15px; border: 1px solid #ddd; border-radius: 3px; "
  "font-family: inherit; }\n"
  "    textarea { min-height: 150px; resize: vertical; }\n"
  "    button { background: #3498db; color: white; padding: 12px 30px; "
  "border: none; border-radius: 3px; cursor: pointer; font-size: 16px; }\n"
  "    button:hover { background: #2980b9; }\n"
  "    .error { background: #e74c3c; color: white; padding: 15px; "
  "border-radius: 3px; margin-bottom: 20px; }\n"
  "    .success { background: #27ae60; color: white; padding: 15px; "
  "border-radius: 3px; margin-bottom: 20px; }\n"
  "    footer { text-align: center; color: #7f8c8d; margin-top: 50px; "
  "padding-top: 30px; border-top: 1px solid #ddd; }\n"
  "  </style>\n"
  "</head>\n"
  "<body>\n"
  "  <header>\n"
  "    <h1>Tor Dynhost Blog</h1>\n"
  "    <div class=\"tagline\">A RESTful blog running inside Tor itself</div>\n"
  "  </header>\n"
  "  <nav>\n"
  "    <a href=\"/blog\">All Posts</a>\n"
  "    <a href=\"/blog/new\">New Post</a>\n"
  "    <a href=\"/\">Back to Main Menu</a>\n"
  "  </nav>\n"
  "  %s\n"
  "  <footer>\n"
  "    Powered by Tor Dynhost MVC Framework<br>\n"
  "    Running entirely within the Tor process\n"
  "  </footer>\n"
  "</body>\n"
  "</html>\n";

/** Blog controller actions */

static void blog_index_action(mvc_controller_t *ctrl, mvc_request_t *req,
                             mvc_response_t *resp);
static void blog_show_action(mvc_controller_t *ctrl, mvc_request_t *req,
                            mvc_response_t *resp);
static void blog_new_action(mvc_controller_t *ctrl, mvc_request_t *req,
                           mvc_response_t *resp);
static void blog_create_action(mvc_controller_t *ctrl, mvc_request_t *req,
                              mvc_response_t *resp);
static void comment_create_action(mvc_controller_t *ctrl, mvc_request_t *req,
                                 mvc_response_t *resp);

/** Helper functions */

static char *
format_time(time_t timestamp)
{
  struct tm *tm = localtime(&timestamp);
  char *buf = tor_malloc(64);
  strftime(buf, 64, "%B %d, %Y at %I:%M %p", tm);
  return buf;
}

static char *
render_layout(const char *title, const char *content)
{
  char *html;
  tor_asprintf(&html, BLOG_LAYOUT, title, content);
  return html;
}

/** Index action - list all posts */
static void
blog_index_action(mvc_controller_t *ctrl, mvc_request_t *req,
                 mvc_response_t *resp)
{
  (void)req; /* Unused in index action */
  mvc_model_t *post_model = ctrl->model;
  smartlist_t *posts = post_model->find_all(post_model);
  smartlist_t *content_parts = smartlist_new();
  
  smartlist_add(content_parts, tor_strdup("<h2>Recent Posts</h2>\n"));
  
  if (smartlist_len(posts) == 0) {
    smartlist_add(content_parts, tor_strdup(
      "<p style=\"text-align: center; color: #7f8c8d; margin: 40px 0;\">"
      "No posts yet. <a href=\"/blog/new\">Create the first post!</a></p>\n"));
  } else {
    /* Show posts in reverse order (newest first) */
    smartlist_reverse(posts);
    
    SMARTLIST_FOREACH_BEGIN(posts, mvc_instance_t *, post) {
      const char *title = strmap_get(post->attributes, "title");
      const char *author = strmap_get(post->attributes, "author");
      const char *content = strmap_get(post->attributes, "content");
      const char *id = strmap_get(post->attributes, "id");
      char *time_str = format_time(post->created_at);
      
      char *post_html;
      tor_asprintf(&post_html,
        "<div class=\"post\">\n"
        "  <h2><a href=\"/blog/post/%s\">%s</a></h2>\n"
        "  <div class=\"post-meta\">by %s on %s</div>\n"
        "  <div class=\"post-content\">%s</div>\n"
        "  <a href=\"/blog/post/%s\">Read more and comment →</a>\n"
        "</div>\n",
        id, title, author, time_str, content, id);
      
      smartlist_add(content_parts, post_html);
      tor_free(time_str);
    } SMARTLIST_FOREACH_END(post);
  }
  
  smartlist_free(posts);
  
  char *content = smartlist_join_strings(content_parts, "", 0, NULL);
  SMARTLIST_FOREACH(content_parts, char *, part, tor_free(part));
  smartlist_free(content_parts);
  
  char *html = render_layout("All Posts", content);
  tor_free(content);
  
  mvc_response_set_body(resp, html);
  tor_free(html);
}

/** Show action - display a single post with comments */
static void
blog_show_action(mvc_controller_t *ctrl, mvc_request_t *req,
                mvc_response_t *resp)
{
  const char *post_id_str = strmap_get(req->params, "id");
  if (!post_id_str) {
    resp->status_code = 404;
    mvc_response_set_body(resp, render_layout("Not Found", 
      "<h2>Post not found</h2>"));
    return;
  }
  
  int post_id = atoi(post_id_str);
  mvc_instance_t *post = ctrl->model->find(ctrl->model, post_id);
  
  if (!post) {
    resp->status_code = 404;
    mvc_response_set_body(resp, render_layout("Not Found", 
      "<h2>Post not found</h2>"));
    return;
  }
  
  const char *title = strmap_get(post->attributes, "title");
  const char *author = strmap_get(post->attributes, "author");
  const char *content = strmap_get(post->attributes, "content");
  char *time_str = format_time(post->created_at);
  
  smartlist_t *content_parts = smartlist_new();
  
  /* Post content */
  char *post_html;
  tor_asprintf(&post_html,
    "<div class=\"post\">\n"
    "  <h2>%s</h2>\n"
    "  <div class=\"post-meta\">by %s on %s</div>\n"
    "  <div class=\"post-content\">%s</div>\n"
    "</div>\n",
    title, author, time_str, content);
  smartlist_add(content_parts, post_html);
  tor_free(time_str);
  
  /* Comments section */
  smartlist_add(content_parts, tor_strdup("<div class=\"comments\">\n"));
  smartlist_add(content_parts, tor_strdup("<h3>Comments</h3>\n"));
  
  /* Find comments for this post */
  mvc_app_t *app = mvc_app_get_global();
  mvc_model_t *comment_model = strmap_get(app->models, "Comment");
  
  if (comment_model) {
    smartlist_t *comments = comment_model->where(comment_model, "post_id", post_id_str);
    
    if (smartlist_len(comments) == 0) {
      smartlist_add(content_parts, tor_strdup(
        "<p style=\"color: #7f8c8d;\">No comments yet. Be the first!</p>\n"));
    } else {
      SMARTLIST_FOREACH_BEGIN(comments, mvc_instance_t *, comment) {
        const char *com_author = strmap_get(comment->attributes, "author");
        const char *com_content = strmap_get(comment->attributes, "content");
        char *com_time = format_time(comment->created_at);
        
        char *comment_html;
        tor_asprintf(&comment_html,
          "<div class=\"comment\">\n"
          "  <div class=\"comment-author\">%s</div>\n"
          "  <div class=\"comment-time\">%s</div>\n"
          "  <p>%s</p>\n"
          "</div>\n",
          com_author, com_time, com_content);
        
        smartlist_add(content_parts, comment_html);
        tor_free(com_time);
      } SMARTLIST_FOREACH_END(comment);
    }
    
    smartlist_free(comments);
  }
  
  /* Comment form */
  char *form_html;
  tor_asprintf(&form_html,
    "<h3>Add a Comment</h3>\n"
    "<form method=\"POST\" action=\"/blog/post/%s/comment\">\n"
    "  <input type=\"text\" name=\"author\" placeholder=\"Your name\" required>\n"
    "  <textarea name=\"content\" placeholder=\"Your comment\" required></textarea>\n"
    "  <button type=\"submit\">Post Comment</button>\n"
    "</form>\n",
    post_id_str);
  smartlist_add(content_parts, form_html);
  
  smartlist_add(content_parts, tor_strdup("</div>\n")); /* Close comments div */
  
  char *page_content = smartlist_join_strings(content_parts, "", 0, NULL);
  SMARTLIST_FOREACH(content_parts, char *, part, tor_free(part));
  smartlist_free(content_parts);
  
  char *html = render_layout(title, page_content);
  tor_free(page_content);
  
  mvc_response_set_body(resp, html);
  tor_free(html);
}

/** New action - show form for creating a post */
static void
blog_new_action(mvc_controller_t *ctrl, mvc_request_t *req,
               mvc_response_t *resp)
{
  (void)ctrl;
  (void)req;
  
  const char *form_html =
    "<h2>Create New Post</h2>\n"
    "<form method=\"POST\" action=\"/blog/create\">\n"
    "  <input type=\"text\" name=\"title\" placeholder=\"Post title\" required>\n"
    "  <input type=\"text\" name=\"author\" placeholder=\"Your name\" required>\n"
    "  <textarea name=\"content\" placeholder=\"Write your post here...\" required></textarea>\n"
    "  <button type=\"submit\">Publish Post</button>\n"
    "</form>\n";
  
  char *html = render_layout("New Post", form_html);
  mvc_response_set_body(resp, html);
  tor_free(html);
}

/** Create action - create a new post */
static void
blog_create_action(mvc_controller_t *ctrl, mvc_request_t *req,
                  mvc_response_t *resp)
{
  const char *title = strmap_get(req->params, "title");
  const char *author = strmap_get(req->params, "author");
  const char *content = strmap_get(req->params, "content");
  
  if (!title || !author || !content) {
    resp->status_code = 400;
    mvc_response_set_body(resp, render_layout("Error",
      "<div class=\"error\">All fields are required!</div>"));
    return;
  }
  
  /* Create post attributes */
  strmap_t *attrs = strmap_new();
  strmap_set(attrs, "title", tor_strdup(title));
  strmap_set(attrs, "author", tor_strdup(author));
  strmap_set(attrs, "content", tor_strdup(content));
  
  /* Create the post */
  mvc_instance_t *post = ctrl->model->create(ctrl->model, attrs);
  strmap_free(attrs, tor_free_);
  
  if (!post) {
    resp->status_code = 500;
    mvc_response_set_body(resp, render_layout("Error",
      "<div class=\"error\">Failed to create post!</div>"));
    return;
  }
  
  /* Redirect to the new post */
  const char *post_id = strmap_get(post->attributes, "id");
  strmap_set(resp->headers, "Location", tor_strdup("/blog"));
  resp->status_code = 303; /* See Other */
  
  char *success_msg;
  tor_asprintf(&success_msg,
    "<div class=\"success\">Post created successfully!</div>\n"
    "<p>View your post <a href=\"/blog/post/%s\">here</a> or "
    "<a href=\"/blog\">return to all posts</a>.</p>",
    post_id);
  
  char *html = render_layout("Post Created", success_msg);
  mvc_response_set_body(resp, html);
  tor_free(html);
  tor_free(success_msg);
}

/** Create comment action */
static void
comment_create_action(mvc_controller_t *ctrl, mvc_request_t *req,
                     mvc_response_t *resp)
{
  const char *post_id = strmap_get(req->params, "post_id");
  const char *author = strmap_get(req->params, "author");
  const char *content = strmap_get(req->params, "content");
  
  if (!post_id || !author || !content) {
    resp->status_code = 400;
    mvc_response_set_body(resp, render_layout("Error",
      "<div class=\"error\">All fields are required!</div>"));
    return;
  }
  
  /* Create comment attributes */
  strmap_t *attrs = strmap_new();
  strmap_set(attrs, "post_id", tor_strdup(post_id));
  strmap_set(attrs, "author", tor_strdup(author));
  strmap_set(attrs, "content", tor_strdup(content));
  
  /* Create the comment */
  mvc_instance_t *comment = ctrl->model->create(ctrl->model, attrs);
  strmap_free(attrs, tor_free_);
  
  if (!comment) {
    resp->status_code = 500;
    mvc_response_set_body(resp, render_layout("Error",
      "<div class=\"error\">Failed to create comment!</div>"));
    return;
  }
  
  /* Redirect back to the post */
  char *location;
  tor_asprintf(&location, "/blog/post/%s", post_id);
  strmap_set(resp->headers, "Location", location);
  /* Don't free location - strmap_set takes ownership */
  resp->status_code = 303; /* See Other */
  
  mvc_response_set_body(resp, render_layout("Comment Added",
    "<div class=\"success\">Comment added successfully!</div>"));
}

/** Initialize the blog application */
int
dynhost_blog_init(void)
{
  /* Create the blog app */
  mvc_app_t *app = mvc_app_new("blog");
  
  /* Create Post model */
  mvc_model_t *post_model = mvc_model_new("Post");
  mvc_model_add_field(post_model, "title", MVC_FIELD_STRING, NULL);
  mvc_model_add_field(post_model, "author", MVC_FIELD_STRING, NULL);
  mvc_model_add_field(post_model, "content", MVC_FIELD_TEXT, NULL);
  
  /* Add validations */
  mvc_validation_t *title_required = tor_malloc_zero(sizeof(mvc_validation_t));
  title_required->type = MVC_VAL_REQUIRED;
  title_required->message = tor_strdup("Title is required");
  mvc_model_add_validation(post_model, "title", title_required);
  
  mvc_validation_t *title_length = tor_malloc_zero(sizeof(mvc_validation_t));
  title_length->type = MVC_VAL_LENGTH;
  title_length->message = tor_strdup("Title must be between 3 and 100 characters");
  title_length->params.length.min = 3;
  title_length->params.length.max = 100;
  mvc_model_add_validation(post_model, "title", title_length);
  
  /* Create Comment model */
  mvc_model_t *comment_model = mvc_model_new("Comment");
  mvc_model_add_field(comment_model, "post_id", MVC_FIELD_INTEGER, NULL);
  mvc_model_add_field(comment_model, "author", MVC_FIELD_STRING, NULL);
  mvc_model_add_field(comment_model, "content", MVC_FIELD_TEXT, NULL);
  
  /* Add relationships */
  mvc_model_add_relationship(post_model, "comments", MVC_REL_HAS_MANY, "Comment");
  mvc_model_add_relationship(comment_model, "post", MVC_REL_BELONGS_TO, "Post");
  
  /* Register models */
  mvc_app_register_model(app, post_model);
  mvc_app_register_model(app, comment_model);
  
  /* Create Posts controller */
  mvc_controller_t *posts_ctrl = mvc_controller_new("PostsController", post_model);
  mvc_controller_add_action(posts_ctrl, "index", blog_index_action);
  mvc_controller_add_action(posts_ctrl, "show", blog_show_action);
  mvc_controller_add_action(posts_ctrl, "new", blog_new_action);
  mvc_controller_add_action(posts_ctrl, "create", blog_create_action);
  
  /* Create Comments controller */
  mvc_controller_t *comments_ctrl = mvc_controller_new("CommentsController", comment_model);
  mvc_controller_add_action(comments_ctrl, "create", comment_create_action);
  
  /* Register controllers */
  mvc_app_register_controller(app, posts_ctrl);
  mvc_app_register_controller(app, comments_ctrl);
  
  /* Set up routes */
  mvc_router_t *router = mvc_app_get_router(app);
  mvc_router_add_route(router, "GET", "/blog", posts_ctrl, "index");
  mvc_router_add_route(router, "GET", "/blog/new", posts_ctrl, "new");
  mvc_router_add_route(router, "POST", "/blog/create", posts_ctrl, "create");
  
  /* Note: We'll need to handle dynamic routes like /blog/post/:id differently */
  /* For now, we'll handle them in the webserver integration */
  
  /* Set as global app */
  mvc_app_set_global(app);
  
  log_notice(LD_GENERAL, "Blog MVC application initialized");
  return 0;
}

/** Cleanup the blog application */
void
dynhost_blog_cleanup(void)
{
  mvc_app_t *app = mvc_app_get_global();
  if (!app)
    return;
  
  /* Clean up models */
  mvc_model_t *post_model = strmap_get(app->models, "Post");
  if (post_model) {
    mvc_model_free(post_model);
  }
  
  mvc_model_t *comment_model = strmap_get(app->models, "Comment");
  if (comment_model) {
    mvc_model_free(comment_model);
  }
  
  /* Clean up controllers */
  mvc_controller_t *posts_ctrl = strmap_get(app->controllers, "PostsController");
  if (posts_ctrl) {
    mvc_controller_free(posts_ctrl);
  }
  
  mvc_controller_t *comments_ctrl = strmap_get(app->controllers, "CommentsController");
  if (comments_ctrl) {
    mvc_controller_free(comments_ctrl);
  }
  
  /* Clean up app */
  mvc_app_free(app);
  mvc_app_set_global(NULL);
}

/** Get the blog app instance */
mvc_app_t *
dynhost_blog_get_app(void)
{
  return mvc_app_get_global();
}