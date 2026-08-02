/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "xen_blkfront_priv.h"

int xen_blkfront_queue_read(struct xen_blkfront *front, uint64_t sector, void *data, size_t len)
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

	req_id = front->next_req_id++;
	ret = xen_blkfront_ring_read(front, grant_data.gref, sector, len, req_id, &request_open);
	if (ret == 0) {
		memcpy(data, grant_data.page, len);
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

void xen_blkfront_queue_release_deferred(struct xen_blkfront *front)
{
	if ((front == NULL) || !front->has_deferred_data) {
		return;
	}

	xen_blkfront_transport_free_data_page(&front->deferred_data);
	front->has_deferred_data = false;
}
