<div align="center">

# Universal GPU Unlock (One-Click UEFI Tool)
### High-Performance Tensor Core Rate-Limiter Bypass for NVIDIA GPUs

[![GitHub Release](https://img.shields.io/github/v/release/BrightRaider/universal-gpu-unlock?color=blue&logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest)
[![Pre-release](https://img.shields.io/badge/Pre--release-v1.8.22%20(Recommended)-orange?logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v1.8.22)
[![Downloads](https://img.shields.io/github/downloads/BrightRaider/universal-gpu-unlock/total?color=green&logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases)
[![Stars](https://img.shields.io/github/stars/BrightRaider/universal-gpu-unlock?style=social)](https://github.com/BrightRaider/universal-gpu-unlock)
[![Safety](https://img.shields.io/badge/Safety-100%25%20RAM--Only-success.svg)](#-safety--anti-cheat-guarantee)
[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20Me%20A%20Coffee-ff5e5b?style=flat&logo=kofi&logoColor=white)](https://ko-fi.com/brightraider)

<br>

<a href="https://ko-fi.com/brightraider" target="_blank">
  <img src="https://storage.ko-fi.com/cdn/kofi2.png?v=3" height="38" alt="Buy Me a Coffee at ko-fi.com" />
</a>

<p>
  <b>An automated, universal 1-click UEFI pre-boot unlocker for NVIDIA RTX 40 (Ada), RTX 30 (Ampere) & RTX 20 (Turing) series.</b><br>
  Unlocks full enterprise-grade Tensor Core compute across Windows 10/11, PyTorch, CUDA, and WSL2.
</p>

</div>

---

## ⚡ 60-Second Quick Start

> [!TIP]
> **🚀 Recommended: [Universal GPU Unlock v1.8.22 Pre-release](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v1.8.22)**  
> Version **v1.8.22** is the recommended build for all modern configurations. Key additions:
> - **Extended 30-Second Boot Selection Countdown:** Extended the countdown from 15 to 30 seconds to allow DisplayPort/HDMI monitors to synchronize pixel clocks and give users ample time to read options and select their desired mode.
> - **Redundant Handle-Graph Scan Elimination:** Removed the redundant second handle-table walk in `GuUnlockReadyForWindows()`, resolving firmware stalls on ASRock AM4 / AMD platforms and transitioning directly into display reconnection and Windows Boot Manager handover.
> - **Direct Console Splitter Reconnection:** Proactively reconnects `SystemTable->ConsoleOutHandle` alongside the GPU controller, ensuring monitor sync and display pipeline stability.
> - **Pre-Close Hardware Register Verification:** Moves all silicon register readbacks (`chip`, `cpu`, `hi`, `ss0`, `ss1`, `readout`, `plm`) strictly before `CloseProtocol()`, preventing post-close volatile MMIO reads (`GuRead32(0)`) that triggered a PCIe Master Abort / bus freeze on AMI Aptio V / AMD platforms.
> - **Post-Unlock UEFI GOP Display Controller Reconnection:** Proactively reconnects primary display controllers via `ConnectController(devices[g].handle, ...)` and stabilizes TMDS/DP link training prior to handing over control to Windows Boot Manager (`bootmgfw.efi`), resolving monitor signal loss on systems without integrated graphics.
> - **Universal Control Center UTF-8 Mojibake Fix:** Standardized XML numeric character entities across `Universal-Control-Center.xaml` and native character escapes in `Universal-Control-Center.ps1`, resolving `âœ”` rendering artifacts on all Windows system locales.
> - **Pre-FLR Bus Master Quiescence Drift Tolerance:** Aligned `ConfigStable()` to tolerate bit 2 (`PCI_COMMAND_MASTER`) of Command Register Dword 1 (offset 0x4) during pre-FLR quiescence, resolving `PCI config drift refused: offset=0x4 captured=0x100007 now=0x100003 bits=0x4 flrs=0` (`Release check failed: config-drift`, `unlock_610.c:1764 error=-16`) on ASRock A520M-HVS and Ampere RTX 3080.
> - **Halt Console Output & Keystroke Drain Fix:** Proactively reconnects the display controller via `ConnectController()` and resets `ConOut` prior to calling `GuBootHalt()`, ensuring the diagnostic box and 15-second reboot countdown are visibly rendered on physical displays instead of a frozen underscore cursor. Drains queued keystrokes from `ConIn` to prevent accidental premature reboots.
> - **UEFI Bus Master Attribute Synchronization:** Aligned `GuEnableMaster()` and `GuClearMaster()` attribute state handling with hardware PCI config register assertions, resolving `Bus Master enable failed: status=0x8000000000000007` (`failure unlock_610.c:1356 unlock_transaction_fwsec error=-5`) on ASRock A520M-HVS and AM4 motherboards.
> - **Live Boot Menu Countdown & Diagnostic Halt Screen:** Snappy 15-second live countdown in boot selection with automatic USB log reporting notice and transparent on-screen failure recovery instead of an unmanaged freeze.
> - **Pre-Claim Transaction Bus Master Quiescence Alignment:** Gracefully clears Bus Master via `pci_clear_master()` prior to device region claim on motherboards (like ASRock A520M-HVS) that enable Bus Master during early POST, resolving `failure unlock_610.c:1151 unlock_transaction_claim error=-16`.
> - **AMI Aptio V Display Consumers Topology Alignment:** Removed artificial `(child < 1 && idle < 1)` requirement in `Consumers()` when `Plan.gop` is already selected and verified on the target GPU, resolving `Preflight failed at Consumers(): 0x800000000000000f` on ASRock A520M-HVS and other AMI Aptio V platforms.
> - **Post-Enable Bus Master Preservation in ClaimRestore & Release:** Aligned `ClaimRestore()` and `GuUnlockReleased()` to preserve `EFI_PCI_IO_ATTRIBUTE_BUS_MASTER` when initialized at boot by motherboard POST.
> - **ASRock & AMD AM4 Preflight ReadConfig Bus Master Alignment:** Removed restrictive `|| (c[1] & 4)` assertions and aligned decode attribute checking, resolving `Preflight failed at ReadConfig(): 0x8000000000000003` on ASRock A520M-HVS and AMD AM4 motherboards.
> - **Turing GSP Firmware Mode 1-Click Utility:** Includes `Toggle-Turing-GSP.bat` for seamless configuration of `EnableGpuFirmware = 1` in Windows Registry, allowing Turing (RTX 2080 Ti / TITAN RTX) to activate full enterprise compute.
> - **Enhanced Multi-Precision Hardware Verification:** `Check-GPU-Unlock.bat` tests FP16, BF16, TF32, and pure FP32 CUDA cores as physical negative controls.
> - **ASRock & AMD AM4 Discovery Filter Alignment:** Removed restrictive boot-time Bus Master and Expansion ROM checks, resolving `STOP: stage=discover status=0x8000000000000003` on ASRock A520M-HVS and AMD platforms.
> - **Ampere Device ID Additions:** Added RTX 3080 GA103 (`0x2414`), RTX 3090 Ti (`0x2203`), RTX 3080 Ti OEM (`0x2205`), and `0x220b`.
> - **ASUS / AMI Aptio V 2-Agent Console Topology Support:** Resolves `Preflight failed at Consumers(): 0x800000000000000f` on ASUS PRIME and other AMI Aptio V motherboards with dual or single displays.
> - **Turing Post-FLR Device Status Drift Tolerance:** Resolves `Release check failed: config-drift` on Turing TU102 (RTX 2080 Ti), allowing full handover to Windows Boot Manager.
> - **Turing PCIe FLR Pre-Reset Validation Guard:** Bypassed Ada/Ampere-specific selector checks in `GuFlr()`, resolving `failure unlock_610.c:1764 unlock_transaction_reset error=-5` on RTX 2080 Ti.
> - **Turing TU102 FWSEC Post-FRTS WPR2 Alignment:** Bypassed Ampere-specific post-FWSEC WPR2 assertion (`wpr_hi >> 4`), resolving `unlock_transaction_fwsec error=-5` on RTX 2080 Ti and TITAN RTX.
> - **Turing GFW Readiness & Dynamic Register Alignment:** Bypassed Ampere-specific GFW register assertions (`sample.plm != 0x8b8f`).
> - **Turing Booter Signature & Fuse Hardening:** Resolves `patch_booter error=-34` on Turing silicon with dedicated production RSA-384 signature selection and FWSEC version 0 alignment.
> - **Turing Framebuffer Local Memory Range Detection:** Resolves unmapped MMIO layout halts via `NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE` (`0x00100ce0`) hardware query.
> - **Internal NVMe-ESP Persistence Installer (`[I]`):** 1-Click persistent installation directly onto the internal NVMe EFI System Partition (ESP) with UEFI NVRAM `DriverOrder` registration — USB flash drive never needed again after setup.
> - **Multi-Display GOP Console Auto-Selection:** Resolves dual-monitor preflight halts on ASUS and other motherboards by automatically selecting the primary console GOP handle.
> - **Turing FWSEC vDesc v2 Geometry Parser:** Full support for Turing VBIOS descriptor v2 layout on RTX 20-series (TU102 / RTX 2080 Ti / 2080 Super), mapping DMEM boundaries and signature layouts.
> - **Ampere GA102 FWSEC Timing Hardening:** Eliminates false-halt `error=-5` during WPR2 programming on RTX 3080, 3080 Ti, and 3090.
> - **Headless & Dedicated Compute Accelerators:** Full support for dGPUs with all displays routed via AMD Ryzen (7000/9000 Raphael) or Intel UHD iGPUs.
> - **Expanded Device IDs:** Added RTX 3060 Ti GA104 (`0x2486`), RTX 3050 GA106 (`0x2582`), and Ada AD104/AD106/AD107 variants.
> 
> 📦 **Download Pre-release:** **[Universal_GPU_Unlock_v1.8.16.zip](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v1.8.16)** *(or previous stable: [v1.7.9](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v1.7.9))*.

1. **Download:** Get the recommended build: **[Universal_GPU_Unlock_v1.8.16.zip](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v1.8.16)** *(or [v1.7.9](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest))*.
2. **Provision:** Extract the ZIP, insert a USB flash drive, and run **`Setup.bat`** (formats & sets up all boot files automatically).
3. **Boot & Unlock (or Install Permanently):** Restart your PC, boot from the USB drive (`F8`/`F11`/`F12`):
   - Press **`[U]`** for Standard RAM-Only Unlock (temporary, reverts if USB removed).
   - Press **`[I]`** to Install Permanently to your internal NVMe SSD ESP (`\EFI\Drivers\AdaUnlockDxe.efi`) and UEFI NVRAM `DriverOrder`. Once installed, **unplug the USB drive permanently** — your system auto-unlocks on every boot!
   - Press **`[R]`** at any time to cleanly uninstall from NVRAM and remove the internal ESP driver files.
4. **Verify:** In Windows, run **`Check-GPU-Unlock.bat`** to verify your unlocked TFLOPS throughput (>200 TFLOPS).  
   *(Note for RTX 20 / Turing users: Run **`Toggle-Turing-GSP.bat`** once in Windows as Administrator to bind the official NVIDIA driver to GSP firmware mode).*  
   *(Optional: If you wish to inspect detailed hardware telemetry, SMI sensor readbacks, and differential diagnostics, launch **`Universal-Control-Center.cmd`**).*

---

## 🚀 Supported Hardware

| Generation | Architecture | Supported Graphics Cards | Device IDs |
| :--- | :--- | :--- | :--- |
| **RTX 40 Series** | **Ada Lovelace** | RTX 4090, RTX 4090D, RTX 4080, RTX 4080 SUPER, RTX 4070 Ti SUPER | `0x2684`, `0x2685`, `0x2702`, `0x2703`, `0x2704`, `0x2705` |
| **RTX 30 Series** | **Ampere** | RTX 3090, RTX 3090 Ti, RTX 3080 Ti, RTX 3080 (10/12GB), RTX 3070 Ti, RTX 3070, RTX 3060 Ti, RTX 3060, CMP 90HX | `0x2204`, `0x2208`, `0x2206`, `0x220a`, `0x2216`, `0x2207`, `0x2482`, `0x2484`, `0x2487`, `0x2488`, `0x2489`, `0x2503`, `0x2504`, `0x2507`, `0x2508`, `0x2520`, `0x2560` |
| **RTX 20 Series** | **Turing** (TU102 Full Unlock / Diagnostics) | RTX 2080 Ti, TITAN RTX, RTX 2080 / SUPER, RTX 2070 / SUPER, RTX 2060 / SUPER, CMP 50HX, Quadro RTX | `0x1E04`, `0x1E07`, `0x1E02`, `0x1E81`, `0x1E82`, `0x1E87`, `0x1E84`, `0x1F02`, `0x1F07`, `0x1F06`, `0x1F47`, `0x1F03`, `0x1F08`, `0x1F12`, `0x1E09`, `0x1EB0`, `0x1EB1`, `0x1E30`, `0x1E35` |

- **Any Vendor:** Compatible with Founders Edition, ASUS, MSI, Gigabyte, ZOTAC, PNY, Palit, Gainward, etc.
- **Any Platform:** Intel (Z690, Z790, Z890, B760, Z390) and AMD (AM4/AM5, X570, B550, B650, X670E, X870E).
- **Multi-Drive:** Prefers `bootmgfw.efi` and follows your firmware's boot order when several Windows volumes exist.
- **Multi-GPU & Headless Compute:** Unlocks primary or dedicated compute GPUs. Discrete cards can operate completely headless as dedicated AI/compute accelerators with all monitors connected to integrated graphics (e.g. AMD Ryzen Raphael iGPU or Intel UHD) — resolving [#14](https://github.com/BrightRaider/universal-gpu-unlock/issues/14).
- **ASUS & AMI Console Topology Verified:** Multi-display console detachment verified on ASUS PRIME Z690-A at **127.61 TFLOPS** (100% unrestricted compute) on EVGA GeForce RTX 3080 Ti — resolving [#16](https://github.com/BrightRaider/universal-gpu-unlock/issues/16).
- **Ampere & Turing Silicon Verified:** GA106 (RTX 3060), GA104, GA102 (RTX 3080/3090), and TU102 (RTX 2080 Ti / TITAN RTX) supported with full rate-limiter bypass and zero Windows VRAM loss.
- **Stickless Internal NVMe-ESP Persistence (`[I]` Mode):** Option to install `AdaUnlockDxe.efi` and microcode directly to your internal NVMe SSD's EFI System Partition (`\EFI\Drivers\`) with UEFI NVRAM `DriverOrder` auto-boot. Once installed, the USB stick is completely optional and never needed for booting.

<details>
<summary><b>🔍 Community-Verified Hardware & Live Benchmarks (Click to expand)</b></summary>
<br>

The following GPU models, system motherboards, and compute readbacks have been independently verified on physical hardware by community users:

| Verified GPU | Architecture & Silicon | Motherboard & Platform | Real-World Throughput | Verification Issue |
| :--- | :--- | :--- | :--- | :--- |
| **Palit GeForce RTX 3060 12GB** | Ampere (GA106 LHR) | MSI MPG Z390 GAMING PLUS | **46.08 TFLOPS** (~45% ComfyUI speedup) | [#11](https://github.com/BrightRaider/universal-gpu-unlock/issues/11) |
| **EVGA GeForce RTX 3080 Ti 12GB** | Ampere (GA102) | ASUS PRIME Z690-A | **127.61 TFLOPS** (100% Unrestricted Compute) | [#16](https://github.com/BrightRaider/universal-gpu-unlock/issues/16) |
| **EVGA GeForce RTX 3080 Ti 12GB** | Ampere (GA102) | MSI X570-A PRO | **119.0 TFLOPS** (+84.5% boost, Unrestricted Compute) | [#12](https://github.com/BrightRaider/universal-gpu-unlock/issues/12) |
| **NVIDIA GeForce RTX 3090 24GB** | Ampere (GA102) | Gigabyte TRX40 AORUS MASTER (AMD Threadripper 3960X) | **117.24 TFLOPS** (100% Unrestricted Tensor Compute) | [#13](https://github.com/BrightRaider/universal-gpu-unlock/issues/13) |
| **ZOTAC GeForce RTX 4080 Trinity OC** | Ada Lovelace (AD103) | GIGABYTE X870 AORUS ELITE WIFI7 (AMD 9800X3D) | **174.29 TFLOPS** (Full Unrestricted GEMM) | [#9](https://github.com/BrightRaider/universal-gpu-unlock/issues/9) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | MSI MPG X670E Carbon WIFI (AMD Ryzen 7 7800X3D iGPU) | **255.0 TFLOPS** (+54.5% boost, Headless Compute) | [#14](https://github.com/BrightRaider/universal-gpu-unlock/issues/14) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | Intel Z790 Platform (Multi-Drive ESP) | **255.4 TFLOPS** (Peak Compute) | [#8](https://github.com/BrightRaider/universal-gpu-unlock/issues/8) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | Intel Z690 Platform (Hybrid iGPU & Multi-Display) | **214.0+ TFLOPS** (Sustained FP16 GEMM) | [#5](https://github.com/BrightRaider/universal-gpu-unlock/issues/5) |
| **ASUS WS C621E SAGE Workstation** | Multi-Socket Server | ASPEED AST2500 BMC / IPMI Console | Multi-Console Isolation Verified | [#2](https://github.com/BrightRaider/universal-gpu-unlock/issues/2) |

> [!TIP]
> All benchmarks above were measured directly using **`Check-GPU-Unlock.bat`** under Windows. If your GPU and motherboard combination is verified and working, feel free to share your benchmark in Discussions or in an issue!

</details>

---

## 📊 Real-World Performance Gains

The unlock removes artificial firmware restrictions on the **Tensor Cores (GEMM matrix engines)** while maintaining 100% factory stability on standard shader pipelines:

| Workload / Model | Factory Stock | Unlocked UEFI | Real-World Improvement | Operational Impact |
| :--- | :--- | :--- | :--- | :--- |
| **FLUX.1-dev AI Sampling** | 1.20 it/s (25.0s) | **1.97 it/s (15.2s)** | 🚀 **+64.2% Faster** | High-res image generation in ~24s instead of ~34s |
| **MiniMax-H3 Video DiT** | 15.20 s/it (304s) | **11.15 s/it (223s)** | 🚀 **+36.3% Faster** | Saves **81 seconds** per 5-second video render |
| **FP16 Tensor GEMM ($4096^2$)** | 132.1 TFLOPS | **208.9 TFLOPS** | 🚀 **+58.1% Boost** | Maximum compute throughput within 72MB L2 cache |
| **BF16 Tensor GEMM ($8192^2$)** | 149.3 TFLOPS | **167.4 TFLOPS** | 🚀 **+12.1% Boost** | Sustained large-matrix training acceleration |
| **FP32 CUDA Shaders** | 51.3 TFLOPS | **48.7 TFLOPS** | ✅ **Stock Parity** | Standard raster rendering remains at factory stock |
| **VRAM Copy Bandwidth** | 733.4 GB/s | **707.2 GB/s** | ✅ **Stock Parity** | Physical GDDR6X bus operation unchanged |

---

## 🛡️ Safety & Anti-Cheat Guarantee

- **100% Volatile RAM-Only:** Zero EEPROM writes. Powering off or booting without the USB drive returns the GPU permanently to 100% factory stock.
- **Official NVIDIA Drivers:** Runs seamlessly with standard, unmodified NVIDIA Game Ready and Studio drivers (`nvlddmkm.sys`).
- **Anti-Cheat Safe:** Zero driver patches, kernel hooks, or runtime memory injectors. Competitive anti-cheats (Riot Vanguard, Easy Anti-Cheat, BattlEye, Ricochet) operate with zero detection risk (`UEFISecureBootEnabled: 1` remains active).
- **S3 Sleep Protection:** Includes **`Toggle-Sleep-Button.bat`** in the root directory to hide the "Sleep" button from the Windows Start menu, preventing accidental loss of volatile RAM state.
- **44-Cycle Testbench Proven:** 44 supervised hardware test cycles verified on my primary RTX 4090 platform with a 100% pass rate.
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
├── src/                         <- Full C source code for the universal UEFI application
├── Setup.bat                    <- The 1-Click Provisioner Launcher (Primary tool)
├── Setup-AdaUnlock.ps1          <- Automated PowerShell deployment engine
├── Check-GPU-Unlock.bat         <- Standalone Windows verification benchmark (>200 TFLOPS)
├── Universal-Control-Center.cmd <- Optional Diagnostic & Telemetry Dashboard (WPF GUI)
├── Toggle-Sleep-Button.bat      <- Non-invasive tool to hide/restore Start Menu Sleep button (S3 protection)
└── CHANGELOG.md                 <- Complete release and version history
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
