/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_sys.c
 * @brief Setup and tear down the dynamic onion host subsystem.
 **/

#include "lib/subsys/subsys.h"
#include "feature/dynhost/dynhost.h"
#include "feature/dynhost/dynhost_sys.h"
#include "lib/log/log.h"

/**
 * Initialize the dynamic onion host subsystem.
 */
static int
subsys_dynhost_initialize(void)
{
  log_notice(LD_GENERAL, "Initializing dynamic onion host subsystem at level 52");
  
  int result = dynhost_init_global_state();
  if (result < 0) {
    log_err(LD_BUG, "Failed to initialize dynhost global state (error: %d)", result);
    return -1;
  }
  
  return 0;
}

/**
 * Shutdown the dynamic onion host subsystem.
 */
static void
subsys_dynhost_shutdown(void)
{
  log_notice(LD_GENERAL, "Shutting down dynamic onion host subsystem");
  dynhost_cleanup_global_state();
}

/**
 * Configure dynhost from options.
 */
static int
subsys_dynhost_set_options(void *arg)
{
  const struct or_options_t *options = arg;
  (void)options;  /* Dynhost is always enabled for now */
  return dynhost_configure(options);
}

/**
 * Subsystem definition for dynamic onion host.
 */
const subsys_fns_t sys_dynhost = {
  SUBSYS_DECLARE_LOCATION(),
  .name = "dynhost",
  .supported = true,
  .level = DYNHOST_SUBSYS_LEVEL,
  .initialize = subsys_dynhost_initialize,
  .shutdown = subsys_dynhost_shutdown,
  .set_options = subsys_dynhost_set_options,
};