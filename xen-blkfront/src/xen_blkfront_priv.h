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
	uint8_t *page;
	grant_ref_t gref;
};

struct xen_blkfront {
	char frontend_path[XEN_BLKFRONT_PATH_MAX];
	char backend_path[XEN_BLKFRONT_PATH_MAX];
	blkif_sring_t *sring;
	blkif_front_ring_t ring;
	int32_t ring_gref;
	int evtchn;
	struct k_sem evtchn_sem;
	struct k_mutex request_lock;
	struct xen_blkfront_data_page deferred_data;
	k_timeout_t xs_timeout;
	k_timeout_t backend_retry_delay;
	uint64_t sectors;
	uint64_t next_req_id;
	uint16_t vdev;
	uint16_t backend_domid;
	uint16_t backend_wait_attempts;
	bool failed;
	bool has_deferred_data;
	bool published;
	bool evtchn_bound;
};

int xen_blkfront_xenbus_configure(struct xen_blkfront *front,
				  const struct xen_blkfront_config *cfg);
int xen_blkfront_xenbus_wait_backend_path(struct xen_blkfront *front, char *buf, size_t len,
					  uint16_t attempts, k_timeout_t retry_delay);
int xen_blkfront_xenbus_verify_vdev(struct xen_blkfront *front, char *buf, size_t len,
				    k_timeout_t timeout);
int xen_blkfront_xenbus_publish_frontend(struct xen_blkfront *front, char *buf, size_t len,
					 k_timeout_t timeout);
int xen_blkfront_xenbus_wait_connected(struct xen_blkfront *front, char *buf, size_t len,
				       uint16_t attempts, k_timeout_t retry_delay);
int xen_blkfront_xenbus_read_capacity(struct xen_blkfront *front, char *buf, size_t len,
				      k_timeout_t timeout);
int xen_blkfront_xenbus_close(struct xen_blkfront *front, char *buf, size_t len);

int xen_blkfront_transport_connect(struct xen_blkfront *front);
void xen_blkfront_transport_cleanup(struct xen_blkfront *front);
int xen_blkfront_transport_alloc_data_page(struct xen_blkfront_data_page *data);
void xen_blkfront_transport_free_data_page(struct xen_blkfront_data_page *data);

void xen_blkfront_ring_init(struct xen_blkfront *front);
int xen_blkfront_ring_read(struct xen_blkfront *front, grant_ref_t data_gref, uint64_t sector,
			   size_t len, uint64_t req_id, bool *request_open);

int xen_blkfront_queue_read(struct xen_blkfront *front, uint64_t sector, void *data, size_t len);
void xen_blkfront_queue_release_deferred(struct xen_blkfront *front);

#endif /* XENLIB_XEN_BLKFRONT_PRIV_H */
