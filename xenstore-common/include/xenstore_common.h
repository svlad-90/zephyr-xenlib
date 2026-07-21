/*
 * Copyright (c) 2023 EPAM Systems
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XENLIB_XENSTORE_COMMON_H
#define XENLIB_XENSTORE_COMMON_H

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <xen/public/io/xs_wire.h>
#include <xen/public/xen.h>

#include <zephyr/sys/barrier.h>
#include <zephyr/sys/slist.h>
#include <zephyr/xen/events.h>

/**
 * @brief XenStore entry access permissions.
 */
enum xs_perm {
	/** XenStore entry owner permissions. */
	XS_PERM_NONE = 0x0,
	/** XenStore entry read permissions for a guest domain. */
	XS_PERM_READ = 0x1,
	/** XenStore entry write permissions for a guest domain. */
	XS_PERM_WRITE = 0x2,
	/** XenStore entry read/write permissions for a guest domain. */
	XS_PERM_BOTH = XS_PERM_WRITE | XS_PERM_READ
};

/**
 * @brief XenStore wire permission entry.
 */
struct xenstore_perm {
	enum xs_perm perms;
	domid_t domid;
};

/**
 * @brief Check whether a given path is absolute.
 *
 * @param path Path string to validate.
 * @retval true  Path is non-NULL and begins with '/'.
 * @retval false Path is NULL or relative.
 */
__maybe_unused static inline bool xenstore_is_abs_path(const char *path)
{
	if (!path) {
		return false;
	}

	return path[0] == '/';
}

/**
 * @brief Check whether a path refers to the root ("/").
 *
 * @param path Path string to validate.
 * @retval true  Path equals "/".
 * @retval false Path is NULL, relative, or longer than "/".
 */
__maybe_unused static inline bool xenstore_is_root_path(const char *path)
{
	return (xenstore_is_abs_path(path) && (strlen(path) == 1));
}

/**
 * @brief Return the number of bytes required to store a string.
 *
 * @param str String whose storage requirement is queried.
 * @retval > 0 Length including the trailing NUL when @p str is non-NULL.
 * @retval 0   When @p str is NULL.
 */
__maybe_unused static inline size_t xenstore_str_byte_size(const char *str)
{
	if (!str) {
		return 0;
	}

	return strlen(str) + 1;
}

/**
 * @brief Check whether the producer/consumer indexes overflow the ring size.
 *
 * @param cons Consumer ring position.
 * @param prod Producer ring position.
 * @retval false Indexes are within bounds.
 * @retval true  Indexes points invalid position.
 */
__maybe_unused static inline bool xenstore_check_indexes(XENSTORE_RING_IDX cons,
							 XENSTORE_RING_IDX prod)
{
	return ((prod - cons) > XENSTORE_RING_SIZE);
}

/**
 * @brief Calculate the next readable chunk.
 *
 * @param cons Consumer ring position.
 * @param prod Producer ring position.
 * @param len  Output parameter returning the readable length of chunk.
 *
 * @return Offset chunk starts.
 */
__maybe_unused static inline size_t xenstore_get_input_offset(XENSTORE_RING_IDX cons,
							      XENSTORE_RING_IDX prod, size_t *len)
{
	size_t delta = prod - cons;
	*len = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(cons);

	if (delta < *len) {
		*len = delta;
	}

	return MASK_XENSTORE_IDX(cons);
}

/**
 * @brief Calculate the next writable cnunk.
 *
 * @param cons Consumer ring position.
 * @param prod Producer ring position.
 * @param len  Output parameter returning the writable length of chunk.
 *
 * @return Offset chunk starts.
 */
__maybe_unused static inline size_t get_output_offset(XENSTORE_RING_IDX cons,
						      XENSTORE_RING_IDX prod, size_t *len)
{
	size_t free_space = XENSTORE_RING_SIZE - (prod - cons);

	*len = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(prod);
	if (free_space < *len) {
		*len = free_space;
	}

	return MASK_XENSTORE_IDX(prod);
}

/**
 * @brief Write data into the XenStore ring buffer.
 *
 * @param intf   Domain interface describing the shared ring.
 * @param data   Buffer containing the data to write.
 * @param len    Number of bytes to write.
 * @param client Set to true when invoked from a XenStore client context.
 *
 * @retval >=0   Number of bytes written.
 * @retval <0    Negative errno value on failure.
 */
int xenstore_ring_write(struct xenstore_domain_interface *intf, const void *data, size_t len,
			bool client);

/**
 * @brief Read data from the XenStore ring buffer.
 *
 * @param intf   Domain interface describing the shared ring.
 * @param data   Destination buffer that receives the data, or NULL to discard
 *               bytes while advancing the consumer index.
 * @param len    Maximum number of bytes to read.
 * @param client Set to true when invoked from a XenStore client context.
 *
 * @retval >=0   Number of bytes read.
 * @retval <0    Negative errno value on failure.
 */
int xenstore_ring_read(struct xenstore_domain_interface *intf, void *data, size_t len, bool client);

/**
 * @brief Pack NUL-terminated strings into a XenStore request payload.
 *
 * @param buf         Destination payload buffer.
 * @param buf_len     Size of @p buf.
 * @param strings     Array of strings to pack.
 * @param num_strings Number of array entries.
 * @param payload_len Returned payload length.
 *
 * @retval 0        Success.
 * @retval -EINVAL  Invalid argument or NULL string entry.
 * @retval -E2BIG   Destination buffer is too small.
 */
int xenstore_pack_strings(char *buf, size_t buf_len, const char *const *strings,
			  size_t num_strings, size_t *payload_len);

/**
 * @brief Convert a XenStore permission value to its wire character.
 *
 * @param perm Permission value.
 *
 * @return XenStore wire permission character.
 */
char xenstore_perm_to_char(enum xs_perm perm);

/**
 * @brief Convert a XenStore wire permission character to a permission value.
 *
 * @param perm_char Wire permission character.
 * @param perm      Returned permission value.
 *
 * @retval 0       Success.
 * @retval -EINVAL Unknown permission character or NULL output pointer.
 */
int xenstore_perm_from_char(char perm_char, enum xs_perm *perm);

/**
 * @brief Convert a textual errno representation into its numeric value.
 *
 * @param errstr String representation of errno.
 * @param len    Length of @p errstr in bytes.
 *
 * @retval >0 Corresponding errno value when recognized.
 * @retval 0  When the string does not map to a known errno.
 */
int xenstore_get_error(const char *errstr, size_t len);

#endif /* XENLIB_XENSTORE_COMMON_H */
