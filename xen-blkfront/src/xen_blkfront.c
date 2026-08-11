/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include "xen_blkfront_priv.h"

/* Normalize caller retry settings to the built-in backend wait policy. */
static void normalize_backend_wait(struct xen_blkfront *front,
				   const struct xen_blkfront_config *cfg)
{
	if (cfg->backend_wait_attempts == 0U) {
		front->backend_wait_attempts = XEN_BLKFRONT_BACKEND_WAIT_ATTEMPTS_DEFAULT;
		front->backend_retry_delay = XEN_BLKFRONT_BACKEND_RETRY_DELAY_DEFAULT;
		return;
	}

	front->backend_wait_attempts = cfg->backend_wait_attempts;
	front->backend_retry_delay = cfg->backend_retry_delay;
}

/* Close the XenBus state machine and release transport/data resources. */
static int release_frontend(struct xen_blkfront *front, char *xs_buf, size_t xs_buf_len)
{
	int ret = 0;

	if (front == NULL) {
		return 0;
	}

	k_mutex_lock(&front->request_lock, K_FOREVER);
	ret = xen_blkfront_xenbus_close(front, xs_buf, xs_buf_len);
	xen_blkfront_queue_release_deferred(front);
	xen_blkfront_transport_cleanup(front);
	k_mutex_unlock(&front->request_lock);
	return ret;
}

int xen_blkfront_open(const struct xen_blkfront_config *cfg, struct xen_blkfront **frontp,
		      char *xs_buf, size_t xs_buf_len)
{
	struct xen_blkfront *front;
	int ret;

	if ((cfg == NULL) || (frontp == NULL) || (xs_buf == NULL) || (xs_buf_len == 0) ||
	    (cfg->frontend_path == NULL) || (cfg->backend_path == NULL)) {
		return -EINVAL;
	}

	*frontp = NULL;

	front = k_calloc(1, sizeof(*front));
	if (front == NULL) {
		return -ENOMEM;
	}

	front->ring_gref = -1;
	front->evtchn = -1;
	front->next_req_id = XEN_BLKFRONT_REQ_ID;
	front->xs_timeout = cfg->xs_timeout;
	normalize_backend_wait(front, cfg);
	k_mutex_init(&front->request_lock);

	ret = xen_blkfront_xenbus_configure(front, cfg);
	if (ret != 0) {
		goto fail;
	}

	ret = xen_blkfront_xenbus_wait_backend_path(front, xs_buf, xs_buf_len,
						    front->backend_wait_attempts,
						    front->backend_retry_delay);
	if (ret != 0) {
		goto fail;
	}

	ret = xen_blkfront_xenbus_verify_vdev(front, xs_buf, xs_buf_len, cfg->xs_timeout);
	if (ret != 0) {
		goto fail;
	}

	ret = xen_blkfront_transport_connect(front);
	if (ret != 0) {
		goto fail;
	}

	ret = xen_blkfront_xenbus_publish_frontend(front, xs_buf, xs_buf_len,
						   cfg->xs_timeout);
	if (ret != 0) {
		goto fail;
	}

	ret = xen_blkfront_xenbus_wait_connected(front, xs_buf, xs_buf_len,
						 front->backend_wait_attempts,
						 front->backend_retry_delay);
	if (ret != 0) {
		goto fail;
	}

	ret = xen_blkfront_xenbus_discover(front, xs_buf, xs_buf_len, cfg->xs_timeout);
	if (ret != 0) {
		goto fail;
	}

	*frontp = front;
	return 0;

fail:
	release_frontend(front, xs_buf, xs_buf_len);
	k_free(front);
	return ret;
}

int xen_blkfront_read(struct xen_blkfront *front, uint64_t sector, void *data, size_t len)
{
	uint8_t *cursor = data;
	const uint64_t max_chunk_sectors = XEN_PAGE_SIZE / XEN_BLKFRONT_SECTOR_SIZE;
	uint64_t sector_count;
	uint64_t remaining;
	int ret;

	if ((front == NULL) || (data == NULL) || (len == 0) ||
	    ((len % XEN_BLKFRONT_SECTOR_SIZE) != 0)) {
		return -EINVAL;
	}

	sector_count = len / XEN_BLKFRONT_SECTOR_SIZE;
	if ((front->info.sectors == 0U) || (sector >= front->info.sectors) ||
	    (sector_count > (front->info.sectors - sector))) {
		return -ERANGE;
	}

	remaining = sector_count;
	while (remaining > 0U) {
		uint64_t chunk_sectors = remaining;
		size_t chunk_len;

		if (chunk_sectors > max_chunk_sectors) {
			chunk_sectors = max_chunk_sectors;
		}

		chunk_len = (size_t)(chunk_sectors * XEN_BLKFRONT_SECTOR_SIZE);
		ret = xen_blkfront_queue_read(front, sector, cursor, chunk_len);
		if (ret != 0) {
			return ret;
		}

		sector += chunk_sectors;
		cursor += chunk_len;
		remaining -= chunk_sectors;
	}

	return 0;
}

int xen_blkfront_write(struct xen_blkfront *front, uint64_t sector, const void *data, size_t len)
{
	const uint8_t *cursor = data;
	const uint64_t max_chunk_sectors = XEN_PAGE_SIZE / XEN_BLKFRONT_SECTOR_SIZE;
	uint64_t sector_count;
	uint64_t remaining;
	int ret;

	if ((front == NULL) || (data == NULL) || (len == 0) ||
	    ((len % XEN_BLKFRONT_SECTOR_SIZE) != 0)) {
		return -EINVAL;
	}

	sector_count = len / XEN_BLKFRONT_SECTOR_SIZE;
	if ((front->info.sectors == 0U) || (sector >= front->info.sectors) ||
	    (sector_count > (front->info.sectors - sector))) {
		return -ERANGE;
	}

	if (!front->info.writable) {
		return -EROFS;
	}

	remaining = sector_count;
	while (remaining > 0U) {
		uint64_t chunk_sectors = remaining;
		size_t chunk_len;

		if (chunk_sectors > max_chunk_sectors) {
			chunk_sectors = max_chunk_sectors;
		}

		chunk_len = (size_t)(chunk_sectors * XEN_BLKFRONT_SECTOR_SIZE);
		ret = xen_blkfront_queue_write(front, sector, cursor, chunk_len);
		if (ret != 0) {
			return ret;
		}

		sector += chunk_sectors;
		cursor += chunk_len;
		remaining -= chunk_sectors;
	}

	return 0;
}

int xen_blkfront_flush(struct xen_blkfront *front)
{
	if (front == NULL) {
		return -EINVAL;
	}

	if (!front->info.feature_flush_cache) {
		return -ENOTSUP;
	}

	return xen_blkfront_queue_flush(front);
}

/* Check backend discard alignment rules before a request reaches the ring. */
static bool discard_range_is_aligned(const struct xen_blkfront *front, uint64_t sector,
				     uint64_t sector_count)
{
	uint64_t granularity = front->info.discard_granularity;
	uint64_t alignment = front->info.discard_alignment;
	uint64_t granularity_sectors;
	uint64_t alignment_sectors;

	if (granularity == 0U) {
		granularity = XEN_BLKFRONT_SECTOR_SIZE;
	}

	if (((granularity % XEN_BLKFRONT_SECTOR_SIZE) != 0U) ||
	    ((alignment % XEN_BLKFRONT_SECTOR_SIZE) != 0U)) {
		return false;
	}

	granularity_sectors = granularity / XEN_BLKFRONT_SECTOR_SIZE;
	alignment_sectors = alignment / XEN_BLKFRONT_SECTOR_SIZE;

	return (granularity_sectors != 0U) && (sector >= alignment_sectors) &&
	       (((sector - alignment_sectors) % granularity_sectors) == 0U) &&
	       ((sector_count % granularity_sectors) == 0U);
}

int xen_blkfront_discard(struct xen_blkfront *front, uint64_t sector,
			 uint64_t sector_count, bool secure)
{
	if ((front == NULL) || (sector_count == 0U)) {
		return -EINVAL;
	}

	if (!front->info.feature_discard || (secure && !front->info.discard_secure)) {
		return -ENOTSUP;
	}

	if ((front->info.sectors == 0U) || (sector >= front->info.sectors) ||
	    (sector_count > (front->info.sectors - sector))) {
		return -ERANGE;
	}

	if (!discard_range_is_aligned(front, sector, sector_count)) {
		return -EINVAL;
	}

	return xen_blkfront_queue_discard(front, sector, sector_count, secure);
}

uint64_t xen_blkfront_sectors(const struct xen_blkfront *front)
{
	return (front != NULL) ? front->info.sectors : 0U;
}

int xen_blkfront_get_info(const struct xen_blkfront *front, struct xen_blkfront_info *info)
{
	if ((front == NULL) || (info == NULL)) {
		return -EINVAL;
	}

	*info = front->info;
	return 0;
}

int xen_blkfront_close(struct xen_blkfront *front, char *xs_buf, size_t xs_buf_len)
{
	int ret;

	if ((front != NULL) && front->published && ((xs_buf == NULL) || (xs_buf_len == 0))) {
		return -EINVAL;
	}

	ret = release_frontend(front, xs_buf, xs_buf_len);
	k_free(front);
	return ret;
}
