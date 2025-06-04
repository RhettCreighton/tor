/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost.c
 * @brief Dynamic onion host implementation
 **/

#include "core/or/or.h"
#include "feature/dynhost/dynhost.h"
#include "feature/dynhost/dynhost_message.h"
#include "feature/hs/hs_service.h"
#include "feature/hs/hs_common.h"
#include "feature/hs/hs_client.h"
#include "feature/control/control_hs.h"
#include "lib/crypt_ops/crypto_ed25519.h"
#include "lib/log/log.h"
#include "lib/malloc/malloc.h"
#include "lib/container/smartlist.h"
#include "lib/lock/compat_mutex.h"
#include "lib/encoding/binascii.h"

/** Global dynhost service state */
static dynhost_service_t *global_dynhost_service = NULL;

/**
 * Create a dynamic onion service without port binding.
 */
hs_service_t *
dynhost_create_service(void)
{
  hs_service_t *service = hs_service_new(NULL);
  if (!service) {
    log_err(LD_REND, "Failed to create dynhost service");
    return NULL;
  }
  
  // Generate service keys - create an ed25519 keypair first
  ed25519_keypair_t identity_keypair;
  if (ed25519_keypair_generate(&identity_keypair, 0) < 0) {
    log_err(LD_REND, "Failed to generate dynhost service keys");
    hs_service_free(service);
    return NULL;
  }
  
  // Copy to service keys structure
  ed25519_pubkey_copy(&service->keys.identity_pk, &identity_keypair.pubkey);
  memcpy(&service->keys.identity_sk, &identity_keypair.seckey, sizeof(ed25519_secret_key_t));
  service->keys.is_identify_key_offline = 0;
  
  // Mark as ephemeral so it doesn't need to be loaded from disk
  service->config.is_ephemeral = 1;
  service->config.version = HS_VERSION_THREE;
  
  // Add virtual port mapping - NO REAL PORT
  // Allocate port config with no unix socket path
  hs_port_config_t *port_cfg = tor_malloc_zero(sizeof(hs_port_config_t) + 1);
  port_cfg->virtual_port = 80;  // Virtual port 80
  port_cfg->is_unix_addr = 0;
  port_cfg->real_port = 0;  // No real port - will be handled internally
  tor_addr_make_unspec(&port_cfg->real_addr);
  
  smartlist_add(service->config.ports, port_cfg);
  
  log_notice(LD_REND, "Created dynhost service with virtual port 80");
  return service;
}

/**
 * Initialize global dynhost state.
 */
int
dynhost_init_global_state(void)
{
  if (global_dynhost_service) {
    log_warn(LD_BUG, "Dynhost already initialized");
    return -1;
  }
  
  global_dynhost_service = tor_malloc_zero(sizeof(dynhost_service_t));
  global_dynhost_service->virtual_ports = smartlist_new();
  tor_mutex_init(&global_dynhost_service->handler_mutex);
  global_dynhost_service->next_msg_id = 1;
  
  // Don't create the service immediately - just set up the infrastructure
  // The service will be created later when the system is fully ready
  log_notice(LD_REND, "Dynhost initialized, service creation deferred");
  global_dynhost_service->hs_service = NULL;
  
  // Add default virtual port 80
  dynhost_add_virtual_port(80, 0);
  
  // Initialize message subsystem
  dynhost_message_init();
  
  log_notice(LD_REND, "Dynhost subsystem ready");
  
  return 0;
}

/**
 * Cleanup global dynhost state.
 */
void
dynhost_cleanup_global_state(void)
{
  if (!global_dynhost_service) {
    return;
  }
  
  if (global_dynhost_service->virtual_ports) {
    SMARTLIST_FOREACH(global_dynhost_service->virtual_ports, dynhost_port_t *, port,
                      tor_free(port));
    smartlist_free(global_dynhost_service->virtual_ports);
  }
  
  tor_mutex_uninit(&global_dynhost_service->handler_mutex);
  
  // Note: hs_service is managed by the HS subsystem, don't free here
  
  tor_free(global_dynhost_service->onion_address);
  tor_free(global_dynhost_service);
  global_dynhost_service = NULL;
  
  log_notice(LD_REND, "Dynhost global state cleaned up");
}

/**
 * Configure dynhost from options.
 */
int
dynhost_configure(const struct or_options_t *options)
{
  (void)options; // Currently no options to configure
  
  if (!global_dynhost_service) {
    log_warn(LD_BUG, "Dynhost not initialized during configuration");
    return -1;
  }
  
  // Don't create the service here - wait until the system is fully ready
  log_info(LD_REND, "Dynhost configuration received, service creation deferred");
  
  return 0;
}

/**
 * Activate the dynamic onion host service.
 * This should be called after all subsystems are fully initialized.
 */
int
dynhost_activate_service(void)
{
  if (!global_dynhost_service) {
    log_warn(LD_BUG, "Dynhost not initialized");
    return -1;
  }
  
  if (global_dynhost_service->hs_service) {
    log_info(LD_REND, "Dynhost service already activated");
    return 0;
  }
  
  log_notice(LD_REND, "Activating dynhost ephemeral service");
  
  // Create ephemeral service using the HS subsystem API
  ed25519_secret_key_t *sk = tor_malloc_zero(sizeof(ed25519_secret_key_t));
  ed25519_keypair_t kp;
  if (ed25519_keypair_generate(&kp, 0) < 0) {
    log_err(LD_REND, "Failed to generate dynhost service keys");
    tor_free(sk);
    return -1;
  }
  memcpy(sk, &kp.seckey, sizeof(ed25519_secret_key_t));
  
  // Create ports list
  smartlist_t *ports = smartlist_new();
  hs_port_config_t *port_cfg = tor_malloc_zero(sizeof(hs_port_config_t) + 1);
  port_cfg->virtual_port = 80;
  port_cfg->is_unix_addr = 0;
  port_cfg->real_port = 0;  // No real port - will be handled internally
  tor_addr_make_unspec(&port_cfg->real_addr);
  smartlist_add(ports, port_cfg);
  
  char *address_out = NULL;
  hs_service_add_ephemeral_status_t status = hs_service_add_ephemeral(
      sk, ports, 
      0,    // max_streams_per_rdv_circuit (0 = unlimited)
      0,    // max_streams_close_circuit (0 = don't close)
      0,    // pow_defenses_enabled
      0,    // pow_queue_rate
      0,    // pow_queue_burst
      NULL, // auth_clients_v3
      &address_out);
  
  if (status != RSAE_OKAY) {
    log_err(LD_REND, "Failed to create dynhost ephemeral service: %d", status);
    return -1;
  }
  
  log_notice(LD_REND, "Dynamic onion host ephemeral service created with address: %s",
             address_out);
  
  // Find the service that was just created
  ed25519_public_key_t service_pk;
  if (address_out && hs_parse_address(address_out, &service_pk, NULL, NULL) == 0) {
    global_dynhost_service->hs_service = hs_service_find(&service_pk);
    if (global_dynhost_service->hs_service) {
      log_notice(LD_REND, "Successfully retrieved dynhost service reference");
    } else {
      log_warn(LD_REND, "Failed to find dynhost service after creation");
    }
  }
  
  // Store the address for later use
  if (address_out) {
    global_dynhost_service->onion_address = tor_strdup(address_out);
    tor_free(address_out);
  }
  
  return 0;
}

/**
 * Run periodic dynhost events.
 * This is called by the main loop periodically.
 */
void
dynhost_run_scheduled_events(time_t now)
{
  (void)now;
  
  /* Check and activate the service if needed */
  dynhost_check_and_activate();
}

/**
 * Add a virtual port to the dynhost service.
 */
int
dynhost_add_virtual_port(uint16_t virtual_port, uint32_t isolation_flags)
{
  if (!global_dynhost_service) {
    log_err(LD_BUG, "Dynhost not initialized");
    return -1;
  }
  
  dynhost_port_t *port = tor_malloc_zero(sizeof(dynhost_port_t));
  port->virtual_port = virtual_port;
  port->isolation_flags = isolation_flags;
  
  smartlist_add(global_dynhost_service->virtual_ports, port);
  
  log_info(LD_REND, "Added dynhost virtual port %d", virtual_port);
  return 0;
}

/**
 * Get the global dynhost service.
 */
dynhost_service_t *
dynhost_get_global_service(void)
{
  return global_dynhost_service;
}

/**
 * Check if dynhost service needs activation and activate if ready.
 * This should be called periodically after the system is fully running.
 */
void
dynhost_check_and_activate(void)
{
  static int activation_attempted = 0;
  static int check_count = 0;
  
  /* Log every 10 checks to avoid spam */
  if (++check_count % 10 == 1) {
    log_info(LD_REND, "Dynhost activation check #%d (attempted=%d, global=%p, service=%p)", 
             check_count, activation_attempted, 
             (void*)global_dynhost_service,
             global_dynhost_service ? (void*)global_dynhost_service->hs_service : NULL);
  }
  
  if (!global_dynhost_service || activation_attempted) {
    return;
  }
  
  if (!global_dynhost_service->hs_service) {
    log_notice(LD_REND, "Dynhost service not yet activated, attempting activation");
    if (dynhost_activate_service() == 0) {
      activation_attempted = 1;
      log_notice(LD_REND, "Dynhost service successfully activated");
    } else {
      log_warn(LD_REND, "Failed to activate dynhost service, will retry");
    }
  } else {
    activation_attempted = 1;
  }
}