/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "xen_blkfront_priv.h"

int xen_blkfront_queue_alloc_data_pool(struct xen_blkfront *front)
{
	int ret;

	if (front == NULL) {
		return -EINVAL;
	}

	if (front->data_pool_allocated) {
		return 0;
	}

	for (uint8_t i = 0; i < XEN_BLKFRONT_MAX_SEGMENTS_PER_REQUEST; i++) {
		ret = xen_blkfront_transport_alloc_data_page(&front->data_pool[i]);
		if (ret != 0) {
			xen_blkfront_queue_free_data_pool(front);
			return ret;
		}
	}

	front->data_pool_allocated = true;
	return 0;
}

void xen_blkfront_queue_free_data_pool(struct xen_blkfront *front)
{
	if (front == NULL) {
		return;
	}

	for (uint8_t i = 0; i < XEN_BLKFRONT_MAX_SEGMENTS_PER_REQUEST; i++) {
		xen_blkfront_transport_free_data_page(&front->data_pool[i]);
	}

	front->data_pool_allocated = false;
	front->has_deferred_data = false;
}

/* Bind a caller buffer length to the fixed grant-pool segment layout. */
static int init_data_request(struct xen_blkfront *front,
			     struct xen_blkfront_data_request *data, size_t len)
{
	uint8_t nr_segments;

	if ((front == NULL) || (data == NULL) || (len == 0U) ||
	    ((len % XEN_BLKFRONT_SECTOR_SIZE) != 0U)) {
		return -EINVAL;
	}

	nr_segments = (uint8_t)((len + XEN_PAGE_SIZE - 1U) / XEN_PAGE_SIZE);
	if ((nr_segments == 0U) || (nr_segments > XEN_BLKFRONT_MAX_SEGMENTS_PER_REQUEST)) {
		return -EINVAL;
	}

	if (!front->data_pool_allocated || front->has_deferred_data) {
		return -EIO;
	}

	data->pages = front->data_pool;
	data->len = len;
	data->nr_segments = nr_segments;
	return 0;
}

/* Copy a write buffer into the grant-backed pages visible to blkback. */
static void copy_to_data_request(struct xen_blkfront_data_request *data, const uint8_t *src)
{
	size_t remaining = data->len;

	for (uint8_t i = 0; i < data->nr_segments; i++) {
		size_t segment_len = MIN(remaining, (size_t)XEN_PAGE_SIZE);

		memcpy(data->pages[i].page, src, segment_len);
		src += segment_len;
		remaining -= segment_len;
	}
}

/* Copy a completed read response out of the grant-backed pages. */
static void copy_from_data_request(const struct xen_blkfront_data_request *data, uint8_t *dst)
{
	size_t remaining = data->len;

	for (uint8_t i = 0; i < data->nr_segments; i++) {
		size_t segment_len = MIN(remaining, (size_t)XEN_PAGE_SIZE);

		memcpy(dst, data->pages[i].page, segment_len);
		dst += segment_len;
		remaining -= segment_len;
	}
}

/* Serialize one read/write request, including data copy and failure latching. */
static int xen_blkfront_queue_rw(struct xen_blkfront *front, uint64_t sector,
				 const void *write_data, void *read_data, size_t len,
				 uint8_t operation)
{
	struct xen_blkfront_data_request data_request = { 0 };
	uint64_t req_id;
	bool request_open = false;
	int ret;

	k_mutex_lock(&front->request_lock, K_FOREVER);

	if (front->failed) {
		ret = -EIO;
		goto out_unlock;
	}

	ret = init_data_request(front, &data_request, len);
	if (ret != 0) {
		goto out_unlock;
	}

	if (operation == BLKIF_OP_WRITE) {
		copy_to_data_request(&data_request, write_data);
	}

	req_id = front->next_req_id++;
	ret = xen_blkfront_ring_data_request(front, &data_request, sector, req_id,
					     operation, &request_open);
	if ((ret == 0) && (operation == BLKIF_OP_READ)) {
		copy_from_data_request(&data_request, read_data);
	}

	/* A published request may still let blkback access the data grant. */
	if (request_open) {
		front->has_deferred_data = true;
		front->failed = true;
	}
out_unlock:
	k_mutex_unlock(&front->request_lock);
	return ret;
}

int xen_blkfront_queue_read(struct xen_blkfront *front, uint64_t sector, void *data, size_t len)
{
	return xen_blkfront_queue_rw(front, sector, NULL, data, len, BLKIF_OP_READ);
}

int xen_blkfront_queue_write(struct xen_blkfront *front, uint64_t sector, const void *data,
			     size_t len)
{
	return xen_blkfront_queue_rw(front, sector, data, NULL, len, BLKIF_OP_WRITE);
}

int xen_blkfront_queue_flush(struct xen_blkfront *front)
{
	bool request_open = false;
	uint64_t req_id;
	int ret;

	k_mutex_lock(&front->request_lock, K_FOREVER);

	if (front->failed) {
		ret = -EIO;
		goto out_unlock;
	}

	req_id = front->next_req_id++;
	ret = xen_blkfront_ring_request(front, req_id, BLKIF_OP_FLUSH_DISKCACHE, &request_open);
	if (request_open) {
		front->failed = true;
	}

out_unlock:
	k_mutex_unlock(&front->request_lock);
	return ret;
}

int xen_blkfront_queue_discard(struct xen_blkfront *front, uint64_t sector,
			       uint64_t sector_count, bool secure)
{
	bool request_open = false;
	uint64_t req_id;
	uint8_t flags = secure ? BLKIF_DISCARD_SECURE : 0U;
	int ret;

	k_mutex_lock(&front->request_lock, K_FOREVER);

	if (front->failed) {
		ret = -EIO;
		goto out_unlock;
	}

	req_id = front->next_req_id++;
	ret = xen_blkfront_ring_discard(front, sector, sector_count, flags, req_id,
					&request_open);
	if (request_open) {
		front->failed = true;
	}

out_unlock:
	k_mutex_unlock(&front->request_lock);
	return ret;
}

void xen_blkfront_queue_release_deferred(struct xen_blkfront *front)
{
	if ((front == NULL) || !front->has_deferred_data) {
		return;
	}

	front->has_deferred_data = false;
}
