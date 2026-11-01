#!/usr/bin/env python3
"""
Idempotently patch an upstream Mesa source tree so Intel ANV (`libvulkan_intel.dylib`)
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
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"  patched {path}")


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

    # 2. Allow building intel Vulkan driver on Darwin in top-level meson.build
    top_meson = mesa / "meson.build"
    tm_text = top_meson.read_text(encoding="utf-8")
    if "# ARCXE_DARWIN_MESON" not in tm_text:
        # Relax system_has_kms_drm to include darwin when building intel vulkan
        tm_text = tm_text.replace(
            "system_has_kms_drm = ['openbsd', 'netbsd', 'freebsd', 'gnu/kfreebsd', 'dragonfly', 'linux', 'sunos', 'android', 'managarm'].contains(host_machine.system())",
            "# ARCXE_DARWIN_MESON\nsystem_has_kms_drm = ['openbsd', 'netbsd', 'freebsd', 'gnu/kfreebsd', 'dragonfly', 'linux', 'sunos', 'android', 'managarm', 'darwin'].contains(host_machine.system())",
        )
        top_meson.write_text(tm_text, encoding="utf-8", newline="\n")
        print(f"  patched {top_meson}")

    # 3. Hook intel_ioctl() in src/intel/common/intel_gem.h to route through arc_xnu_ioctl on __APPLE__
    intel_gem_h = mesa / "src" / "intel" / "common" / "intel_gem.h"
    ig_text = intel_gem_h.read_text(encoding="utf-8")
    if "ARCXE_XNU_IOCTL" not in ig_text:
        ig_hook = """/* ARCXE_XNU_IOCTL */
#ifdef __APPLE__
extern int arc_xnu_ioctl(int fd, unsigned long request, void *arg);
#endif
static inline int
intel_ioctl(int fd, unsigned long request, void *arg)
{
#ifdef __APPLE__
   return arc_xnu_ioctl(fd, request, arg);
#else"""
        ig_text = ig_text.replace(
            "static inline int\nintel_ioctl(int fd, unsigned long request, void *arg)\n{",
            ig_hook,
            1,
        )
        ig_text = ig_text.replace(
            "   return ret;\n}",
            "   return ret;\n#endif\n}",
            1,
        )
        intel_gem_h.write_text(ig_text, encoding="utf-8")
        print(f"  patched {intel_gem_h}")

    # 4. Hook xe_gem_mmap() in src/intel/vulkan/xe/anv_kmd_backend.c & munmap in anv_allocator.c
    xe_kmd = mesa / "src" / "intel" / "vulkan" / "xe" / "anv_kmd_backend.c"
    replace_once(
        xe_kmd,
        '#include "drm-uapi/xe_drm.h"',
        '#include "drm-uapi/xe_drm.h"\n#ifdef __APPLE__\n#include "../anv_xnu_drm_shim.h"\n#endif',
        "anv_xnu_drm_shim.h",
    )
    replace_once(
        xe_kmd,
        "static void *\nxe_gem_mmap(struct anv_device *device, struct anv_bo *bo, uint64_t offset,\n            uint64_t size, void *placed_addr)\n{",
        "static void *\nxe_gem_mmap(struct anv_device *device, struct anv_bo *bo, uint64_t offset,\n            uint64_t size, void *placed_addr)\n{\n#ifdef __APPLE__\n   /* ARCXE_XNU_MMAP */\n   return arc_xnu_gem_mmap(device->fd, bo->gem_handle, offset, size, placed_addr);\n#endif",
        "ARCXE_XNU_MMAP",
    )

    anv_alloc = mesa / "src" / "intel" / "vulkan" / "anv_allocator.c"
    if anv_alloc.exists():
        aa_text = anv_alloc.read_text(encoding="utf-8")
        if "ARCXE_XNU_MUNMAP" not in aa_text:
            aa_text = (
                "#ifdef __APPLE__\n/* ARCXE_XNU_MUNMAP */\n#include \"anv_xnu_drm_shim.h\"\n"
                "#define munmap(a, l) arc_xnu_gem_munmap((a), (l))\n#endif\n" + aa_text
            )
            anv_alloc.write_text(aa_text, encoding="utf-8")
            print(f"  patched {anv_alloc}")

    # 5. Hook open/close & syncobj provider in src/intel/vulkan/anv_physical_device.c and anv_device.c
    anv_pdev = mesa / "src" / "intel" / "vulkan" / "anv_physical_device.c"
    replace_once(
        anv_pdev,
        "#include <xf86drm.h>",
        '#include <xf86drm.h>\n#ifdef __APPLE__\n#include "anv_xnu_drm_shim.h"\n#endif',
        "anv_xnu_drm_shim.h",
    )
    replace_once(
        anv_pdev,
        "   fd = open(path, O_RDWR | O_CLOEXEC);",
        "#ifdef __APPLE__\n   /* ARCXE_XNU_OPEN */\n   fd = (strcmp(path, ARC_XNU_FAKE_RENDER_NODE) == 0) ? arc_xnu_open_device() : open(path, O_RDWR | O_CLOEXEC);\n#else\n   fd = open(path, O_RDWR | O_CLOEXEC);\n#endif",
        "ARCXE_XNU_OPEN",
    )
    replace_once(
        anv_pdev,
        "   } else {\n      device->sync_syncobj_type = vk_drm_syncobj_get_type(fd);\n   }",
        "   } else {\n#ifdef __APPLE__\n      /* ARCXE_XNU_SYNC */\n      device->sync_syncobj_type = vk_drm_syncobj_get_type_from_provider(arc_xnu_sync_provider(fd));\n#else\n      device->sync_syncobj_type = vk_drm_syncobj_get_type(fd);\n#endif\n   }",
        "ARCXE_XNU_SYNC",
    )
    replace_once(
        anv_pdev,
        "   close(device->local_fd);",
        "#ifdef __APPLE__\n   /* ARCXE_XNU_CLOSE_PDEV */\n   arc_xnu_close_device(device->local_fd);\n#else\n   close(device->local_fd);\n#endif",
        "ARCXE_XNU_CLOSE_PDEV",
    )

    anv_dev = mesa / "src" / "intel" / "vulkan" / "anv_device.c"
    if anv_dev.exists():
        ad_text = anv_dev.read_text(encoding="utf-8")
        if "ARCXE_XNU_DEV_OPEN" not in ad_text:
            ad_text = '#ifdef __APPLE__\n/* ARCXE_XNU_DEV_OPEN */\n#include "anv_xnu_drm_shim.h"\n#endif\n' + ad_text
            ad_text = ad_text.replace(
                "device->fd = open(physical_device->path, O_RDWR | O_CLOEXEC);",
                "#ifdef __APPLE__\n   device->fd = arc_xnu_open_device();\n#else\n   device->fd = open(physical_device->path, O_RDWR | O_CLOEXEC);\n#endif",
            )
            ad_text = ad_text.replace(
                "close(device->fd);",
                "#ifdef __APPLE__\n   arc_xnu_close_device(device->fd);\n#else\n   close(device->fd);\n#endif",
            )
            anv_dev.write_text(ad_text, encoding="utf-8")
            print(f"  patched {anv_dev}")

    # 6. Add anv_xnu_drm_shim.c and IOKit/CoreFoundation link args in src/intel/vulkan/meson.build
    anv_meson = mesa / "src" / "intel" / "vulkan" / "meson.build"
    am_text = anv_meson.read_text(encoding="utf-8")
    if "anv_xnu_drm_shim.c" not in am_text:
        am_text = am_text.replace(
            "  'anv_device.c',",
            "  'anv_device.c',\n  'anv_xnu_drm_shim.c',",
            1,
        )
        am_text = am_text.replace(
            "libanv_common = static_library(",
            "# ARCXE_DARWIN_LINK\n"
            "if host_machine.system() == 'darwin'\n"
            "  anv_flags += ['-DSUPPORT_INTEL_INTEGRATED_GPUS=1']\n"
            "endif\n\n"
            "libanv_common = static_library(",
            1,
        )
        am_text = am_text.replace(
            "vulkan_icd_link_args,",
            "vulkan_icd_link_args, '-Wl,-framework,IOKit', '-Wl,-framework,CoreFoundation',",
            1,
        )
        anv_meson.write_text(am_text, encoding="utf-8", newline="\n")
        print(f"  patched {anv_meson}")

    print("Mesa ANV Darwin/XNU patch applied successfully.")


if __name__ == "__main__":
    main()
