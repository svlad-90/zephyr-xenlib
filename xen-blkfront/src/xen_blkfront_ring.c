/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "xen_blkfront_priv.h"

void xen_blkfront_ring_init(struct xen_blkfront *front)
{
	XEN_FRONT_RING_INIT(&front->ring, front->sring, XEN_PAGE_SIZE);
}

/* Fill and publish a request that has no data segments, currently flush. */
static int submit_request(struct xen_blkfront *front, uint64_t req_id, uint8_t operation,
			  bool *request_open)
{
	struct blkif_request *req;
	int notify;
	int ret;

	if (operation != BLKIF_OP_FLUSH_DISKCACHE) {
		return -EINVAL;
	}

	if (RING_FULL(&front->ring)) {
		return -EAGAIN;
	}

	req = RING_GET_REQUEST(&front->ring, front->ring.req_prod_pvt);
	memset(req, 0, sizeof(*req));
	req->operation = operation;
	req->handle = front->vdev;
	req->id = req_id;

	front->ring.req_prod_pvt++;
	*request_open = true;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&front->ring, notify);
	if (notify) {
		ret = notify_evtchn((evtchn_port_t)front->evtchn);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

/* Fill and publish one ordinary blkif read/write request with data segments. */
static int submit_data_request(struct xen_blkfront *front,
			       const struct xen_blkfront_data_request *data,
			       uint64_t sector, uint64_t req_id, uint8_t operation,
			       bool *request_open)
{
	struct blkif_request *req;
	size_t remaining;
	int notify;
	int ret;

	if (((operation != BLKIF_OP_READ) && (operation != BLKIF_OP_WRITE)) ||
	    (data == NULL) || (data->pages == NULL) || (data->nr_segments == 0U) ||
	    (data->nr_segments > XEN_BLKFRONT_MAX_SEGMENTS_PER_REQUEST) ||
	    (data->nr_segments > BLKIF_MAX_SEGMENTS_PER_REQUEST) ||
	    (data->len == 0U)) {
		return -EINVAL;
	}

	if (RING_FULL(&front->ring)) {
		return -EAGAIN;
	}

	req = RING_GET_REQUEST(&front->ring, front->ring.req_prod_pvt);
	memset(req, 0, sizeof(*req));
	req->operation = operation;
	req->nr_segments = data->nr_segments;
	req->handle = front->vdev;
	req->id = req_id;
	req->sector_number = sector;

	remaining = data->len;
	for (uint8_t i = 0; i < data->nr_segments; i++) {
		size_t segment_len = MIN(remaining, (size_t)XEN_PAGE_SIZE);

		if ((segment_len == 0U) ||
		    ((segment_len % XEN_BLKFRONT_SECTOR_SIZE) != 0U)) {
			return -EINVAL;
		}

		req->seg[i].gref = data->pages[i].gref;
		req->seg[i].first_sect = 0U;
		req->seg[i].last_sect =
			(uint8_t)((segment_len / XEN_BLKFRONT_SECTOR_SIZE) - 1U);
		remaining -= segment_len;
	}

	if (remaining != 0U) {
		return -EINVAL;
	}

	front->ring.req_prod_pvt++;
	*request_open = true;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&front->ring, notify);
	if (notify) {
		ret = notify_evtchn((evtchn_port_t)front->evtchn);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

/* Fill and publish one discard request using the blkif discard layout. */
static int submit_discard_request(struct xen_blkfront *front, uint64_t sector,
				  uint64_t sector_count, uint8_t flags,
				  uint64_t req_id, bool *request_open)
{
	blkif_request_discard_t *req;
	int notify;
	int ret;

	if (RING_FULL(&front->ring)) {
		return -EAGAIN;
	}

	req = (blkif_request_discard_t *)RING_GET_REQUEST(&front->ring,
							   front->ring.req_prod_pvt);
	memset(req, 0, sizeof(*req));
	req->operation = BLKIF_OP_DISCARD;
	req->flag = flags;
	req->handle = front->vdev;
	req->id = req_id;
	req->sector_number = sector;
	req->nr_sectors = sector_count;

	front->ring.req_prod_pvt++;
	*request_open = true;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&front->ring, notify);
	if (notify) {
		ret = notify_evtchn((evtchn_port_t)front->evtchn);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

/* Poll and sleep until the matching response is copied from the shared ring. */
static int wait_response(struct xen_blkfront *front, uint64_t req_id, uint8_t operation,
			 bool *request_open)
{
	struct blkif_response rsp;
	int more;

	for (int attempt = 0; attempt < XEN_BLKFRONT_RESPONSE_ATTEMPTS; attempt++) {
		RING_FINAL_CHECK_FOR_RESPONSES(&front->ring, more);
		if (more) {
			xen_rmb();
			RING_COPY_RESPONSE(&front->ring, front->ring.rsp_cons, &rsp);
			front->ring.rsp_cons++;

			if ((rsp.id != req_id) || (rsp.operation != operation)) {
				return -EIO;
			}

			*request_open = false;
			if (rsp.status == BLKIF_RSP_OKAY) {
				return 0;
			}
			if (rsp.status == BLKIF_RSP_EOPNOTSUPP) {
				return -ENOTSUP;
			}
			return -EIO;
		}

		(void)k_sem_take(&front->evtchn_sem, XEN_BLKFRONT_RESPONSE_POLL);
	}

	return -ETIMEDOUT;
}

int xen_blkfront_ring_request(struct xen_blkfront *front, uint64_t req_id,
			      uint8_t operation, bool *request_open)
{
	int ret;

	*request_open = false;
	ret = submit_request(front, req_id, operation, request_open);
	if (ret != 0) {
		return ret;
	}

	return wait_response(front, req_id, operation, request_open);
}

int xen_blkfront_ring_data_request(struct xen_blkfront *front,
				   const struct xen_blkfront_data_request *data,
				   uint64_t sector, uint64_t req_id,
				   uint8_t operation, bool *request_open)
{
	int ret;

	*request_open = false;
	ret = submit_data_request(front, data, sector, req_id, operation, request_open);
	if (ret != 0) {
		return ret;
	}

	return wait_response(front, req_id, operation, request_open);
}

int xen_blkfront_ring_discard(struct xen_blkfront *front, uint64_t sector,
			      uint64_t sector_count, uint8_t flags, uint64_t req_id,
			      bool *request_open)
{
	int ret;

	*request_open = false;
	ret = submit_discard_request(front, sector, sector_count, flags, req_id,
				     request_open);
	if (ret != 0) {
		return ret;
	}

	return wait_response(front, req_id, BLKIF_OP_DISCARD, request_open);
}
