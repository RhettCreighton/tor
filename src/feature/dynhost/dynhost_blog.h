/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_blog.h
 * @brief Blog application for dynhost MVC framework
 *
 * This is a demonstration application showing how to use the dynhost MVC
 * framework to build a RESTful blog with posts and comments. It showcases:
 * - Model creation with validations
 * - Controller actions for CRUD operations
 * - RESTful routing
 * - Form handling
 * - Relationships between models
 *
 * The blog data is stored in memory and persists while Tor is running.
 * To test: http://[onion_address]/blog
 **/

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_BLOG_H
#define TOR_FEATURE_DYNHOST_DYNHOST_BLOG_H

#include "feature/dynhost/dynhost_mvc.h"

/* Initialize the blog application */
int dynhost_blog_init(void);

/* Cleanup the blog application */
void dynhost_blog_cleanup(void);

/* Get the blog app instance */
mvc_app_t *dynhost_blog_get_app(void);

#endif /* !defined(TOR_FEATURE_DYNHOST_DYNHOST_BLOG_H) */