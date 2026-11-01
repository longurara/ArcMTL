# Intel Arc (Xe-LPG / Meteor Lake `8086:7D55`) Metal 3 & Display Driver cho macOS 26 Tahoe & macOS 15 Sequoia

Bộ driver đồ họa **Metal 3 + Vulkan 1.3/1.4 (Mesa ANV 26.2.6) + Display (`IOFramebuffer` / `IOGraphicsAccelerator2`)** dành cho iGPU **Intel Arc Graphics** trên vi xử lý **Intel Core Ultra 7 155H** (`PCI\VEN_8086&DEV_7D55`, kiến trúc **Xe-LPG IP 12.70**, Media **Xe-LPM+ IP 13.00**, Display **IP v14**, bộ nhớ hợp nhất UMA).

Được thiết kế chuyên biệt để **chung sống an toàn 100%** với bộ driver **NullMoth NVIDIA (`RTX 4070`)** trên cả **macOS 26 Tahoe** và **macOS 15 Sequoia**:
- **Chế độ Dual-GPU (`arc_headless=1`)**: `WindowServer` tiếp tục chạy siêu mượt trên `RTX 4070`, trong khi mọi ứng dụng Metal 3 / Compute (`MTLCopyAllDevices()`) nhìn thấy và sử dụng đồng thời **cả 2 GPU** (`RTX 4070` + `Intel Arc Graphics`).
- **Chế độ Laptop Độc lập / Tiết kiệm Pin (`arcfb=1 arcaccel=1 -nvoff`)**: Tắt hoàn toàn dGPU NVIDIA, xuất hình và tăng tốc toàn bộ giao diện macOS `WindowServer` Metal 3 trực tiếp trên iGPU Intel Arc với cơ chế **Zero-Copy UMA `IOSurface` -> GGTT `PLANE_SURF` Flip**.

---

## 1. Kiến trúc Tổng thể

```
┌────────────────────────────────────────────────────────────────────────────┐
│                   Ứng dụng macOS 15+ & WindowServer (Metal 3)              │
└─────────────────────────────────────┬──────────────────────────────────────┘
                                      │ Metal.framework (MTLIOAccelDevice)
┌─────────────────────────────────────▼──────────────────────────────────────┐
│  ArcMTLDriver.bundle  (/Library/GPUBundles/ArcMTLDriver.bundle)            │
│  • Toàn bộ class ObjC đổi tên sang ArcMTL* (0 xung đột với NVMTLDriver)   │
│  • Gọi libarcmtl_translate.dylib dịch Apple AIR / Metallib -> SPIR-V       │
│  • Zero-copy UMA buffer/IOSurface qua VK_EXT_external_memory_host          │
└──────────────────┬───────────────────────────────────────┬─────────────────┘
                   │ Vulkan 1.3/1.4 C API                  │ IOSurface Flip
┌──────────────────▼──────────────────────┐   ┌────────────▼─────────────────┐
│  libvulkan_intel.dylib (Mesa ANV 26.2)  │   │  ArcAccel.kext & ArcFB.kext  │
│  • Compiler Intel BRW chuẩn cho Xe-LPG  │   │  • IOGraphicsAccelerator2    │
│  • anv_xnu_drm_shim.c dịch DRM_IOCTL_XE │   │  • Display IP 14 Scanout     │
│    sang IOConnectCallMethod / MapMemory │   │  • Zero-copy GGTT PLANE_SURF │
└──────────────────┬──────────────────────┘   └────────────┬─────────────────┘
                   │ IOUserClient (17 selectors)           │ Kernel ABI
┌──────────────────▼───────────────────────────────────────▼─────────────────┐
│                         ArcXe.kext  (ArcXeService)                         │
│  • Forcewake GT0 + Render + Media GT1, lập trình Meteor Lake PAT[0..4]     │
│  • Quản lý bộ nhớ UMA (ArcBo), GGTT (Wa_22019338487) & 48-bit 4-lvl PPGTT │
│  • Thực thi lệnh GPU trên RCS0 / CCS0 / BCS0 (Direct Execlist + GuC v70)   │
└─────────────────────────────────────┬──────────────────────────────────────┘
                                      │ PCI BAR0 (16MB MMIO+GSM) / BAR2 (256MB)
┌─────────────────────────────────────▼──────────────────────────────────────┐
│     Phần cứng: Intel(R) Arc(TM) Graphics (8086:7D55 — 8 Xe-cores, 128 EUs) │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Cấu trúc Thư mục

| Đường dẫn | Vai trò |
| :--- | :--- |
| `include/arc_regs_mtl.h` | Thanh ghi phần cứng Meteor Lake Xe-LPG (12.70), Media (13.00), Display v14, PAT, GGTT/PPGTT, LRC, GuC v70 |
| `include/arc_uapi.h` | Giao diện ABI 17 hàm giữa `ArcXe.kext` và userspace (`libvulkan_intel.dylib`, `arc_probe`) |
| `include/arc_kern_abi.h` | Giao diện Kernel-to-Kernel giữa `ArcXe.kext` ↔ `ArcFB.kext` / `ArcAccel.kext` |
| `kexts/ArcXe/` | KMD lõi quản lý phần cứng Xe-LPG, bộ nhớ UMA/PPGTT, Execlist & nạp firmware `mtl_guc_70.bin` |
| `kexts/ArcFB/` | `IOFramebuffer` kế thừa GOP EFI & điều khiển thanh ghi `PLANE_SURF_1_A` / VSync Display IP 14 |
| `kexts/ArcAccel/` | `IOGraphicsAccelerator2` cầu nối `WindowServer` & `Metal.framework` (hỗ trợ cả `arc_headless=1` và `arcaccel=1`) |
| `mesa-anv/` | Backend Darwin/XNU IOKit (`anv_xnu_drm_shim.c`) để biên dịch Mesa Intel ANV trực tiếp trên macOS |
| `plugin/` | Bộ chuyển đổi `adapt_arc_plugin.py` tạo `ArcMTLDriver.bundle` cách ly hoàn toàn khỏi `NVMTLDriver.bundle` |
| `firmware/mtl_guc_70.bin` | Firmware GuC v70 chính hãng Intel cho Meteor Lake |
| `tools/` | 3 công cụ kiểm thử theo từng tầng: `arc_probe`, `vk_offscreen_test`, `mtl_offscreen_test` |
| `package/` | Bộ cài đặt `install.sh` & gỡ cài đặt `uninstall.sh` bảo toàn tuyệt đối kext NullMoth RTX 4070 |

---

## 3. Tải gói Build sẵn từ GitHub Actions hoặc Tự Biên dịch (macOS 26 Tahoe / macOS 15 Sequoia)

### Cách A — Tải trực tiếp gói `ArcMTL-macos26-15-x86_64.tar.gz` từ GitHub Actions (Khuyên dùng)
GitHub Actions tự động biên dịch trọn bộ trên **macOS 26 Tahoe (`macos-26-intel`)** và kiểm tra liên kết kernel bằng `kmutil` trên cả **macOS 26 Tahoe** lẫn **macOS 15 Sequoia (`macos-15-intel`)**.
1. Vào tab **Actions** trên repo [`longurara/ArcMTL`](https://github.com/longurara/ArcMTL/actions), tải artifact **`ArcMTL-macos26-15-x86_64`**.
2. Giải nén trên máy Mac của bạn — bộ cài `package/install.sh` sẽ tự nhận diện `kern.osrelease` (Darwin 25.x = macOS 26 Tahoe, Darwin 24.x = macOS 15 Sequoia) để nạp đúng biến thể `ArcAccel.kext`.

### Cách B — Tự biên dịch trực tiếp trên máy Mac

#### Bước 3.1 — Cài đặt các gói công cụ build qua Homebrew
```bash
xcode-select --install 2>/dev/null || true
brew install ccache rust meson ninja pkgconf bison flex glslang llvm spirv-llvm-translator spirv-tools libclc
python3 -m pip install --user --break-system-packages mako pyyaml packaging
```

#### Bước 3.2 — Tải mã nguồn phụ thuộc & tự động patch
```bash
bash scripts/fetch_deps.sh
```
Lệnh này tải `Vulkan-Headers`, `Mesa 26.2.6`, và sparse-checkout `translator/` + `plugin/` của NullMoth vào `third_party/`, sau đó tự động gắn `anv_xnu_drm_shim.c` vào Mesa và sinh mã nguồn `ArcMTLDriver` (đã đổi tên toàn bộ class sang `ArcMTL*`).

#### Bước 3.3 — Biên dịch toàn bộ Kexts + Vulkan ANV + Metal Bundle + Tools
```bash
bash build/build_all.sh
```
Toàn bộ sản phẩm sau khi build sẽ nằm gọn trong thư mục `out/`:
- `out/kexts/{ArcXe.kext, ArcFB.kext, ArcAccel.kext}` (kèm `out/kexts/26/ArcAccel.kext` & `out/kexts/15/ArcAccel.kext`)
- `out/fw/mtl_guc_70.bin`
- `out/arcmtl/{libvulkan_intel.dylib, libarcmtl_translate.dylib, intel_icd.json}`
- `out/GPUBundles/{ArcMTLDriver.bundle, arcmtl-allow.txt}`
- `out/tools/{arc_probe, vk_offscreen_test, mtl_offscreen_test}`

---

## 4. Quy trình Kiểm thử 4 Giai đoạn An toàn Tuyệt đối (Zero-Risk Protocol)

> **Lưu ý BIOS/OpenCore trước khi test**: Hãy chắc chắn trong BIOS iGPU Intel Arc đang được bật (**IGPU Multi-Monitor / Integrated Graphics = Enabled**) để macOS nhìn thấy thiết bị PCI `8086:7D55` cùng lúc với `RTX 4070`. Hệ thống của bạn đã có sẵn SIP `0x0A43` và `AMFIPass` từ NullMoth nên không cần chỉnh lại SIP.

### Giai đoạn 1 — Kiểm tra Phần cứng & Thực thi Lệnh GPU Kernel (`ArcXe.kext`)
Ở giai đoạn này, **chỉ nạp `ArcXe.kext`** (không nạp `ArcFB` hay `ArcAccel`). Màn hình macOS của bạn vẫn chạy 100% trên RTX 4070.
```bash
sudo bash package/install.sh --stage1
```
Nếu macOS hỏi quyền trong **System Settings → Privacy & Security**, bấm **Allow** rồi khởi động lại máy (không cần sửa `boot-args`). Sau khi vào macOS, chạy:
```bash
./out/tools/arc_probe
```
**Kết quả mong đợi**:
- In đúng `PCI Identity: 8086:7d55`, `Graphics IP: 12.70 — Xe-LPG`, `Media IP: 13.00 — Xe-LPM+`, `8 Xe-cores, 128 Vector Engines`.
- Tạo 48-bit PPGTT VM, gửi batch lệnh GPU `MI_STORE_DATA_IMM` vào `RCS0`, đợi fence hoàn tất và đọc zero-copy từ RAM UMA ra `0xa2c7d550155cafe0` → báo `[PASS] Stage 1 PASSED`!

### Giai đoạn 2 — Kiểm tra Vulkan 1.3 (Mesa ANV) Offscreen
Ngay trong cùng phiên khởi động của Giai đoạn 1, chạy tiếp:
```bash
./out/tools/vk_offscreen_test
```
**Kết quả mong đợi**:
- Nạp `libvulkan_intel.dylib`, nhận diện `Intel(R) Arc(TM) Graphics (8086:7d55)`, chạy lệnh GPU `vkCmdFillBuffer` trên 64 KiB bộ nhớ UMA, đồng bộ qua `VkFence` và báo `[PASS] Stage 2 PASSED`!

### Giai đoạn 3 — Chế độ Song song 2 GPU Metal 3 (`RTX 4070` + `Intel Arc`)
Cài đặt đầy đủ cả 3 kexts + Metal Bundle:
```bash
sudo bash package/install.sh
```
Thêm **`arc_headless=1`** vào `boot-args` trong `config.plist` của OpenCore (giữ nguyên các flag NullMoth hiện tại của RTX 4070) rồi khởi động lại.
- `WindowServer` tiếp tục vẽ màn hình trên **RTX 4070**.
- Chạy công cụ kiểm thử Metal 3:
```bash
./out/tools/mtl_offscreen_test
```
**Kết quả mong đợi**:
- `MTLCopyAllDevices()` liệt kê **cả 2 GPU** cùng lúc: `NVMTLDevice` (RTX 4070) và `ArcMTLDevice` (Intel Arc Graphics)!
- Biên dịch shader Metal (MSL) lúc chạy sang SPIR-V, tính toán FMA chuẩn xác trên 4096 phần tử float và render pass vào `IOSurface` → báo `[PASS] Stage 3 PASSED`!

### Giai đoạn 4 — Chế độ Laptop Độc lập / Tiết kiệm Pin trên Intel Arc
Khi muốn chạy hoàn toàn bằng iGPU Intel Arc trên màn hình laptop (tắt RTX 4070):
- Đổi `boot-args` trong OpenCore sang:
  ```text
  arcfb=1 arcaccel=1 -nvoff
  ```
  *(trong đó `-nvoff` hoặc flag tắt dGPU tương ứng tạm ngắt driver NVIDIA, còn `arcfb=1 arcaccel=1` kích hoạt `ArcFB` + `ArcAccel` cho `WindowServer`)*.

---

## 5. Tham chiếu `boot-args` & Cứu hộ Tức thì

| Boot-arg | Tác dụng |
| :--- | :--- |
| *(mặc định, không thêm flag)* | Chỉ `ArcXe.kext` hoạt động nền; `ArcFB` và `ArcAccel` tự động nhường quyền (`probe` trả về `nullptr`) |
| `arc_headless=1` | Bật Metal 3 trên Intel Arc ở chế độ **Dual-GPU** (ứng dụng & compute dùng được cả Arc lẫn RTX 4070; `WindowServer` giữ trên RTX 4070) |
| `arcfb=1` | Bật `ArcFB.kext` quản lý màn hình iGPU Intel Arc |
| `arcaccel=1` | Bật `ArcAccel.kext` đầy đủ cho `WindowServer` compositing trên Intel Arc |
| `arc_execlist=1` | Ép `ArcXe.kext` dùng Direct Hardware Execlist (bỏ qua nạp GuC v70 nếu cần debug lệnh phần cứng) |
| **`-arcoff`** | **Công tắc khẩn cấp**: Vô hiệu hóa lập tức cả 3 kext (`ArcXe`, `ArcFB`, `ArcAccel`) ngay từ menu OpenCore |

### Gỡ cài đặt sạch (Giữ nguyên 100% driver NullMoth RTX 4070)
```bash
sudo bash package/uninstall.sh
```
Hoặc khôi phục nguyên trạng file `AuxiliaryKernelExtensions.kc` từ bản sao lưu tự động trước khi cài:
```bash
sudo bash package/uninstall.sh --restore-backup
```

---

## 6. Bản quyền & Giấy phép (Licensing & Attribution)

Dự án này được phát hành theo hướng nghiên cứu cá nhân / phi thương mại (**Option A**), tuân thủ nghiêm ngặt giấy phép của từng thành phần nguồn:
- **PolyForm Noncommercial License 1.0.0**: Áp dụng cho phần mã thích ứng từ `nullmoth/nvidia-macos-driver` (`ArcAccel`, `ArcFB`, `ArcMTLDriver`).
  - `Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)`
- **GNU Lesser General Public License v3.0 (LGPL-3.0)**: Bộ dịch AIR/Bitcode sang SPIR-V (`libarcmtl_translate.dylib`, dẫn xuất từ `metal2vulkan`).
- **MIT License**: `ArcXe.kext`, `anv_xnu_drm_shim.c`, các công cụ kiểm thử trong `tools/`, và Mesa Intel ANV (`libvulkan_intel.dylib`).
- **Intel Firmware License**: `firmware/mtl_guc_70.bin` (từ `linux-firmware/i915`).

Xem chi tiết đầy đủ trong [LICENSE](./LICENSE) và [NOTICE](./NOTICE).
