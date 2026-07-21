/*
 * Copyright (c) 2023 EPAM Systems
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <xenstore_common.h>

int xenstore_ring_write(struct xenstore_domain_interface *intf, const void *data, size_t len,
			bool client)
{
	size_t avail;
	void *dest;
	XENSTORE_RING_IDX cons, prod;

	cons = client ? intf->req_cons : intf->rsp_cons;
	prod = client ? intf->req_prod : intf->rsp_prod;
	z_barrier_dmem_fence_full();

	if (xenstore_check_indexes(cons, prod)) {
		return -EINVAL;
	}

	dest = (client ? intf->req : intf->rsp) + get_output_offset(cons, prod, &avail);
	if (avail < len) {
		len = avail;
	}

	memcpy(dest, data, len);
	z_barrier_dmem_fence_full();
	if (client) {
		intf->req_prod += len;
	} else {
		intf->rsp_prod += len;
	}

	return len;
}

int xenstore_ring_read(struct xenstore_domain_interface *intf, void *data, size_t len, bool client)
{
	size_t avail;
	const void *src;
	XENSTORE_RING_IDX cons, prod;

	cons = client ? intf->rsp_cons : intf->req_cons;
	prod = client ? intf->rsp_prod : intf->req_prod;
	z_barrier_dmem_fence_full();

	if (xenstore_check_indexes(cons, prod)) {
		return -EIO;
	}

	src = (client ? intf->rsp : intf->req) + xenstore_get_input_offset(cons, prod, &avail);
	if (avail < len) {
		len = avail;
	}

	if (data) {
		memcpy(data, src, len);
	}

	z_barrier_dmem_fence_full();
	if (client) {
		intf->rsp_cons += len;
	} else {
		intf->req_cons += len;
	}

	return len;
}

int xenstore_pack_strings(char *buf, size_t buf_len, const char *const *strings,
			  size_t num_strings, size_t *payload_len)
{
	size_t off = 0;

	if (!buf || !strings || !payload_len) {
		return -EINVAL;
	}

	for (size_t i = 0; i < num_strings; i++) {
		size_t str_len;

		if (!strings[i]) {
			return -EINVAL;
		}

		str_len = xenstore_str_byte_size(strings[i]);
		if (str_len > buf_len - off) {
			return -E2BIG;
		}

		memcpy(buf + off, strings[i], str_len);
		off += str_len;
	}

	*payload_len = off;

	return 0;
}

char xenstore_perm_to_char(enum xs_perm perm)
{
	switch (perm & XS_PERM_BOTH) {
	case XS_PERM_WRITE:
		return 'w';
	case XS_PERM_READ:
		return 'r';
	case XS_PERM_BOTH:
		return 'b';
	default:
		return 'n';
	}
}

int xenstore_perm_from_char(char perm_char, enum xs_perm *perm)
{
	if (!perm) {
		return -EINVAL;
	}

	switch (perm_char) {
	case 'w':
		*perm = XS_PERM_WRITE;
		break;
	case 'r':
		*perm = XS_PERM_READ;
		break;
	case 'b':
		*perm = XS_PERM_BOTH;
		break;
	case 'n':
		*perm = XS_PERM_NONE;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int xenstore_get_error(const char *errstr, size_t len)
{
	size_t i;

	if (!errstr) {
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(xsd_errors); i++) {
		const char *known = xsd_errors[i].errstring;
		size_t known_len = strlen(known);

		if (len == known_len && memcmp(errstr, known, known_len) == 0) {
			return xsd_errors[i].errnum;
		}
		if (len == known_len + 1 && memcmp(errstr, known, known_len) == 0 &&
		    errstr[known_len] == '\0') {
			return xsd_errors[i].errnum;
		}
	}

	return 0;
}
