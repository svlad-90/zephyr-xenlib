/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <xenstore_cli.h>
#include <xen/public/io/protocols.h>
#include <xen/public/io/xenbus.h>

#include "xen_blkfront_priv.h"

static int copy_path(char *dst, size_t dst_len, const char *src)
{
	size_t len;

	if ((dst == NULL) || (src == NULL)) {
		return -EINVAL;
	}

	len = strlen(src);
	if (len >= dst_len) {
		return -ENAMETOOLONG;
	}

	memcpy(dst, src, len + 1);
	return 0;
}

static int make_path(char *dst, size_t dst_len, const char *base, const char *node)
{
	int ret;

	ret = snprintk(dst, dst_len, "%s/%s", base, node);
	if ((ret < 0) || ((size_t)ret >= dst_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int parse_u64(const char *value, uint64_t *result)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(value, &end, 10);
	if ((errno != 0) || (end == value) || (*end != '\0')) {
		return -EINVAL;
	}

	*result = (uint64_t)parsed;
	return 0;
}

static int read_string(const char *path, char *buf, size_t len, k_timeout_t timeout)
{
	ssize_t ret;

	ret = xs_read_timeout(path, buf, len, XS_TRANSACTION_NONE, timeout);
	if (ret < 0) {
		return (int)ret;
	}

	if ((size_t)ret >= len) {
		return -EMSGSIZE;
	}

	buf[ret] = '\0';
	return 0;
}

static int write_string(const char *path, const char *value, char *buf, size_t len,
			k_timeout_t timeout)
{
	ssize_t ret;

	ret = xs_write_timeout(path, value, strlen(value), buf, len, XS_TRANSACTION_NONE,
			       timeout);
	return (ret < 0) ? (int)ret : 0;
}

static int write_uint(const char *path, uint32_t value, char *buf, size_t len,
		      k_timeout_t timeout)
{
	char value_buf[16];

	snprintk(value_buf, sizeof(value_buf), "%u", value);
	return write_string(path, value_buf, buf, len, timeout);
}

static int write_front_node(struct xen_blkfront *front, const char *node, const char *value,
			    char *buf, size_t len, k_timeout_t timeout)
{
	char path[XEN_BLKFRONT_PATH_MAX];
	int ret;

	ret = make_path(path, sizeof(path), front->frontend_path, node);
	if (ret != 0) {
		return ret;
	}

	return write_string(path, value, buf, len, timeout);
}

static int write_front_uint(struct xen_blkfront *front, const char *node, uint32_t value,
			    char *buf, size_t len, k_timeout_t timeout)
{
	char path[XEN_BLKFRONT_PATH_MAX];
	int ret;

	ret = make_path(path, sizeof(path), front->frontend_path, node);
	if (ret != 0) {
		return ret;
	}

	return write_uint(path, value, buf, len, timeout);
}

static int wait_front_node(struct xen_blkfront *front, const char *node, char *buf,
			   size_t len, uint16_t attempts, k_timeout_t retry_delay)
{
	char path[XEN_BLKFRONT_PATH_MAX];
	int ret;

	ret = make_path(path, sizeof(path), front->frontend_path, node);
	if (ret != 0) {
		return ret;
	}

	for (uint16_t attempt = 0; attempt < attempts; attempt++) {
		ret = read_string(path, buf, len, K_MSEC(500));
		if (ret == 0) {
			return 0;
		}

		if ((attempt + 1U) < attempts) {
			k_sleep(retry_delay);
		}
	}

	return -ETIMEDOUT;
}

int xen_blkfront_xenbus_configure(struct xen_blkfront *front,
				  const struct xen_blkfront_config *cfg)
{
	int ret;

	ret = copy_path(front->frontend_path, sizeof(front->frontend_path),
			cfg->frontend_path);
	if (ret != 0) {
		return ret;
	}

	ret = copy_path(front->backend_path, sizeof(front->backend_path), cfg->backend_path);
	if (ret != 0) {
		return ret;
	}

	front->vdev = cfg->vdev;
	front->backend_domid = cfg->backend_domid;
	return 0;
}

int xen_blkfront_xenbus_wait_backend_path(struct xen_blkfront *front, char *buf, size_t len,
					  uint16_t attempts, k_timeout_t retry_delay)
{
	return wait_front_node(front, "backend", buf, len, attempts, retry_delay);
}

int xen_blkfront_xenbus_verify_vdev(struct xen_blkfront *front, char *buf, size_t len,
				    k_timeout_t timeout)
{
	char path[XEN_BLKFRONT_PATH_MAX];
	uint64_t vdev;
	int ret;

	ret = make_path(path, sizeof(path), front->frontend_path, "virtual-device");
	if (ret != 0) {
		return ret;
	}

	ret = read_string(path, buf, len, timeout);
	if (ret != 0) {
		return ret;
	}

	ret = parse_u64(buf, &vdev);
	if (ret != 0) {
		return ret;
	}

	return (vdev == front->vdev) ? 0 : -EINVAL;
}

int xen_blkfront_xenbus_publish_frontend(struct xen_blkfront *front, char *buf, size_t len,
					 k_timeout_t timeout)
{
	int ret;

	ret = write_front_uint(front, "ring-ref", (uint32_t)front->ring_gref, buf, len,
			       timeout);
	if (ret != 0) {
		return ret;
	}

	ret = write_front_uint(front, "event-channel", (uint32_t)front->evtchn, buf, len,
			       timeout);
	if (ret != 0) {
		return ret;
	}

	ret = write_front_node(front, "protocol", XEN_IO_PROTO_ABI_NATIVE, buf, len, timeout);
	if (ret != 0) {
		return ret;
	}

	ret = write_front_node(front, "feature-persistent", "0", buf, len, timeout);
	if (ret != 0) {
		return ret;
	}

	ret = write_front_uint(front, "state", XenbusStateInitialised, buf, len, timeout);
	if (ret == 0) {
		front->published = true;
	}

	return ret;
}

int xen_blkfront_xenbus_wait_connected(struct xen_blkfront *front, char *buf, size_t len,
				       uint16_t attempts, k_timeout_t retry_delay)
{
	char path[XEN_BLKFRONT_PATH_MAX];
	uint64_t state;
	int ret;

	ret = make_path(path, sizeof(path), front->backend_path, "state");
	if (ret != 0) {
		return ret;
	}

	for (uint16_t attempt = 0; attempt < attempts; attempt++) {
		ret = read_string(path, buf, len, K_MSEC(500));
		if (ret == 0) {
			ret = parse_u64(buf, &state);
			if (ret != 0) {
				return ret;
			}

			if (state == XenbusStateConnected) {
				return 0;
			}
		}

		if ((attempt + 1U) < attempts) {
			k_sleep(retry_delay);
		}
	}

	return -ETIMEDOUT;
}

static int wait_backend_state(struct xen_blkfront *front, char *buf, size_t len,
			      enum xenbus_state first, enum xenbus_state second)
{
	char path[XEN_BLKFRONT_PATH_MAX];
	uint64_t state;
	int ret;

	ret = make_path(path, sizeof(path), front->backend_path, "state");
	if (ret != 0) {
		return ret;
	}

	for (uint16_t attempt = 0; attempt < front->backend_wait_attempts; attempt++) {
		ret = read_string(path, buf, len, K_MSEC(500));
		if (ret == 0) {
			ret = parse_u64(buf, &state);
			if (ret != 0) {
				return ret;
			}

			if ((state == first) || (state == second)) {
				return 0;
			}
		}

		if ((attempt + 1U) < front->backend_wait_attempts) {
			k_sleep(front->backend_retry_delay);
		}
	}

	return -ETIMEDOUT;
}

int xen_blkfront_xenbus_read_capacity(struct xen_blkfront *front, char *buf, size_t len,
				      k_timeout_t timeout)
{
	char path[XEN_BLKFRONT_PATH_MAX];
	int ret;

	ret = make_path(path, sizeof(path), front->backend_path, "sectors");
	if (ret != 0) {
		return ret;
	}

	ret = read_string(path, buf, len, timeout);
	if (ret != 0) {
		return ret;
	}

	return parse_u64(buf, &front->sectors);
}

int xen_blkfront_xenbus_close(struct xen_blkfront *front, char *buf, size_t len)
{
	int ret;

	if (!front->published) {
		return 0;
	}

	if ((buf == NULL) || (len == 0)) {
		return -EINVAL;
	}

	ret = write_front_uint(front, "state", XenbusStateClosing, buf, len, front->xs_timeout);
	if (ret != 0) {
		return ret;
	}

	ret = wait_backend_state(front, buf, len, XenbusStateClosing, XenbusStateClosed);
	if (ret != 0) {
		return ret;
	}

	ret = write_front_uint(front, "state", XenbusStateClosed, buf, len, front->xs_timeout);
	if (ret != 0) {
		return ret;
	}

	ret = wait_backend_state(front, buf, len, XenbusStateClosed, XenbusStateClosed);
	if (ret != 0) {
		return ret;
	}

	front->published = false;
	return 0;
}
