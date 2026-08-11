# SPDX-License-Identifier: Apache-2.0

add_library(XEN_BLKFRONT INTERFACE)

target_include_directories(XEN_BLKFRONT INTERFACE include)

zephyr_library()
zephyr_library_sources(
  src/xen_blkfront.c
  src/xen_blkfront_queue.c
  src/xen_blkfront_ring.c
  src/xen_blkfront_transport.c
  src/xen_blkfront_xenbus.c
)
zephyr_library_link_libraries(XEN_BLKFRONT XENSTORE_CLI)
zephyr_include_directories(include)
