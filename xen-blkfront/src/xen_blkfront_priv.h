/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XENLIB_XEN_BLKFRONT_PRIV_H
#define XENLIB_XEN_BLKFRONT_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/xen/events.h>
#include <zephyr/xen/generic.h>
#include <zephyr/xen/gnttab.h>

#include <xen_blkfront.h>

#ifndef mb
#define mb() barrier_dmem_fence_full()
#endif
#ifndef rmb
#define rmb() barrier_dmem_fence_full()
#endif
#ifndef wmb
#define wmb() barrier_dmem_fence_full()
#endif
#ifndef xen_mb
#define xen_mb() barrier_dmem_fence_full()
#endif
#ifndef xen_rmb
#define xen_rmb() barrier_dmem_fence_full()
#endif
#ifndef xen_wmb
#define xen_wmb() barrier_dmem_fence_full()
#endif

#include <xen/public/io/blkif.h>

#define XEN_BLKFRONT_PATH_MAX 128
#define XEN_BLKFRONT_REQ_ID 0x62666e7400000001ULL
#define XEN_BLKFRONT_RESPONSE_ATTEMPTS 200
#define XEN_BLKFRONT_RESPONSE_POLL K_MSEC(10)
#define XEN_BLKFRONT_BACKEND_WAIT_ATTEMPTS_DEFAULT 60U
#define XEN_BLKFRONT_BACKEND_RETRY_DELAY_DEFAULT K_MSEC(100)

struct xen_blkfront_data_page {
	/* Guest page shared with blkback for one data segment. */
	uint8_t *page;
	/* Grant-table reference that gives the backend temporary page access. */
	grant_ref_t gref;
};

struct xen_blkfront {
	/* XenStore frontend node owned by this guest, for example device/vbd/51712. */
	char frontend_path[XEN_BLKFRONT_PATH_MAX];
	/* XenStore backend node published by the service domain. */
	char backend_path[XEN_BLKFRONT_PATH_MAX];
	/* Shared ring page mapped in the guest and granted to blkback. */
	blkif_sring_t *sring;
	/* Xen ring helper state that tracks producer and consumer indexes. */
	blkif_front_ring_t ring;
	/* Grant reference for sring; -1 means no ring grant is active. */
	int32_t ring_gref;
	/* Event-channel port used for backend notifications; -1 means closed. */
	int evtchn;
	/* Raised by the event-channel callback when a response may be ready. */
	struct k_sem evtchn_sem;
	/* Serializes all requests that use the shared ring and data pool. */
	struct k_mutex request_lock;
	struct xen_blkfront_data_page deferred_data;
	/* Timeout used for individual XenStore read and write operations. */
	k_timeout_t xs_timeout;
	/* Delay between backend state/path polling attempts. */
	k_timeout_t backend_retry_delay;
	/* Backend geometry and feature flags discovered from XenStore. */
	struct xen_blkfront_info info;
	/* Monotonic request id source used to match ring responses. */
	uint64_t next_req_id;
	/* Xen virtual-device id expected in frontend/backend protocol nodes. */
	uint16_t vdev;
	/* Domain id of the blkback provider used for grants and event channel. */
	uint16_t backend_domid;
	/* Number of retries for backend path and state transitions. */
	uint16_t backend_wait_attempts;
	/* Sticky failure after ambiguous request completion or protocol mismatch. */
	bool failed;
	/* A published data request may still have backend access to data_pool. */
	bool has_deferred_data;
	/* True after frontend XenStore nodes have been published. */
	bool published;
	/* True after evtchn has a registered Zephyr callback. */
	bool evtchn_bound;
};

/*
 * XenBus configuration and lifecycle helpers.
 * These own XenStore paths, state publication, backend polling, and feature
 * discovery.  They do not allocate rings, event channels, or data pages.
 */

/* Copy immutable frontend/backend paths and ids from the public config. */
int xen_blkfront_xenbus_configure(struct xen_blkfront *front,
				  const struct xen_blkfront_config *cfg);
/* Wait until the service domain writes the backend path under the frontend node. */
int xen_blkfront_xenbus_wait_backend_path(struct xen_blkfront *front, char *buf, size_t len,
					  uint16_t attempts, k_timeout_t retry_delay);
/* Check that XenStore virtual-device matches the vdev requested by the caller. */
int xen_blkfront_xenbus_verify_vdev(struct xen_blkfront *front, char *buf, size_t len,
				    k_timeout_t timeout);
/* Publish ring/event-channel/protocol nodes and move frontend to Initialised. */
int xen_blkfront_xenbus_publish_frontend(struct xen_blkfront *front, char *buf, size_t len,
					 k_timeout_t timeout);
/* Poll the backend XenBus state until blkback reports Connected. */
int xen_blkfront_xenbus_wait_connected(struct xen_blkfront *front, char *buf, size_t len,
				       uint16_t attempts, k_timeout_t retry_delay);
/* Read backend geometry and optional feature nodes into front->info. */
int xen_blkfront_xenbus_discover(struct xen_blkfront *front, char *buf, size_t len,
				 k_timeout_t timeout);
/* Drive the frontend/backend XenBus close handshake for a published frontend. */
int xen_blkfront_xenbus_close(struct xen_blkfront *front, char *buf, size_t len);

/*
 * Transport helpers.
 * These allocate guest resources that are visible to Xen: the shared ring page,
 * event channel, and grant-backed data pages.
 */

/* Allocate and initialize the ring page plus backend event channel. */
int xen_blkfront_transport_connect(struct xen_blkfront *front);
/* Tear down event-channel and grant-table resources owned by the frontend. */
void xen_blkfront_transport_cleanup(struct xen_blkfront *front);
/* Allocate one page and grant backend access for a data segment. */
int xen_blkfront_transport_alloc_data_page(struct xen_blkfront_data_page *data);
/* End backend access and free one grant-backed data segment page. */
void xen_blkfront_transport_free_data_page(struct xen_blkfront_data_page *data);

/*
 * Ring helpers.
 * These write protocol requests into the shared ring, notify blkback, and wait
 * for the matching response.
 */

/* Initialize Xen ring indexes around an already allocated shared ring page. */
void xen_blkfront_ring_init(struct xen_blkfront *front);
int xen_blkfront_ring_request(struct xen_blkfront *front, grant_ref_t data_gref, uint64_t sector,
			      size_t len, uint64_t req_id, uint8_t operation,
			      bool *request_open);
/* Submit a discard request over a sector range and wait for completion. */
int xen_blkfront_ring_discard(struct xen_blkfront *front, uint64_t sector,
			      uint64_t sector_count, uint8_t flags, uint64_t req_id,
			      bool *request_open);

/*
 * Queue helpers.
 * These serialize public operations, manage data-page ownership, assign request
 * ids, and mark the frontend failed if a published request is ambiguous.
 */

/* Submit a read request and copy completed data from the grant pool. */
int xen_blkfront_queue_read(struct xen_blkfront *front, uint64_t sector, void *data, size_t len);
/* Copy caller data into the grant pool, submit a write, and wait for response. */
int xen_blkfront_queue_write(struct xen_blkfront *front, uint64_t sector, const void *data,
			     size_t len);
/* Submit a backend cache-flush request. */
int xen_blkfront_queue_flush(struct xen_blkfront *front);
/* Submit a backend discard request for an already validated sector range. */
int xen_blkfront_queue_discard(struct xen_blkfront *front, uint64_t sector,
			       uint64_t sector_count, bool secure);
/* Clear deferred data ownership after the frontend has been closed. */
void xen_blkfront_queue_release_deferred(struct xen_blkfront *front);

#endif /* XENLIB_XEN_BLKFRONT_PRIV_H */
