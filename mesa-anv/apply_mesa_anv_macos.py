#!/usr/bin/env python3
"""
Idempotently patch an upstream Mesa 26.2.x source tree so Intel ANV (`libvulkan_intel.dylib`)
compiles natively on macOS 15 Sequoia / 26 Tahoe (`x86_64-apple-darwin`) and talks directly
to `ArcXe.kext` (`ArcXeService`) via `anv_xnu_drm_shim.c` + IOKit.framework.
"""

import shutil
import sys
from pathlib import Path


def replace_once(path: Path, old: str, new: str, marker: str) -> None:
    text = path.read_text(encoding="utf-8")
    if marker in text:
        return
    if old not in text:
        raise RuntimeError(f"Expected anchor not found in {path}:\n{old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"  patched {path}")


def replace_all(path: Path, old: str, new: str, marker: str, expected_min: int = 1) -> None:
    text = path.read_text(encoding="utf-8")
    if marker in text:
        return
    count = text.count(old)
    if count < expected_min:
        raise RuntimeError(
            f"Expected anchor at least {expected_min}x (found {count}) in {path}:\n{old}"
        )
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"  patched {path} ({count} site(s))")


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: apply_mesa_anv_macos.py <path-to-mesa-src>", file=sys.stderr)
        sys.exit(1)

    mesa = Path(sys.argv[1]).resolve()
    here = Path(__file__).resolve().parent
    repo_root = here.parent

    # 1. Copy Darwin IOKit shim + headers into Mesa's src/intel/vulkan/ and include/
    anv_dir = mesa / "src" / "intel" / "vulkan"
    inc_dir = mesa / "include"
    shutil.copy2(here / "anv_xnu_drm_shim.h", anv_dir / "anv_xnu_drm_shim.h")
    shutil.copy2(here / "anv_xnu_drm_shim.c", anv_dir / "anv_xnu_drm_shim.c")
    shutil.copy2(here / "xf86drm_macos_stub.h", inc_dir / "xf86drm.h")
    shutil.copy2(repo_root / "include" / "arc_uapi.h", anv_dir / "arc_uapi.h")
    shutil.copy2(repo_root / "include" / "arc_regs_mtl.h", anv_dir / "arc_regs_mtl.h")

    # 2. Top-level meson.build: allow Intel ANV on Darwin (src/util/build_id.c already
    #    implements Mach-O LC_UUID when DETECT_OS_APPLE is true, without dl_iterate_phdr).
    #    Keep dep_libdrm = null_dep so Darwin never searches for Linux libdrm.
    replace_once(
        mesa / "meson.build",
        "elif with_intel_vk or with_intel_hasvk\n"
        "  error('Intel \"Anvil\" Vulkan driver requires the dl_iterate_phdr function')",
        "# ARCXE_DARWIN_MESON\n"
        "elif (with_intel_vk or with_intel_hasvk) and host_machine.system() != 'darwin'\n"
        "  error('Intel \"Anvil\" Vulkan driver requires the dl_iterate_phdr function')",
        "ARCXE_DARWIN_MESON",
    )

    # 3. Route src/util/libdrm.h and src/util/os_drm.h through our include/xf86drm.h shim on Darwin
    #    so vk_instance.c (drmGetDevices2) and intel_kmd.c (drmGetVersion) reach anv_xnu_drm_shim.c.
    replace_once(
        mesa / "src" / "util" / "libdrm.h",
        "#ifdef HAVE_LIBDRM\n#include <xf86drm.h>",
        "/* ARCXE_DARWIN_LIBDRM_H */\n#if defined(HAVE_LIBDRM) || defined(__APPLE__)\n#include <xf86drm.h>",
        "ARCXE_DARWIN_LIBDRM_H",
    )
    replace_once(
        mesa / "src" / "util" / "os_drm.h",
        "   do {\n      ret = ioctl(fd, request, arg);\n   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));",
        "   do {\n"
        "#ifdef __APPLE__\n"
        "      /* ARCXE_DARWIN_OS_DRM_H */\n"
        "      ret = drmIoctl(fd, request, arg);\n"
        "#else\n"
        "      ret = ioctl(fd, request, arg);\n"
        "#endif\n"
        "   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));",
        "ARCXE_DARWIN_OS_DRM_H",
    )

    # 4. Compile vk_drm_syncobj.c into libvulkan_lite_runtime.a on Darwin (uses util_sync_provider vtable)
    replace_once(
        mesa / "src" / "vulkan" / "runtime" / "meson.build",
        "if dep_libdrm.found()\n"
        "  vulkan_lite_runtime_files += files('vk_drm_syncobj.c')\n"
        "  vulkan_lite_runtime_deps += dep_libdrm\n"
        "endif",
        "# ARCXE_DARWIN_VK_SYNCOBJ\n"
        "if dep_libdrm.found() or host_machine.system() == 'darwin'\n"
        "  vulkan_lite_runtime_files += files('vk_drm_syncobj.c')\n"
        "  vulkan_lite_runtime_deps += dep_libdrm\n"
        "endif",
        "ARCXE_DARWIN_VK_SYNCOBJ",
    )

    # 5. Hook intel_ioctl() in src/intel/common/intel_gem.h to route through arc_xnu_ioctl on __APPLE__
    replace_once(
        mesa / "src" / "intel" / "common" / "intel_gem.h",
        "static inline int\n"
        "intel_ioctl(int fd, unsigned long request, void *arg)\n"
        "{\n"
        "    int ret;\n\n"
        "    do {\n"
        "#ifdef HAVE_INTEL_VIRTIO\n"
        "      ret = intel_virtio_ioctl(fd, request, arg);\n"
        "#else\n"
        "      ret = ioctl(fd, request, arg);\n"
        "#endif\n"
        "    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));\n"
        "    return ret;\n"
        "}",
        "/* ARCXE_XNU_IOCTL */\n"
        "#ifdef __APPLE__\n"
        "extern int arc_xnu_ioctl(int fd, unsigned long request, void *arg);\n"
        "#endif\n"
        "static inline int\n"
        "intel_ioctl(int fd, unsigned long request, void *arg)\n"
        "{\n"
        "#ifdef __APPLE__\n"
        "    return arc_xnu_ioctl(fd, request, arg);\n"
        "#else\n"
        "    int ret;\n\n"
        "    do {\n"
        "#ifdef HAVE_INTEL_VIRTIO\n"
        "      ret = intel_virtio_ioctl(fd, request, arg);\n"
        "#else\n"
        "      ret = ioctl(fd, request, arg);\n"
        "#endif\n"
        "    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));\n"
        "    return ret;\n"
        "#endif\n"
        "}",
        "ARCXE_XNU_IOCTL",
    )

    # 6. Hook xe_gem_mmap() in src/intel/vulkan/xe/anv_kmd_backend.c,
    #    anv_device_unmap_bo() in anv_allocator.c, and disable 2 MiB slab parent BOs in anv_slab_bo.c
    xe_kmd = anv_dir / "xe" / "anv_kmd_backend.c"
    replace_once(
        xe_kmd,
        '#include "drm-uapi/xe_drm.h"',
        '#include "drm-uapi/xe_drm.h"\n#ifdef __APPLE__\n#include "../anv_xnu_drm_shim.h"\n#endif',
        "anv_xnu_drm_shim.h",
    )
    replace_once(
        xe_kmd,
        "static void *\n"
        "xe_gem_mmap(struct anv_device *device, struct anv_bo *bo, uint64_t offset,\n"
        "            uint64_t size, void *placed_addr)\n"
        "{",
        "static void *\n"
        "xe_gem_mmap(struct anv_device *device, struct anv_bo *bo, uint64_t offset,\n"
        "            uint64_t size, void *placed_addr)\n"
        "{\n"
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_MMAP */\n"
        "   return arc_xnu_gem_mmap(device->fd, bo->gem_handle, offset, size, placed_addr);\n"
        "#endif",
        "ARCXE_XNU_MMAP",
    )

    anv_alloc = anv_dir / "anv_allocator.c"
    replace_once(
        anv_alloc,
        '#include "util/os_mman.h"',
        '#include "util/os_mman.h"\n#ifdef __APPLE__\n#include "anv_xnu_drm_shim.h"\n#endif',
        "anv_xnu_drm_shim.h",
    )
    replace_once(
        anv_alloc,
        "   } else {\n"
        "      VG(VALGRIND_FREELIKE_BLOCK(map, 0));\n"
        "      munmap(map, map_size);\n"
        "   }",
        "   } else {\n"
        "      VG(VALGRIND_FREELIKE_BLOCK(map, 0));\n"
        "#ifdef __APPLE__\n"
        "      /* ARCXE_XNU_MUNMAP */\n"
        "      arc_xnu_gem_munmap(map, map_size);\n"
        "#else\n"
        "      munmap(map, map_size);\n"
        "#endif\n"
        "   }",
        "ARCXE_XNU_MUNMAP",
    )

    replace_once(
        anv_dir / "anv_slab_bo.c",
        "   if (ANV_DEBUG(NO_SLAB))\n      return true;",
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_NO_SLAB: XNU IOConnectMapMemory64 maps 1:1 per IOMemoryDescriptor */\n"
        "   return true;\n"
        "#endif\n"
        "   if (ANV_DEBUG(NO_SLAB))\n      return true;",
        "ARCXE_XNU_NO_SLAB",
    )

    # 7. Hook open/close, placed mmap feature flags, and syncobj provider in anv_physical_device.c
    anv_pdev = anv_dir / "anv_physical_device.c"
    replace_once(
        anv_pdev,
        "#include <xf86drm.h>",
        '#include <xf86drm.h>\n#ifdef __APPLE__\n#include "anv_xnu_drm_shim.h"\n#include "util/u_sync_provider.h"\n#endif',
        "anv_xnu_drm_shim.h",
    )
    replace_once(
        anv_pdev,
        "      .EXT_map_memory_placed                 = device->info.has_mmap_offset,",
        "#ifdef __APPLE__\n"
        "      /* ARCXE_XNU_NO_PLACED_EXT */\n"
        "      .EXT_map_memory_placed                 = false,\n"
        "#else\n"
        "      .EXT_map_memory_placed                 = device->info.has_mmap_offset,\n"
        "#endif",
        "ARCXE_XNU_NO_PLACED_EXT",
    )
    replace_once(
        anv_pdev,
        "      /* VK_EXT_map_memory_placed */\n"
        "      .memoryMapPlaced = true,\n"
        "      .memoryMapRangePlaced = false,\n"
        "      .memoryUnmapReserve = true,",
        "      /* VK_EXT_map_memory_placed */\n"
        "#ifdef __APPLE__\n"
        "      /* ARCXE_XNU_NO_PLACED_FEAT */\n"
        "      .memoryMapPlaced = false,\n"
        "      .memoryMapRangePlaced = false,\n"
        "      .memoryUnmapReserve = false,\n"
        "#else\n"
        "      .memoryMapPlaced = true,\n"
        "      .memoryMapRangePlaced = false,\n"
        "      .memoryUnmapReserve = true,\n"
        "#endif",
        "ARCXE_XNU_NO_PLACED_FEAT",
    )
    replace_once(
        anv_pdev,
        "   fd = open(path, O_RDWR | O_CLOEXEC);",
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_OPEN */\n"
        "   fd = (strcmp(path, ARC_XNU_FAKE_RENDER_NODE) == 0) ? arc_xnu_open_device() : open(path, O_RDWR | O_CLOEXEC);\n"
        "#else\n"
        "   fd = open(path, O_RDWR | O_CLOEXEC);\n"
        "#endif",
        "ARCXE_XNU_OPEN",
    )
    replace_once(
        anv_pdev,
        "   } else {\n      device->sync_syncobj_type = vk_drm_syncobj_get_type(fd);\n   }",
        "   } else {\n"
        "#ifdef __APPLE__\n"
        "      /* ARCXE_XNU_SYNC */\n"
        "      struct util_sync_provider *sync = arc_xnu_sync_provider(fd);\n"
        "      device->sync_syncobj_type = vk_drm_syncobj_get_type_from_provider(sync);\n"
        "      if (sync)\n"
        "         sync->finalize(sync);\n"
        "#else\n"
        "      device->sync_syncobj_type = vk_drm_syncobj_get_type(fd);\n"
        "#endif\n"
        "   }",
        "ARCXE_XNU_SYNC",
    )
    replace_once(
        anv_pdev,
        "fail_fd:\n   intel_virtio_unref_fd(fd);\n   close(fd);",
        "fail_fd:\n   intel_virtio_unref_fd(fd);\n"
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_CLOSE_FAIL_FD */\n"
        "   arc_xnu_close_device(fd);\n"
        "#else\n"
        "   close(fd);\n"
        "#endif",
        "ARCXE_XNU_CLOSE_FAIL_FD",
    )
    replace_once(
        anv_pdev,
        "   close(device->local_fd);",
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_CLOSE_PDEV */\n"
        "   arc_xnu_close_device(device->local_fd);\n"
        "#else\n"
        "   close(device->local_fd);\n"
        "#endif",
        "ARCXE_XNU_CLOSE_PDEV",
    )

    # 8. Hook open, sync provider, and close in src/intel/vulkan/anv_device.c
    anv_dev = anv_dir / "anv_device.c"
    replace_once(
        anv_dev,
        "#include <xf86drm.h>",
        '#include <xf86drm.h>\n#ifdef __APPLE__\n#include "anv_xnu_drm_shim.h"\n#endif',
        "anv_xnu_drm_shim.h",
    )
    replace_once(
        anv_dev,
        "   device->fd = open(physical_device->path, O_RDWR | O_CLOEXEC);",
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_DEV_OPEN */\n"
        "   device->fd = arc_xnu_open_device();\n"
        "#else\n"
        "   device->fd = open(physical_device->path, O_RDWR | O_CLOEXEC);\n"
        "#endif",
        "ARCXE_XNU_DEV_OPEN",
    )
    replace_once(
        anv_dev,
        "   if (physical_device->info.is_virtio)\n"
        "      device->vk.sync = intel_virtio_sync_provider(device->fd);\n"
        "   else\n"
        "      vk_device_set_drm_fd(&device->vk, device->fd);",
        "   if (physical_device->info.is_virtio) {\n"
        "      device->vk.sync = intel_virtio_sync_provider(device->fd);\n"
        "   } else {\n"
        "      vk_device_set_drm_fd(&device->vk, device->fd);\n"
        "#ifdef __APPLE__\n"
        "      /* ARCXE_XNU_DEV_SYNC */\n"
        "      device->vk.sync = arc_xnu_sync_provider(device->fd);\n"
        "#endif\n"
        "   }",
        "ARCXE_XNU_DEV_SYNC",
    )
    replace_all(
        anv_dev,
        "   close(device->fd);",
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_DEV_CLOSE */\n"
        "   arc_xnu_close_device(device->fd);\n"
        "#else\n"
        "   close(device->fd);\n"
        "#endif",
        "ARCXE_XNU_DEV_CLOSE",
        expected_min=2,
    )

    # 9. Add anv_xnu_drm_shim.c and IOKit/CoreFoundation link args in src/intel/vulkan/meson.build
    anv_meson = anv_dir / "meson.build"
    replace_once(
        anv_meson,
        "  'anv_device.c',",
        "  'anv_device.c',\n  'anv_xnu_drm_shim.c',",
        "anv_xnu_drm_shim.c",
    )
    replace_once(
        anv_meson,
        "  link_args : [vulkan_icd_link_args, ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections],",
        "  # ARCXE_DARWIN_LINK\n"
        "  link_args : [vulkan_icd_link_args, '-Wl,-framework,IOKit', '-Wl,-framework,CoreFoundation', ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections],",
        "ARCXE_DARWIN_LINK",
    )

    print("Mesa 26.2 ANV Darwin/XNU patch applied successfully.")


if __name__ == "__main__":
    main()
