/* SPDX-FileCopyrightText: 2025 Rhett Creighton
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dynhost_sys.h
 * @brief Header for feature/dynhost/dynhost_sys.c
 **/

#ifndef TOR_FEATURE_DYNHOST_DYNHOST_SYS_H
#define TOR_FEATURE_DYNHOST_DYNHOST_SYS_H

extern const struct subsys_fns_t sys_dynhost;

/**
 * Subsystem level for the dynamic onion host system.
 *
 * Defined here so that it can be shared between the real and stub
 * definitions.
 **/
#define DYNHOST_SUBSYS_LEVEL (52)

#endif /* !defined(TOR_FEATURE_DYNHOST_DYNHOST_SYS_H) */