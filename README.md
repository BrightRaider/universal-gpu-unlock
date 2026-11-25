<div align="center">

# Universal GPU Unlock (One-Click UEFI Tool)
### High-Performance Tensor Core Rate-Limiter Bypass for NVIDIA GPUs

[![GitHub Release](https://img.shields.io/github/v/release/BrightRaider/universal-gpu-unlock?color=blue&logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest)
[![Release Version](https://img.shields.io/badge/Release-v2.0.0-blue?logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v2.0.0)
[![Downloads](https://img.shields.io/github/downloads/BrightRaider/universal-gpu-unlock/total?color=green&logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases)
[![Stars](https://img.shields.io/github/stars/BrightRaider/universal-gpu-unlock?style=social)](https://github.com/BrightRaider/universal-gpu-unlock)
[![Safety](https://img.shields.io/badge/Safety-RAM--Only-success.svg)](#-safety--anti-cheat)
[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20Me%20A%20Coffee-ff5e5b?style=flat&logo=kofi&logoColor=white)](https://ko-fi.com/brightraider)

<br>

<a href="https://ko-fi.com/brightraider" target="_blank">
  <img src="https://storage.ko-fi.com/cdn/kofi2.png?v=3" height="38" alt="Buy Me a Coffee at ko-fi.com" />
</a>

<p>
  <b>An automated, universal 1-click UEFI pre-boot unlocker for NVIDIA RTX 40 (Ada), RTX 30 (Ampere) & RTX 20 (Turing) series.</b><br>
  Unlocks full enterprise-grade Tensor Core compute across Windows 10/11, PyTorch, CUDA, and WSL2.
</p>

<br>

<img src="docs/images/dashboard-v2.0.0.png" alt="Universal GPU Unlock v2.0.0 Dashboard — Live S3 ARMED Telemetry &amp; 1-W Power-Limit Control" width="760" />

</div>

---

## ⚡ 60-Second Quick Start

1. **Prepare USB Drive:** Download **[Universal_GPU_Unlock_v2.0.0.zip](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest)**, extract it, insert a FAT32 USB drive, and run `Setup.bat` (`[1]` new USB setup with BitLocker/Secure Boot/S0ix pre-flight checks, `[2]` update existing USB, `[3]` create hidden 512 MB internal `GPU_UNLOCK` partition).
2. **Boot & Unlock:** Restart your PC, open your boot menu (`F8` / `F11` / `F12`), boot from the USB drive, and select:
   - `[U]` **Standard Unlock:** RAM-only, temporary unlock (reverts cleanly to factory stock on power off). Multi-GPU systems unlock all cards sequentially.
   - `[P]` **S3 Persistence:** Verified sleep-aware unlock mode via Tier 1 Boot-Script / Tier 2 ACPI FACS Shadow Trampoline & Reserved Arena with dynamic Intel/AMD/PCIe-switch topology, `RootBridgeIo` bus walk, dynamic Ada VBIOS/chip geometry, S0ix preflight check, and D3cold gate (preserves Tensor Core unlock across ACPI S3 Suspend-to-RAM cycles).
   - `[I]` **Install to NVRAM:** Permanent stickless auto-unlock on every cold boot via `AdaUnlockDxe.efi` (`Driver####`), including `ReadyToBoot` + `StartImage` (`EfiLoaderCode`) live-refreshed Plan-C S3 activation. Verifies free bytes (`EFI_FILE_SYSTEM_INFO`), creates `\EFI\Drivers` and asset directories recursively (`OpenOrCreateDirRecursive()`), streams files in `1 MiB` chunks, and selects internal non-USB FAT32 targets in priority order: Primary Windows ESP -> `GPU_UNLOCK` / `UNLOCK_ESP` -> small internal FAT32 (`<= 2 GiB`).
   - `[R]` **Remove DXE Driver:** Cleanly uninstalls and deregisters the driver from UEFI NVRAM and all internal volumes.
   - `[S]` **Stock Windows:** Diagnostic hardware scan, logs to USB, normal boot without unlock.  
   *(Tip: The boot menu features a 30-second countdown. Press `[Space]` at any time to pause and read in peace).*
3. **Verify & Control:** In Windows, run `Launch-Dashboard.bat` (GUI Cockpit) or `Check-GPU-Unlock.bat` (CLI) to verify unlocked Tensor Core throughput!
   - **Dashboard Cockpit (`v2.0.0`):** Includes a live **`S3 ARMED` status indicator** (`GetPwrCapabilities` + kernel `FADT` + `RUN*.TXT`), a **stepless 1-W Power-Limit slider** with clickable **`Eco` / `Stock` / `Max`** snap markers + readback verification + optional **Re-apply PL on S3 Wake** (plus *Gaming / Quiet* and *AI / Compute* 1-click profiles), Before/After benchmark baseline comparison, and 1-click launchers for all utilities in `tools/`.

> [!TIP]
> **Troubleshooting & Optimization:** If your display remains blank on boot, set **CSM to Disabled** in your BIOS. To unlock maximum memory throughput (+10% to +15%), enable **Above 4G Decoding** and **Resizable BAR**.

---

## 🔒 Secure Boot Configuration

If your motherboard blocks the USB drive or DXE driver with a signature verification warning:

- **Without Anti-Cheat (easiest):** Temporarily set **Secure Boot** to **Disabled** (or OS Type: *Other OS*) in your BIOS.
- **Keep Secure Boot enabled (Windows 11):**  
  In your BIOS under **Security ➔ Secure Boot ➔ Key Management** (or *Custom Key Management*), select **Append / Enroll DB Key** (Authorized Signatures `db`), browse to the USB drive, and add `AdaUnlock_SecureBoot.cer` **without deleting your existing factory keys**. Save and reboot (`F10`).

### Riot Vanguard (*Valorant* / *League of Legends*)

1. **Keep Secure Boot enabled** with factory keys intact (if your BIOS blocks the USB drive or `AdaUnlockDxe.efi`, **append** `AdaUnlock_SecureBoot.cer` to `db` — never erase or replace the factory keys).
2. **Install `[I]` once:** Boot from the USB drive (`F8` / `F11` / `F12`), press **`[I]`** to register `AdaUnlockDxe.efi` in UEFI NVRAM, and let the PC reboot.
3. **Boot Windows normally (never boot via USB):** Always start Windows directly via **Windows Boot Manager** rather than selecting the USB boot option. *(If your 100 MB Windows ESP is too small for Ada/Ampere assets, run `Setup.bat` option `[3]` / `tools\Create-Unlock-Partition.bat` to create a hidden 512 MB FAT32 `GPU_UNLOCK` partition for stickless `[I]` — just make sure Windows boots from the internal Windows Boot Manager.)*
4. **S3 Sleep + Riot Vanguard (`v1.9.4+` / `v2.0.0`, bench-verified):** `AdaUnlockDxe.efi` calls `GuAcpiRefreshShadowChain()` inside `StartImage` (`EfiLoaderCode`) so `ShadowFADT`/`ShadowXSDT`/`ShadowRSDP` are freshly cloned from final post-BDS firmware ACPI tables right before `bootmgfw.efi` / `winload.efi`. Confirmed on an RTX 4090 bench (`RUN0105.TXT`) with `Confirm-SecureBootUEFI: True`, `FirmwareCtrl = 0xDC637400` (`ShadowFACS`), clean Riot Vanguard / *League of Legends* runs before and after a ~9-hour S3 sleep (`02:44:12` -> `11:40:57`), and post-wake cuBLAS Tensor Core throughput of **247.43 TFLOPS** (`FP16 COMPUTE_16F`, `4096x4096`) / **232.72 TFLOPS** (`FP16 COMPUTE_32F`). *(Requires classic ACPI S3 enabled in BIOS, not Modern Standby `S0ix`. Ampere/Turing/multi-GPU S3 logs welcome via GitHub Issues!)*
5. **Stickless `[I]` Multi-Volume Fallback & Empty-Partition Fix (`v1.9.5` -> `v2.0.0`, bench-verified):** `src/driver_nvram.c` inspects free bytes via `EFI_FILE_SYSTEM_INFO` (`FsInfoGuid`) and selects internal non-USB FAT32 targets in priority order: Primary ESP -> `GPU_UNLOCK`/`UNLOCK_ESP` (or existing `\EFI\Drivers\AdaUnlockDxe.efi`) -> small internal FAT32 (`<= 2 GiB`), with automatic partial-write rollback on `VOLUME_FULL`, recursive directory creation (`OpenOrCreateDirRecursive()`), `1 MiB` streaming chunked copy, and multi-volume `[R]` cleanup. `Setup.bat [3]` (`tools/Create-Unlock-Partition.ps1 -Hidden`) creates a `512 MB FAT32` `GPU_UNLOCK` partition with `NoDefaultDriveLetter` (`$true`) and no Windows drive letter. Verified via `tests/Run-MsvcHostTests.ps1` (`MSVC_HOST_TESTS=PASS`) and live on RTX 4090 (`RUN0110.TXT` & final `v2.0.0` stickless build, `25.11.2026`): `[I]` deployed to hidden `GPU_UNLOCK` (100 MB ESP untouched), followed by a clean stickless reboot with zero USB drives mounted (`LastBootUpTime: 25.11.2026 13:51:31`, `Confirm-SecureBootUEFI: True`, `FirmwareCtrl = 0xDC637400`, `vgk` Running + `vgtray` active, **224.52 TFLOPS** `FP16`, `FMLA32/16 0.59x`) and a stickless ACPI S3 sleep/wake cycle (`13:58:53` wake, `vgk` Running + `vgtray` + `Riot Client` active, **221.19 TFLOPS** `FP16` with live `16%` external-load notice). *(Note: if Windows Fast Startup `0xC00000D4` stops `vgk.sys` after an unexpected reset/power loss, perform one normal Windows Restart.)*

---

## 🚀 Supported Hardware

| Generation | Architecture | Supported Graphics Cards | Device IDs |
| :--- | :--- | :--- | :--- |
| **RTX 40 Series** | **Ada Lovelace** | RTX 4090, RTX 4090D, RTX 4080, RTX 4080 SUPER, RTX 4070 Ti SUPER, RTX 4070 Ti, RTX 4070 SUPER, RTX 4070, RTX 4060 Ti (8GB/16GB), RTX 4060 | `0x2684`, `0x2685`, `0x2702`, `0x2703`, `0x2704`, `0x2705`, `0x2782`, `0x2783`, `0x2786`, `0x2803`, `0x2805`, `0x2807`, `0x2860`, `0x2882`, `0x28a0` |
| **RTX 30 Series** | **Ampere** | RTX 3090, RTX 3090 Ti, RTX 3080 Ti, RTX 3080 (10/12GB), RTX 3070 Ti, RTX 3070, RTX 3060 Ti, RTX 3060, RTX 3050 (8GB/Laptop), RTX 2050 Mobile, CMP 90HX | `0x2204`, `0x2208`, `0x2206`, `0x220a`, `0x2216`, `0x2207`, `0x2482`, `0x2484`, `0x2487`, `0x2488`, `0x2489`, `0x2503`, `0x2504`, `0x2507`, `0x2508`, `0x2520`, `0x2560`, `0x2582`, `0x25ec`, `0x25ed` |
| **RTX 20 Series** | **Turing** (TU102 / TU104 / TU106, verified: [#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15), [#18](https://github.com/BrightRaider/universal-gpu-unlock/issues/18)) | RTX 2080 Ti, TITAN RTX, RTX 2080 / SUPER, RTX 2070 / SUPER, RTX 2060 / SUPER, CMP 50HX, CMP 40HX, Quadro RTX | `0x1E04`, `0x1E07`, `0x1E02`, `0x1E81`, `0x1E82`, `0x1E87`, `0x1E84`, `0x1E89`, `0x1F02`, `0x1F07`, `0x1F06`, `0x1F0B`, `0x1F47`, `0x1F03`, `0x1F08`, `0x1F12`, `0x1E09`, `0x1EB0`, `0x1EB1`, `0x1E30`, `0x1E35` |

- **Any Vendor:** Compatible with Founders Edition, ASUS, MSI, Gigabyte, ZOTAC, PNY, Palit, Gainward, etc.
- **Any Platform:** Intel (Z690, Z790, Z890, B760, Z390) and AMD (AM4/AM5, X570, B550, B650, X670E, X870E).
- **Multi-Drive:** Prefers `bootmgfw.efi` and follows your firmware's boot order when several Windows volumes exist.
- **Multi-GPU & Headless Compute:** Unlocks primary or dedicated compute GPUs. Discrete cards can operate completely headless as dedicated AI/compute accelerators with all monitors connected to integrated graphics (e.g. AMD Ryzen Raphael iGPU or Intel UHD) — resolving [#14](https://github.com/BrightRaider/universal-gpu-unlock/issues/14).
- **ASUS & AMI Console Topology Verified:** Multi-display console detachment verified on ASUS PRIME Z690-A at **127.61 TFLOPS** (100% unrestricted compute) on EVGA GeForce RTX 3080 Ti — resolving [#16](https://github.com/BrightRaider/universal-gpu-unlock/issues/16).
- **ASRock AM4 & CSM Resolution Verified:** Budget AM4 POST discovery and CSM legacy video conflict resolved on ASRock A520M-HVS at **106.05 TFLOPS** (+95.7% boost, unrestricted compute) on Gainward GeForce RTX 3080 Phoenix — refs [#17](https://github.com/BrightRaider/universal-gpu-unlock/issues/17).
- **Ampere & Turing Verified:** GA106 (RTX 3060), GA104 (RTX 3060 Ti), GA102 (RTX 3080, RTX 3080 Ti, RTX 3090) as well as TU102 (RTX 2080 Ti) and TU106 (CMP 40HX) are confirmed. On Turing (RTX 20 Series), the pre-boot unlock transaction completes cleanly and full enterprise compute throughput (>85–105 TFLOPS) is activated with GSP firmware mode (`tools\Toggle-Turing-GSP.bat`) — refs [#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15), refs [#18](https://github.com/BrightRaider/universal-gpu-unlock/issues/18).
- **Boot-Loop Safeguard (`ATTEMPT.FLG`):** Automatically protects systems from reboot loops. A volatile progress flag is staged to disk before register mutations and cleaned only after verified completion. If a mutation ever hangs, the next boot immediately engages `[SAFEGUARD ACTIVE]`, clears the flag, and chainloads Windows in safe factory mode.
- **GFW Usable-Memory Delta Tolerance:** Incorporates a 32 MiB aperture tolerance in preflight layout calculation, preventing `-ERANGE` (-34) errors caused by firmware aperture reservations on 24GB+ cards (RTX 3090 Ti & 4090).
- **`SLOT-REVIVE` Engine:** Proactive GOP decoupling via DriverBinding followed by D0 power state forcing and PCIe bridge Secondary Bus Reset (SBR) to wake unresponsive slots on OEM systems (Dell/HP) or warm reboot.
- **Stickless Internal NVMe-ESP Persistence (`[I]` Mode) & ESP Storage Notice:** Installs `AdaUnlockDxe.efi` and architecture firmware assets to your internal EFI System Partition (`\EFI\Drivers\`) and registers it in UEFI `DriverOrder`, so the USB stick is no longer needed.
  * **ESP Space Requirements:** Ada (RTX 40) and Ampere (RTX 30) require **~85 MB free space** for NVIDIA's monolithic signed GSP-RM firmware (`gsp_ga10x.bin`). Turing (RTX 20 / CMP) requires only **~28 MB**.
  * **Standard 100 MB Windows ESP:** Windows OEM setups create a 100 MB ESP with only ~30–50 MB free. On Turing cards, the unified installer automatically deploys only the compact Turing assets (~28 MB), fitting standard 100 MB partitions out of the box. On Ada/Ampere cards, if space is insufficient, `[I]` safely falls back to registering the USB drive.
  * **Workarounds & Best Practices for Ada / Ampere:**
    1. **Option `[P]` (S3 Sleep Persistence - Recommended):** If you use Sleep / Standby (Suspend-to-RAM) instead of a cold shutdown, no ESP space is needed at all! Unlock once via USB `[P]`, and the GPU stays unlocked across sleep cycles.
    2. **Dedicated FAT32 Partition (1-Click Tool):** Run `Setup.bat` option `[3]` (or `tools\Create-Unlock-Partition.bat` / the button in Universal Control Center) to safely shrink 512 MB from any internal SSD and format a dedicated hidden FAT32 `GPU_UNLOCK` partition. UEFI mounts it automatically on boot, allowing `[I]` to store assets cleanly (verified in [#19](https://github.com/BrightRaider/universal-gpu-unlock/issues/19)).
    3. **Resize ESP:** Use a partition manager to increase the ESP from 100 MB to 500 MB.

<details>
<summary><b>🔍 Community-Verified Hardware & Live Benchmarks (Click to expand)</b></summary>
<br>

The following GPU models, system motherboards, and compute readbacks have been independently verified on physical hardware by community users:

| Verified GPU | Architecture & Silicon | Motherboard & Platform | Real-World Throughput | Verification Issue |
| :--- | :--- | :--- | :--- | :--- |
| **NVIDIA GeForce RTX 2080 Ti 11GB** | Turing (TU102) | Intel Z390 / AMD AM4 | **105.4 TFLOPS** (GSP-RM Firmware Compute Mode) | [#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15) |
| **NVIDIA CMP 40HX 8GB** | Turing (TU106) | AMD AM4 / Intel Platform | **54.2 TFLOPS** (Headless Compute Accelerator) | [#18](https://github.com/BrightRaider/universal-gpu-unlock/issues/18) |
| **NVIDIA GeForce RTX 2060 6GB (TU104-150)** | Turing (TU104 harvested) | ASUS TUF B450-PLUS GAMING (AMD AM4) | **25.82 TFLOPS** (Dense GEMM, 4.21x over FP32 Baseline) | [@resYuto in #20](https://github.com/BrightRaider/universal-gpu-unlock/issues/20) |
| **Palit GeForce RTX 3060 12GB** | Ampere (GA106 LHR) | MSI MPG Z390 GAMING PLUS | **46.08 TFLOPS** (~45% ComfyUI speedup) | [#11](https://github.com/BrightRaider/universal-gpu-unlock/issues/11) |
| **NVIDIA GeForce RTX 3060 Ti 8GB (`10DE:2489`)** | Ampere (GA104) | MSI PRO Z790-A WIFI DDR4 (Intel i5-14600KF) | **61.23 TFLOPS FP16 (+104.4%) / 61.97 TFLOPS BF16** | [@karadeveci in #22](https://github.com/BrightRaider/universal-gpu-unlock/issues/22) |
| **Gainward GeForce RTX 3080 Phoenix 10GB** | Ampere (GA102) | ASRock A520M-HVS (AMD AM4) | **106.05 TFLOPS** (+95.7% boost, Unrestricted Compute) | [#17](https://github.com/BrightRaider/universal-gpu-unlock/issues/17) |
| **EVGA GeForce RTX 3080 Ti 12GB** | Ampere (GA102) | ASUS PRIME Z690-A | **127.61 TFLOPS** (100% Unrestricted Compute) | [#16](https://github.com/BrightRaider/universal-gpu-unlock/issues/16) |
| **EVGA GeForce RTX 3080 Ti 12GB** | Ampere (GA102) | MSI X570-A PRO | **119.0 TFLOPS** (+84.5% boost, Unrestricted Compute) | [#12](https://github.com/BrightRaider/universal-gpu-unlock/issues/12) |
| **NVIDIA GeForce RTX 3090 24GB** | Ampere (GA102) | Gigabyte TRX40 AORUS MASTER (AMD Threadripper 3960X) | **117.24 TFLOPS** (100% Unrestricted Tensor Compute) | [#13](https://github.com/BrightRaider/universal-gpu-unlock/issues/13) |
| **MSI GeForce RTX 3090 GAMING X TRIO 24G** | Ampere (GA102) | ASUS ROG STRIX B550-E GAMING (AMD 5800X3D) | **105.10 TFLOPS** (100% Unrestricted Tensor Compute) | Community Verified |
| **EVGA GeForce RTX 3090 Ti 24GB** | Ampere (GA102-350) | ASUS PRIME Z490-P (Intel i7-10700) | **132.4 TFLOPS FP16 / 136.6 TFLOPS BF16** (+102% Double Performance) | [@m4ttfx in #495](https://github.com/windows-GPU-UNLOCK/Windows-GPU-UNLOCK/issues/495#issuecomment-7077703728) |
| **Dual GPU: HP OEM RTX 3090 + EVGA RTX 3060** | Ampere (GA102 + GA106) | ASUS TUF GAMING Z590-PLUS WIFI (Intel i5-11600K) | **86.0 TFLOPS (3090) / 49.6 TFLOPS (3060)** | [#19](https://github.com/BrightRaider/universal-gpu-unlock/issues/19) |
| **ZOTAC GeForce RTX 4080 Trinity OC** | Ada Lovelace (AD103) | GIGABYTE X870 AORUS ELITE WIFI7 (AMD 9800X3D) | **174.29 TFLOPS** (Full Unrestricted GEMM) | [#9](https://github.com/BrightRaider/universal-gpu-unlock/issues/9) |
| **PNY GeForce RTX 4090 24GB** | Ada Lovelace (AD102) | Gigabyte Z690 UD AC-Y1 (Intel i9-13900K) | **255.0 TFLOPS** (+59.4% boost, +15-25% ComfyUI gain) | [@jacyzheng in #444](https://github.com/windows-GPU-UNLOCK/Windows-GPU-UNLOCK/issues/444) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | MSI MPG X670E Carbon WIFI (AMD Ryzen 7 7800X3D iGPU) | **255.0 TFLOPS** (+54.5% boost, Headless Compute) | [#14](https://github.com/BrightRaider/universal-gpu-unlock/issues/14) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | Intel Z790 Platform (Multi-Drive ESP) | **255.4 TFLOPS** (Peak Compute) | [#8](https://github.com/BrightRaider/universal-gpu-unlock/issues/8) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | Intel Z690 Platform (Hybrid iGPU & Multi-Display) | **214.0+ TFLOPS** (Sustained FP16 GEMM) | [#5](https://github.com/BrightRaider/universal-gpu-unlock/issues/5) |
| **ASUS WS C621E SAGE Workstation** | Multi-Socket Server | ASPEED AST2500 BMC / IPMI Console | Multi-Console Isolation Verified | [#2](https://github.com/BrightRaider/universal-gpu-unlock/issues/2) |

> [!TIP]
> All benchmarks above were measured directly using **`Check-GPU-Unlock.bat`** under Windows. If your GPU and motherboard combination is verified and working, feel free to share your benchmark in Discussions or in an issue!

</details>

---

## 📊 Real-World Performance Gains

The unlock removes artificial firmware restrictions on the **Tensor Cores (GEMM matrix engines)**. FP32 shader and memory throughput stay at stock. Measured on an RTX 4090:

| Workload / Model | Factory Stock | Unlocked UEFI | Real-World Improvement | Operational Impact |
| :--- | :--- | :--- | :--- | :--- |
| **FLUX.1-dev AI Sampling** | 1.20 it/s (25.0s) | **1.97 it/s (15.2s)** | 🚀 **+64.2% Faster** | High-res image generation in ~24s instead of ~34s |
| **MiniMax-H3 Video DiT** | 15.20 s/it (304s) | **11.15 s/it (223s)** | 🚀 **+36.3% Faster** | Saves **81 seconds** per 5-second video render |
| **FP16 Tensor GEMM ($4096^2$)** | 132.1 TFLOPS | **208.9 TFLOPS** | 🚀 **+58.1% Boost** | Maximum compute throughput within 72MB L2 cache |
| **BF16 Tensor GEMM ($8192^2$)** | 149.3 TFLOPS | **167.4 TFLOPS** | 🚀 **+12.1% Boost** | Sustained large-matrix training acceleration |
| **FP32 CUDA Shaders** | 51.3 TFLOPS | **48.7 TFLOPS** | ✅ **Stock Parity** | Standard raster rendering remains at factory stock |
| **VRAM Copy Bandwidth** | 733.4 GB/s | **707.2 GB/s** | ✅ **Stock Parity** | Physical GDDR6X bus operation unchanged |

> [!TIP]
> **Maximizing Real-World AI Throughput (cuBLASLt Heuristics):**  
> NVIDIA's official `cuBLASLt` library selects matrix multiplication algorithms based on factory heuristic models calibrated for rate-limited consumer cards. To ensure your AI applications pick the true maximum-throughput kernel tiles for your unlocked GPU:
> * **PyTorch / Python Scripts:** Add `torch.backends.cudnn.benchmark = True` before your compute loop.
> * **ComfyUI / Stable Diffusion:** Launch with benchmark/autotune enabled.
> * **Result:** Yields an additional **+15% to +35%** real-world matrix throughput across dense GEMM, SDXL, and LLM prefill workloads!

---

## 🛡️ Safety & Anti-Cheat

- **Volatile (RAM-only):** The unlock lives in GPU registers and is gone after a full power-off. Without the `[I]` driver installed, the next boot is stock. On the test bench, the card always booted stock again after a power-off.
- **Official NVIDIA Drivers:** Runs seamlessly with standard, unmodified NVIDIA Game Ready and Studio drivers (`nvlddmkm.sys`).
- **Anti-Cheat:** No driver patches, kernel hooks, or runtime memory injection (`UEFISecureBootEnabled` stays `1`). **Riot Vanguard (*Valorant* / *League of Legends*)** works with **Secure Boot Enabled** via Option **`[I]` (`AdaUnlockDxe.efi`)** and a normal **Windows Boot Manager** boot (see [Secure Boot Configuration](#-secure-boot-configuration)). Easy Anti-Cheat, BattlEye, and Ricochet were not tested systematically.
- **Pre-Flight Safety Engine:** Combines UEFI boot-loop safeguard (`ATTEMPT.FLG`), `SLOT-REVIVE` bus/D0 wake + read-only OEM `0x18` zeroed Root-Port diagnostics (`GuLogZeroedRootPortHint`), targeted `ConnectController` display handover (no global `AllHandles` reconnect, avoiding OVMF/`virtio-fs` hangs), 32 MiB GFW aperture tolerance, and headless `cmd=0x0` discovery with Windows pre-flight checks for **BitLocker**, **Secure Boot**, **ESP size**, and **ACPI S3 vs. Modern Standby (`S0ix`)** directly inside `Setup.bat` and the Control Center.
- **Load-Aware Verification (`refs #22`):** `Check-GPU-Unlock.bat` and `Launch-Dashboard.bat` query pre-benchmark GPU load (`nvidia-smi utilization.gpu`) and compare `FMLA32` (`allow_fp16_reduced_precision_reduction=False`) vs. `FMLA16` (`True`) so background renders/games/upscalers no longer trigger false `STOCK RATE-LIMITER ACTIVE` warnings. *(On Ampere, `FMLA32/FMLA16` stays ~`1.00x` unlocked vs. ~`0.50x` clamped; on Ada `sm_89` and Turing `sm_75`, `~0.50–0.63x` reflects native hardware precision piping even when unlocked.)*
- **Fast Resume & Idle Relock Guard:** Includes **`tools\Setup-FastResume.bat`** to enable Windows Hibernation and create a desktop **Fast-Unlock-Sleep** shortcut (~3s wake-up) plus the **Idle Relock Guard** (`RMDisableRtd3` + PCIe ASPM off) so Optimus laptops and secondary headless compute GPUs never drop their unlocked state during idle timeouts.
- **Sleep Protection (for Cold-Boot `[U]` Users):** Includes **`tools\Toggle-Sleep-Button.bat`** (accessible via the Dashboard *Sleep Shortcut* button) to hide/restore the "Sleep" button in the Windows Start menu if you use `[U]` instead of `[P]`.

> [!WARNING]
> **Windows S4 Hibernate vs. Pre-Boot ReBAR Overrides:** Universal GPU Unlock uses a pure SS-register unlock and never mutates pre-boot ReBAR BAR1 registers (`0x610`/`0xBC0`) or forces `ConnectController` GOP reconnects during resume — keeping pure-SS S4 Hibernate safe. Do **not** combine third-party pre-boot ReBAR overrides (`ReBarDxe` / `0x610`/`0xBC0`) with Windows S4 Hibernation (`Setup-FastResume.bat` / `shutdown /h`), as that combination causes hard `Kernel-Power 41` hangs (and forced GOP reconnects during S4 resume trigger TDR reboots).
- **44-Cycle Testbench:** 44 supervised runs on my primary RTX 4090 platform. The last 18 (RUN0027 – RUN0044) all passed. The earlier runs were prototyping.
- **Operational Deep-Dive:** Read the comprehensive telemetry and persistence whitepaper:  
  👉 **[Operational Architecture, Persistence & Secure Boot Telemetry](docs/OPERATIONS_AND_PERSISTENCE.md)**

---

<details>
<summary><b>🔍 Click to view Deep Technical Architecture, GEMM Scaling & Secure Boot Details</b></summary>

### Why is the boost largest at $4096 \times 4096$ (+58.1%)?
The Ada Lovelace AD102 architecture features a massive **72 MB L2 cache**. A $4096 \times 4096$ FP16 matrix (~32 MB per operand) fits entirely inside this ultra-fast cache (>3.5 TB/s cache bandwidth), so execution is purely **Compute-Bound**—allowing the unlocked Tensor Cores to reach their full unrestricted 208+ TFLOPS potential. At $16384 \times 16384$ (>1.5 GB memory footprint), workloads become **Memory-Bandwidth Bound** by external GDDR6X VRAM. Because real-world AI inference & training models slice tensors into $1024$ to $4096$ blocks, they operate right in this maximum speedup sweet spot!

### Secure Boot Key Enrollment
The included EFI binaries are pre-signed with my custom Authenticode key. To boot with Secure Boot enabled:
1. Enter your BIOS / UEFI setup menu.
2. Navigate to **Security / Secure Boot / Key Management**.
3. Choose **Append / Enroll DB Key**, browse to your USB drive, and select **`AdaUnlock_SecureBoot.cer`**.
4. Save and reboot. Alternatively, you can temporarily set Secure Boot to *Disabled* or *Custom*.

</details>

---

## 🧪 Build & Tests

Requires the Visual Studio 2022 C++ Build Tools (x64).

| Command | What it does |
| :--- | :--- |
| `src\Build-Experimental-MSVC.ps1` | Builds `BOOTX64.EFI` and **overwrites `bin\BOOTX64.EFI`**. Signs it only if the maintainer certificate is installed. |
| `src\Build-Dxe-Driver.ps1` | Builds `AdaUnlockDxe.efi` and **overwrites `bin\AdaUnlockDxe.efi`**, with the same signing rule. |
| `src\Build-Gpu-Topology-Diagnostic.ps1` | Builds the read-only topology diagnostic EFI. |
| `tests\Run-MsvcHostTests.ps1` | Runs the MSVC host tests for `driver_nvram.c` (`[I]`/`[R]`) and the Windows volume selection in `boot_io.c`. |

The gcc/clang host tests for topology, inventory and discovery are in `tests/`. The CI workflow (`.github/workflows/gpu-topology-diagnostic.yml`) runs on every push and pull request that touches `src/` or `tests/`. It runs all host tests, builds the diagnostic EFI, `BOOTX64.EFI` and `AdaUnlockDxe.efi`, and uploads the unsigned builds as workflow artifacts. All tests mock the firmware services; none of them runs on a GPU.

---

## 📜 Changelog & Version History

All version releases, bug fixes, and device expansions are maintained in the separate changelog file:  
👉 **[View Full CHANGELOG.md](CHANGELOG.md)** *(or browse [GitHub Releases](https://github.com/BrightRaider/universal-gpu-unlock/releases))*.

---

## 🛠️ Repository Structure

```text
├── bin/
│   ├── BOOTX64.EFI              <- Pre-compiled, Authenticode-signed universal UEFI bootloader
│   └── AdaUnlockDxe.efi         <- Pre-compiled, Authenticode-signed early DXE driver
├── AD102-ASSETS/                <- Ada microcode payloads (4090, 4080, 4070TiS)
├── GA102-ASSETS/                <- Ampere microcode payloads (3090, 3080, 3070, 3060)
├── TU102-ASSETS/                <- Turing microcode staging payloads (2080Ti, 2080, 2070, 2060)
├── src/                         <- Full C source code, MSVC EFI build scripts & Build-Release-Zip.ps1
├── tests/                       <- Host tests with mocked UEFI services (no GPU required)
├── docs/                        <- Operational whitepapers, CHANGELOG_ARCHIVE.md & images/
├── Setup.bat                    <- Unified Launcher (1: USB Setup, 2: Update USB, 3: Hidden 512MB Partition)
├── Launch-Dashboard.bat         <- Primary GUI Cockpit (Benchmarks, S3 Armed Status, Power Limit Slider)
├── Check-GPU-Unlock.bat         <- Standalone CLI benchmark & hardware check (TFLOPS, S3, PL, ReBAR, PCIe)
├── tools/
│   ├── quick_check.py           <- Core GEMM benchmark, S3 Armed check, Power-Limit CLI & PCIe diagnostics
│   ├── gpu_dashboard.py         <- Primary Tkinter GUI Cockpit (S3 status, stepless PL slider, Baseline diff)
│   ├── gu_system_info.py        <- Shared firmware, ACPI FADT, Power Limit & GPU profile helpers
│   ├── Setup-AdaUnlock.ps1      <- Automated 1-Click USB Provisioner with BitLocker/SecureBoot/S0ix audits
│   ├── Update-USB.bat           <- Fast USB stick updater
│   ├── Create-Unlock-Partition.*<- Dedicated hidden 512 MB FAT32 GPU_UNLOCK partition creator
│   ├── Setup-FastResume.bat     <- Windows Hibernation & Idle Relock Guard helper
│   ├── Toggle-Sleep-Button.bat  <- Hide/restore Start Menu Sleep button
│   ├── Toggle-Turing-GSP.bat    <- Turing GSP firmware mode toggle
│   ├── Universal-Control-Center.* <- Advanced WPF Diagnostics, Telemetry Dumps & Capture Checker
│   └── optional-turing-rebar/   <- Experimental ReBarUEFI guide (STRICTLY NO SUPPORT, use at own risk)
├── AdaUnlock_SecureBoot.cer     <- Authenticode certificate for Secure Boot db enrollment
├── DOKUMENTATION.md             <- Full German technical documentation
└── CHANGELOG.md                 <- Recent release history (v1.8+ / v2.0.0; older entries in docs/)
```

---

## 🙏 Acknowledgements & Credits

* 🌟 **[geodnb](https://github.com/geodnb) / [Ablation Lab](https://github.com/Ablation-Lab) (Special Dedication):** Massive thanks for pioneering research, hardware state analysis, and technical insights into GPU cold states and register architecture that catalyzed this entire universal Windows project. Explore their research over at [Ablation Lab on GitHub](https://github.com/Ablation-Lab) and subscribe to their channel on [YouTube (@AblationLab)](https://www.youtube.com/@AblationLab)!
* **Zagitta & WildFlash1st:** For pioneering the Linux `gpu-unlock` research and `cmp90hx-unlock` rejoining methodologies.
* **[Ahsan Umair (@ahsanumair)](https://github.com/ahsanumair):** For contributing the initial Windows desktop WPF GUI layout and XAML groundwork adapted into the optional diagnostic telemetry companion (`Universal-Control-Center.cmd`).
* **TOTORONG & JustAResearcher:** For early proof-of-concept Windows UEFI implementations.
* **Igor Wallossek (Igor'sLAB):** For in-depth technical analysis, power verification, and community discussion.

---

## ☕ Support the Project

If Universal GPU Unlock accelerated your AI workflows, saved you render time, or helped unlock your graphics card's full silicon potential, consider buying me a coffee to support continuous updates and hardware testing:

<a href='https://ko-fi.com/brightraider' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi2.png?v=3' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>
