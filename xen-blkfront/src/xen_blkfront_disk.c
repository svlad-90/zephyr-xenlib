/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/disk.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/xen/generic.h>

#include <xenstore_cli.h>

#include "xen_blkfront_priv.h"

#define XEN_BLKFRONT_DISK_MAX_SECTORS_PER_REQ XEN_BLKFRONT_MAX_SECTORS_PER_REQUEST
#define XEN_BLKFRONT_DISK_NAME_MAX 16
#define XEN_BLKFRONT_BACKEND_DOMID_PREFIX "/local/domain/"

struct xen_blkfront_disk {
	/* Zephyr disk_access registration record embedded in this slot. */
	struct disk_info info;
	/* Serializes this disk slot's frontend handle and metadata. */
	struct k_mutex lock;
	/* Stable disk_access name, for example XENBLK0. */
	char name[XEN_BLKFRONT_DISK_NAME_MAX];
	/* Frontend XenStore path assigned to this slot. */
	char frontend_path[XEN_BLKFRONT_PATH_MAX];
	/* Backend XenStore path read from frontend_path/backend. */
	char backend_path[XEN_BLKFRONT_PATH_MAX];
	/* Open protocol frontend, or NULL while the disk is not initialized. */
	struct xen_blkfront *front;
	/* Parsed Xen virtual-device id used for deterministic sorting. */
	uint16_t vdev;
	/* Backend domain id parsed from backend_path or the configured default. */
	uint16_t backend_domid;
	/* True after the slot has been bound to a XenStore vbd entry. */
	bool configured;
	/* Per-slot scratch buffer used by XenStore and close/open operations. */
	char xs_buf[CONFIG_XEN_BLKFRONT_XS_BUF_SIZE];
};

/* Protects registry-wide discovery, slot assignment, and watch registration. */
K_MUTEX_DEFINE(blkfront_registry_lock);
/* Bounded disk_access registry; each entry can bind one Xen vbd node. */
static struct xen_blkfront_disk blkfront_disks[CONFIG_XEN_BLKFRONT_MAX_DISKS];

/* disk_access init callback; lazily discovers and opens a configured slot. */
static int blkfront_disk_init(struct disk_info *disk);
/* disk_access status callback; reports whether the slot has an open frontend. */
static int blkfront_disk_status(struct disk_info *disk);
/* disk_access read callback; chunks requests at the blkfront segment limit. */
static int blkfront_disk_read(struct disk_info *disk, uint8_t *data_buf,
			      uint32_t start_sector, uint32_t num_sector);
/* disk_access write callback; chunks requests at the blkfront segment limit. */
static int blkfront_disk_write(struct disk_info *disk, const uint8_t *data_buf,
			       uint32_t start_sector, uint32_t num_sector);
/* disk_access erase callback; maps erase to Xen discard when supported. */
static int blkfront_disk_erase(struct disk_info *disk, uint32_t start_sector,
			       uint32_t num_sector);
/* Close an open frontend while ctx->lock is already held. */
static int blkfront_disk_deinit_locked(struct xen_blkfront_disk *ctx);
/* disk_access ioctl callback for init/deinit/sync and geometry queries. */
static int blkfront_disk_ioctl(struct disk_info *disk, uint8_t cmd, void *buff);
/* Scan XenStore and bind present vbd entries to disk_access slots. */
static int blkfront_discover_disks(void);

/* Zephyr disk_access vtable for every registered XENBLK<n> slot. */
static const struct disk_operations blkfront_disk_ops = {
	.init = blkfront_disk_init,
	.status = blkfront_disk_status,
	.read = blkfront_disk_read,
	.write = blkfront_disk_write,
	.erase = blkfront_disk_erase,
	.ioctl = blkfront_disk_ioctl,
};

/* Parse a decimal vbd name or domid value into a 16-bit integer. */
static int blkfront_parse_u16(const char *value, uint16_t *result)
{
	char *end;
	unsigned long parsed;

	if ((value == NULL) || (value[0] == '\0') || (value[0] < '0') || (value[0] > '9')) {
		return -EINVAL;
	}

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if ((errno != 0) || (end == value) || (*end != '\0') || (parsed > UINT16_MAX)) {
		return -EINVAL;
	}

	*result = (uint16_t)parsed;
	return 0;
}

/* Extract backend domid from a /local/domain/<domid>/... backend path. */
static uint16_t blkfront_backend_domid_from_path(const char *backend_path)
{
	const char *cursor;
	char domid_buf[6];
	size_t len = 0;
	uint16_t domid;

	if (strncmp(backend_path, XEN_BLKFRONT_BACKEND_DOMID_PREFIX,
		    strlen(XEN_BLKFRONT_BACKEND_DOMID_PREFIX)) != 0) {
		return CONFIG_XEN_BLKFRONT_BACKEND_DOMID;
	}

	cursor = backend_path + strlen(XEN_BLKFRONT_BACKEND_DOMID_PREFIX);
	while ((cursor[len] >= '0') && (cursor[len] <= '9') && (len < (sizeof(domid_buf) - 1U))) {
		domid_buf[len] = cursor[len];
		len++;
	}
	domid_buf[len] = '\0';

	if ((cursor[len] != '/') || (blkfront_parse_u16(domid_buf, &domid) != 0)) {
		return CONFIG_XEN_BLKFRONT_BACKEND_DOMID;
	}

	return domid;
}

/* Build the stable Zephyr disk name for a registry slot. */
static int blkfront_make_disk_name(char *name, size_t len, size_t index)
{
	int ret;

	ret = snprintk(name, len, "%s%u", CONFIG_XEN_BLKFRONT_DISK_NAME_PREFIX,
		       (unsigned int)index);
	if ((ret < 0) || ((size_t)ret >= len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

/* Build a frontend vbd path from the configured root and child name. */
static int blkfront_make_device_path(char *path, size_t len, const char *vdev)
{
	int ret;

	ret = snprintk(path, len, "%s/%s", CONFIG_XEN_BLKFRONT_DEVICE_ROOT, vdev);
	if ((ret < 0) || ((size_t)ret >= len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

/* Find the already configured slot for a virtual-device id. */
static struct xen_blkfront_disk *blkfront_find_by_vdev(uint16_t vdev)
{
	for (size_t i = 0; i < ARRAY_SIZE(blkfront_disks); i++) {
		if (blkfront_disks[i].configured && (blkfront_disks[i].vdev == vdev)) {
			return &blkfront_disks[i];
		}
	}

	return NULL;
}

/* Find the first free registry slot that can accept a newly discovered vbd. */
static struct xen_blkfront_disk *blkfront_first_unconfigured(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(blkfront_disks); i++) {
		if (!blkfront_disks[i].configured) {
			return &blkfront_disks[i];
		}
	}

	return NULL;
}

/* Open the frontend for a disk_access slot, discovering the slot on demand. */
static int blkfront_disk_init(struct disk_info *disk)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	int ret = 0;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	if (ctx->front != NULL) {
		goto out;
	}

	if (!ctx->configured) {
		k_mutex_unlock(&ctx->lock);
		ret = blkfront_discover_disks();
		k_mutex_lock(&ctx->lock, K_FOREVER);
		if (ret != 0) {
			goto out;
		}
		if (!ctx->configured) {
			ret = -ENODEV;
			goto out;
		}
	}

	const struct xen_blkfront_config cfg = {
		.frontend_path = ctx->frontend_path,
		.backend_path = ctx->backend_path,
		.vdev = ctx->vdev,
		.backend_domid = ctx->backend_domid,
		.xs_timeout = K_MSEC(CONFIG_XEN_BLKFRONT_XS_TIMEOUT_MS),
	};

	ret = xen_blkfront_open(&cfg, &ctx->front, ctx->xs_buf, sizeof(ctx->xs_buf));

out:
	k_mutex_unlock(&ctx->lock);
	return ret;
}

/* Bind one XenStore vbd child to an unconfigured disk_access slot. */
static int blkfront_disk_configure(struct xen_blkfront_disk *ctx, const char *vdev_name)
{
	char backend_node[XEN_BLKFRONT_PATH_MAX];
	ssize_t len;
	int ret;

	ret = blkfront_parse_u16(vdev_name, &ctx->vdev);
	if (ret != 0) {
		return ret;
	}

	ret = blkfront_make_device_path(ctx->frontend_path, sizeof(ctx->frontend_path),
					vdev_name);
	if (ret != 0) {
		return ret;
	}

	ret = snprintk(backend_node, sizeof(backend_node), "%s/backend", ctx->frontend_path);
	if ((ret < 0) || ((size_t)ret >= sizeof(backend_node))) {
		return -ENAMETOOLONG;
	}

	len = xs_read_timeout(backend_node, ctx->backend_path, sizeof(ctx->backend_path),
			      XS_TRANSACTION_NONE, K_MSEC(CONFIG_XEN_BLKFRONT_XS_TIMEOUT_MS));
	if (len < 0) {
		return (int)len;
	}
	if ((size_t)len >= sizeof(ctx->backend_path)) {
		return -EMSGSIZE;
	}

	ctx->backend_path[len] = '\0';
	ctx->backend_domid = blkfront_backend_domid_from_path(ctx->backend_path);
	ctx->configured = true;

	return 0;
}

/* Configure a vbd if it is new, or mark the existing slot present. */
static int blkfront_configure_one(const char *vdev_name)
{
	struct xen_blkfront_disk *ctx;
	/* Xen virtual-device id parsed from the vbd child name. */
	uint16_t vdev;
	int ret;

	ret = blkfront_parse_u16(vdev_name, &vdev);
	if (ret != 0) {
		return ret;
	}

	ctx = blkfront_find_by_vdev(vdev);
	if (ctx != NULL) {
		return 0;
	}

	ctx = blkfront_first_unconfigured();
	if (ctx == NULL) {
		return -ENOSPC;
	}

	return blkfront_disk_configure(ctx, vdev_name);
}

/* Discover current vbd children, configure slots, and drop removed devices. */
static int blkfront_discover_disks(void)
{
	char dir[CONFIG_XEN_BLKFRONT_XS_BUF_SIZE];
	size_t pos = 0;
	ssize_t len;
	int ret = 0;

	k_mutex_lock(&blkfront_registry_lock, K_FOREVER);

	ret = xs_init();
	if (ret != 0) {
		goto out;
	}

	len = xs_directory_timeout(CONFIG_XEN_BLKFRONT_DEVICE_ROOT, dir, sizeof(dir),
				   XS_TRANSACTION_NONE,
				   K_MSEC(CONFIG_XEN_BLKFRONT_XS_TIMEOUT_MS));
	if (len == -ENOENT) {
		ret = 0;
		goto out;
	}
	if (len < 0) {
		ret = (int)len;
		goto out;
	}

	while (pos < (size_t)len) {
		const char *entry = &dir[pos];
		size_t remaining = (size_t)len - pos;
		size_t entry_len = strnlen(entry, remaining);

		if (entry_len == remaining) {
			ret = -EMSGSIZE;
			goto out;
		}

		if (entry_len > 0U) {
			ret = blkfront_configure_one(entry);
			if (ret == -ENOENT) {
				ret = 0;
			} else if (ret == -ENOSPC) {
				ret = 0;
				break;
			} else if (ret != 0) {
				goto out;
			}
		}

		pos += entry_len + 1U;
	}

out:
	k_mutex_unlock(&blkfront_registry_lock);
	return ret;
}

/* Report initialized/uninitialized state to Zephyr disk_access. */
static int blkfront_disk_status(struct disk_info *disk)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	int ret;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	ret = (ctx->front != NULL) ? DISK_STATUS_OK : DISK_STATUS_UNINIT;
	k_mutex_unlock(&ctx->lock);

	return ret;
}

/* Read sectors through the open blkfront handle for this disk_access slot. */
static int blkfront_disk_read(struct disk_info *disk, uint8_t *data_buf,
			      uint32_t start_sector, uint32_t num_sector)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	uint32_t sector = start_sector;
	uint32_t remaining = num_sector;
	uint8_t *cursor = data_buf;
	uint64_t sectors;
	int ret;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	if ((ctx->front == NULL) || (data_buf == NULL)) {
		ret = -EINVAL;
		goto out;
	}

	if (num_sector == 0U) {
		ret = 0;
		goto out;
	}

	sectors = xen_blkfront_sectors(ctx->front);
	if ((sectors == 0U) || (start_sector >= sectors) ||
	    (num_sector > (sectors - start_sector))) {
		ret = -ERANGE;
		goto out;
	}

	while (remaining > 0U) {
		uint32_t chunk = MIN(remaining, XEN_BLKFRONT_DISK_MAX_SECTORS_PER_REQ);
		size_t len = chunk * XEN_BLKFRONT_SECTOR_SIZE;

		ret = xen_blkfront_read(ctx->front, sector, cursor, len);
		if (ret != 0) {
			goto out;
		}

		sector += chunk;
		remaining -= chunk;
		cursor += len;
	}

	ret = 0;

out:
	k_mutex_unlock(&ctx->lock);
	return ret;
}

/* Write sectors through the open blkfront handle for this disk_access slot. */
static int blkfront_disk_write(struct disk_info *disk, const uint8_t *data_buf,
			       uint32_t start_sector, uint32_t num_sector)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	uint32_t sector = start_sector;
	uint32_t remaining = num_sector;
	const uint8_t *cursor = data_buf;
	uint64_t sectors;
	int ret;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	if ((ctx->front == NULL) || (data_buf == NULL)) {
		ret = -EINVAL;
		goto out;
	}

	if (num_sector == 0U) {
		ret = 0;
		goto out;
	}

	sectors = xen_blkfront_sectors(ctx->front);
	if ((sectors == 0U) || (start_sector >= sectors) ||
	    (num_sector > (sectors - start_sector))) {
		ret = -ERANGE;
		goto out;
	}

	while (remaining > 0U) {
		uint32_t chunk = MIN(remaining, XEN_BLKFRONT_DISK_MAX_SECTORS_PER_REQ);
		size_t len = chunk * XEN_BLKFRONT_SECTOR_SIZE;

		ret = xen_blkfront_write(ctx->front, sector, cursor, len);
		if (ret != 0) {
			goto out;
		}

		sector += chunk;
		remaining -= chunk;
		cursor += len;
	}

	ret = 0;

out:
	k_mutex_unlock(&ctx->lock);
	return ret;
}

/* Translate disk_access erase into a non-secure Xen discard request. */
static int blkfront_disk_erase(struct disk_info *disk, uint32_t start_sector,
			       uint32_t num_sector)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	int ret;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	if (ctx->front == NULL) {
		ret = -EINVAL;
		goto out;
	}

	if (num_sector == 0U) {
		ret = 0;
		goto out;
	}

	ret = xen_blkfront_discard(ctx->front, start_sector, num_sector, false);

out:
	k_mutex_unlock(&ctx->lock);
	return ret;
}

/* Close the protocol frontend and detach it from the slot under ctx->lock. */
static int blkfront_disk_deinit_locked(struct xen_blkfront_disk *ctx)
{
	struct xen_blkfront *front = ctx->front;

	if (front == NULL) {
		return 0;
	}

	ctx->front = NULL;
	return xen_blkfront_close(front, ctx->xs_buf, sizeof(ctx->xs_buf));
}

/* Locking wrapper for disk_access deinitialization. */
static int blkfront_disk_deinit(struct xen_blkfront_disk *ctx)
{
	int ret;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	ret = blkfront_disk_deinit_locked(ctx);
	k_mutex_unlock(&ctx->lock);

	return ret;
}

/* Dispatch disk_access control and geometry operations to blkfront APIs. */
static int blkfront_disk_ioctl(struct disk_info *disk, uint8_t cmd, void *buff)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	struct xen_blkfront_info info;
	uint64_t sectors;
	int ret;

	switch (cmd) {
	case DISK_IOCTL_CTRL_INIT:
		return blkfront_disk_init(disk);
	case DISK_IOCTL_CTRL_DEINIT:
		return blkfront_disk_deinit(ctx);
	case DISK_IOCTL_CTRL_SYNC:
		k_mutex_lock(&ctx->lock, K_FOREVER);
		if (ctx->front == NULL) {
			ret = -EINVAL;
			goto out;
		}

		ret = xen_blkfront_flush(ctx->front);
		goto out;
	case DISK_IOCTL_GET_SECTOR_COUNT:
		k_mutex_lock(&ctx->lock, K_FOREVER);
		if ((ctx->front == NULL) || (buff == NULL)) {
			ret = -EINVAL;
			goto out;
		}

		sectors = xen_blkfront_sectors(ctx->front);
		if (sectors > UINT32_MAX) {
			ret = -EOVERFLOW;
			goto out;
		}

		*(uint32_t *)buff = (uint32_t)sectors;
		ret = 0;
		goto out;
	case DISK_IOCTL_GET_SECTOR_SIZE:
		k_mutex_lock(&ctx->lock, K_FOREVER);
		if ((ctx->front == NULL) || (buff == NULL)) {
			ret = -EINVAL;
			goto out;
		}

		ret = xen_blkfront_get_info(ctx->front, &info);
		if (ret != 0) {
			goto out;
		}

		*(uint32_t *)buff = info.sector_size;
		ret = 0;
		goto out;
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
		k_mutex_lock(&ctx->lock, K_FOREVER);
		if ((ctx->front == NULL) || (buff == NULL)) {
			ret = -EINVAL;
			goto out;
		}

		ret = xen_blkfront_get_info(ctx->front, &info);
		if (ret != 0) {
			goto out;
		}

		if (!info.feature_discard || (info.discard_granularity == 0U) ||
		    ((info.discard_granularity % XEN_BLKFRONT_SECTOR_SIZE) != 0U)) {
			*(uint32_t *)buff = 1U;
			ret = 0;
			goto out;
		}

		*(uint32_t *)buff = info.discard_granularity / XEN_BLKFRONT_SECTOR_SIZE;
		ret = 0;
		goto out;
	default:
		return -EINVAL;
	}

out:
	k_mutex_unlock(&ctx->lock);
	return ret;
}

/* Register all bounded XENBLK<n> slots and initialize hotplug discovery work. */
static int blkfront_disk_register(void)
{
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(blkfront_disks); i++) {
		struct xen_blkfront_disk *ctx = &blkfront_disks[i];

		ret = blkfront_make_disk_name(ctx->name, sizeof(ctx->name), i);
		if (ret != 0) {
			return ret;
		}

		k_mutex_init(&ctx->lock);
		ctx->info.name = ctx->name;
		ctx->info.ops = &blkfront_disk_ops;

		ret = disk_access_register(&ctx->info);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

SYS_INIT(blkfront_disk_register, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
