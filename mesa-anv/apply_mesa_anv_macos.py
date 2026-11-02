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

    # 2b. src/util/build_id.h: Mach-O LC_UUID on Apple platforms is 16 bytes (128-bit UUID),
    #     whereas ELF GNU build-id SHA-1 is 20 bytes. copy_build_id_to_sha1() already supports
    #     16-byte LC_UUID, so set BUILD_ID_EXPECTED_HASH_LENGTH = 16 on DETECT_OS_APPLE so
    #     anv_physical_device_init_uuids() succeeds during vkEnumeratePhysicalDevices().
    replace_once(
        mesa / "src" / "util" / "build_id.h",
        "#define BUILD_ID_EXPECTED_HASH_LENGTH 20 /* sha1 */",
        "/* ARCXE_DARWIN_BUILD_ID_LEN */\n"
        "#if DETECT_OS_APPLE\n"
        "#define BUILD_ID_EXPECTED_HASH_LENGTH 16 /* Mach-O LC_UUID */\n"
        "#else\n"
        "#define BUILD_ID_EXPECTED_HASH_LENGTH 20 /* sha1 */\n"
        "#endif",
        "ARCXE_DARWIN_BUILD_ID_LEN",
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

    # 4b. Enable vk_image::drm_format_mod and vk_common_GetImageDrmFormatModifierPropertiesEXT on Darwin
    #     (Mesa ANV uses image->vk.drm_format_mod unconditionally across anv_private.h / anv_image.c).
    replace_once(
        mesa / "src" / "vulkan" / "runtime" / "vk_image.h",
        "#if DETECT_OS_LINUX || DETECT_OS_BSD\n"
        "   /* VK_EXT_drm_format_modifier",
        "/* ARCXE_DARWIN_VK_IMAGE_H */\n"
        "#if DETECT_OS_LINUX || DETECT_OS_BSD || DETECT_OS_APPLE\n"
        "   /* VK_EXT_drm_format_modifier",
        "ARCXE_DARWIN_VK_IMAGE_H",
    )
    replace_all(
        mesa / "src" / "vulkan" / "runtime" / "vk_image.c",
        "#if DETECT_OS_LINUX || DETECT_OS_BSD",
        "/* ARCXE_DARWIN_VK_IMAGE_C */\n#if DETECT_OS_LINUX || DETECT_OS_BSD || DETECT_OS_APPLE",
        "ARCXE_DARWIN_VK_IMAGE_C",
        expected_min=3,
    )

    # 4c. Prefer per-user / App Sandbox $TMPDIR in src/util/anon_file.c on Darwin before /tmp fallback
    replace_once(
        mesa / "src" / "util" / "anon_file.c",
        '    env = os_get_option("XDG_RUNTIME_DIR");\n'
        "    if (env && env[0] != '\\0') {\n"
        '        n = asprintf(&buf, "%s", env);\n'
        "        if (n < 0)\n"
        "           return NULL;\n"
        "        return buf;\n"
        "    }",
        '    env = os_get_option("XDG_RUNTIME_DIR");\n'
        "    if (env && env[0] != '\\0') {\n"
        '        n = asprintf(&buf, "%s", env);\n'
        "        if (n < 0)\n"
        "           return NULL;\n"
        "        return buf;\n"
        "    }\n"
        "#ifdef __APPLE__\n"
        "    /* ARCXE_DARWIN_TMPDIR */\n"
        '    env = os_get_option("TMPDIR");\n'
        "    if (env && env[0] != '\\0' && stat(env, &st) == 0 && S_ISDIR(st.st_mode)) {\n"
        '        n = asprintf(&buf, "%s", env);\n'
        "        if (n < 0)\n"
        "           return NULL;\n"
        "        while (n > 1 && buf[n - 1] == '/')\n"
        "           buf[--n] = '\\0';\n"
        "        return buf;\n"
        "    }\n"
        "#endif",
        "ARCXE_DARWIN_TMPDIR",
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
    # ArcXe has a fixed RCC policy: every render LRC sets COMMON_SLICE_CHICKEN3
    # bit13 before user batches, also after resets. Its Xe ioctl facade does not
    # expose Linux's configurable per-queue property. ANV must use this known
    # backend policy instead of inferring BTI-only keying from the absent flag.
    replace_once(
        anv_pdev,
        "   device->rt_change_needs_flush =\n"
        "      !instance->drirc.perf.state_cache_perf_fix ||\n"
        "      !platform_supports_btp_bit_rcc;",
        "   device->rt_change_needs_flush =\n"
        "      !instance->drirc.perf.state_cache_perf_fix ||\n"
        "      !platform_supports_btp_bit_rcc;\n"
        "#ifdef __APPLE__\n"
        "   /* ARCXE_FIXED_BTP_BTI_RCC: kernel always enables BTP+BTI keying.\n"
        "    * Keep the drirc opt-out for comparing legacy flushes.\n"
        "    */\n"
        "   device->rt_change_needs_flush = !instance->drirc.perf.state_cache_perf_fix;\n"
        "   fprintf(stderr, \"[ArcXe RCC] BTP+BTI keying; legacy RT-change flush=%u\\n\",\n"
        "           (unsigned)device->rt_change_needs_flush);\n"
        "#endif",
        "ARCXE_FIXED_BTP_BTI_RCC",
    )
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
        "   struct intel_device_info devinfo;\n"
        "   if (!intel_get_device_info_from_fd(fd, &devinfo, 9, -1)) {\n"
        "      result = VK_ERROR_INCOMPATIBLE_DRIVER;\n"
        "      goto fail_fd;\n"
        "   }",
        "   struct intel_device_info devinfo;\n"
        "   if (!intel_get_device_info_from_fd(fd, &devinfo, 9, -1)) {\n"
        "      result = VK_ERROR_INCOMPATIBLE_DRIVER;\n"
        "      goto fail_fd;\n"
        "   }\n"
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_NO_AUX_CCS: Disable multi-process global AuxCCS page table\n"
        "    * (GFX_AUX_TABLE_BASE_ADDR 0x4200 / CCS_AUX_INV 0x4208) by default so\n"
        "    * all 3D VkImage render targets and sampled textures render directly\n"
        "    * into uncompressed coherent UMA tiles (ISL_AUX_USAGE_NONE).\n"
        "    */\n"
        "   {\n"
        '      const char *aux_env = getenv("ARCMTL_AUX_CCS");\n'
        "      if (!(aux_env && aux_env[0] == '1')) {\n"
        "         process_intel_debug_variable();\n"
        "         BITSET_SET(intel_debug, DEBUG_NO_CCS);\n"
        "         devinfo.has_aux_map = false;\n"
        "         devinfo.has_flat_ccs = false;\n"
        "      }\n"
        "   }\n"
        "#endif",
        "ARCXE_XNU_NO_AUX_CCS",
    )
    replace_once(
        anv_pdev,
        "   intel_common_update_device_info(fd, &device->info);",
        "   intel_common_update_device_info(fd, &device->info);\n"
        "#ifdef __APPLE__\n"
        "   /* ARCXE_XNU_NO_AUX_CCS_POST */\n"
        "   if (INTEL_DEBUG(DEBUG_NO_CCS)) {\n"
        "      device->info.has_aux_map = false;\n"
        "      device->info.has_flat_ccs = false;\n"
        "   }\n"
        "#endif",
        "ARCXE_XNU_NO_AUX_CCS_POST",
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
        "  link_args : [vulkan_icd_link_args, '-Wl,-undefined,dynamic_lookup', '-Wl,-framework,IOKit', '-Wl,-framework,CoreFoundation', ld_args_build_id, ld_args_bsymbolic, ld_args_gc_sections],",
        "ARCXE_DARWIN_LINK",
    )
    # Upgrade older ARCXE_DARWIN_LINK trees if re-applied locally
    meson_txt = anv_meson.read_text(encoding="utf-8")
    if "ARCXE_DARWIN_LINK" in meson_txt and "'-Wl,-undefined,dynamic_lookup'" not in meson_txt:
        anv_meson.write_text(
            meson_txt.replace(
                "'-Wl,-framework,IOKit'",
                "'-Wl,-undefined,dynamic_lookup', '-Wl,-framework,IOKit'",
                1,
            ),
            encoding="utf-8",
            newline="\n",
        )

    # 10. The failing ArcXe path is a render copy from LINEAR into TILE4.
    # Keep equal-sized, uncompressed color formats at their original bpb on
    # Meteor Lake instead of redescribing a 32-bpb render target as 128-bpb.
    # This is a bounded workaround pending on-device validation, not evidence
    # that all 128-bpb TILE4 render targets are unsupported by the hardware.
    blorp_blit = mesa / "src" / "intel" / "blorp" / "blorp_blit.c"
    replace_once(
        blorp_blit,
        '#include "util/u_math.h"',
        '#include "util/u_math.h"\n'
        '#ifdef __APPLE__\n#include <stdio.h> /* ARCXE_BLORP_DIAGNOSTICS */\n#endif',
        "ARCXE_BLORP_DIAGNOSTICS",
    )
    copy_scale_anchor = (
        "   int copy_fmt_bpb = MIN2(src_fmtl->bpb * max_fmt_scale_src,\n"
        "                           dst_fmtl->bpb * max_fmt_scale_dst);"
    )
    replace_once(
        blorp_blit,
        copy_scale_anchor,
        "#ifdef __APPLE__\n"
        "   /* ARCXE_MTL_LINEAR_TILE4_NATIVE_BPB: avoid format widening on\n"
        "    * the render upload path that times out on ArcXe. Preserve the\n"
        "    * existing compute/blitter, compressed and mixed-format paths.\n"
        "    */\n"
        "   if (intel_device_info_is_mtl(devinfo) &&\n"
        "       !(batch->flags & (BLORP_BATCH_USE_COMPUTE | BLORP_BATCH_USE_BLITTER)) &&\n"
        "       params.src.surf.tiling == ISL_TILING_LINEAR &&\n"
        "       params.dst.surf.tiling == ISL_TILING_4 &&\n"
        "       params.src.aux_usage == ISL_AUX_USAGE_NONE &&\n"
        "       params.dst.aux_usage == ISL_AUX_USAGE_NONE &&\n"
        "       !isl_surf_usage_is_depth_or_stencil(params.src.surf.usage) &&\n"
        "       !isl_surf_usage_is_depth_or_stencil(params.dst.surf.usage) &&\n"
        "       src_fmtl->bw == 1 && src_fmtl->bh == 1 &&\n"
        "       dst_fmtl->bw == 1 && dst_fmtl->bh == 1 &&\n"
        "       src_fmtl->bpb == 32 && dst_fmtl->bpb == 32) {\n"
        "      if (INTEL_DEBUG(DEBUG_BLORP))\n"
        "         fprintf(stderr, \"[ArcXe BLORP] LINEAR->TILE4 %ux%u: \"\n"
        "              \"native 32-bpb render copy (scale %d/%d -> 1/1)\\n\",\n"
        "              src_width, src_height, max_fmt_scale_src, max_fmt_scale_dst);\n"
        "      max_fmt_scale_src = max_fmt_scale_dst = 1;\n"
        "   }\n"
        "#endif\n" + copy_scale_anchor,
        "ARCXE_MTL_LINEAR_TILE4_NATIVE_BPB",
    )

    # 11. Native-bpb render uploads still hang on the 4x4 regression test.
    # Use BLORP's existing compute implementation on the same RCS queue for
    # supported color uploads. Do not advertise another hardware queue or
    # force compute on unsupported depth/stencil/compressed/MSAA transfers.
    anv_blorp = anv_dir / "anv_blorp.c"
    replace_once(
        anv_blorp,
        '#include "genxml/gen80_pack.h"',
        '#include "genxml/gen80_pack.h"\n'
        '#ifdef __APPLE__\n'
        '#include <stdio.h> /* ARCXE_COMPUTE_UPLOAD_INCLUDES */\n'
        '#include "util/u_debug.h"\n'
        '#endif',
        "ARCXE_COMPUTE_UPLOAD_INCLUDES",
    )
    upload_loop = "   for (unsigned z = 0; z < extent.depth; z++) {\n      blorp_copy(batch, &src->surf"
    replace_once(
        anv_blorp,
        upload_loop,
        "#ifdef __APPLE__\n"
        "   /* ARCXE_MTL_COMPUTE_UPLOAD: keep raster/RT writes out of uploads.\n"
        "    * ARCMTL_RENDER_UPLOAD=1 permits comparison in a fresh process.\n"
        "    */\n"
        "   const enum blorp_batch_flags arc_saved_flags = batch->flags;\n"
        "   const bool arc_compute_upload = memory_to_image &&\n"
        "      intel_device_info_is_mtl(cmd_buffer->device->info) &&\n"
        "      (cmd_buffer->queue_family->queueFlags & VK_QUEUE_COMPUTE_BIT) &&\n"
        "      !(batch->flags & (BLORP_BATCH_USE_BLITTER | BLORP_BATCH_USE_COMPUTE)) &&\n"
        "      sub_resource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&\n"
        "      src->isl_surf->tiling == ISL_TILING_LINEAR &&\n"
        "      dst->isl_surf->tiling == ISL_TILING_4 &&\n"
        "      src->surf.aux_usage == ISL_AUX_USAGE_NONE &&\n"
        "      dst->surf.aux_usage == ISL_AUX_USAGE_NONE &&\n"
        "      src->isl_surf->samples == 1 && dst->isl_surf->samples == 1 &&\n"
        "      isl_format_get_layout(src->isl_surf->format)->bw == 1 &&\n"
        "      isl_format_get_layout(src->isl_surf->format)->bh == 1 &&\n"
        "      isl_format_get_layout(dst->isl_surf->format)->bw == 1 &&\n"
        "      isl_format_get_layout(dst->isl_surf->format)->bh == 1 &&\n"
        "      isl_format_get_layout(src->isl_surf->format)->bpb == 32 &&\n"
        "      isl_format_get_layout(dst->isl_surf->format)->bpb == 32 &&\n"
        "      blorp_copy_supports_compute(batch->blorp, src->isl_surf,\n"
        "                                  dst->isl_surf, dst->surf.aux_usage) &&\n"
        "      !debug_get_bool_option(\"ARCMTL_RENDER_UPLOAD\", false);\n"
        "   if (arc_compute_upload) {\n"
        "      batch->flags |= BLORP_BATCH_USE_COMPUTE;\n"
        "      if (INTEL_DEBUG(DEBUG_BLORP))\n"
        "         fprintf(stderr, \"[ArcXe BLORP] LINEAR->TILE4 %ux%u: compute upload on RCS\\n\",\n"
        "                 extent.width, extent.height);\n"
        "   }\n"
        "#endif\n\n" + upload_loop,
        "ARCXE_MTL_COMPUTE_UPLOAD",
    )
    upload_end = (
        "      memory.surf.addr.offset += mem_layout->image_stride_B;\n"
        "   }\n\n"
        "   /* The next copy parameters"
    )
    replace_once(
        anv_blorp,
        upload_end,
        "      memory.surf.addr.offset += mem_layout->image_stride_B;\n"
        "   }\n\n"
        "#ifdef __APPLE__\n"
        "   /* ARCXE_MTL_COMPUTE_UPLOAD_FLUSH: the public transfer operation\n"
        "    * ran as compute, so publish HDC writes before another consumer.\n"
        "    */\n"
        "   if (arc_compute_upload) {\n"
        "      anv_add_pending_pipe_bits(cmd_buffer,\n"
        "         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,\n"
        "         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,\n"
        "         ANV_PIPE_HDC_PIPELINE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT |\n"
        "         ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT,\n"
        "         \"ArcXe compute upload visibility\");\n"
        "   }\n"
        "   batch->flags = arc_saved_flags;\n"
        "#endif\n\n"
        "   /* The next copy parameters",
        "ARCXE_MTL_COMPUTE_UPLOAD_FLUSH",
    )

    print("Mesa 26.2 ANV Darwin/XNU patch applied successfully.")


if __name__ == "__main__":
    main()
