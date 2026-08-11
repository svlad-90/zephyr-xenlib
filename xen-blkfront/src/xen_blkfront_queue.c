/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "xen_blkfront_priv.h"

/* Serialize one read/write request, including data copy and failure latching. */
static int xen_blkfront_queue_rw(struct xen_blkfront *front, uint64_t sector,
				 const void *write_data, void *read_data, size_t len,
				 uint8_t operation)
{
	struct xen_blkfront_data_page grant_data;
	uint64_t req_id;
	bool request_open = false;
	int ret;

	k_mutex_lock(&front->request_lock, K_FOREVER);

	if (front->failed) {
		ret = -EIO;
		goto out_unlock;
	}

	ret = xen_blkfront_transport_alloc_data_page(&grant_data);
	if (ret != 0) {
		goto out_unlock;
	}

	if (operation == BLKIF_OP_WRITE) {
		memcpy(grant_data.page, write_data, len);
	}

	req_id = front->next_req_id++;
	ret = xen_blkfront_ring_request(front, grant_data.gref, sector, len, req_id, operation,
					&request_open);
	if ((ret == 0) && (operation == BLKIF_OP_READ)) {
		memcpy(read_data, grant_data.page, len);
	}

	/* A published request may still let blkback access the data grant. */
	if (request_open) {
		front->deferred_data = grant_data;
		front->has_deferred_data = true;
		front->failed = true;
	} else {
		xen_blkfront_transport_free_data_page(&grant_data);
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
	ret = xen_blkfront_ring_request(front, 0, 0, 0, req_id, BLKIF_OP_FLUSH_DISKCACHE,
					&request_open);
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

	xen_blkfront_transport_free_data_page(&front->deferred_data);
	front->has_deferred_data = false;
}
