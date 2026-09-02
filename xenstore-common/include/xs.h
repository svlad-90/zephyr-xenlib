/*
 * Copyright (c) 2026 EPAM Systems
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XENLIB_XS_H
#define XENLIB_XS_H

#include <stdint.h>
#include <sys/types.h>

#include <zephyr/kernel.h>

#include <xenstore_common.h>

/** Convenience constant for requests issued outside a XenStore transaction. */
#define XS_TRANSACTION_NONE 0U

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the default timeout used by non-timeout API variants.
 *
 * @param[in]     tout       Default timeout for operations that wait for a XenStore reply.
 */
void xs_set_default_timeout(k_timeout_t tout);

/**
 * @brief Initialize the XenStore API implementation.
 *
 * @retval 0 on success.
 * @retval -errno on failure.
 */
int xs_init(void);

#ifdef __cplusplus
}
#endif

#endif /* XENLIB_XS_H */
