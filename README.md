<div align="center">

# Universal GPU Unlock (One-Click UEFI Tool)
### High-Performance Tensor Core Rate-Limiter Bypass for NVIDIA GPUs

[![GitHub Release](https://img.shields.io/github/v/release/BrightRaider/universal-gpu-unlock?color=blue&logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest)
[![Release Version](https://img.shields.io/badge/Release-v1.8.23%20(Latest)-blue?logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v1.8.23)
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

</div>

---

## ⚡ 60-Second Quick Start

> [!IMPORTANT]
> **BIOS Prerequisite:** Ensure **CSM (Compatibility Support Module)** is set to **Disabled** (pure UEFI mode) in your motherboard BIOS. Modern GeForce RTX 30 & 40 GPUs require native UEFI to avoid legacy VGA conflicts (Code 43) and to unlock Resizable BAR.

1. **Download:** Get the latest release package: **[Universal_GPU_Unlock_v1.8.23.zip](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest)**.
2. **Provision:** Extract the ZIP, insert a USB flash drive (FAT32), and run **`Setup.bat`** (formats & copies all required UEFI files automatically).
3. **Boot & Unlock:** Restart your PC, open your motherboard boot menu (`F8` / `F11` / `F12`), and boot from the USB drive:
   - **`[U]` Standard Unlock:** RAM-only, temporary unlock (reverts cleanly on complete power off).
   - **`[I]` Install to NVRAM:** Registers the UEFI DXE driver into motherboard NVRAM and internal ESP for automatic, stickless boot on every cold start.
   - *(Tip: The boot menu features a 30-second countdown. Press `[Space]` at any time to pause and read in peace).*
4. **Verify:** In Windows, run **`Check-GPU-Unlock.bat`** to verify your unlocked Tensor Core throughput (>200 TFLOPS on RTX 4090).
   - *(RTX 20 / Turing users: Run `Toggle-Turing-GSP.bat` once as Administrator to bind NVIDIA drivers to GSP firmware mode).*
   - *(Optional: Launch `Universal-Control-Center.cmd` for detailed hardware telemetry, SMI sensor readbacks, and diagnostics).*

---

## 🚀 Supported Hardware

| Generation | Architecture | Supported Graphics Cards | Device IDs |
| :--- | :--- | :--- | :--- |
| **RTX 40 Series** | **Ada Lovelace** | RTX 4090, RTX 4090D, RTX 4080, RTX 4080 SUPER, RTX 4070 Ti SUPER | `0x2684`, `0x2685`, `0x2702`, `0x2703`, `0x2704`, `0x2705` |
| **RTX 30 Series** | **Ampere** | RTX 3090, RTX 3090 Ti, RTX 3080 Ti, RTX 3080 (10/12GB), RTX 3070 Ti, RTX 3070, RTX 3060 Ti, RTX 3060, CMP 90HX | `0x2204`, `0x2208`, `0x2206`, `0x220a`, `0x2216`, `0x2207`, `0x2482`, `0x2484`, `0x2487`, `0x2488`, `0x2489`, `0x2503`, `0x2504`, `0x2507`, `0x2508`, `0x2520`, `0x2560` |
| **RTX 20 Series** | **Turing** (TU102, in testing: [#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15)) | RTX 2080 Ti, TITAN RTX, RTX 2080 / SUPER, RTX 2070 / SUPER, RTX 2060 / SUPER, CMP 50HX, Quadro RTX | `0x1E04`, `0x1E07`, `0x1E02`, `0x1E81`, `0x1E82`, `0x1E87`, `0x1E84`, `0x1F02`, `0x1F07`, `0x1F06`, `0x1F47`, `0x1F03`, `0x1F08`, `0x1F12`, `0x1E09`, `0x1EB0`, `0x1EB1`, `0x1E30`, `0x1E35` |

- **Any Vendor:** Compatible with Founders Edition, ASUS, MSI, Gigabyte, ZOTAC, PNY, Palit, Gainward, etc.
- **Any Platform:** Intel (Z690, Z790, Z890, B760, Z390) and AMD (AM4/AM5, X570, B550, B650, X670E, X870E).
- **Multi-Drive:** Prefers `bootmgfw.efi` and follows your firmware's boot order when several Windows volumes exist.
- **Multi-GPU & Headless Compute:** Unlocks primary or dedicated compute GPUs. Discrete cards can operate completely headless as dedicated AI/compute accelerators with all monitors connected to integrated graphics (e.g. AMD Ryzen Raphael iGPU or Intel UHD) — resolving [#14](https://github.com/BrightRaider/universal-gpu-unlock/issues/14).
- **ASUS & AMI Console Topology Verified:** Multi-display console detachment verified on ASUS PRIME Z690-A at **127.61 TFLOPS** (100% unrestricted compute) on EVGA GeForce RTX 3080 Ti — resolving [#16](https://github.com/BrightRaider/universal-gpu-unlock/issues/16).
- **ASRock AM4 & CSM Resolution Verified:** Budget AM4 POST discovery and CSM legacy video conflict resolved on ASRock A520M-HVS at **106.05 TFLOPS** (+95.7% boost, unrestricted compute) on Gainward GeForce RTX 3080 Phoenix — refs [#17](https://github.com/BrightRaider/universal-gpu-unlock/issues/17).
- **Ampere Verified, Turing in Testing:** GA106 (RTX 3060) and GA102 (RTX 3080, RTX 3080 Ti, RTX 3090) are confirmed in the community table below. GA104 is supported but not yet confirmed in an issue. On TU102 (RTX 2080 Ti) the unlock transaction completes, and the throughput gain is still being confirmed in [#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15).
- **Stickless Internal NVMe-ESP Persistence (`[I]` Mode):** Installs `AdaUnlockDxe.efi` and the firmware assets to your internal EFI System Partition (`\EFI\Drivers\`) and registers it in the UEFI `DriverOrder`, so the USB stick is no longer needed. If the copy to the ESP fails, `[I]` registers the driver on the USB drive instead, which then has to stay connected. If an unlock fails, the driver disables itself for the next boot, and `[I]` enables it again.

<details>
<summary><b>🔍 Community-Verified Hardware & Live Benchmarks (Click to expand)</b></summary>
<br>

The following GPU models, system motherboards, and compute readbacks have been independently verified on physical hardware by community users:

| Verified GPU | Architecture & Silicon | Motherboard & Platform | Real-World Throughput | Verification Issue |
| :--- | :--- | :--- | :--- | :--- |
| **Palit GeForce RTX 3060 12GB** | Ampere (GA106 LHR) | MSI MPG Z390 GAMING PLUS | **46.08 TFLOPS** (~45% ComfyUI speedup) | [#11](https://github.com/BrightRaider/universal-gpu-unlock/issues/11) |
| **Gainward GeForce RTX 3080 Phoenix 10GB** | Ampere (GA102) | ASRock A520M-HVS (AMD AM4) | **106.05 TFLOPS** (+95.7% boost, Unrestricted Compute) | [#17](https://github.com/BrightRaider/universal-gpu-unlock/issues/17) |
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

The unlock removes artificial firmware restrictions on the **Tensor Cores (GEMM matrix engines)**. FP32 shader and memory throughput stay at stock. Measured on an RTX 4090:

| Workload / Model | Factory Stock | Unlocked UEFI | Real-World Improvement | Operational Impact |
| :--- | :--- | :--- | :--- | :--- |
| **FLUX.1-dev AI Sampling** | 1.20 it/s (25.0s) | **1.97 it/s (15.2s)** | 🚀 **+64.2% Faster** | High-res image generation in ~24s instead of ~34s |
| **MiniMax-H3 Video DiT** | 15.20 s/it (304s) | **11.15 s/it (223s)** | 🚀 **+36.3% Faster** | Saves **81 seconds** per 5-second video render |
| **FP16 Tensor GEMM ($4096^2$)** | 132.1 TFLOPS | **208.9 TFLOPS** | 🚀 **+58.1% Boost** | Maximum compute throughput within 72MB L2 cache |
| **BF16 Tensor GEMM ($8192^2$)** | 149.3 TFLOPS | **167.4 TFLOPS** | 🚀 **+12.1% Boost** | Sustained large-matrix training acceleration |
| **FP32 CUDA Shaders** | 51.3 TFLOPS | **48.7 TFLOPS** | ✅ **Stock Parity** | Standard raster rendering remains at factory stock |
| **VRAM Copy Bandwidth** | 733.4 GB/s | **707.2 GB/s** | ✅ **Stock Parity** | Physical GDDR6X bus operation unchanged |

---

## 🛡️ Safety & Anti-Cheat

- **Volatile (RAM-only):** The unlock lives in GPU registers and is gone after a full power-off. Without the `[I]` driver installed, the next boot is stock. On the test bench, the card always booted stock again after a power-off.
- **Official NVIDIA Drivers:** Runs seamlessly with standard, unmodified NVIDIA Game Ready and Studio drivers (`nvlddmkm.sys`).
- **Anti-Cheat:** No driver patches, kernel hooks or runtime memory injection. With the image or certificate enrolled in `db`, `UEFISecureBootEnabled` stays `1`. Individual anti-cheat engines (Riot Vanguard, Easy Anti-Cheat, BattlEye, Ricochet) were not tested systematically.
- **S3 Sleep Protection:** Includes **`Toggle-Sleep-Button.bat`** in the root directory to hide the "Sleep" button from the Windows Start menu, preventing accidental loss of volatile RAM state.
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
├── src/                         <- Full C source code for the universal UEFI application
├── tests/                       <- Host tests with mocked UEFI services (no GPU required)
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
