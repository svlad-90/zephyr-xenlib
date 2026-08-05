/*
 * Copyright (c) 2026 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XENLIB_XEN_BLKFRONT_H
#define XENLIB_XEN_BLKFRONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEN_BLKFRONT_SECTOR_SIZE 512U

struct xen_blkfront;

/**
 * @brief Xen PV block backend capabilities discovered from XenStore.
 */
struct xen_blkfront_info {
	/** Backend capacity in 512-byte sectors. */
	uint64_t sectors;
	/** Backend logical sector size in bytes. */
	uint32_t sector_size;
	/** Backend physical sector size in bytes. */
	uint32_t physical_sector_size;
	/** Backend device information bitmap, using VDISK_* bits from blkif.h. */
	uint32_t info;
	/** Maximum backend request ring order, where 0 means one ring page. */
	uint32_t max_ring_page_order;
	/** Maximum backend request ring pages. */
	uint32_t max_ring_pages;
	/** Maximum backend queues advertised through multi-queue-max-queues. */
	uint32_t multi_queue_max_queues;
	/** Maximum indirect request segments, or 0 when indirect requests are absent. */
	uint32_t max_indirect_segments;
	/** Discard alignment in bytes. */
	uint32_t discard_alignment;
	/** Discard granularity in bytes. */
	uint32_t discard_granularity;
	/** Backend accepted write requests at discovery time. */
	bool writable;
	/** Backend advertises BLKIF_OP_WRITE_BARRIER support. */
	bool feature_barrier;
	/** Backend advertises BLKIF_OP_FLUSH_DISKCACHE support. */
	bool feature_flush_cache;
	/** Backend advertises BLKIF_OP_DISCARD support. */
	bool feature_discard;
	/** Backend advertises secure discard support. */
	bool discard_secure;
	/** Backend advertises persistent grant support. */
	bool feature_persistent;
};

/**
 * @brief Xen PV block frontend connection parameters.
 */
struct xen_blkfront_config {
	/** XenStore frontend device path, for example "/local/domain/1/device/vbd/51712". */
	const char *frontend_path;
	/** XenStore backend device path, for example "/local/domain/0/backend/vbd/1/51712". */
	const char *backend_path;
	/** Virtual block-device handle assigned by the toolstack. */
	uint16_t vdev;
	/** Domain id of the blkback backend. Dom0 is normally 0. */
	uint16_t backend_domid;
	/** Timeout for one XenStore request issued by this frontend. */
	k_timeout_t xs_timeout;
	/** XenStore read attempts while waiting for backend-created nodes and state. */
	uint16_t backend_wait_attempts;
	/** Delay between backend wait attempts. */
	k_timeout_t backend_retry_delay;
};

/**
 * @brief Open and connect a Xen PV block frontend.
 *
 * The returned handle is owned by the caller and must be closed with
 * xen_blkfront_close(). This minimal frontend supports one outstanding block
 * request at a time. If a submitted request times out or its completion state
 * becomes ambiguous, the handle enters a failed state: later reads fail until
 * the caller closes the handle.
 *
 * @param cfg         Connection parameters.
 * @param front       Output handle.
 * @param xs_buf      Caller-provided temporary XenStore response buffer. This
 *                    must be valid when closing a frontend that reached
 *                    XenStore publication.
 * @param xs_buf_len  Size of @p xs_buf.
 *
 * @retval 0       Frontend connected to blkback.
 * @retval -errno  Failed to connect.
 */
int xen_blkfront_open(const struct xen_blkfront_config *cfg, struct xen_blkfront **front,
		      char *xs_buf, size_t xs_buf_len);

/**
 * @brief Read one or more sectors into a caller buffer.
 *
 * @param front   Open frontend handle.
 * @param sector  First 512-byte sector to read.
 * @param data    Destination buffer.
 * @param len     Number of bytes to read; must be a non-zero multiple of 512
 *                bytes. The driver may split larger reads into smaller Xen
 *                block protocol requests internally.
 *
 * @retval 0       Read completed successfully.
 * @retval -ERANGE Requested sector range is outside the backend capacity.
 * @retval -errno  Failed to submit or complete the request.
 */
int xen_blkfront_read(struct xen_blkfront *front, uint64_t sector, void *data, size_t len);

/**
 * @brief Write one or more sectors from a caller buffer.
 *
 * @param front   Open frontend handle.
 * @param sector  First 512-byte sector to write.
 * @param data    Source buffer.
 * @param len     Number of bytes to write; must be a non-zero multiple of 512
 *                bytes. The driver may split larger writes into smaller Xen
 *                block protocol requests internally.
 *
 * @retval 0       Write completed successfully.
 * @retval -ERANGE Requested sector range is outside the backend capacity.
 * @retval -errno  Failed to submit or complete the request.
 */
int xen_blkfront_write(struct xen_blkfront *front, uint64_t sector, const void *data,
		       size_t len);

/**
 * @brief Commit backend volatile write cache to stable storage.
 *
 * @param front Open frontend handle.
 *
 * @retval 0        Flush request completed successfully.
 * @retval -ENOTSUP Backend did not advertise flush support or rejected the request.
 * @retval -errno   Failed to submit or complete the request.
 */
int xen_blkfront_flush(struct xen_blkfront *front);

/**
 * @brief Tell the backend that a sector range no longer contains useful data.
 *
 * Discard is the Xen block protocol operation behind trim/unmap. It does not
 * transfer a data page; it asks blkback to release or forget the specified
 * sectors when the backing storage supports that operation.
 *
 * @param front        Open frontend handle.
 * @param sector       First 512-byte sector to discard.
 * @param sector_count Number of contiguous 512-byte sectors to discard.
 * @param secure       Request secure discard. This requires backend support.
 *
 * @retval 0        Discard request completed successfully.
 * @retval -ENOTSUP Backend did not advertise discard support, secure discard
 *                  was requested without support, or blkback rejected the
 *                  request as unsupported.
 * @retval -ERANGE  Requested sector range is outside the backend capacity.
 * @retval -EINVAL  Requested range is empty or not discard-granularity aligned.
 * @retval -errno   Failed to submit or complete the request.
 */
int xen_blkfront_discard(struct xen_blkfront *front, uint64_t sector,
			 uint64_t sector_count, bool secure);

/**
 * @brief Return the sector count advertised by the backend.
 *
 * @param front Open frontend handle.
 *
 * @return Number of 512-byte sectors, or 0 for an invalid handle.
 */
uint64_t xen_blkfront_sectors(const struct xen_blkfront *front);

/**
 * @brief Copy backend capabilities discovered during xen_blkfront_open().
 *
 * @param front Open frontend handle.
 * @param info  Output capability structure.
 *
 * @retval 0       Capabilities copied.
 * @retval -EINVAL Invalid handle or output pointer.
 */
int xen_blkfront_get_info(const struct xen_blkfront *front, struct xen_blkfront_info *info);

/**
 * @brief Disconnect and destroy a Xen PV block frontend.
 *
 * The caller must not use @p front after this function returns, and must not
 * call this function concurrently with another API operating on the same
 * handle.
 *
 * @param front       Open frontend handle. NULL is accepted.
 * @param xs_buf      Caller-provided temporary XenStore response buffer.
 * @param xs_buf_len  Size of @p xs_buf.
 *
 * @retval 0       Frontend was disconnected or @p front was NULL.
 * @retval -EINVAL @p front reached XenStore publication, but @p xs_buf is NULL
 *                 or @p xs_buf_len is zero. The handle remains open so the
 *                 caller can retry with a valid buffer.
 * @retval -errno  Frontend resources were released, but the XenBus shutdown
 *                 handshake did not complete cleanly.
 */
int xen_blkfront_close(struct xen_blkfront *front, char *xs_buf, size_t xs_buf_len);

#ifdef __cplusplus
}
#endif

#endif /* XENLIB_XEN_BLKFRONT_H */
