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

#include <xenstore_common.h>

/**
 * @brief Watch-event callback called by the XenStore client.
 *
 * A watch event is an asynchronous server notification that a watched
 * XenStore path changed. The callback runs from the client's RX thread, so it
 * should return quickly and must protect any application-owned shared state
 * it touches.
 *
 * @param path      Changed XenStore path from the event.
 * @param token     Watch token supplied when the watch was registered.
 * @param user_data Opaque pointer registered with
 *                  xs_client_set_watch_callback().
 */
typedef void (*xs_client_watch_cb_t)(const char *path, const char *token,
				     void *user_data);

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
 *
 * @retval true  xs_client_connect() completed successfully.
 * @retval false The client is not connected yet.
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

/**
 * @brief List child names under a XenStore path (XS_DIRECTORY).
 *
 * The server returns a sequence of NUL-terminated child names. The returned
 * byte count is the raw payload length, not a string count.
 *
 * @param path    Absolute XenStore path.
 * @param out     Destination buffer for the raw child-name payload.
 * @param out_len Size of @p out.
 * @retval >=0    Raw payload byte count.
 * @retval -EINVAL Invalid argument.
 * @retval -ENOSPC Destination buffer is too small.
 * @retval <0     Other negative errno, including the server's error reply.
 */
int xs_client_directory(const char *path, char *out, size_t out_len);

/**
 * @brief Create a XenStore node or directory (XS_MKDIR).
 *
 * @param path Absolute XenStore path to create.
 * @retval 0   Success.
 * @retval <0  Negative errno, including the server's error reply.
 */
int xs_client_mkdir(const char *path);

/**
 * @brief Remove a XenStore node or subtree (XS_RM).
 *
 * @param path Absolute XenStore path to remove.
 * @retval 0   Success.
 * @retval <0  Negative errno, including the server's error reply.
 */
int xs_client_rm(const char *path);

/**
 * @brief Read XenStore permissions for a path (XS_GET_PERMS).
 *
 * Pass @p perms as NULL to query only the number of permission entries. In
 * that mode @p num_perms is still required and receives the entry count.
 *
 * @param path      Path to inspect.
 * @param perms     Destination array, or NULL for count-only mode.
 * @param num_perms In: array capacity. Out: returned permission count.
 * @retval 0        Success.
 * @retval -ENOSPC  Destination array is too small; @p num_perms contains the
 *                  required entry count.
 * @retval <0       Other negative errno, including the server's error reply.
 */
int xs_client_get_perms(const char *path, struct xenstore_perm *perms,
			size_t *num_perms);

/**
 * @brief Set XenStore permissions for a path (XS_SET_PERMS).
 *
 * @param path      Path whose permissions should be replaced.
 * @param perms     Permission entries to send to the server.
 * @param num_perms Number of entries in @p perms.
 * @retval 0        Success.
 * @retval -EINVAL  Invalid argument.
 * @retval -E2BIG   Encoded request does not fit into one XenStore payload.
 * @retval <0       Other negative errno, including the server's error reply.
 */
int xs_client_set_perms(const char *path, const struct xenstore_perm *perms,
			size_t num_perms);

/**
 * @brief Register a callback for asynchronous watch events.
 *
 * The client keeps one process-wide watch callback. Calling this function
 * again replaces the previous callback and user_data. The callback runs from
 * the XenStore client RX thread. The path and token pointers are valid only
 * until the callback returns.
 *
 * When this is called from an application thread, it waits for any currently
 * running callback to finish before replacing or clearing the callback pair.
 * When called from the callback itself, it updates the pair immediately to
 * avoid waiting on the same RX thread that is executing the callback.
 *
 * @param cb        Callback to install, or NULL to clear the callback.
 * @param user_data Opaque pointer passed back to @p cb.
 */
void xs_client_set_watch_callback(xs_client_watch_cb_t cb, void *user_data);

/**
 * @brief Add a XenStore watch (XS_WATCH).
 *
 * @param path  Path to watch.
 * @param token Caller-chosen token returned with matching watch events.
 * @retval 0    Success.
 * @retval <0   Negative errno, including the server's error reply.
 */
int xs_client_watch(const char *path, const char *token);

/**
 * @brief Remove a XenStore watch (XS_UNWATCH).
 *
 * @param path  Path used when the watch was registered.
 * @param token Token used when the watch was registered.
 * @retval 0    Success.
 * @retval <0   Negative errno, including the server's error reply.
 */
int xs_client_unwatch(const char *path, const char *token);

/**
 * @brief Remove all watches registered by this XenStore connection.
 *
 * @retval 0  Success.
 * @retval <0 Negative errno, including the server's error reply.
 */
int xs_client_reset_watches(void);

/**
 * @brief Return `/local/domain/<domid>` for a domain (XS_GET_DOMAIN_PATH).
 *
 * @param domid   Domain id to query.
 * @param out     Destination buffer for the returned path string.
 * @param out_len Size of @p out.
 * @retval >=0    Reply string size including the trailing NUL.
 * @retval -EINVAL Invalid argument or malformed server reply.
 * @retval -ENOSPC Destination buffer is too small.
 * @retval <0     Other negative errno, including the server's error reply.
 */
int xs_client_get_domain_path(domid_t domid, char *out, size_t out_len);

#endif /* XENLIB_XENSTORE_CLIENT_H */
