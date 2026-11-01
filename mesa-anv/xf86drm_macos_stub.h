/*
 * SPDX-License-Identifier: MIT
 *
 * Minimal xf86drm.h replacement for building Mesa Intel ANV natively on macOS (Darwin / XNU)
 * without Linux libdrm. Implemented in anv_xnu_drm_shim.c over IOKit (ArcXeService).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioccom.h>
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DRM_IOC_VOID
#define DRM_IOC_VOID                    IOC_VOID
#define DRM_IOC_READ                    IOC_OUT
#define DRM_IOC_WRITE                   IOC_IN
#define DRM_IOC_READWRITE               IOC_INOUT
#define DRM_IOC(dir, group, nr, size)   _IOC(dir, group, nr, size)
#endif

#define DRM_NODE_PRIMARY                0
#define DRM_NODE_CONTROL                1
#define DRM_NODE_RENDER                 2
#define DRM_NODE_MAX                    3

#define DRM_BUS_PCI                     0
#define DRM_BUS_USB                     1
#define DRM_BUS_PLATFORM                2
#define DRM_BUS_HOST1X                  3

#define DRM_DEVICE_GET_PCI_REVISION     (1u << 0)

#ifndef DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL         (1u << 0)
#endif
#ifndef DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT  (1u << 1)
#endif
#ifndef DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE   (1u << 2)
#endif
#ifndef DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE    (1u << 3)
#endif

#define DRM_PLATFORM_DEVICE_NAME_LEN    512
#define DRM_HOST1X_DEVICE_NAME_LEN      512

typedef unsigned int drm_magic_t;

typedef struct _drmPciBusInfo {
    uint16_t domain;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint8_t  revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmPlatformBusInfo {
    char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
    char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmHost1xBusInfo {
    char fullname[DRM_HOST1X_DEVICE_NAME_LEN];
} drmHost1xBusInfo, *drmHost1xBusInfoPtr;

typedef struct _drmDevice {
    char **nodes;
    int    available_nodes;
    int    bustype;
    union {
        drmPciBusInfoPtr      pci;
        drmPlatformBusInfoPtr platform;
        drmHost1xBusInfoPtr   host1x;
    } businfo;
    union {
        drmPciDeviceInfoPtr      pci;
        drmPlatformDeviceInfoPtr platform;
    } deviceinfo;
} drmDevice, *drmDevicePtr;

typedef struct _drmVersion {
    int   version_major;
    int   version_minor;
    int   version_patchlevel;
    int   name_len;
    char *name;
    int   date_len;
    char *date;
    int   desc_len;
    char *desc;
} drmVersion, *drmVersionPtr;

int           drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void          drmFreeDevices(drmDevicePtr devices[], int count);
int           drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void          drmFreeDevice(drmDevicePtr *device);
drmVersionPtr drmGetVersion(int fd);
void          drmFreeVersion(drmVersionPtr v);
int           drmIoctl(int fd, unsigned long request, void *arg);
char         *drmGetRenderDeviceNameFromFd(int fd);
int           drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);

static inline int drmGetMagic(int fd, drm_magic_t *magic)
{
    (void)fd; (void)magic;
    return -EINVAL;
}

static inline int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
    (void)dev_id; (void)flags; (void)device;
    return -ENOENT;
}

static inline char *drmGetDeviceNameFromFd2(int fd)
{
    return drmGetRenderDeviceNameFromFd(fd);
}

static inline int drmGetNodeTypeFromFd(int fd)
{
    (void)fd;
    return DRM_NODE_RENDER;
}

#ifdef __cplusplus
}
#endif
