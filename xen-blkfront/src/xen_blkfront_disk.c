/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/drivers/disk.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/xen/generic.h>

#include <xen_blkfront.h>

#define XEN_BLKFRONT_DISK_MAX_SECTORS_PER_REQ \
	(XEN_PAGE_SIZE / XEN_BLKFRONT_SECTOR_SIZE)

struct xen_blkfront_disk {
	struct disk_info info;
	struct k_mutex lock;
	struct xen_blkfront *front;
	char xs_buf[CONFIG_XEN_BLKFRONT_XS_BUF_SIZE];
};

static struct xen_blkfront_disk blkfront_disk = {
	.info = {
		.name = CONFIG_XEN_BLKFRONT_DISK_NAME,
	},
};

static int blkfront_disk_init(struct disk_info *disk)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	const struct xen_blkfront_config cfg = {
		.frontend_path = CONFIG_XEN_BLKFRONT_FRONTEND_PATH,
		.backend_path = CONFIG_XEN_BLKFRONT_BACKEND_PATH,
		.vdev = CONFIG_XEN_BLKFRONT_VDEV,
		.backend_domid = CONFIG_XEN_BLKFRONT_BACKEND_DOMID,
		.xs_timeout = K_MSEC(CONFIG_XEN_BLKFRONT_XS_TIMEOUT_MS),
	};
	int ret = 0;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	if (ctx->front != NULL) {
		goto out;
	}

	ret = xen_blkfront_open(&cfg, &ctx->front, ctx->xs_buf, sizeof(ctx->xs_buf));

out:
	k_mutex_unlock(&ctx->lock);
	return ret;
}

static int blkfront_disk_status(struct disk_info *disk)
{
	struct xen_blkfront_disk *ctx = CONTAINER_OF(disk, struct xen_blkfront_disk, info);
	int ret;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	ret = (ctx->front != NULL) ? DISK_STATUS_OK : DISK_STATUS_UNINIT;
	k_mutex_unlock(&ctx->lock);

	return ret;
}

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

static int blkfront_disk_deinit_locked(struct xen_blkfront_disk *ctx)
{
	struct xen_blkfront *front = ctx->front;

	if (front == NULL) {
		return 0;
	}

	ctx->front = NULL;
	return xen_blkfront_close(front, ctx->xs_buf, sizeof(ctx->xs_buf));
}

static int blkfront_disk_deinit(struct xen_blkfront_disk *ctx)
{
	int ret;

	k_mutex_lock(&ctx->lock, K_FOREVER);
	ret = blkfront_disk_deinit_locked(ctx);
	k_mutex_unlock(&ctx->lock);

	return ret;
}

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

static const struct disk_operations blkfront_disk_ops = {
	.init = blkfront_disk_init,
	.status = blkfront_disk_status,
	.read = blkfront_disk_read,
	.write = blkfront_disk_write,
	.erase = blkfront_disk_erase,
	.ioctl = blkfront_disk_ioctl,
};

static int blkfront_disk_register(void)
{
	k_mutex_init(&blkfront_disk.lock);
	blkfront_disk.info.ops = &blkfront_disk_ops;
	return disk_access_register(&blkfront_disk.info);
}

SYS_INIT(blkfront_disk_register, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
