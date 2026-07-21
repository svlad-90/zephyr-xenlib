/*
 * Copyright (c) 2025 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * XenStore client (frontend) for a Zephyr DomU. See xenstore_client.h.
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
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
#define XS_RING_POLL_SPINS 16

/*
 * One in-flight XenStore request.
 *
 * A caller thread creates this ticket before it writes a request into the
 * shared XenStore ring. The RX thread later reads replies from that same ring,
 * finds the ticket by req_id, copies the matching reply here, and wakes the
 * caller through done.
 */
struct xs_pending_req {
	sys_snode_t node;          /* Link in the global pending-request list. */
	struct k_sem done;         /* Caller waits here until RX completes this request. */
	struct xsd_sockmsg hdr;    /* Reply header copied from the XenStore ring. */
	uint32_t req_id;           /* Wire id used to match a reply to this request. */
	int rc;                    /* Local transport error, or 0 when a reply arrived. */
	size_t payload_len;        /* Number of valid reply bytes in payload. */
	char payload[XENSTORE_PAYLOAD_MAX]; /* Reply body copied by the RX thread. */
};

static struct xenstore_domain_interface *xs_intf;
static evtchn_port_t xs_evtchn;
/* RX waits on this when the response ring is empty. */
static struct k_sem xs_rx_sem;
/* TX waits on this when the request ring is full. */
static struct k_sem xs_tx_sem;
/* Counts free request tickets; callers wait here when all tickets are busy. */
static struct k_sem xs_pending_slots;
/* Serializes connection setup so only one RX thread/ring binding is created. */
static K_MUTEX_DEFINE(xs_connect_lock);
/* Serializes bytes written by callers into the shared request ring. */
static struct k_mutex xs_tx_lock;
/* Protects the pending-request list and ticket completion/removal. */
static struct k_mutex xs_pending_lock;
/* All requests that have been sent or are being sent and still need a reply. */
static sys_slist_t xs_pending_reqs;
/* Next wire request id; replies carry this id back for ticket matching. */
static uint32_t xs_req_id;
/* True after the XenStore page is mapped and the event channel is bound. */
static bool xs_connected;

static void xs_rx_thread_fn(void *p1, void *p2, void *p3);
static struct k_thread xs_rx_thread;
static K_THREAD_STACK_DEFINE(xs_rx_stack, CONFIG_XEN_STORE_CLIENT_RX_STACK_SIZE);

/* RX scratch buffer used before a reply is copied into a request ticket. */
static char xs_rx_payload[XENSTORE_PAYLOAD_MAX];
/* Protects the callback pointer and its user_data as one consistent pair. */
static K_MUTEX_DEFINE(xs_watch_cb_lock);
/* Signalled when the RX thread finishes running the current watch callback. */
static K_CONDVAR_DEFINE(xs_watch_cb_idle);
/* True while the RX thread is executing the copied application callback. */
static bool xs_watch_cb_running;
/* Single process-wide callback invoked by the RX thread for watch events. */
static xs_client_watch_cb_t xs_watch_cb;
/* Opaque application pointer passed back unchanged to xs_watch_cb. */
static void *xs_watch_cb_data;

static void xs_event_cb(void *priv)
{
	ARG_UNUSED(priv);
	/* One Xen doorbell can mean either new replies or freed request space. */
	k_sem_give(&xs_rx_sem);
	k_sem_give(&xs_tx_sem);
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

	k_sem_init(&xs_rx_sem, 0, 1);
	k_sem_init(&xs_tx_sem, 0, 1);
	k_sem_init(&xs_pending_slots, CONFIG_XEN_STORE_CLIENT_MAX_PENDING,
		   CONFIG_XEN_STORE_CLIENT_MAX_PENDING);
	k_mutex_init(&xs_tx_lock);
	k_mutex_init(&xs_pending_lock);
	sys_slist_init(&xs_pending_reqs);

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
	k_thread_create(&xs_rx_thread, xs_rx_stack, K_THREAD_STACK_SIZEOF(xs_rx_stack),
			xs_rx_thread_fn, NULL, NULL, NULL,
			CONFIG_XEN_STORE_CLIENT_RX_PRIORITY, 0, K_NO_WAIT);
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

/*
 * Copy exactly len request bytes from buf into the XenStore request ring.
 *
 * The request ring is a small shared byte queue from client to server. If it
 * is full, this helper rings the server doorbell and waits on xs_tx_sem until
 * an event suggests the server may have consumed bytes. On success it returns
 * len. On ring/write/wait failure it returns a negative errno.
 */
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
			if (k_sem_take(&xs_tx_sem, XS_REPLY_TIMEOUT) != 0) {
				return -ETIMEDOUT;
			}
			continue;
		}
		off += w;
	}
	notify_evtchn(xs_evtchn);
	return (int)len;
}

/*
 * Copy up to len currently available response bytes from the XenStore ring.
 *
 * This helper is intentionally non-blocking for "no bytes available". The RX
 * thread is the only response-ring reader, and xs_read_full() owns the waiting
 * policy. A positive return can therefore be a short read, zero means the ring
 * had no more bytes right now, and a negative value is a ring read error.
 */
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
			return (int)off;
		}
		off += r;
		notify_evtchn(xs_evtchn);
	}
	return (int)len;
}

/*
 * Wait until exactly len response bytes have been consumed.
 *
 * The RX thread uses this when it needs a complete XenStore header or payload
 * before dispatching a message. If buf is NULL, bytes are discarded; this is
 * used to drain an oversized payload so the next message starts at a clean
 * ring position. On success it returns len, otherwise a negative errno.
 */
static int xs_read_full(void *buf, size_t len)
{
	uint8_t *p = buf;
	size_t off = 0;

	while (off < len) {
		int r = xs_read_all(p ? p + off : NULL, len - off);

		if (r < 0) {
			return r;
		}
		if (r == 0) {
			notify_evtchn(xs_evtchn);
			for (int i = 0; i < XS_RING_POLL_SPINS; i++) {
				k_busy_wait(50);
				r = xs_read_all(p ? p + off : NULL, len - off);
				if (r != 0) {
					break;
				}
			}
			if (r == 0) {
				k_yield();
				continue;
			}
			if (r < 0) {
				return r;
			}
		}
		off += r;
	}
	return (int)len;
}

/*
 * Find the pending ticket for one wire request id.
 *
 * The caller must already hold xs_pending_lock because the pending list is
 * shared by caller threads and the RX thread. When prev is not NULL, it is
 * filled with the previous list node so sys_slist_remove() can unlink the
 * found ticket from Zephyr's singly linked list.
 */
static struct xs_pending_req *xs_find_pending_locked(uint32_t req_id,
						    sys_snode_t **prev)
{
	sys_snode_t *prev_node = NULL;
	struct xs_pending_req *req;

	SYS_SLIST_FOR_EACH_CONTAINER(&xs_pending_reqs, req, node) {
		if (req->req_id == req_id) {
			if (prev) {
				*prev = prev_node;
			}
			return req;
		}
		prev_node = &req->node;
	}

	return NULL;
}

/*
 * Finish one normal server reply and wake the waiting caller.
 *
 * The RX thread calls this after it has read a full XenStore reply. The reply
 * still lives in the RX scratch buffer, so this helper finds the caller's
 * pending ticket by req_id, copies the reply into that ticket, returns the
 * pending slot, and gives req->done so the caller can continue.
 */
static void xs_complete_request(struct xsd_sockmsg *hdr, const char *payload,
				size_t payload_len)
{
	sys_snode_t *prev = NULL;
	struct xs_pending_req *req;

	k_mutex_lock(&xs_pending_lock, K_FOREVER);
	req = xs_find_pending_locked(hdr->req_id, &prev);
	if (!req) {
		k_mutex_unlock(&xs_pending_lock);
		LOG_WRN("unexpected xenstore reply req_id=%u type=%u",
			hdr->req_id, hdr->type);
		return;
	}

	sys_slist_remove(&xs_pending_reqs, prev, &req->node);
	k_sem_give(&xs_pending_slots);
	req->hdr = *hdr;
	req->payload_len = payload_len;
	memcpy(req->payload, payload, payload_len);
	req->rc = 0;
	k_sem_give(&req->done);
	k_mutex_unlock(&xs_pending_lock);
}

/*
 * Finish one pending request with a local client-side error.
 *
 * This is used when the client cannot finish the transport work for a request:
 * for example, a ring read/write fails or the server advertises a payload that
 * does not fit in the RX buffer. There may already be a caller sleeping on the
 * request ticket, so the helper records rc and wakes that caller the same way
 * a normal reply would.
 */
static void xs_complete_request_error(uint32_t req_id, int rc)
{
	sys_snode_t *prev = NULL;
	struct xs_pending_req *req;

	k_mutex_lock(&xs_pending_lock, K_FOREVER);
	req = xs_find_pending_locked(req_id, &prev);
	if (req) {
		sys_slist_remove(&xs_pending_reqs, prev, &req->node);
		k_sem_give(&xs_pending_slots);
		req->rc = rc;
		k_sem_give(&req->done);
	}
	k_mutex_unlock(&xs_pending_lock);
}

/*
 * Decode and dispatch one asynchronous XS_WATCH_EVENT message.
 *
 * Watch events are not replies to a caller waiting in xs_talk(); they are
 * server notifications that arrive on the same response ring. The payload is
 * two NUL-terminated strings packed into one byte buffer: path first, then the
 * caller-supplied watch token. The RX thread validates both strings stay inside
 * @payload_len, then calls the single registered application callback.
 */
static void xs_handle_watch_event(const char *payload, size_t payload_len)
{
	size_t path_len;
	size_t token_len;
	const char *token;
	xs_client_watch_cb_t cb;
	void *cb_data;

	if (payload_len == 0) {
		return;
	}

	path_len = strnlen(payload, payload_len) + 1;
	if (path_len >= payload_len) {
		LOG_WRN("malformed xenstore watch event");
		return;
	}

	token = payload + path_len;
	token_len = strnlen(token, payload_len - path_len);
	if (token_len == payload_len - path_len) {
		LOG_WRN("malformed xenstore watch event token");
		return;
	}

	k_mutex_lock(&xs_watch_cb_lock, K_FOREVER);
	cb = xs_watch_cb;
	cb_data = xs_watch_cb_data;
	if (cb) {
		xs_watch_cb_running = true;
	}
	k_mutex_unlock(&xs_watch_cb_lock);

	if (cb) {
		cb(payload, token, cb_data);
		k_mutex_lock(&xs_watch_cb_lock, K_FOREVER);
		xs_watch_cb_running = false;
		k_condvar_signal(&xs_watch_cb_idle);
		k_mutex_unlock(&xs_watch_cb_lock);
	}
}

/*
 * Single reader for the XenStore response ring.
 *
 * The response ring is one shared incoming byte stream. Application callers do
 * not read it directly; otherwise one caller could consume another caller's
 * reply. This thread reads each message, handles watch events immediately, and
 * routes normal replies to the matching pending request by req_id.
 */
static void xs_rx_thread_fn(void *p1, void *p2, void *p3)
{
	struct xsd_sockmsg hdr;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int rc;

		rc = xs_read_full(&hdr, sizeof(hdr));
		if (rc < 0) {
			LOG_ERR("xenstore response header read failed (%d)", rc);
			continue;
		}

		if (hdr.len > sizeof(xs_rx_payload)) {
			(void)xs_read_full(NULL, hdr.len);
			xs_complete_request_error(hdr.req_id, -E2BIG);
			continue;
		}

		rc = xs_read_full(xs_rx_payload, hdr.len);
		if (rc < 0) {
			xs_complete_request_error(hdr.req_id, rc);
			continue;
		}

		if (hdr.type == XS_WATCH_EVENT) {
			xs_handle_watch_event(xs_rx_payload, hdr.len);
			k_yield();
			continue;
		}

		xs_complete_request(&hdr, xs_rx_payload, hdr.len);
		k_yield();
	}
}

/*
 * Send one XenStore request and wait for the matching reply.
 *
 * The caller takes a pending slot, creates a request ticket, registers it by a
 * fresh req_id, and writes the request header/payload under xs_tx_lock so bytes
 * from concurrent callers cannot interleave. After the write, the caller does
 * not read the response ring; it sleeps on its own ticket until the RX thread
 * completes it or XS_REPLY_TIMEOUT expires.
 */
static int xs_talk(uint32_t type, const void *payload, size_t payload_len,
		   char *out, size_t out_len)
{
	struct xs_pending_req *req;
	struct xsd_sockmsg hdr;
	size_t plen;
	int rc;

	if (!xs_connected) {
		return -ENOTCONN;
	}
	if (payload_len > XENSTORE_PAYLOAD_MAX) {
		return -E2BIG;
	}

	k_sem_take(&xs_pending_slots, K_FOREVER);
	req = k_malloc(sizeof(*req));
	if (!req) {
		k_sem_give(&xs_pending_slots);
		return -ENOMEM;
	}
	k_sem_init(&req->done, 0, 1);
	req->rc = -EIO;
	req->payload_len = 0;

	hdr.type = type;
	hdr.tx_id = 0;
	hdr.len = (uint32_t)payload_len;

	k_mutex_lock(&xs_pending_lock, K_FOREVER);
	hdr.req_id = ++xs_req_id;
	req->req_id = hdr.req_id;
	sys_slist_append(&xs_pending_reqs, &req->node);
	k_mutex_unlock(&xs_pending_lock);

	k_mutex_lock(&xs_tx_lock, K_FOREVER);
	rc = xs_write_all(&hdr, sizeof(hdr));
	if (rc < 0) {
		k_mutex_unlock(&xs_tx_lock);
		xs_complete_request_error(req->req_id, rc);
		goto wait_done;
	}
	if (payload_len) {
		rc = xs_write_all(payload, payload_len);
		if (rc < 0) {
			k_mutex_unlock(&xs_tx_lock);
			xs_complete_request_error(req->req_id, rc);
			goto wait_done;
		}
	}
	notify_evtchn(xs_evtchn);
	k_mutex_unlock(&xs_tx_lock);

wait_done:
	if (k_sem_take(&req->done, XS_REPLY_TIMEOUT) != 0) {
		sys_snode_t *prev = NULL;

		k_mutex_lock(&xs_pending_lock, K_FOREVER);
		if (xs_find_pending_locked(req->req_id, &prev)) {
			sys_slist_remove(&xs_pending_reqs, prev, &req->node);
			k_sem_give(&xs_pending_slots);
		}
		k_mutex_unlock(&xs_pending_lock);
		k_free(req);
		return -ETIMEDOUT;
	}

	if (req->rc < 0) {
		rc = req->rc;
		k_free(req);
		return rc;
	}

	plen = req->payload_len;
	if (req->hdr.type == XS_ERROR) {
		int e = xenstore_get_error(req->payload, plen);

		rc = e ? -e : -EIO;
		k_free(req);
		return rc;
	}

	if (out && out_len) {
		size_t n = plen < out_len ? plen : out_len;

		memcpy(out, req->payload, n);
	}

	k_free(req);
	return (int)plen;
}

/*
 * Send a request whose reply must be exactly one NUL-terminated string.
 *
 * xs_talk() is the raw transport helper: it copies reply bytes, but it does
 * not know whether those bytes are a C string, a list of strings, or another
 * XenStore payload format. This wrapper is for the simpler string-reply APIs.
 * It reads into a full-size scratch buffer first, checks that the server reply
 * contains a terminating '\0', then copies the complete string into @out.
 *
 * Returns the copied string size including the terminating '\0', or a negative
 * errno. -ENOSPC means the server did return a valid string, but @out is too
 * small to hold that whole string plus its terminator.
 */
static int xs_talk_string(uint32_t type, const void *payload, size_t payload_len,
			  char *out, size_t out_len)
{
	char *reply;
	size_t str_len;
	int rc;

	if (!out || out_len == 0) {
		return -EINVAL;
	}

	reply = k_malloc(XENSTORE_PAYLOAD_MAX);
	if (!reply) {
		return -ENOMEM;
	}

	rc = xs_talk(type, payload, payload_len, reply, XENSTORE_PAYLOAD_MAX);
	if (rc < 0) {
		k_free(reply);
		return rc;
	}

	str_len = strnlen(reply, (size_t)rc);
	if (str_len == (size_t)rc) {
		k_free(reply);
		return -EINVAL;
	}
	if (str_len + 1 > out_len) {
		k_free(reply);
		return -ENOSPC;
	}

	memcpy(out, reply, str_len + 1);
	k_free(reply);
	return (int)(str_len + 1);
}

int xs_client_write(const char *path, const char *value)
{
	char *buf;
	size_t pl, vl;
	int rc;

	if (!path || !value) {
		return -EINVAL;
	}
	pl = strlen(path) + 1;
	vl = strlen(value);
	if (pl + vl > XENSTORE_PAYLOAD_MAX) {
		return -E2BIG;
	}

	buf = k_malloc(XENSTORE_PAYLOAD_MAX);
	if (!buf) {
		return -ENOMEM;
	}

	memcpy(buf, path, pl);           /* path + NUL */
	memcpy(buf + pl, value, vl);     /* value (no trailing NUL) */

	rc = xs_talk(XS_WRITE, buf, pl + vl, NULL, 0);
	k_free(buf);
	return rc < 0 ? rc : 0;
}

int xs_client_read(const char *path, char *out, size_t out_len)
{
	int rc;

	if (!path) {
		return -EINVAL;
	}
	if (!out || out_len == 0) {
		return -EINVAL;
	}

	rc = xs_talk(XS_READ, path, strlen(path) + 1, out, out_len);
	if (rc < 0) {
		return rc;
	}
	if ((size_t)rc >= out_len) {
		out[out_len - 1] = '\0';
		return -ENOSPC;
	}

	out[rc] = '\0';
	return rc;
}

int xs_client_directory(const char *path, char *out, size_t out_len)
{
	int rc;

	if (!path) {
		return -EINVAL;
	}
	if (!out || out_len == 0) {
		return -EINVAL;
	}

	rc = xs_talk(XS_DIRECTORY, path, strlen(path) + 1, out, out_len);
	if (rc < 0) {
		return rc;
	}
	if ((size_t)rc > out_len) {
		return -ENOSPC;
	}

	return rc;
}

int xs_client_mkdir(const char *path)
{
	int rc;

	if (!path) {
		return -EINVAL;
	}

	rc = xs_talk(XS_MKDIR, path, strlen(path) + 1, NULL, 0);
	return rc < 0 ? rc : 0;
}

int xs_client_rm(const char *path)
{
	int rc;

	if (!path) {
		return -EINVAL;
	}

	rc = xs_talk(XS_RM, path, strlen(path) + 1, NULL, 0);
	return rc < 0 ? rc : 0;
}

/*
 * Decode one XenStore permission string into the public permission struct.
 *
 * The server sends permissions as text entries such as "b1" or "r42": the
 * first character is the access mode, and the decimal suffix is the domain id
 * the mode applies to. This helper parses one such entry after the caller has
 * split the raw XS_GET_PERMS reply into individual NUL-terminated strings.
 */
static int xs_parse_perm_string(const char *str, struct xenstore_perm *perm)
{
	char *endptr;
	int rc;

	if (!str || !perm) {
		return -EINVAL;
	}

	rc = xenstore_perm_from_char(str[0], &perm->perms);
	if (rc) {
		return rc;
	}

	perm->domid = strtoul(str + 1, &endptr, 10);
	if (*endptr != '\0') {
		return -EINVAL;
	}

	return 0;
}

int xs_client_get_perms(const char *path, struct xenstore_perm *perms,
			size_t *num_perms)
{
	char *payload;
	size_t off = 0, count = 0, capacity;
	int rc;

	if (!path || !num_perms) {
		return -EINVAL;
	}

	capacity = *num_perms;
	payload = k_malloc(XENSTORE_PAYLOAD_MAX);
	if (!payload) {
		return -ENOMEM;
	}

	rc = xs_talk(XS_GET_PERMS, path, strlen(path) + 1, payload,
		     XENSTORE_PAYLOAD_MAX);
	if (rc < 0) {
		k_free(payload);
		return rc;
	}

	while (off < (size_t)rc) {
		size_t remaining = (size_t)rc - off;
		size_t len = strnlen(payload + off, remaining);

		if (len == remaining) {
			k_free(payload);
			return -EINVAL;
		}
		if (perms && count < capacity) {
			int parse_rc = xs_parse_perm_string(payload + off, &perms[count]);

			if (parse_rc) {
				k_free(payload);
				return parse_rc;
			}
		}
		count++;
		off += len + 1;
	}

	if (perms && capacity < count) {
		*num_perms = count;
		k_free(payload);
		return -ENOSPC;
	}

	*num_perms = count;
	k_free(payload);
	return 0;
}

/*
 * Append one permission entry to an XS_SET_PERMS request payload.
 *
 * XenStore sends permissions as compact strings: access character followed by
 * domain id, for example "b1". @off points to the next free byte in @buf.
 * snprintk() writes the entry plus its terminating '\0'; on success @off moves
 * past that terminator so the next permission string can be appended directly
 * after it.
 */
static int xs_append_perm(char *buf, size_t buf_len, size_t *off,
			  const struct xenstore_perm *perm)
{
	int len;

	if (!buf || !off || !perm || *off >= buf_len) {
		return -EINVAL;
	}

	len = snprintk(buf + *off, buf_len - *off, "%c%u",
		       xenstore_perm_to_char(perm->perms), perm->domid);
	if (len < 0 || (size_t)len >= buf_len - *off) {
		return -E2BIG;
	}

	*off += (size_t)len + 1;

	return 0;
}

int xs_client_set_perms(const char *path, const struct xenstore_perm *perms,
			size_t num_perms)
{
	char *payload;
	size_t off;
	int rc;

	if (!path || !perms || num_perms == 0) {
		return -EINVAL;
	}

	off = strlen(path) + 1;
	if (off >= XENSTORE_PAYLOAD_MAX) {
		return -E2BIG;
	}
	payload = k_malloc(XENSTORE_PAYLOAD_MAX);
	if (!payload) {
		return -ENOMEM;
	}
	memcpy(payload, path, off);

	for (size_t i = 0; i < num_perms; i++) {
		rc = xs_append_perm(payload, XENSTORE_PAYLOAD_MAX, &off, &perms[i]);
		if (rc) {
			k_free(payload);
			return rc;
		}
	}

	rc = xs_talk(XS_SET_PERMS, payload, off, NULL, 0);
	k_free(payload);
	return rc < 0 ? rc : 0;
}

void xs_client_set_watch_callback(xs_client_watch_cb_t cb, void *user_data)
{
	k_mutex_lock(&xs_watch_cb_lock, K_FOREVER);
	while (xs_watch_cb_running && k_current_get() != &xs_rx_thread) {
		k_condvar_wait(&xs_watch_cb_idle, &xs_watch_cb_lock, K_FOREVER);
	}
	xs_watch_cb = cb;
	xs_watch_cb_data = user_data;
	k_mutex_unlock(&xs_watch_cb_lock);
}

/*
 * Send XS_WATCH or XS_UNWATCH.
 *
 * Both requests use the same wire payload shape: the watched path followed by
 * the caller-chosen token, packed as two adjacent NUL-terminated strings
 * (`path\0token\0`). @type selects whether the server should add or remove
 * that watch entry.
 */
static int xs_watch_op(uint32_t type, const char *path, const char *token)
{
	char *payload;
	const char *strings[] = { path, token };
	size_t payload_len;
	int rc;

	if (!path || !token) {
		return -EINVAL;
	}

	payload = k_malloc(XENSTORE_PAYLOAD_MAX);
	if (!payload) {
		return -ENOMEM;
	}

	rc = xenstore_pack_strings(payload, XENSTORE_PAYLOAD_MAX, strings,
				   ARRAY_SIZE(strings), &payload_len);
	if (rc) {
		k_free(payload);
		return rc;
	}

	rc = xs_talk(type, payload, payload_len, NULL, 0);
	k_free(payload);
	return rc < 0 ? rc : 0;
}

int xs_client_watch(const char *path, const char *token)
{
	return xs_watch_op(XS_WATCH, path, token);
}

int xs_client_unwatch(const char *path, const char *token)
{
	return xs_watch_op(XS_UNWATCH, path, token);
}

int xs_client_reset_watches(void)
{
	int rc = xs_talk(XS_RESET_WATCHES, NULL, 0, NULL, 0);

	return rc < 0 ? rc : 0;
}

int xs_client_get_domain_path(domid_t domid, char *out, size_t out_len)
{
	char payload[16];
	int len;

	len = snprintk(payload, sizeof(payload), "%u", domid);
	if (len < 0 || (size_t)len >= sizeof(payload)) {
		return -E2BIG;
	}

	return xs_talk_string(XS_GET_DOMAIN_PATH, payload, (size_t)len, out,
			      out_len);
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
