/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "xen_blkfront_priv.h"

/* Wake the synchronous ring waiter when the backend signals the event channel. */
static void evtchn_cb(void *param)
{
	struct xen_blkfront *front = param;

	k_sem_give(&front->evtchn_sem);
}

/* Allocate the shared ring page and grant it to the backend domain. */
static int alloc_ring_page(struct xen_blkfront *front)
{
	int32_t gref;

	gref = gnttab_alloc_and_grant((void **)&front->sring, false);
	if (gref < 0) {
		return gref;
	}

	front->ring_gref = gref;
	xen_blkfront_ring_init(front);
	return 0;
}

/* Allocate and bind the event channel used for backend notifications. */
static int alloc_evtchn(struct xen_blkfront *front)
{
	int evtchn;
	int ret;

	evtchn = alloc_unbound_event_channel((domid_t)front->backend_domid);
	if (evtchn < 0) {
		return evtchn;
	}

	front->evtchn = evtchn;
	k_sem_init(&front->evtchn_sem, 0, 1);

	ret = bind_event_channel((evtchn_port_t)front->evtchn, evtchn_cb, front);
	if (ret != 0) {
		return ret;
	}
	front->evtchn_bound = true;

	return unmask_event_channel((evtchn_port_t)front->evtchn);
}

int xen_blkfront_transport_connect(struct xen_blkfront *front)
{
	int ret;

	ret = alloc_ring_page(front);
	if (ret != 0) {
		return ret;
	}

	return alloc_evtchn(front);
}

void xen_blkfront_transport_cleanup(struct xen_blkfront *front)
{
	if (front == NULL) {
		return;
	}

	if (front->evtchn_bound) {
		(void)unbind_event_channel((evtchn_port_t)front->evtchn);
		front->evtchn_bound = false;
	}

	if (front->evtchn >= 0) {
		(void)evtchn_close((evtchn_port_t)front->evtchn);
		front->evtchn = -1;
	}

	if (front->ring_gref >= 0) {
		(void)gnttab_end_access((grant_ref_t)front->ring_gref);
		front->ring_gref = -1;
	}

	if (front->sring != NULL) {
		k_free(front->sring);
		front->sring = NULL;
	}
}

int xen_blkfront_transport_alloc_data_page(struct xen_blkfront_data_page *data)
{
	int32_t gref;

	if (data == NULL) {
		return -EINVAL;
	}

	data->page = NULL;
	data->gref = 0;

	gref = gnttab_alloc_and_grant((void **)&data->page, false);
	if (gref < 0) {
		return gref;
	}

	data->gref = (grant_ref_t)gref;
	memset(data->page, 0, XEN_PAGE_SIZE);
	return 0;
}

void xen_blkfront_transport_free_data_page(struct xen_blkfront_data_page *data)
{
	if ((data == NULL) || (data->page == NULL)) {
		return;
	}

	(void)gnttab_end_access(data->gref);
	k_free(data->page);
	data->page = NULL;
	data->gref = 0;
}
