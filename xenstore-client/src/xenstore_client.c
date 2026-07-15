/*
 * Copyright (c) 2025 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * XenStore client (frontend) for a Zephyr DomU. See xenstore_client.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/logging/log.h>

#include <zephyr/xen/public/xen.h>
#include <zephyr/xen/public/hvm/params.h>
#include <zephyr/xen/generic.h>
#include <zephyr/xen/hvm.h>
#include <zephyr/xen/events.h>

#include <xen/public/io/xs_wire.h>
#include <xenstore_common.h>

#include "xenstore_client.h"

LOG_MODULE_REGISTER(xenstore_client);

#define XS_REPLY_TIMEOUT K_MSEC(5000)

static struct xenstore_domain_interface *xs_intf;
static evtchn_port_t xs_evtchn;
static struct k_sem xs_event_sem;
/* Serializes connection setup so only one RX thread/ring binding is created. */
static K_MUTEX_DEFINE(xs_connect_lock);
static struct k_mutex xs_lock;
static uint32_t xs_req_id;
static bool xs_connected;

static void xs_event_cb(void *priv)
{
	ARG_UNUSED(priv);
	k_sem_give(&xs_event_sem);
}

int xs_client_connect(void)
{
	uint64_t store_pfn = 0, store_evtchn = 0;
	mm_reg_t va;
	int rc;

	k_mutex_lock(&xs_connect_lock, K_FOREVER);

	if (xs_connected) {
		rc = 0;
		goto out_unlock;
	}

	rc = hvm_get_parameter(HVM_PARAM_STORE_PFN, DOMID_SELF, &store_pfn);
	if (rc || store_pfn == 0 || !~store_pfn) {
		rc = rc ? rc : -ENODEV;
		goto out_unlock;
	}
	rc = hvm_get_parameter(HVM_PARAM_STORE_EVTCHN, DOMID_SELF, &store_evtchn);
	if (rc || store_evtchn == 0) {
		/* No event channel: this domain is not a xenstore client
		 * (e.g. it is the xenstore domain itself).
		 */
		rc = rc ? rc : -ENODEV;
		goto out_unlock;
	}

	k_sem_init(&xs_event_sem, 0, 1);
	k_mutex_init(&xs_lock);

	/* Map the guest's own xenstore ring page. */
	device_map(&va, (uintptr_t)(store_pfn << XEN_PAGE_SHIFT), XEN_PAGE_SIZE,
		   K_MEM_CACHE_NONE);
	xs_intf = (struct xenstore_domain_interface *)va;

	/* Attach a handler to the (Xen-provisioned) store event channel. */
	rc = bind_event_channel((evtchn_port_t)store_evtchn, xs_event_cb, NULL);
	if (rc < 0) {
		LOG_ERR("bind store evtchn %llu failed (%d)", store_evtchn, rc);
		goto out_unlock;
	}
	xs_evtchn = (evtchn_port_t)store_evtchn;
	unmask_event_channel(xs_evtchn);

	/*
	 * Reconnection handshake. Xen initialises a dom0less client's ring to
	 * XENSTORE_RECONNECT; the frontend resets the ring indexes and marks it
	 * CONNECTED to signal the server it is ready. (For a ring Xen already
	 * left CONNECTED there is nothing to do.)
	 */
	if (xs_intf->connection == XENSTORE_RECONNECT) {
		xs_intf->req_cons = xs_intf->req_prod = 0;
		xs_intf->rsp_cons = xs_intf->rsp_prod = 0;
		z_barrier_dmem_fence_full();
		xs_intf->connection = XENSTORE_CONNECTED;
		z_barrier_dmem_fence_full();
		notify_evtchn(xs_evtchn);
	}

	xs_connected = true;
	LOG_INF("xenstore client connected (gfn 0x%llx, evtchn %u)",
		store_pfn, xs_evtchn);
	rc = 0;

out_unlock:
	k_mutex_unlock(&xs_connect_lock);
	return rc;
}

bool xs_client_is_connected(void)
{
	bool connected;

	k_mutex_lock(&xs_connect_lock, K_FOREVER);
	connected = xs_connected;
	k_mutex_unlock(&xs_connect_lock);

	return connected;
}

/* Write @len bytes to the request ring, draining via the event channel. */
static int xs_write_all(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t off = 0;

	while (off < len) {
		int w = xenstore_ring_write(xs_intf, p + off, len - off, true);

		if (w < 0) {
			return w;
		}
		if (w == 0) {
			notify_evtchn(xs_evtchn);
			if (k_sem_take(&xs_event_sem, XS_REPLY_TIMEOUT) != 0) {
				return -ETIMEDOUT;
			}
			continue;
		}
		off += w;
	}
	notify_evtchn(xs_evtchn);
	return (int)len;
}

/* Read @len bytes from the response ring, waiting via the event channel. */
static int xs_read_all(void *buf, size_t len)
{
	uint8_t *p = buf;
	size_t off = 0;

	while (off < len) {
		int r = xenstore_ring_read(xs_intf, p ? p + off : NULL,
					   len - off, true);

		if (r < 0) {
			return r;
		}
		if (r == 0) {
			if (k_sem_take(&xs_event_sem, XS_REPLY_TIMEOUT) != 0) {
				return -ETIMEDOUT;
			}
			continue;
		}
		off += r;
		/* We freed ring space; let the server know. */
		notify_evtchn(xs_evtchn);
	}
	return (int)len;
}

/* One synchronous request/response exchange. */
static int xs_talk(uint32_t type, const void *payload, size_t payload_len,
		   char *out, size_t out_len)
{
	struct xsd_sockmsg hdr;
	char tmp[XENSTORE_PAYLOAD_MAX];
	size_t plen;
	int rc;

	if (!xs_connected) {
		return -ENOTCONN;
	}

	k_mutex_lock(&xs_lock, K_FOREVER);

	hdr.type = type;
	hdr.req_id = ++xs_req_id;
	hdr.tx_id = 0;
	hdr.len = (uint32_t)payload_len;

	rc = xs_write_all(&hdr, sizeof(hdr));
	if (rc < 0) {
		goto out_unlock;
	}
	if (payload_len) {
		rc = xs_write_all(payload, payload_len);
		if (rc < 0) {
			goto out_unlock;
		}
	}

	rc = xs_read_all(&hdr, sizeof(hdr));
	if (rc < 0) {
		goto out_unlock;
	}

	plen = hdr.len;
	if (plen > sizeof(tmp)) {
		plen = sizeof(tmp);
	}
	rc = xs_read_all(tmp, plen);
	if (rc < 0) {
		goto out_unlock;
	}

	if (hdr.type == XS_ERROR) {
		int e = xenstore_get_error(tmp, plen);

		rc = e ? -e : -EIO;
		goto out_unlock;
	}

	if (out && out_len) {
		size_t n = plen < out_len ? plen : out_len - 1;

		memcpy(out, tmp, n);
		out[n] = '\0';
	}
	rc = (int)plen;

out_unlock:
	k_mutex_unlock(&xs_lock);
	return rc;
}

int xs_client_write(const char *path, const char *value)
{
	char buf[XENSTORE_ABS_PATH_MAX + XENSTORE_PAYLOAD_MAX];
	size_t pl, vl;
	int rc;

	if (!path || !value) {
		return -EINVAL;
	}
	pl = strlen(path) + 1;
	vl = strlen(value);
	if (pl + vl > sizeof(buf)) {
		return -E2BIG;
	}

	memcpy(buf, path, pl);           /* path + NUL */
	memcpy(buf + pl, value, vl);     /* value (no trailing NUL) */

	rc = xs_talk(XS_WRITE, buf, pl + vl, NULL, 0);
	return rc < 0 ? rc : 0;
}

int xs_client_read(const char *path, char *out, size_t out_len)
{
	if (!path) {
		return -EINVAL;
	}
	return xs_talk(XS_READ, path, strlen(path) + 1, out, out_len);
}

#ifdef CONFIG_XEN_STORE_CLIENT_AUTO_CONNECT
static int xs_client_auto_connect(void)
{
	int rc = xs_client_connect();

	/* -ENODEV simply means this domain is not a xenstore client. */
	if (rc && rc != -ENODEV) {
		LOG_WRN("xenstore client auto-connect failed (%d)", rc);
	}
	return 0;
}
SYS_INIT(xs_client_auto_connect, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif /* CONFIG_XEN_STORE_CLIENT_AUTO_CONNECT */
