/*
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XENLIB_XENSTORE_CLIENT_INTERNAL_H
#define XENLIB_XENSTORE_CLIENT_INTERNAL_H

#include <xenstore_client.h>

#include <zephyr/sys/slist.h>

struct xs_watcher {
	sys_snode_t node;
	xs_watch_cb cb;
	void *param;
	struct k_sem callback_idle;
	uint32_t dispatch_gen;
	bool registered;
	bool callback_running;
	bool release_after_callback;
};

#endif /* XENLIB_XENSTORE_CLIENT_INTERNAL_H */
