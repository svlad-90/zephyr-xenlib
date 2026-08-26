# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 TOKITA Hiroshi

add_library(XENSTORE_CLIENT INTERFACE)

target_include_directories(XENSTORE_CLIENT INTERFACE include)

zephyr_library()
zephyr_library_sources(src/xenstore_client.c)
zephyr_library_link_libraries(XENSTORE_CLIENT XEN_STORE_COMMON)
zephyr_include_directories(include)
