/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include "xen_blkfront_priv.h"

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

	ret = xen_blkfront_xenbus_read_capacity(front, xs_buf, xs_buf_len, cfg->xs_timeout);
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
	uint64_t sector_count;

	if ((front == NULL) || (data == NULL) || (len == 0) || (len > XEN_PAGE_SIZE) ||
	    ((len % XEN_BLKFRONT_SECTOR_SIZE) != 0)) {
		return -EINVAL;
	}

	sector_count = len / XEN_BLKFRONT_SECTOR_SIZE;
	if ((front->sectors == 0U) || (sector >= front->sectors) ||
	    (sector_count > (front->sectors - sector))) {
		return -ERANGE;
	}

	return xen_blkfront_queue_read(front, sector, data, len);
}

uint64_t xen_blkfront_sectors(const struct xen_blkfront *front)
{
	return (front != NULL) ? front->sectors : 0U;
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
