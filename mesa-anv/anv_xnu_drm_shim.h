/*
 * SPDX-License-Identifier: MIT
 *
 * Darwin / XNU IOKit Transport Shim for Mesa Intel ANV (`libvulkan_intel.dylib`).
 * Translates Mesa's Linux `xe_drm.h` ioctls, GEM mmap, syncobj provider, and libdrm queries
 * directly into `IOConnectCallMethod` / `IOConnectMapMemory64` calls to `ArcXe.kext` (`ArcXeService`).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct util_sync_provider;

#define ARC_XNU_FAKE_RENDER_NODE "/tmp/arcxe-renderD128"

int   arc_xnu_open_device(void);
void  arc_xnu_close_device(int fd);
bool  arc_xnu_is_fd(int fd);
int   arc_xnu_ioctl(int fd, unsigned long request, void *arg);
void *arc_xnu_gem_mmap(int fd, uint32_t gem_handle, uint64_t offset, uint64_t size, void *placed_addr);
int   arc_xnu_gem_munmap(void *addr, size_t length);

struct util_sync_provider *arc_xnu_sync_provider(int fd);

#ifdef __cplusplus
}
#endif
