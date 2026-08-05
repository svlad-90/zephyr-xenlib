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

static int submit_request(struct xen_blkfront *front, grant_ref_t data_gref, uint64_t sector,
			  size_t len, uint64_t req_id, uint8_t operation, bool *request_open)
{
	struct blkif_request *req;
	bool data_request = (operation == BLKIF_OP_READ) || (operation == BLKIF_OP_WRITE);
	int notify;
	int ret;

	if (!data_request && (operation != BLKIF_OP_FLUSH_DISKCACHE)) {
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

	if (data_request) {
		req->nr_segments = 1;
		req->sector_number = sector;
		req->seg[0].gref = data_gref;
		req->seg[0].first_sect = 0;
		req->seg[0].last_sect = (len / XEN_BLKFRONT_SECTOR_SIZE) - 1U;
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

int xen_blkfront_ring_request(struct xen_blkfront *front, grant_ref_t data_gref, uint64_t sector,
			      size_t len, uint64_t req_id, uint8_t operation,
			      bool *request_open)
{
	int ret;

	*request_open = false;
	ret = submit_request(front, data_gref, sector, len, req_id, operation, request_open);
	if (ret != 0) {
		return ret;
	}

	return wait_response(front, req_id, operation, request_open);
}
