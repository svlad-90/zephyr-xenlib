/*
 * Copyright (c) 2023 EPAM Systems
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <xenstore_common.h>

#include <stdlib.h>

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
	if (data) {
		memcpy(data, src, len);
	}
	}

	z_barrier_dmem_fence_full();
	if (client) {
		intf->rsp_cons += len;
	} else {
		intf->req_cons += len;
	}

	return len;
}

int xenstore_perm_to_wire(enum xs_perm perm, char *wire)
{
	if (!wire) {
		return -EINVAL;
	}

	switch (perm) {
	case XS_PERM_NONE:
		*wire = 'n';
		return 0;
	case XS_PERM_READ:
		*wire = 'r';
		return 0;
	case XS_PERM_WRITE:
		*wire = 'w';
		return 0;
	case XS_PERM_BOTH:
		*wire = 'b';
		return 0;
	default:
		return -EINVAL;
	}
}

int xenstore_perm_from_wire(char wire, enum xs_perm *perm)
{
	if (!perm) {
		return -EINVAL;
	}

	switch (wire) {
	case 'n':
		*perm = XS_PERM_NONE;
		return 0;
	case 'r':
		*perm = XS_PERM_READ;
		return 0;
	case 'w':
		*perm = XS_PERM_WRITE;
		return 0;
	case 'b':
		*perm = XS_PERM_BOTH;
		return 0;
	default:
		return -EINVAL;
	}
}

int xenstore_perm_parse_wire(const char *raw, size_t raw_len, struct xs_perm_entry *perms,
			     size_t perms_num, size_t *parsed_num)
{
	size_t off = 0;
	size_t copied = 0;

	if (!raw || !perms || !parsed_num) {
		return -EINVAL;
	}

	*parsed_num = 0;
	while (off < raw_len) {
		const char *entry = raw + off;
		const char *nul = memchr(entry, '\0', raw_len - off);
		char *endptr;
		unsigned long domid;
		int ret;

		if (!nul || (nul - entry) < 2) {
			return -EINVAL;
		}
		if (copied == perms_num) {
			return -E2BIG;
		}

		ret = xenstore_perm_from_wire(entry[0], &perms[copied].perm);
		if (ret < 0) {
			return ret;
		}

		domid = strtoul(entry + 1, &endptr, 10);
		if (endptr != nul || domid > DOMID_MASK) {
			return -EINVAL;
		}

		perms[copied].domid = (domid_t)domid;
		copied++;
		off += (size_t)(nul - entry) + 1;
	}

	*parsed_num = copied;
	return 0;
}

int xenstore_perm_format_wire(char *buf, size_t len, const struct xs_perm_entry *perm)
{
	char wire;
	int ret;

	if (!buf || !perm) {
		return -EINVAL;
	}

	ret = xenstore_perm_to_wire(perm->perm, &wire);
	if (ret < 0) {
		return ret;
	}

	return snprintf(buf, len, "%c%u", wire, perm->domid);
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
