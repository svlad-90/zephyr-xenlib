/*
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XENLIB_XENSTORE_CLIENT_H
#define XENLIB_XENSTORE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/kernel.h>

#include <xenstore_common.h>

/** Convenience constant for requests issued outside a XenStore transaction. */
#define XS_TRANSACTION_NONE 0U

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Watch notification callback.
 *
 * Invoked when a XenStore watch fires.
 *
 * @param path   Absolute XenStore path that triggered the watch. The pointer is
 *               only valid for the duration of the callback.
 * @param token  User-supplied token associated with the watch registration. The
 *               pointer is only valid for the duration of the callback.
 * @param param  Opaque user pointer provided at watcher initialization.
 */
typedef void (*xs_watch_cb)(const char *path, const char *token, void *param);

/**
 * @brief Opaque XenStore watcher descriptor allocated by the client library.
 */
struct xs_watcher;

/**
 * @brief Initialize the XenStore client.
 *
 * Safe to call multiple times and from multiple threads. Once initialization
 * succeeds, later calls return success without rebuilding the transport.
 *
 * The client is intended to live for the lifetime of the Zephyr domain. There
 * is no public shutdown/deinitialize API. Internal fatal-error cleanup may
 * drop the transport state, but applications must not rely on calling
 * xs_init() after such a failure as a supported recovery contract.
 *
 * @retval 0       Initialization succeeded or was already active.
 * @retval -errno  Initialization failed.
 */
int xs_init(void);

/**
 * @brief Set the default timeout for XenStore operations.
 *
 * Operations that do not take an explicit timeout parameter will use this value.
 * K_FOREVER is the default, causing the calling thread to block indefinitely.
 *
 * @param tout  Default timeout to use for XenStore operations.
 */
void xs_set_default_timeout(k_timeout_t tout);

/**
 * @brief Register a watcher callback.
 *
 * Registered watchers receive every watch event delivered to this client. The
 * callback should inspect the event path and token to filter events it cares
 * about. Use xs_watch_timeout() and xs_unwatch_timeout() separately to manage
 * remote XenStore watch subscriptions.
 *
 * @param cb    Callback function invoked for watch notifications.
 * @param param Opaque user data passed to the callback.
 *
 * @retval non-NULL Watcher descriptor.
 * @retval NULL     Failed to register the watcher.
 */
struct xs_watcher *xs_watcher_register(xs_watch_cb cb, void *param);

/**
 * @brief Unregister a watcher callback.
 *
 * After this function returns, no future callback will be started for @p w. If
 * a callback is already running on another thread, this function waits for it
 * to finish before releasing the watcher object. Calling this from the callback
 * itself is allowed; in that case the object is released when the callback
 * returns to the library.
 *
 * @param w Watcher descriptor returned by xs_watcher_register().
 *
 * @retval 0      Succeeded or the watcher was already unregistered.
 * @retval -errno Failed to unregister.
 */
int xs_watcher_unregister(struct xs_watcher *w);

/**
 * @brief Set permissions for a XenStore entry.
 *
 * @param path      The path of the entry whose permissions are updated.
 * @param perms     Permission entries to serialize into the XenStore request.
 * @param perms_num The number of @p perms entries; must be from 1 to 32.
 * @param tx_id     XenStore transaction identifier (0 when not in a transaction).
 * @param tout      Timeout to wait for the response.
 *
 * @retval 0        Succeeded in setting permissions.
 * @retval -EINVAL  Invalid argument, including @p perms_num equal to zero.
 * @retval -E2BIG   @p perms_num is greater than 32.
 * @retval -errno   Request failed.
 *
 * @see xs_set_permissions().
 */
int xs_set_permissions_timeout(const char *path, const struct xs_perm_entry *perms,
			       size_t perms_num, uint32_t tx_id, k_timeout_t tout);

/**
 * @brief Set permissions for a XenStore entry with the default timeout.
 *
 * @param path      The path of the entry whose permissions are updated.
 * @param perms     Permission entries to serialize into the XenStore request.
 * @param perms_num The number of @p perms entries; must be from 1 to 32.
 * @param tx_id     XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval 0        Succeeded in setting permissions.
 * @retval -EINVAL  Invalid argument, including @p perms_num equal to zero.
 * @retval -E2BIG   @p perms_num is greater than 32.
 * @retval -errno   Request failed.
 *
 * @see xs_set_permissions_timeout().
 */
int xs_set_permissions(const char *path, const struct xs_perm_entry *perms, size_t perms_num,
		       uint32_t tx_id);

/**
 * @brief Get permissions for a XenStore entry.
 *
 * @param path      The path of the entry whose permissions are queried.
 * @param perms     Permission-entry buffer to store the result.
 * @param perms_num Number of entries available in @p perms.
 * @param tx_id     XenStore transaction identifier (0 when not in a transaction).
 * @param tout      Timeout to wait for the response.
 *
 * @retval >=0      Number of permission entries copied into @p perms.
 * @retval -errno   Request failed.
 *
 * @see xs_get_permissions().
 */
ssize_t xs_get_permissions_timeout(const char *path, struct xs_perm_entry *perms,
				   size_t perms_num, uint32_t tx_id, k_timeout_t tout);

/**
 * @brief Get permissions for a XenStore entry with the default timeout.
 *
 * @param path      The path of the entry whose permissions are queried.
 * @param perms     Permission-entry buffer to store the result.
 * @param perms_num Number of entries available in @p perms.
 * @param tx_id     XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval >=0      Number of permission entries copied into @p perms.
 * @retval -errno   Request failed.
 *
 * @see xs_get_permissions_timeout().
 */
ssize_t xs_get_permissions(const char *path, struct xs_perm_entry *perms, size_t perms_num,
			   uint32_t tx_id);

/**
 * Response buffer contract for APIs that take @p buf and @p len:
 *
 * Successful requests copy at most @p len payload bytes into @p buf. If the
 * copied payload is shorter than @p len, the library appends a trailing NUL
 * byte. If the payload length exactly equals @p len, the request succeeds and
 * @p buf is not NUL-terminated. If the payload is longer than @p len,
 * the request fails. Passing @p buf as NULL or @p len as zero returns -EINVAL.
 */

/**
 * @brief Read a XenStore path.
 *
 * @param path     The path to read the value.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 * @param tout     Timeout to wait for the response; use K_FOREVER to block
 *                 indefinitely.
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 */
ssize_t xs_read_timeout(const char *path, char *buf, size_t len, uint32_t tx_id, k_timeout_t tout);

/**
 * @brief Remove a XenStore entry.
 *
 * @param path     The path of the entry to remove.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 * @param tout     Timeout to wait for the response.
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 *
 * @see xs_rm().
 */
ssize_t xs_rm_timeout(const char *path, char *buf, size_t len, uint32_t tx_id, k_timeout_t tout);

/**
 * @brief Write a value to XenStore entry.
 *
 * @param path       The path to write the value.
 * @param value      The value to write.
 * @param value_len  The bytes of @p value.
 * @param buf        A string buffer to store the result.
 * @param len        The length of @p buf in bytes.
 * @param tx_id      XenStore transaction identifier (0 when not in a transaction).
 * @param tout       Timeout to wait for the response.
 *
 * @retval >=0       Number of payload bytes copied into @p buf.
 * @retval -errno    Request failed.
 *
 * @see xs_write().
 */
ssize_t xs_write_timeout(const char *path, const char *value, size_t value_len, char *buf,
			 size_t len, uint32_t tx_id, k_timeout_t tout);

/**
 * @brief Enumerate a XenStore directory.
 *
 * @param path     The absolute directory path to enumerate.
 * @param buf      A buffer to store the result.
 *                 The directory-entries are returned as a byte sequence of
 *                 NUL-separated strings.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 * @param tout     Timeout to wait for the response.
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 */
ssize_t xs_directory_timeout(const char *path, char *buf, size_t len, uint32_t tx_id,
			     k_timeout_t tout);

/**
 * @brief Start watching XenStore value changes.
 *
 * @param path     The absolute path to watch.
 * @param token    A user token to identify watch request.
 * @param buf      A buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 * @param tout     Timeout to wait for the response.
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 */
ssize_t xs_watch_timeout(const char *path, const char *token, char *buf, size_t len, uint32_t tx_id,
			 k_timeout_t tout);

/**
 * @brief Stop watching XenStore value changes.
 *
 * @param path     Absolute path to unwatch.
 * @param token    A user token to identify watch request.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 * @param tout     Timeout to wait for the response.
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 *
 * @see xs_unwatch().
 */
ssize_t xs_unwatch_timeout(const char *path, const char *token, char *buf, size_t len,
			   uint32_t tx_id, k_timeout_t tout);

/**
 * @brief Read a XenStore path with the default timeout.
 *
 * @param path     The path to read the value.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 *
 * @see xs_read_timeout().
 */
ssize_t xs_read(const char *path, char *buf, size_t len, uint32_t tx_id);

/**
 * @brief Remove a XenStore entry with the default timeout.
 *
 * @param path     The path of the entry to remove.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 *
 * @see xs_rm_timeout().
 */
ssize_t xs_rm(const char *path, char *buf, size_t len, uint32_t tx_id);

/**
 * @brief Write a value to XenStore entry with the default timeout.
 *
 * @param path       The path to write the value.
 * @param value      The value to write.
 * @param value_len  The bytes of @p value.
 * @param buf        A string buffer to store the result.
 * @param len        The length of @p buf in bytes.
 * @param tx_id      XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval >=0       Number of payload bytes copied into @p buf.
 * @retval -errno    Request failed.
 *
 * @see xs_write_timeout().
 */
ssize_t xs_write(const char *path, const char *value, size_t value_len, char *buf, size_t len,
		 uint32_t tx_id);

/**
 * @brief Enumerate a XenStore directory with the default timeout.
 *
 * @param path     The directory path to enumerate.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 *
 * @see xs_directory_timeout().
 */
ssize_t xs_directory(const char *path, char *buf, size_t len, uint32_t tx_id);

/**
 * @brief Start watching XenStore value changes with the default timeout.
 *
 * @param path     Absolute path to watch.
 * @param token    A user token to identify watch request.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 *
 * @see xs_watch_timeout().
 */
ssize_t xs_watch(const char *path, const char *token, char *buf, size_t len, uint32_t tx_id);

/**
 * @brief Stop watching XenStore value changes with the default timeout.
 *
 * @param path     Absolute path to unwatch.
 * @param token    A user token to identify watch request.
 * @param buf      A string buffer to store the result.
 * @param len      The length of @p buf in bytes.
 * @param tx_id    XenStore transaction identifier (0 when not in a transaction).
 *
 * @retval >=0     Number of payload bytes copied into @p buf.
 * @retval -errno  Request failed.
 *
 * @see xs_unwatch_timeout().
 */
ssize_t xs_unwatch(const char *path, const char *token, char *buf, size_t len, uint32_t tx_id);

#if defined(CONFIG_XEN_STORE_CLIENT) && !defined(CONFIG_XEN_STORE_SRV)
static inline int xss_read(const char *path, char *value, size_t len)
{
	ssize_t ret = xs_read(path, value, len, XS_TRANSACTION_NONE);

	return (ret < 0) ? (int)ret : 0;
}

static inline int xss_write(const char *path, const char *value)
{
	char response[sizeof("ENOTEMPTY")];
	ssize_t ret;

	if (!value) {
		return -EINVAL;
	}

	ret = xs_write(path, value, strlen(value), response, sizeof(response), XS_TRANSACTION_NONE);
	return (ret < 0) ? (int)ret : 0;
}

static inline int xss_set_perm(const char *path, domid_t domid, enum xs_perm perm)
{
	const struct xs_perm_entry entry = {
		.domid = domid,
		.perm = perm,
	};

	return xs_set_permissions(path, &entry, 1, XS_TRANSACTION_NONE);
}

static inline int xss_rm(const char *path)
{
	char response[sizeof("ENOTEMPTY")];
	ssize_t ret = xs_rm(path, response, sizeof(response), XS_TRANSACTION_NONE);

	return (ret < 0) ? (int)ret : 0;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
