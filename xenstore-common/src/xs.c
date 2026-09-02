/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <xs.h>

static k_timeout_t default_timeout = K_FOREVER;

void xs_set_default_timeout(k_timeout_t tout)
{
	default_timeout = tout;
}

static k_timeout_t xs_default_timeout(void)
{
	return default_timeout;
}

ssize_t xs_read(const char *path, char *buf, size_t len, uint32_t tx_id)
{
	return xs_read_timeout(path, buf, len, tx_id, xs_default_timeout());
}

int xs_write(const char *path, const char *value, uint32_t tx_id)
{
	return xs_write_timeout(path, value, tx_id, xs_default_timeout());
}

int xs_rm(const char *path, uint32_t tx_id)
{
	return xs_rm_timeout(path, tx_id, xs_default_timeout());
}
