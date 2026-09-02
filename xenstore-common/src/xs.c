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
