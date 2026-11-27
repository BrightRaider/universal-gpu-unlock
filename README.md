<div align="center">

# Universal GPU Unlock

**English** | [简体中文](docs/README.zh-CN.md) | [Deutsch](docs/README.de.md)

Pre-boot UEFI Tensor Core unlocker for NVIDIA RTX 40 (Ada), RTX 30 (Ampere) & RTX 20 (Turing) GPUs.

[![GitHub Release](https://img.shields.io/github/v/release/BrightRaider/universal-gpu-unlock?color=blue&logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest)
[![Release Version](https://img.shields.io/badge/Release-v2.0.0-blue?logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases/tag/v2.0.0)
[![Downloads](https://img.shields.io/github/downloads/BrightRaider/universal-gpu-unlock/total?color=green&logo=github)](https://github.com/BrightRaider/universal-gpu-unlock/releases)
[![Stars](https://img.shields.io/github/stars/BrightRaider/universal-gpu-unlock?style=social)](https://github.com/BrightRaider/universal-gpu-unlock)
[![Safety](https://img.shields.io/badge/Safety-RAM--Only-success.svg)](#safety--anti-cheat)
[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20Me%20A%20Coffee-ff5e5b?style=flat&logo=kofi&logoColor=white)](https://ko-fi.com/brightraider)

<p>
  Persistent ACPI S3 Sleep · Stickless Auto-Boot · Secure Boot &amp; Vanguard Tested · PyTorch, CUDA &amp; WSL2
</p>

<img src="docs/images/dashboard-v2.0.0.png" alt="Universal GPU Unlock Dashboard — Live S3 ARMED Status &amp; Power-Limit Control" width="760" />

</div>

---

I built Universal GPU Unlock as a PC gamer who runs local AI workloads on the same machine. NVIDIA limits FP16/BF16 Tensor Core matrix throughput in consumer GeForce firmware. My focus with `v2.0.0` was solving the practical daily-driver issues that prevented me from leaving an unlocker active permanently: stickless cold boots (`[I]`), surviving Windows ACPI S3 sleep (`[P]` / `[I]`), and keeping Secure Boot + Riot Vanguard working without VBIOS flashing (`0` permanent writes, `0%` gaming difference).

## Quick Start

1. **Prepare (`Setup.bat`):** Download and extract **[Universal_GPU_Unlock_v2.0.0.zip](https://github.com/BrightRaider/universal-gpu-unlock/releases/latest)**, plug in a FAT32 USB flash drive, and run `Setup.bat`:
   - `[1]` **New USB setup** *(runs BitLocker / Secure Boot / S0ix pre-flight checks)*
   - `[2]` **Update existing USB**
   - `[3]` **Create hidden `512 MB` `GPU_UNLOCK` partition** *(optional — enables stickless boot on `100 MB` Windows ESPs)*
   - `[4]` **Open Dashboard Cockpit** / `[5]` **Export Latest Boot Log** *(copies `RUN*.TXT` even from hidden `GPU_UNLOCK` / protected ESPs into `Boot-Logs\`)*
2. **Boot & Unlock:** Boot from the USB drive (`F8` / `F11` / `F12`) and press one key *(press `[Space]` anytime to pause the 30s menu timer)*:
   - `[U]` **Standard Unlock:** Volatile RAM-only unlock (reverts to stock on power-off).
   - `[P]` **S3 Persistence:** Keeps the unlock active across Windows Sleep / Standby (ACPI S3).
   - `[I]` **Stickless Auto-Unlock (`NVRAM`):** Installs `AdaUnlockDxe.efi` (`Driver####`) so cold boots unlock automatically and survive ACPI S3 sleep with no USB drive attached afterward.
   - `[R]` **Remove DXE Driver:** Cleanly removes `[I]` from UEFI NVRAM and internal partitions.
   - `[S]` **Stock Windows:** Runs read-only diagnostic scan/log only and boots stock.
3. **Verify & Tune in Windows:** Run **`Launch-Dashboard.bat`** (GUI Cockpit with `EN` / `简体中文` / `DE` switcher: live `S3 ARMED` indicator, 1-W power-limit slider, SM clock-cap (`-lgc`) controls with S3-wake re-apply, baseline comparison) or **`Check-GPU-Unlock.bat`** (CLI benchmark & `FMLA32/16` check).

> [!TIP]
> **First-Boot BIOS Checklist:** Set **CSM = Disabled** (Pure UEFI) to avoid blank screens or Error Code 43, and enable **Above 4G Decoding** + **Resizable BAR** for full memory throughput (+10% to +15%).

---

## Secure Boot & Anti-Cheat

- **Without Anti-Cheat (easiest):** Temporarily set **Secure Boot** to **Disabled** (or *Other OS*) in BIOS.
- **Keep Secure Boot enabled (Windows 11):** In BIOS under **Security ➔ Secure Boot ➔ Key Management ➔ Authorized Signatures (`db`)**, choose **Append / Enroll DB Key**, select `AdaUnlock_SecureBoot.cer` from the USB drive, and save (`F10`). *(Always **append** — never delete Microsoft/OEM factory keys.)*

### Riot Vanguard (*Valorant* / *League of Legends*)

1. **Keep Secure Boot enabled** with factory keys intact *(append `AdaUnlock_SecureBoot.cer` to `db` only if your BIOS blocks the driver)*.
2. **Install `[I]` once:** Boot from USB once (`F8` / `F11` / `F12`) and press **`[I]`**. *(If your Windows ESP is only `100 MB`, run `Setup.bat [3]` first to create the hidden `512 MB` `GPU_UNLOCK` partition for Ada/Ampere assets.)*
3. **Boot Windows normally:** Unplug the USB drive and boot via **Windows Boot Manager** (`bootmgfw.efi`) — never from the USB entry.

<details>
<summary><b>Verified Vanguard + Stickless S3 Notes</b></summary>

- **S3 Sleep + Vanguard:** Tested with **Secure Boot enabled** (`Confirm-SecureBootUEFI: True`) and `vgk` / `vgtray` / Riot Client active across both stickless reboots (**224.52 TFLOPS**) and ACPI S3 sleep/wake (**221.19 TFLOPS**; requires classic **ACPI S3** in BIOS, not Modern Standby `S0ix`).
- **Fast Startup note:** If an unexpected power loss triggers Windows Fast Startup `0xC00000D4` and temporarily stops `vgk.sys`, run one normal Windows **Restart** (bypasses `hiberfil.sys`).
- **Technical Deep-Dive:** [OPERATIONS_AND_PERSISTENCE.md](docs/OPERATIONS_AND_PERSISTENCE.md) · [DOCUMENTATION.md](DOCUMENTATION.md)

</details>

---

## Supported Hardware

| Generation | Architecture | Supported Graphics Cards | Device IDs |
| :--- | :--- | :--- | :--- |
| **RTX 40 Series** | **Ada Lovelace** | RTX 4090 / 4090D, RTX 4080 / SUPER, RTX 4070 Ti SUPER / Ti / SUPER / 4070, RTX 4060 Ti (8GB/16GB) / 4060, RTX 4090 / 4080 / 4070 / 4060 Laptop | `0x2684`, `0x2685`, `0x2702`, `0x2703`, `0x2704`, `0x2705`, `0x2717`, `0x2757`, `0x2782`, `0x2783`, `0x2786`, `0x27a0`, `0x27e0`, `0x2803`, `0x2805`, `0x2807`, `0x2820`, `0x2860`, `0x2882`, `0x28a0`, `0x28e0` |
| **RTX 30 Series** | **Ampere** | RTX 3090, RTX 3090 Ti, RTX 3080 Ti, RTX 3080 (10/12GB), RTX 3070 Ti, RTX 3070, RTX 3060 Ti, RTX 3060, RTX 3050 (8GB/Laptop), RTX 2050 Mobile, CMP 90HX | `0x2204`, `0x2208`, `0x2206`, `0x220a`, `0x2216`, `0x2207`, `0x2482`, `0x2484`, `0x2487`, `0x2488`, `0x2489`, `0x2503`, `0x2504`, `0x2507`, `0x2508`, `0x2520`, `0x2560`, `0x2582`, `0x25ec`, `0x25ed` |
| **RTX 20 Series** | **Turing** (`TU102` / `TU104` / `TU106`, [#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15), [#18](https://github.com/BrightRaider/universal-gpu-unlock/issues/18)) | RTX 2080 Ti, TITAN RTX, RTX 2080 / SUPER, RTX 2070 / SUPER, RTX 2060 / SUPER, CMP 50HX, CMP 40HX, Quadro RTX | `0x1E04`, `0x1E07`, `0x1E02`, `0x1E81`, `0x1E82`, `0x1E87`, `0x1E84`, `0x1E89`, `0x1F02`, `0x1F07`, `0x1F06`, `0x1F0B`, `0x1F47`, `0x1F03`, `0x1F08`, `0x1F12`, `0x1E09`, `0x1EB0`, `0x1EB1`, `0x1E30`, `0x1E35` |

- **Platforms & Multi-GPU:** Works across Founders Edition / AIB cards, Intel (`Z390`–`Z890`) & AMD (`AM4`/`AM5`), multi-drive / VeraCrypt UEFI systems, and **Multi-GPU / Headless Compute** (display on iGPU, discrete GPU compute-only).
- **Hardware Boundaries (`GTX 16xx` / USB4 eGPUs / Laptops):**
  - **GTX 16 Series (`TU116` / `TU117`) is intentionally excluded:** These GPUs have no Tensor Cores and fail Windows `nvlddmkm` `RmInitAdapter` with **Error Code 43** even when pre-boot register writes succeed.
  - **USB4 / Thunderbolt eGPUs (e.g., `ASM2464PD`):** Built for internal PCIe slots. Tunneled USB4/Thunderbolt eGPUs are unsupported/experimental due to pre-`ExitBootServices` ACPI power-chain hangs (`_PR0`/D3Cold) and OS Connection Manager tunnel resets (`Usb4DeviceRouter` / Linux `thunderbolt`).
  - **Laptop Hybrid / Optimus vs. dGPU-Direct:** On laptops (`RTX 4090`–`4060` Laptop), BIOS **dGPU-Direct (MUX)** retains the unlock most reliably. In Hybrid/Optimus mode, Windows Runtime D3 (`D3cold`) can reset idle GPUs unless guarded (run `tools\Setup-FastResume.bat` to apply `RMDisableRtd3` / ASPM off).
- **ESP Space for Stickless `[I]`:** **Turing** needs ~28 MB (fits standard `100 MB` Windows ESPs). **Ada & Ampere** need ~85 MB — run `Setup.bat [3]` to create a hidden `512 MB` `GPU_UNLOCK` partition, leave the USB drive plugged in as storage (`bootmgfw.efi` still boots Windows), or use `[P]` S3 Sleep.
- **Turing GSP Mode (`RTX 20` / `CMP 40HX`):** After pre-boot unlock, run `tools\Toggle-Turing-GSP.bat` once as Administrator in Windows to enable full compute throughput ([#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15), [#18](https://github.com/BrightRaider/universal-gpu-unlock/issues/18)).
- **Boot-Loop Safeguard:** Stages a temporary `ATTEMPT.FLG` before hardware writes and reverts to stock Windows boot automatically if an unlock attempt is interrupted.

<details>
<summary><b>Community-Verified Hardware & Live Benchmarks</b></summary>
<br>

| Verified GPU | Architecture & Silicon | Motherboard & Platform | Measured Throughput | Reference |
| :--- | :--- | :--- | :--- | :--- |
| **NVIDIA GeForce RTX 2080 Ti 11GB** | Turing (TU102) | Intel Z390 / AMD AM4 | **105.4 TFLOPS** (GSP Compute Mode) | [#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15) |
| **NVIDIA CMP 40HX 8GB** | Turing (TU106) | AMD AM4 / Intel Platform | **54.2 TFLOPS** (Headless Compute) | [#18](https://github.com/BrightRaider/universal-gpu-unlock/issues/18) |
| **NVIDIA GeForce RTX 2060 6GB (TU104-150)** | Turing (TU104) | ASUS TUF B450-PLUS GAMING (AM4) | **25.82 TFLOPS** (Dense GEMM, 4.21x over FP32) | [@resYuto in #20](https://github.com/BrightRaider/universal-gpu-unlock/issues/20) |
| **Palit GeForce RTX 3060 12GB** | Ampere (GA106 LHR) | MSI MPG Z390 GAMING PLUS | **46.08 TFLOPS** (~45% ComfyUI speedup) | [#11](https://github.com/BrightRaider/universal-gpu-unlock/issues/11) |
| **Gigabyte GeForce RTX 3060 12GB (`10DE:2504`)** | Ampere (GA106) | Gigabyte B760M D2H (ReBAR 16GB) | **47.0 TFLOPS** (`24.2 -> 47.0`, +94.2%, USB & Stickless `[I]`) | [@henry-richard7 in #24](https://github.com/BrightRaider/universal-gpu-unlock/issues/24) |
| **NVIDIA GeForce RTX 3060 Ti 8GB (`10DE:2489`)** | Ampere (GA104) | MSI PRO Z790-A WIFI DDR4 (i5-14600KF) | **61.23 TFLOPS FP16 (+104.4%) / 61.97 TFLOPS BF16** | [@karadeveci in #22](https://github.com/BrightRaider/universal-gpu-unlock/issues/22) |
| **Gainward GeForce RTX 3080 Phoenix 10GB** | Ampere (GA102) | ASRock A520M-HVS (AM4) | **106.05 TFLOPS** (+95.7%) | [#17](https://github.com/BrightRaider/universal-gpu-unlock/issues/17) |
| **EVGA GeForce RTX 3080 Ti 12GB** | Ampere (GA102) | ASUS PRIME Z690-A | **127.61 TFLOPS** | [#16](https://github.com/BrightRaider/universal-gpu-unlock/issues/16) |
| **EVGA GeForce RTX 3080 Ti 12GB** | Ampere (GA102) | MSI X570-A PRO | **119.0 TFLOPS** (+84.5%) | [#12](https://github.com/BrightRaider/universal-gpu-unlock/issues/12) |
| **NVIDIA GeForce RTX 3090 24GB** | Ampere (GA102) | Gigabyte TRX40 AORUS MASTER (3960X) | **117.24 TFLOPS** | [#13](https://github.com/BrightRaider/universal-gpu-unlock/issues/13) |
| **MSI GeForce RTX 3090 GAMING X TRIO 24G** | Ampere (GA102) | ASUS ROG STRIX B550-E GAMING | **105.10 TFLOPS** | Community Verified |
| **EVGA GeForce RTX 3090 Ti 24GB** | Ampere (GA102-350) | ASUS PRIME Z490-P (i7-10700) | **132.4 TFLOPS FP16 / 136.6 TFLOPS BF16** (+102%) | [@m4ttfx in #495](https://github.com/windows-GPU-UNLOCK/Windows-GPU-UNLOCK/issues/495#issuecomment-7077703728) |
| **Dual GPU: HP OEM RTX 3090 + EVGA RTX 3060** | Ampere (GA102 + GA106) | ASUS TUF GAMING Z590-PLUS WIFI | **86.0 TFLOPS (3090) / 49.6 TFLOPS (3060)** | [#19](https://github.com/BrightRaider/universal-gpu-unlock/issues/19) |
| **ZOTAC GeForce RTX 4080 Trinity OC** | Ada Lovelace (AD103) | GIGABYTE X870 AORUS ELITE WIFI7 (9800X3D) | **174.29 TFLOPS** | [#9](https://github.com/BrightRaider/universal-gpu-unlock/issues/9) |
| **PNY GeForce RTX 4090 24GB** | Ada Lovelace (AD102) | Gigabyte Z690 UD AC-Y1 (i9-13900K) | **255.0 TFLOPS** (+59.4%, +15–25% ComfyUI gain) | [@jacyzheng in #444](https://github.com/windows-GPU-UNLOCK/Windows-GPU-UNLOCK/issues/444) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | MSI MPG X670E Carbon WIFI (7800X3D iGPU) | **255.0 TFLOPS** (+54.5%, Headless & S3 verified) | [@Lizardon1024 in #14](https://github.com/BrightRaider/universal-gpu-unlock/issues/14) · [#25](https://github.com/BrightRaider/universal-gpu-unlock/issues/25) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | Intel Z790 (Multi-Drive ESP) | **255.4 TFLOPS** | [#8](https://github.com/BrightRaider/universal-gpu-unlock/issues/8) |
| **NVIDIA GeForce RTX 4090** | Ada Lovelace (AD102) | Intel Z690 (Hybrid iGPU / Multi-Display) | **214.0+ TFLOPS** | [#5](https://github.com/BrightRaider/universal-gpu-unlock/issues/5) |
| **ASUS WS C621E SAGE Workstation** | Multi-Socket Server | ASPEED AST2500 BMC / IPMI | Multi-Console Isolation Verified | [#2](https://github.com/BrightRaider/universal-gpu-unlock/issues/2) |

> [!TIP]
> All benchmarks above were measured with **`Check-GPU-Unlock.bat`** under Windows. If your board and GPU combo works well, feel free to share your output in Discussions or Issues.

</details>

---

## Performance Benchmarks (RTX 4090)

The unlock lifts the firmware rate-limiter on **Tensor Core matrix operations (GEMM)**. Standard FP32 CUDA shaders and VRAM bandwidth stay at stock:

| Workload / Format | Factory Stock | Unlocked UEFI | Measured Change |
| :--- | :--- | :--- | :--- |
| **FLUX.1-dev Sampling** | 1.20 it/s (25.0s) | **1.97 it/s (15.2s)** | **+64.2% faster** |
| **MiniMax-H3 Video DiT** | 15.20 s/it (304s) | **11.15 s/it (223s)** | **+36.3% faster** |
| **FP16 Tensor GEMM ($4096^2$)** | 132.1 TFLOPS | **208.9 TFLOPS** | **+58.1%** *(L2 cache bound)* |
| **BF16 Tensor GEMM ($8192^2$)** | 149.3 TFLOPS | **167.4 TFLOPS** | **+12.1%** |
| **FP32 CUDA Shaders** | 51.3 TFLOPS | **48.7 TFLOPS** | **Stock parity** *(no gaming impact)* |
| **VRAM Copy Bandwidth** | 733.4 GB/s | **707.2 GB/s** | **Stock parity** |

> [!NOTE]
> **cuBLASLt Kernel Heuristics:** NVIDIA's `cuBLASLt` library picks matrix multiplication heuristics calibrated for rate-limited consumer cards. Setting `torch.backends.cudnn.benchmark = True` in PyTorch (or enabling autotuning in ComfyUI) adds another **+15% to +35%** throughput across dense GEMM, SDXL, and LLM prefill workloads.

### Ada Efficiency Sweetspot: Clock-Capping (`-lgc`) vs. Power Limits (`-pl`)

Sustained `10.1s` (`242`-frame, `1184×896`) **MiniMax-H3** Video DiT runs on an unlocked **RTX 4090 (`+600 MHz` VRAM)** show why Ada workstation cards like the RTX 6000 Ada target `~300 W`, and why locking SM clocks beats lowering `-pl`:

| Tuning Profile *(Admin CLI)* | Render Time | Avg Power | Energy / Video | Takeaway |
| :--- | :--- | :--- | :--- | :--- |
| **`450 W` Stock (`-pl 450`)** | `6m 24s` (`384.4s`) | `411.4 W` | `43.93 Wh` | Peak speed; scales only `+3.1%` above `2445 MHz` at `+40–60 W` cost |
| **`315 W` Limit (`-pl 315` / `70%`)** | `7m 42s` (`462.5s`) | `307.0 W` | `39.44 Wh` | `SW Power Cap` stalls SM clock (`~2108 MHz` avg, `20.3%` slower) |
| **`-lgc 210,2450` *(Open `450 W` PL)*** | **`6m 36s`** (`396.4s`) | `370.8 W` | **`40.89 Wh`** (`-6.9%`) | **Sweetspot:** **66s faster** than `70% PL`, within `3.1%` of `450 W` |
| **`-lgc 210,2300` *(Open `450 W` PL)*** | **`7m 08s`** (`427.9s`) | `325.6 W` | **`38.74 Wh`** (`-11.8%`) | **Max Efficiency:** **34.6s faster & fewer total Wh** than `70% PL` |

- **Practical setup:** Use **`Launch-Dashboard.bat`** (`SM CLOCK CAP` ➔ **Sweetspot** / **Max Eff** or **AI Efficiency** preset) or `nvidia-smi -lgc 210,2450` / `210,2300` paired with `+600–700 MHz` VRAM (`+3.5–4.5%` faster sampling in CUDA `P2` state; `+800/+900 MHz` adds no extra gain due to GDDR6X timing/ECC overhead). Click **Gaming / Stock** (`-rgc`) afterward before gaming. *(Preset targets apply to RTX 40 Ada; RTX 30/20 scale automatically to supported bins. Full sweep data in [DOCUMENTATION.md](DOCUMENTATION.md#1353-ada-rtx-4090-efficiency-sweetspot--clock-capping-vs-power-limiting).)*

---

## Safety & Anti-Cheat

- **Volatile (RAM-only):** Lives strictly in GPU registers and resets to factory stock on cold power-off (`0` permanent VBIOS writes).
- **Official NVIDIA Drivers:** Works with unmodified NVIDIA Game Ready and Studio drivers (`nvlddmkm.sys`).
- **Anti-Cheat:** No driver patches, kernel hooks, or runtime memory injection (`UEFISecureBootEnabled` remains `1`). **Riot Vanguard (*Valorant* / *League of Legends*)** works with **Secure Boot enabled** via `[I]` (`AdaUnlockDxe.efi`) and a normal **Windows Boot Manager** boot (see [Secure Boot & Anti-Cheat](#secure-boot--anti-cheat)). Easy Anti-Cheat, BattlEye, and Ricochet were not tested systematically.
- **Load- & S3-Aware Checks:** `Check-GPU-Unlock.bat` and `Launch-Dashboard.bat` check live GPU utilization (`nvidia-smi` > 15%) and `FMLA32/FMLA16` ratios ahead of benchmarking so active apps never trigger false `STOCK RATE-LIMITER ACTIVE` warnings.
- **Sleep & Hibernation Helpers:** Includes `tools\Setup-FastResume.bat` (Hibernate shortcut + Idle Relock Guard `RMDisableRtd3` / PCIe ASPM off for Optimus & headless GPUs) and `tools\Toggle-Sleep-Button.bat` (hide Start Menu Sleep when using `[U]`).

> [!WARNING]
> **Windows S4 Hibernate vs. Pre-Boot ReBAR Overrides:** Universal GPU Unlock uses a pure SS-register unlock without mutating pre-boot ReBAR BAR1 registers (`0x610`/`0xBC0`), keeping pure-SS S4 Hibernate safe. Do **not** combine third-party pre-boot ReBAR overrides (`ReBarDxe` / `0x610`/`0xBC0`) with Windows S4 Hibernation (`Setup-FastResume.bat` / `shutdown /h`), as that combination causes `Kernel-Power 41` hangs (and forced GOP reconnects during S4 resume trigger TDR reboots).
- **Operational & Persistence Deep-Dive:** Read the telemetry, UEFI safeguard, and S3 architecture guide in **[OPERATIONS_AND_PERSISTENCE.md](docs/OPERATIONS_AND_PERSISTENCE.md)**.

---

<details>
<summary><b>Technical Architecture & Workload Scaling Notes</b></summary>

### Why is the boost largest at $4096 \times 4096$ (+58.1%)?
Ada Lovelace AD102 has a **72 MB L2 cache**. A $4096 \times 4096$ FP16 matrix (~32 MB per operand) fits inside L2 (>3.5 TB/s internal bandwidth), making execution purely **compute-bound**. At $16384 \times 16384$ (>1.5 GB), workloads become **memory-bandwidth bound** by external GDDR6X VRAM. Real-world AI training and diffusion models slice tensors into $1024$–$4096$ tiles, sitting right in the compute-bound sweet spot.

### Why does quantized `Q4` LLM token decode barely change (~0–5%)?
Single-stream quantized LLM decode (**`Q4` / `Q4_K_M` / `MoE` decode**) — as well as memory-bound video filters (similar to Topaz Video AI in `#22`) — is limited by **VRAM memory bandwidth**, which stays at factory stock (`0%` change). Compute-bound **BF16/FP16 training**, **ComfyUI / FLUX / Video DiT** sampling, and **long-context LLM prefill** saturate the Tensor Cores directly and gain substantially.

</details>

---

## Build & Tests

Requires Visual Studio 2022 C++ Build Tools (x64).

| Command | What it does |
| :--- | :--- |
| `src\Build-Experimental-MSVC.ps1` | Builds `BOOTX64.EFI` and **overwrites `bin\BOOTX64.EFI`**. Signs it only if the maintainer certificate is installed. |
| `src\Build-Dxe-Driver.ps1` | Builds `AdaUnlockDxe.efi` and **overwrites `bin\AdaUnlockDxe.efi`**, with the same signing rule. |
| `src\Build-Gpu-Topology-Diagnostic.ps1` | Builds the read-only topology diagnostic EFI. |
| `tests\Run-MsvcHostTests.ps1` | Runs all 12 MSVC host test suites (`driver_nvram`, `dxe_main`, `boot_select` PE32+ & rotation, `csm_handover`, `gpu_inventory`, `gpu_topology`, `gpu_discovery`, `turing_local`, `s3_resume`, `multi_gpu_second_preflight`, `already_unlocked`, `s3_non_identity`). |
| `python -m unittest tests.test_clock_cap -v` | Runs unit tests for `nvidia-smi -lgc` parsing, Ada/Ampere presets, laptop thresholds, wake reapply, PLM log decoding, and trilingual dashboard i18n. |

The gcc/clang host tests for topology, inventory, and discovery live in `tests/`. The CI workflow (`.github/workflows/gpu-topology-diagnostic.yml`) runs on every push and pull request touching `src/` or `tests/`. All tests mock firmware services and `nvidia-smi`; none runs on live GPU hardware.

---

## Changelog

Full release history and device additions are tracked in **[CHANGELOG.md](CHANGELOG.md)** *(or under [GitHub Releases](https://github.com/BrightRaider/universal-gpu-unlock/releases))*.

---

## Repository Structure

```text
├── bin/                         <- Signed BOOTX64.EFI, AdaUnlockDxe.efi & co-located docs
├── assets/
│   ├── AD102-ASSETS/            <- Ada Lovelace microcode payloads (RTX 40 series)
│   ├── GA102-ASSETS/            <- Ampere microcode payloads (RTX 30 series)
│   └── TU102-ASSETS/            <- Turing microcode staging payloads (RTX 20 / CMP series)
├── src/                         <- C/ASM source code, MSVC EFI build scripts & Build-Release-Zip.ps1
├── tests/                       <- Host C tests (mocked UEFI) & Python unit tests
├── docs/                        <- Translations (README.zh-CN.md, README.de.md), whitepapers, archive & images/
├── Setup.bat                    <- Launcher (1: USB Setup, 2: Update USB, 3: Hidden Partition, 4: Dashboard, 5: Export Log)
├── Launch-Dashboard.bat         <- Primary GUI Cockpit (Benchmarks, S3 Armed, PL / -lgc Controls, Log export)
├── Check-GPU-Unlock.bat         <- Standalone CLI benchmark & hardware telemetry check
├── tools/                       <- PowerShell & Python setup/diagnostic utilities
├── AdaUnlock_SecureBoot.cer     <- Authenticode certificate for Secure Boot db enrollment
├── DOCUMENTATION.md             <- Full English technical documentation
└── CHANGELOG.md                 <- Recent release history (older entries in docs/CHANGELOG_ARCHIVE.md)
```

---

## Acknowledgements

* **[geodnb](https://github.com/geodnb) / [Ablation Lab](https://github.com/Ablation-Lab):** Pioneering research, cold-state analysis, and register architecture insights that made this universal Windows port possible ([Ablation Lab on GitHub](https://github.com/Ablation-Lab) · [YouTube @AblationLab](https://www.youtube.com/@AblationLab)).
* **Zagitta & WildFlash1st:** Linux `gpu-unlock` research and `cmp90hx-unlock` work.
* **[Ahsan Umair (@ahsanumair)](https://github.com/ahsanumair):** Initial Windows WPF UI layout groundwork adapted into `tools/Universal-Control-Center.cmd`.
* **TOTORONG & JustAResearcher:** Early proof-of-concept Windows UEFI implementations.
* **Igor Wallossek (Igor'sLAB):** Technical analysis, power verification, and community discussion.

---

## Support

If Universal GPU Unlock saved render time or sped up your AI workflows, feel free to buy me a coffee to support ongoing updates and hardware testing:

<a href='https://ko-fi.com/brightraider' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi2.png?v=3' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>
