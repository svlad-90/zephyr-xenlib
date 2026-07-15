/*
 * Copyright (c) 2025 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * XenStore client (frontend) for a Zephyr DomU.
 *
 * A dom0less guest is handed a xenstore ring (HVM_PARAM_STORE_PFN) and an
 * event channel (HVM_PARAM_STORE_EVTCHN) by Xen, with the ring's connection
 * state initialised to XENSTORE_RECONNECT. Unlike the server side (which lives
 * in xenstore-srv and only ever sets RECONNECT for dom0less domains), the guest
 * has to complete the reconnection handshake itself and then it may exchange
 * requests with the server. This module implements that client half.
 */

#ifndef XENLIB_XENSTORE_CLIENT_H
#define XENLIB_XENSTORE_CLIENT_H

#include <errno.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Connect the XenStore client.
 *
 * Reads the guest's HVM_PARAM_STORE_PFN / HVM_PARAM_STORE_EVTCHN, maps the
 * ring, performs the RECONNECT -> CONNECTED handshake and binds the store
 * event channel. Idempotent: a second call after a successful connect is a
 * no-op returning 0.
 *
 * @retval 0        Connected (or already connected).
 * @retval -ENODEV  No store ring/evtchn provisioned for this domain
 *                  (e.g. this is the xenstore domain, not a client).
 * @retval <0       Other negative errno on failure.
 */
int xs_client_connect(void);

/**
 * @brief Whether the client has completed the connection handshake.
 */
bool xs_client_is_connected(void);

/**
 * @brief Write a value to a XenStore path (XS_WRITE).
 *
 * @param path  Absolute XenStore path.
 * @param value NUL-terminated value to store.
 * @retval 0   Success.
 * @retval <0  Negative errno (including the server's error reply).
 */
int xs_client_write(const char *path, const char *value);

/**
 * @brief Read a value from a XenStore path (XS_READ).
 *
 * @param path    Absolute XenStore path.
 * @param out     Destination buffer (NUL-terminated on success).
 * @param out_len Size of @p out.
 * @retval >=0 Number of payload bytes returned by the server.
 * @retval <0  Negative errno (including the server's error reply).
 */
int xs_client_read(const char *path, char *out, size_t out_len);

#endif /* XENLIB_XENSTORE_CLIENT_H */
