# ArcMTL — Intel Arc (Xe-LPG / Meteor Lake) Metal 3, Vulkan 1.3 & Display Driver for macOS 26 Tahoe & macOS 15 Sequoia

**ArcMTL** is an experimental **Metal 3 + Vulkan 1.3/1.4 (Mesa ANV 26.2.6) + Display (`IOFramebuffer` / `IOGraphicsAccelerator2`)** graphics driver stack for **Intel Arc Integrated Graphics (Xe-LPG)** on **macOS 26 Tahoe** (`Darwin 25.x`, `x86_64`) and **macOS 15 Sequoia** (`Darwin 24.x`, `x86_64`).

Primary bring-up target: **Intel Core Ultra 7 155H — Intel(R) Arc(TM) Graphics (`PCI 8086:7D55`)** featuring **Xe-LPG Graphics IP 12.70 / 12.71** (8 Xe-cores, 128 Vector Engines), **Xe-LPM+ Media IP 13.00**, **Xe-LPD+ Display IP v14**, and Unified Memory Architecture (UMA).

Designed from the ground up for **100% safe co-existence** alongside the **NullMoth NVIDIA driver** (e.g., `GeForce RTX 4070 Laptop GPU`) on hybrid **Optimus** laptops:
- **Dual-GPU Metal 3 Mode (`arc_headless=1`)**: `WindowServer` runs on the boot display (`IONDRVFramebuffer` in Optimus mode or `NVRMFB` in dGPU/external-display mode), while all macOS Metal 3 & Compute apps (`MTLCopyAllDevices()`) simultaneously discover and use **both GPUs** (`NVIDIA GeForce RTX 4070` + `Intel(R) Arc(TM) Graphics`).
- **Full Intel Arc Metal 3 Desktop Mode (`arcfb=1 arcaccel=1`)**: `ArcFB.kext` + `ArcAccel.kext` take over the internal eDP panel (`2560x1600`, 60–240 Hz) with full Metal 3 `WindowServer` compositing, **zero-copy UMA `IOSurface` -> GGTT `PLANE_SURF` VBlank page-flipping**, and native macOS brightness control via South Display PWM.

---

## 1. Architecture Overview

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                  macOS 15 / 26 Apps & WindowServer (Metal 3)                 │
└──────────────────────────────────────┬───────────────────────────────────────┘
                                       │ Metal.framework (MTLIOAccelDevice)
┌──────────────────────────────────────▼───────────────────────────────────────┐
│  ArcMTLDriver.bundle  (/Library/GPUBundles/ArcMTLDriver.bundle)              │
│  • Full ObjC/C++ symbol isolation (ArcMTL* prefix — zero clash w/ NullMoth)  │
│  • Translates Apple AIR / Metallib bitcode -> SPIR-V (libarcmtl_translate)   │
│  • Zero-copy UMA buffer & IOSurface import via VK_EXT_external_memory_host   │
└───────────────────┬────────────────────────────────────────┬─────────────────┘
                    │ Vulkan 1.3/1.4 C API                   │ IOSurface Flip
┌───────────────────▼───────────────────────┐   ┌────────────▼─────────────────┐
│  libvulkan_intel.dylib (Mesa ANV 26.2.6)  │   │  ArcAccel.kext & ArcFB.kext  │
│  • Native Intel BRW compiler for Xe-LPG   │   │  • Dual-ABI (macOS 15 & 26)  │
│  • anv_xnu_drm_shim.c maps DRM_IOCTL_XE_* │   │  • Display IP 14 DSM Scanout │
│    to IOUserClient + IOConnectMapMemory64 │   │  • Zero-copy GGTT PLANE_SURF │
└───────────────────┬───────────────────────┘   └────────────┬─────────────────┘
                    │ IOUserClient (17 selectors)            │ Kernel ABI
┌───────────────────▼────────────────────────────────────────▼─────────────────┐
│                          ArcXe.kext  (ArcXeService)                          │
│  • Forcewake (GT0 + Render + Media GT1), Xe-LPG PAT[0..4] coherency table    │
│  • DSM IODeviceMemory registration for UEFI GOP -> IOBootNDRV handoff        │
│  • UMA GEM BOs, 4 GiB GGTT (Wa_22019338487) & 48-bit 4-level PML4 PPGTT      │
│  • Unified 3D + Compute submission on RCS0 & BCS0 (Direct Execlist / GuC 70) │
└──────────────────────────────────────┬───────────────────────────────────────┘
                                       │ BAR0 (16 MiB MMIO+GSM) + DSM (128 MiB)
┌──────────────────────────────────────▼───────────────────────────────────────┐
│      Hardware: Intel(R) Arc(TM) Graphics (8086:7D55 — 8 Xe-cores, 128 EUs)   │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Supported PCI Device IDs (Xe-LPG / Display IP v14)

| PCI ID | Product Family | Graphics / Display IP |
| :--- | :--- | :--- |
| `8086:7D55` | Intel Core Ultra H-Series — **Intel(R) Arc(TM) Graphics** (155H / 185H, 8 Xe-cores) | Xe-LPG `12.70` / `12.71`, Display `v14` |
| `8086:7DD5` | Intel Core Ultra H-Series Arc Graphics | Xe-LPG `12.70` / `12.71`, Display `v14` |
| `8086:7D40` / `7D45` / `7D60` | Intel Core Ultra U / H-Series Intel Graphics (4 Xe-cores) | Xe-LPG `12.70` / `12.71`, Display `v14` |
| `8086:7D41` / `7D51` / `7D67` / `7DD1` | Intel Core Ultra 200U / 200H / 200S (Arrow Lake Xe-LPG+) | Xe-LPG `12.74`, Display `v14` |

---

## 2. Repository Layout

| Path | Description |
| :--- | :--- |
| `include/arc_regs_mtl.h` | Hardware register definitions for Xe-LPG (`12.70`/`12.71`), Media (`13.00`), Display `v14`, PAT, GGTT/PPGTT, LRC, and GuC v70 |
| `include/arc_uapi.h` | 17-selector user/kernel ABI shared between `ArcXe.kext`, `anv_xnu_drm_shim.c`, and `arc_probe` |
| `include/arc_kern_abi.h` | In-kernel `callPlatformFunction` ABI between `ArcXe.kext`, `ArcFB.kext`, and `ArcAccel.kext` |
| `kexts/ArcXe/` | Core KMD (`BootKC`-compatible: links only `IOPCIFamily` + XNU KPIs). Manages Forcewake, PAT, DSM GOP handoff, UMA BOs, GGTT/PPGTT, Execlist & optional GuC v70 |
| `kexts/ArcFB/` | Display IP v14 `IOFramebuffer` (`AuxKC`): resolves GOP scanout via DSM GGTT PTEs & `PE_state.video`, measures `PIPE_FRMCOUNT` refresh rate, drives PWM backlight & VBlank flips |
| `kexts/ArcAccel/` | Dual-ABI `IOGraphicsAccelerator2` bridge (`ARC_TAHOE=0` for macOS 15 Sequoia, `ARC_TAHOE=1` for macOS 26 Tahoe) |
| `mesa-anv/` | Darwin/XNU IOKit backend (`anv_xnu_drm_shim.c`) enabling native Mesa 26.2.6 Intel ANV compilation on macOS (`<os/os_sync_wait_on_address.h>` futexes + zero-copy UMA mapping) |
| `plugin/` | `adapt_arc_plugin.py` generator producing `ArcMTLDriver.bundle` with complete ObjC class & C symbol isolation from `NVMTLDriver.bundle` |
| `firmware/mtl_guc_70.bin` | Official Intel Meteor Lake GuC v70 firmware binary (`linux-firmware/i915`) |
| `tools/` | Staged hardware verification tools: `arc_probe`, `vk_offscreen_test`, and `mtl_offscreen_test` |
| `package/install_opencore_userspace.sh` | Hybrid OpenCore (`ArcXe.kext` in `EFI/OC/Kexts`) + `AuxKC` (`ArcFB.kext` & `ArcAccel.kext`) + Userspace installer preserving 100% of NullMoth |
| `package/install.sh` / `uninstall.sh` | Standalone `AuxKC` installer and atomic rollback/uninstaller for macOS 15 & macOS 26 |

---

## 3. Why Hybrid OpenCore + AuxKC on macOS 15 & 26?

On Intel `x86_64` macOS 15 Sequoia and macOS 26 Tahoe:
1. **`BootKernelExtensions.kc`** contains `com.apple.iokit.IOPCIFamily`, whereas `IOGraphicsFamily`, `IONDRVSupport`, `IOAcceleratorFamily2`, and `IOSurface` reside in **`SystemKernelExtensions.kc`**.
   - Consequently, **`ArcXe.kext`** can be injected directly by OpenCore (`EFI/OC/Kexts/ArcXe.kext`) at early PCI configuration time, while **`ArcFB.kext`** and **`ArcAccel.kext`** are linked into **`/Library/KernelCollections/AuxiliaryKernelExtensions.kc`** via `kmutil` alongside any existing NullMoth `NV*.kext`.
2. **Meteor Lake UEFI GOP Data Stolen Memory (`DSM`) Handoff**:
   - Unlike legacy Intel Gen9–Gen12 iGPUs, Meteor Lake Xe-LPG has **no CPU-mappable GGTT aperture on `BAR2`**. UEFI GOP places `PE_state.video.v_baseAddr` directly in **Data Stolen Memory** (e.g., `0x78800000`), outside `BAR0` (`0xA0000000`) and `BAR2` (`0x90000000`).
   - During early PCI bring-up, `ArcXe.kext` automatically appends the hardware `DSM` region (`[DSMBASE .. +GMS)`) to `IOPCIDevice`'s `IODeviceMemory` array so Apple's `IOBootNDRV::fromRegistryEntry` (`IONDRVFramebuffer`) immediately recognizes the internal eDP panel in Optimus mode even before `ArcFB.kext` is armed.

---

## 4. Getting Prebuilt Releases (GitHub Actions) or Building from Source

### Option A — Download Prebuilt Artifacts from GitHub Actions (Recommended)

Every push to `main` runs `.github/workflows/build.yml`, verifying kernel linking via Apple `kmutil` on **both macOS 26 Tahoe (`macos-26-intel`) and macOS 15 Sequoia (`macos-15-intel`)**:
- **`ArcMTL-macos26-15-x86_64`**: Complete release tarball containing all 4 kext variants (`ArcXe.kext`, `ArcFB.kext`, `15/ArcAccel.kext`, `26/ArcAccel.kext`), `mtl_guc_70.bin`, Mesa 26.2.6 `libvulkan_intel.dylib`, `libarcmtl_translate.dylib`, `ArcMTLDriver.bundle`, diagnostic tools, and installers.
- **`ArcKexts-macos26-15-x86_64`**: Fast (~25s) `kmutil`-verified kernel-extension-only tarball for rapid kernel development; `install_opencore_userspace.sh` automatically overlays it onto `ArcMTL-macos26-15-x86_64.tar.gz` when placed in the same directory.

### Option B — Build Locally on macOS (15 Sequoia or 26 Tahoe)

```bash
# 1. Install build toolchain
xcode-select --install 2>/dev/null || true
brew install ccache rust meson ninja pkgconf bison flex glslang llvm spirv-llvm-translator spirv-tools libclc
python3 -m pip install --user --break-system-packages mako pyyaml packaging

# 2. Fetch & patch third-party dependencies (Vulkan-Headers, Mesa 26.2.6, translator/plugin)
bash scripts/fetch_deps.sh

# 3. Build kexts (macOS 15 & 26), Rust SPIR-V translator, Mesa ANV, ArcMTLDriver.bundle & tools
bash build/build_all.sh
```

Build outputs are staged cleanly under `out/`:
- `out/kexts/{ArcXe.kext, ArcFB.kext, 15/ArcAccel.kext, 26/ArcAccel.kext}`
- `out/fw/mtl_guc_70.bin`
- `out/arcmtl/{libvulkan_intel.dylib, libarcmtl_translate.dylib, intel_icd.json}`
- `out/GPUBundles/{ArcMTLDriver.bundle, arcmtl-allow.txt}`
- `out/tools/{arc_probe, vk_offscreen_test, mtl_offscreen_test}`

---

## 5. Installation & Staged Hardware Bring-Up

### Prerequisites (BIOS & OpenCore)
1. **BIOS**: Enable **Optimus / Hybrid Graphics** (so PCI `00:02.0` `8086:7D55` is visible alongside your dGPU).
2. **OpenCore `config.plist`**:
   - Remove any `disable-gpu` property under `DeviceProperties -> Add -> PciRoot(0x0)/Pci(0x2,0x0)` (and remove `-wegnoigpu` from `boot-args` if present).
   - Inject `ArcXe.kext` under `Kernel -> Add` (`MinKernel: 24.0.0`, `Arch: x86_64`).
   - Ensure `com.apple.iokit.IONDRVSupport` is **not** blocked under `Kernel -> Block` for initial Optimus boot so `IONDRVFramebuffer` brings up the desktop GUI via `ArcXe`'s DSM handoff.

### Workflow A — Hybrid OpenCore + NullMoth Optimus Setup (Recommended)

Copy `install_opencore_userspace.sh` and `ArcMTL-macos26-15-x86_64.tar.gz` (plus optional `ArcKexts-macos26-15-x86_64.tar.gz`) to `/Volumes/EFI/ArcMTL/`, boot into macOS, and run:

```bash
sudo diskutil mount EFI
sudo bash /Volumes/EFI/ArcMTL/install_opencore_userspace.sh --enable-dual-metal
```

This single command:
1. Installs `/Library/ArcDriver/fw/mtl_guc_70.bin`, `/Library/GPUBundles/arcmtl/*`, `/Library/GPUBundles/ArcMTLDriver.bundle`, and `/usr/local/bin/{arc_probe,vk_offscreen_test,mtl_offscreen_test}`.
2. Immediately executes **`arc_probe`** (48-bit PPGTT + live `RCS0` `MI_STORE_DATA_IMM` execution across 128 EUs) and **`vk_offscreen_test`** (Mesa 26.2.6 ANV Vulkan 1.3 offscreen render to `/var/tmp/arc_vk_test.ppm`).
3. Pre-flights `kmutil create -n aux --allow-missing-kdk` in `/var/tmp` — aborting before touching disk if any existing `com.nullmoth.*` kext or `com.arc.*` kext is missing — then atomically updates `AuxiliaryKernelExtensions.kc`, syncs Preboot via `/usr/sbin/kcditto`, and sets `arc_headless=1` in OpenCore `config.plist`.

Reboot and verify Dual-GPU Metal 3:
```bash
/usr/local/bin/mtl_offscreen_test /var/tmp/arc_mtl_test.ppm
```

Once verified, switch `WindowServer` to full Intel Arc Metal 3 Desktop acceleration (`ArcFB` zero-copy VBlank page-flips + PWM backlight) anytime via:
```bash
sudo diskutil mount EFI
sudo bash /Volumes/EFI/ArcMTL/install_opencore_userspace.sh --enable-arc-desktop
sudo reboot
```

### Workflow B — Standalone AuxKC Installation (`package/install.sh`)

If you prefer installing all three kexts (`ArcXe.kext`, `ArcFB.kext`, `ArcAccel.kext`) directly into `/Library/Extensions` + `AuxiliaryKernelExtensions.kc`:

```bash
# Stage 1 & 2 only (ArcXe.kext + Vulkan 1.3 userspace; leaves display stack untouched)
sudo bash package/install.sh --stage1

# Full stack (ArcXe + ArcFB + ArcAccel [auto-selects macOS 15 or 26 ABI] + Metal 3 bundle)
sudo bash package/install.sh
```

---

## 6. Boot Arguments & Emergency Recovery

| Boot Argument | Description |
| :--- | :--- |
| *(default — no flags)* | `ArcXe.kext` initializes Xe-LPG hardware, GGTT/PPGTT, and DSM GOP handoff; `ArcFB` and `ArcAccel` stay idle |
| `arc_headless=1` | Enables `ArcAccel.kext` in **Dual-GPU Headless Metal 3 Mode** (`MTLCopyAllDevices()` exposes both dGPU and Intel Arc while `WindowServer` stays on the boot framebuffer) |
| `arcfb=1` | Enables `ArcFB.kext` native Display IP v14 framebuffer takeover, `PIPE_FRMCOUNT` VBlank pacing, and South Display PWM brightness |
| `arcaccel=1` | Enables `ArcAccel.kext` full `WindowServer` desktop compositing & DisplayPipe transactions on `ArcFB` |
| `arc_guc=1` | Enables asynchronous GuC v70 firmware loading (`mtl_guc_70.bin`) & CTB submission (defaults to **Direct Hardware Execlist** when omitted) |
| `-arcnoflip` | Forces `ArcDisplayPipe` to use Write-Combined CPU scanout copy instead of GGTT `PLANE_SURF` page-flipping |
| `-arcmanualfb` | Defers `ArcAccel` DisplayPipe arming until `sudo sysctl -w debug.arcaccelfb=1` is run manually |
| `-arcfboff` | Disables `ArcFB.kext` only while keeping `ArcXe.kext` / `ArcAccel.kext` active |
| **`-arcoff`** | **Emergency kill-switch**: completely disables `ArcXe`, `ArcFB`, and `ArcAccel` from the OpenCore boot picker |

### Clean Uninstall / Atomic Rollback

```bash
# Remove Arc kexts & GPUBundles while preserving 100% of NullMoth kexts in AuxKC
sudo bash package/uninstall.sh

# Or restore the exact pre-install AuxiliaryKernelExtensions.kc snapshot
sudo bash package/uninstall.sh --restore-backup
```

---

## 7. Licensing & Attribution

This project is developed for personal, academic, and non-commercial research (**Option A**), strictly complying with the license terms of each upstream component:

- **PolyForm Noncommercial License 1.0.0**: Applies to components adapted from `nullmoth/nvidia-macos-driver` (`ArcAccel`, `ArcFB`, `ArcMTLDriver`).
  - `Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)`
- **GNU Lesser General Public License v3.0 (LGPL-3.0)**: Applies to the AIR/Bitcode-to-SPIR-V translator library (`libarcmtl_translate.dylib`, derived from `metal2vulkan`).
- **MIT License**: Applies to `ArcXe.kext`, `include/arc_regs_mtl.h`, `include/arc_uapi.h`, `mesa-anv/anv_xnu_drm_shim.c`, `tools/*`, and Mesa Intel ANV (`libvulkan_intel.dylib`).
- **Intel Firmware License**: Applies to `firmware/mtl_guc_70.bin` (redistributed unmodified from `linux-firmware/i915`).

See [LICENSE](./LICENSE) and [NOTICE](./NOTICE) for full license texts and attributions.
