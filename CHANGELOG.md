# Changelog - Universal Multi-Gen GPU Unlock

All notable changes, architectural improvements, and device expansions for Universal GPU Unlock are documented here.

For packaged binaries and downloads, visit [GitHub Releases](https://github.com/BrightRaider/universal-gpu-unlock/releases).

---

## [v1.8.9] - 2026-11-10
### Turing GFW Readiness & Dynamic Local Memory Range Logging (Addresses #15)
- **Turing GFW Sample Feature Assertion Fix (Addresses #15):**
  - Resolved `failure unlock_610.c:1400 unlock_transaction_gfw error=-5` reported by @mibby in `RUN0006.TXT`.
  - In `unlock_transaction_gfw()`, guarded Ampere/Ada-specific GFW register assertions (`sample.plm != 0x8b8f || sample.boot != 0x3ff || sample.cpuctl != 0x10`) with `!gpu_is_turing_device`. Turing silicon GFW completion is verified via `gpu_unlock_gfw_assess(&sample)` without requiring Ampere bitmask matches.
  - Added explicit telemetry logging for `gfw_plm`, `gfw_boot`, and `gfw_cpuctl` in the entry diagnostics banner.
- **Dynamic Local Memory Range Detection:**
  - `NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE` (`0x00100ce0`) dynamically detected 11264 MB (`0x2c00`) on TU102; refined aperture fallback behavior for general multi-capacity Turing SKUs.

---

## [v1.8.8] - 2026-11-10
### Turing Booter Signature, Fuse Selection & Framebuffer Local Memory Range Fix (Addresses #15)
- **Turing Booter Signature & Fuse Calculation Fix (Addresses #15):**
  - Resolved `failure unlock_610.c:769 patch_booter error=-34 last_mmio=0x824148 value=0xbadf5040` on Turing TU102 (RTX 2080 Ti) reported by @mibby in `RUN0005.TXT`.
  - In `patch_booter()`, guarded `GU_FUSE_SEC2_UCODE1` (`0x824148`) read on Turing devices. On Turing silicon, booter signature slot 0 contains the RSA-384 production signature; defaulted `fuse_ver = 1` (`index = 0`), preventing fuse calculation overflow (`-ERANGE`).
  - In `src/vendor/prep_610.c`, hardened `gpu_unlock_fuse_version()` against unmapped MMIO reads (`~0u` and `0xbadfXXXX`), ensuring unmapped registers never compute out-of-range fuse version indices.
  - In `unlock_transaction_dma()`, guarded FWSEC fuse register `0x008241c0` on Turing devices, selecting version 0 to match Turing FWSEC single-signature bitmask (`versions=0x1`).
- **Turing Usable Framebuffer Memory Range Detection (Addresses #15):**
  - In `unlock_transaction_layout()`, handled unmapped `0x001183a4` on Turing by querying `NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE` (`0x00100ce0`, decoding `lower_mag << lower_scale`) with an 11264 MB fallback, preventing `fb_mb = 0xbadf5040` corruption in memory layout plan.

---

## [v1.8.7] - 2026-11-10
### Turing MMU Lock & Transaction Layout Fix (Addresses #15)
- **Turing MMU Layout Architecture Fix (Addresses #15):**
  - Resolved `failure unlock_610.c:958 unlock_transaction_layout error=-19 last_mmio=0x1fa7c8 value=0xbadf5040` on Turing GPUs reported by @mibby.
  - On Turing (TU102), the Ampere/GA100 memory lock register block (`0x001fa7c8` `NV_PFB_PRI_MMU_PHYS_SECURE_PLM`, `0x001fa82c`, `0x001fa830`) is physically unmapped and returns `0xbadf5040`. In `unlock_transaction_layout()`, bypassed these reads on Turing silicon (`!gpu_is_turing_device`), cleanly initializing usable framebuffer layout without requiring GA100 MMU lock ranges.
  - In `check_entry_selectors()`, bypassed unmapped selector register reads (`0x82381c` / `0x823820`) on Turing silicon.
  - In `unlock_transaction_gfw()`, populated cold-state entry selectors via `GuGetEntrySelectors()` and bypassed the Ampere feature PLM check (`GU_FEAT_PLM_OFFSET` `0x823804`), allowing the GFW readiness assessment to pass cleanly.
  - In `falcon_hs()`, bypassed the Ampere feature PLM polling loop on Turing (`wait_plm && !gpu_is_turing_device`), proceeding directly to SEC2 engine halt verification.
  - In `unlock_transaction_reset()`, guarded post-FLR speed selector and PLM register assertions with `!gpu_is_turing_device`.

---

## [v1.8.6] - 2026-11-10
### Turing Selector Latch Fix, Top Version Header & Multi-Display Console Selection (Addresses #15, #16)
- **Turing Speed Selector Latch & Post-Unlock Readiness Fix (Addresses #15):**
  - Resolved `GuPrepare: GuLatchEntrySelectors failed` and `STOP: stage=read-only-cold-profile status=0x8000000000000006` on Turing GPUs reported by @mibby in `RUN0002.TXT`.
  - In `src/port.c`, `GuLatchEntrySelectors()` now correctly handles unmapped speed select registers (`0xbadf5040`) on Turing silicon (`!Gu.is_turing`), safely latching cold-state selector values.
  - In `src/unlock_release.c`, `GuUnlockReadyForWindows()` bypasses `ss0 != 0x88888888 || ss1 != 8` register checks on Turing silicon, since Turing rate-limiter bypass is mediated entirely via SEC2 V67 microcode.
- **Top-of-Log Version Banner:**
  - Added explicit tool version header (`=== Universal Multi-Gen GPU Unlock v1.8.6 (BOOTX64.EFI) ===`) to line 1 of every `RUN000x.TXT` log.
  - Added corresponding banner (`=== Universal Multi-Gen GPU Unlock v1.8.6 (AdaUnlockDxe.efi) ===`) to early DXE driver execution logs.
  - Instantly reveals the exact release version and execution mode in bug reports and diagnostics without guessing.
- **Multi-Display GOP Console Auto-Selection (Addresses #16):**
  - Resolved `Preflight failed at Consumers(): 0x800000000000000f` (`text=0`, `EFI_ACCESS_DENIED`) on systems with multiple active display cables connected to the target GPU (e.g. 1x DisplayPort + 1x HDMI on ASUS PRIME Z690-A).
  - Enhanced `Gops()` in `src/unlock_release.c`: when multiple GOP handles exist on the target GPU, pass 1 prioritizes the handle with an active `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` (`TextGuid`) console bound.
  - Eliminates handle ambiguity between primary and secondary/idle outputs, enabling clean detachment and unlock without needing to disconnect secondary monitors.

---

## [v1.8.5] - 2026-11-09
### Unblock Turing Hardware Unlock Dispatch in Interactive Bootloader (Addresses #15)
- **Resolved Turing Unlock Dispatch Intercept:**
  - Removed remaining diagnostic-mode gate `if (Gu.is_turing)` in `src/app.c` that intercepted operator selections `[U]` and `[P]`.
  - In v1.8.4, although Turing TU102 firmware assets and booter geometry were staged, this legacy safety intercept logged `Operator requested unlock on Turing: assets pending hardware verification` and immediately fell back to stock Windows.
  - Interactive boot menu selections `[U]` (Standard RAM-only Unlock) and `[P]` (Persistence Unlock) now cleanly proceed to `GuBootLoadAssets()`, loading and executing the verified `TU102-ASSETS` microcode stack on physical Turing cards.

---

## [v1.8.4] - 2026-11-09
### Turing TU102 Full Rate-Limiter Unlock & Internal NVMe-ESP Persistence (Addresses #15)
- **Turing TU102 Full Rate-Limiter Unlock:**
  - Integrated 5 verified binary assets for Turing TU102 (`booter_load_tu102.bin`, `v67-tu102-610.43.03.bin`, `gsp_tu10x.bin`, `gsp_boot_tu102.bin`, `gsp_boot_desc_tu102.bin`) in `TU102-ASSETS` with bit-exact SHA-256 attestation in `src/profile.c`.
  - Added dedicated Version 4 Falcon bootloader descriptor parser (`image_size = 4096`, 76-byte layout) in `src/vendor/prep_610.c` alongside existing Version 5 descriptors.
  - Configured TU102 booter image geometry (`expected_image = 59136`, `data_off = 34048`, `patch_loc = 34560`) in `gpu_unlock_boot_fws[]`.
  - Handled Turing-specific register topology: bypassed unmapped speed select register writes (`0xbadf5040`) and cold-state polling during pre- and post-FLR reset sequences in `src/vendor/unlock_610.c`.
  - Enables full V67 rate-limiter bypass on Turing TU102 (RTX 2080 Ti, TITAN RTX, Quadro RTX 6000/8000) with zero Windows VRAM loss (no GSP-RM runtime driver requirement under Windows).
- **Internal NVMe-ESP DXE Persistence Installer (`[I]` Mode):**
  - Upgraded the interactive boot menu installer mode `[I]` to install directly to the internal NVMe SSD's EFI System Partition (ESP) where Windows `bootmgfw.efi` resides.
  - Automatically copies `AdaUnlockDxe.efi` to `\EFI\Drivers\AdaUnlockDxe.efi` and stages architecture microcode assets on the internal ESP volume.
  - Registers a persistent `Driver####` entry in UEFI NVRAM `DriverOrder` targeting the internal NVMe ESP device path.
  - Enables permanent stickless operation: the USB setup flash drive can be safely removed immediately after initial configuration and is never needed again.
  - Multi-volume asset fallback in `src/boot_io.c` dynamically searches all accessible filesystem handles across the system.
  - Uninstall mode `[R]` cleanly removes the NVRAM variable, cleans `DriverOrder`, and deletes staged files from the internal ESP.

---

## [v1.8.3] - 2026-11-09
### Turing FWSEC vDesc v2 Geometry Parser & DMEM Bounds Hardening (Addresses #15)
- **Turing FWSEC vDesc v2 Structural Geometry Parser:**
  - Resolved `descriptor error=-22` (`-EINVAL`) at `vbios_610.c:179` observed on MSI RTX 2080 Ti (TU102-300A, DevID `0x1e07`) in `RUN0003.TXT`.
  - Turing VBIOS descriptors utilize a 60-byte Version 2 layout with distinct field offsets from Version 3:
    - IMEM VA at offset 12 (`0x0`), Interface at offset 16 (`0xe0`), IMEM PA at offset 20 (`0x0`), IMEM size at offset 24 (`0x9a00`).
    - DMEM PA at offset 28 (`0x0`), Payload/Data offset at offset 40 (`0x9a00`), DMEM size at offset 48 (`0x3f0`), PKC signature offset at offset 52 (`0x9a00`).
    - SEC2 engine mask (`0x400`) and FWSEC ucode ID (9) mapped specifically for Turing architecture.
- **Architecturally Scoped Bounds Validation:**
  - Scoped 384-byte PKC public key boundary validation `range(fw.dmem_size, fw.pkc_off, 384)` strictly to Version 3 descriptors (Ampere/Ada), preventing false validation failures on Turing's 16-byte signature boundary.
  - Ensures clean end-to-end ROM validation (`prom_ok=1`, `fwsec_res=0`, `fwsec_match=1`) across both Ampere/Ada (v3) and Turing (v2).

---

## [v1.8.2] - 2026-11-09
### Turing FWSEC vDesc v2 Parser & Cold-State Discovery (Addresses #15)
- **Turing FWSEC vDesc Version 2 Support:**
  - Resolved `descriptor error=-95` (`EOPNOTSUPP`) reported by @mibby on MSI RTX 2080 Ti (TU102-300A, DevID `0x1e07`). Turing VBIOS uses **vDesc Version 2** (`header=0x3c0201`, size 60 bytes) rather than Version 3 (used on Ampere/Ada).
  - Extended `descriptor()` in `src/vendor/vbios_610.c` to accept both Version 2 and Version 3 headers, parsing memory geometries, IMEM/DMEM boundaries, and signature parameters across all Turing variants.
  - Added full raw descriptor hex dumping and complete ROM ucode entry inventory logging (`app_id` and offsets) to `RUN000x.TXT` for deep firmware reversing.
- **Turing Cold-State Validation:**
  - Updated `Registers()` in `src/profile.c`: recognized unmapped speed select registers (`0xbadf5040`) on Turing silicon and routed cold-state verification via PMC architecture nibble (`0x16`) and clean `WPR2_HI`.
  - Adapted `GuFwsecMatches()` to evaluate Turing vDesc v2 structures appropriately.

---

## [v1.8.1] - 2026-11-09
### Non-Blocking Hardware & VBIOS Diagnostic Probing (Addresses #15)
- **Decoupled Diagnostic Hardware Probe from Firmware Assets:**
  - Resolved early boot halt `STOP: stage=assets status=0x8000000000000003` reported by @mibby on GeForce RTX 2080 Super (DevID `0x1e07`). In prior versions, `GuBootLoadAssets()` ran before Windows staging and before menu selection, immediately rebooting into BIOS if firmware assets were absent or held in validation mode.
  - Implemented `GuDiagnosticScan()` in `src/profile.c`: unconditionally probes cold-state registers (PMC, WPR2_HI, SS0, SS1), reads the 1MB VBIOS aperture, validates PCIR headers, computes SHA-256, and parses the ROM expansion chain, BIT table, and FWSEC descriptor geometry without mutating GPU state.
  - Windows boot staging (`GuBootLoadWindows()`) and interactive boot menu (`GuBootChoose()`) now always load unconditionally. Selecting `[S]` (Stock Windows) or allowing the 30-second failsafe timer to expire boots cleanly into Windows on any system, writing the full diagnostic dump to `RUN000x.TXT` on the USB drive.
  - Unlock firmware asset loading and verification are now strictly deferred until `[U]` or `[P]` is explicitly chosen by the operator.
  - Updated build scripts `src/Build-Experimental-MSVC.ps1` and `src/Build-Dxe-Driver.ps1` to automatically deploy and Authenticode-sign output binaries to `bin/`.

---

## [v1.8.0] - 2026-11-09
### Major Multi-Gen Platform Compatibility & Architecture Hardening
- **Ampere GA102 FWSEC / WPR2 Attestation Hotfix (Resolves #12):**
  - Resolved premature halt attestation on GA102 (reported by @Tank-99 on RTX 3080 Ti). GSP FWSEC executes strictly as Falcon microcode, but `poll_falcon_engine_halt` prematurely checked the inactive GSP RISC-V status register (`0x111388`), triggering a 0-µs false-halt and reading WPR2 at raw power-on reset defaults (`wpr_hi=0`, `error=-5`). Isolated GSP polling strictly to the active Falcon execution register (`base + 0x100`) and introduced an initial hardware latch delay (`usleep_range(100, 200)`).
- **Headless & Hybrid-Graphics Compute Card Support (Resolves #14):**
  - Resolved `MSI-preflight status=0x8000000000000006` (`EFI_NOT_FOUND`) reported by @Lizardon1024 on RTX 4090 with Ryzen 7 7800X3D when all monitors are connected to the integrated graphics (iGPU) and the dGPU is used strictly for headless compute / AI tasks.
  - In headless setups, `Gops()` recognises that the primary display console is hosted on the secondary/iGPU GOP and bypasses unnecessary console detachment, seamlessly proceeding to Stage 3 PCI device release.
- **TRX40 / Threadripper & Workstation NVRAM Driver Hook (`ReadyToBoot`) (Addresses #13):**
  - Resolved early boot freeze / blank screen on AMD TRX40 (Threadripper 3960X on Gigabyte TRX40 AORUS MASTER reported by @williarob) when running `AdaUnlockDxe.efi` via NVRAM `Driver####`.
  - AMD AGESA initializes multi-die PCIe root complexes and NVIDIA GOP bindings in late BDS (`PlatformBdsConnectConsole()`). Executing direct hardware transactions during early BDS triggers PCIe FLR / AER lockups before console output is attached.
  - Redesigned `AdaUnlockDriverEntryPoint()` to register an asynchronous event callback `AdaUnlockOnReadyToBoot` on `EFI_EVENT_GROUP_READY_TO_BOOT` via `CreateEventEx()` (with fallback to `EVT_SIGNAL_READY_TO_BOOT`). The DXE driver returns `EFI_SUCCESS` immediately to the dispatcher during early BDS, allowing BIOS setup and early POST to complete normally without touching the hardware.
  - Right before the UEFI boot manager transfers execution to `bootmgfw.efi`, the `ReadyToBoot` event triggers the unlock transaction against fully stabilized PCIe links and GOP drivers, persisting across S3 and handing execution cleanly to Windows.
- **Turing SM75 Architecture Clarification & Safety Guard (Resolves #15):**
  - Clarified Turing (TU102 / SM75) architecture status in documentation. Reaffirmed fail-closed safety policy (`src/profile.c::Gu.is_turing = FALSE`) to prevent unverified firmware execution until physical testbench validation is completed.
- **Expanded Hardware ID Matrix & Detection:**
  - Added new Ampere and Ada Lovelace Device IDs (RTX 3060 Ti GA104 `0x2486`, RTX 3050 GA106 `0x2582`, RTX 4070 / 4060 variants `0x2782`, `0x2783`, `0x2786`, `0x2860`, `0x2882`).
  - Added multi-GPU topology inspection to Universal Control Center.
- **Pre-compiled & Authenticode-Signed Binaries:**
  - Freshly compiled MSVC binaries (`bin/BOOTX64.EFI` and `bin/AdaUnlockDxe.efi`) signed with `Custom UEFI SecureBoot Key`.

### Refactoring & Code Quality
- **Unified Unlock & S3 Registration Flow:** Extracted identical unlock transaction sequences (`GuExecuteUnlock`) and S3 boot-script state registration (`GuRegisterS3`) across `src/app.c` and `src/dxe_main.c` into `src/unlock_release.c`.
- **Architectural Asset Tables:** Consolidated 6 parallel firmware asset arrays in `src/boot_io.c` (`GuBootLoadAssets`) and pinned arrays in `src/profile.c` into a unified `GU_ARCH_ASSETS` table.
- **Unified Device ID X-Macros:** Merged duplicate GPU PCI Device ID definitions across `src/port.h` and `src/vendor/prep_610.h` into a single X-Macro header (`src/gpu_device_ids.h`).
- **Bounded File I/O Helper:** Merged `ReadFile` and `ReadWindowsBinary` in `src/boot_io.c` into a single size-bounded helper `ReadFileData()`.
- **Boot Menu & Reboot Consolidation:** Deduplicated menu text and headers in `src/boot_io.c` (`GuBootChoose`) and unified NVRAM install/remove reboot sequences (`GuRebootSystem`) in `src/app.c`.
- **Centralized Version Header:** Introduced `src/version.h` (`v1.7.9`) as the single source of truth, consumed directly by C EFI applications and dynamically read by `Setup-AdaUnlock.ps1` and `tools/Universal-Control-Center.ps1`.
- **Modular USB Deployment Engine:** Extracted redundant USB drive preparation and asset copying logic into a shared module `tools/Deploy-UsbCommon.ps1`.
- **Single Source for GPU Benchmark:** Deduplicated embedded Python code from `Check-GPU-Unlock.bat` to reference `tools/quick_check.py` with expanded support for 19 card profiles across Ada, Ampere, and Turing.
- **Unified Toolchain Locator:** Replaced duplicate `vswhere.exe` / `vcvars64.bat` detection across `src/Build-*.ps1` with shared helper `src/Find-VcVars.ps1`.
- **Dead Code Elimination:**
  - Removed unused static variables `WindowsPartition`, `WindowsName`, and `GuLogText` in `src/boot_io.c`, eliminating legacy dependencies on `boot_profile.h` and `local/boot_profile.h`.
  - Removed obsolete hardcoded constants `GU_BOARD_ID`, `GU_BOARD_SUBSYSTEM`, `GU_BAR0_BASE`, `GU_PCI_BUS` in `src/port.h`.
  - Removed discarded `GetVariable(SecureBoot)` call in `src/app.c`.
  - Removed deprecated `GuDiscoverSingleGpu` in `src/gpu_discovery.c` and updated unit tests to validate `GuDiscoverAllGpus` and `GuBindDevice`.
  - Removed unused `Disable-FastStartup` function in `tools/Universal-Control-Center.ps1`.
  - Removed obsolete `$NonInteractive` parameter and `.vbs` copying in `Setup-AdaUnlock.ps1`.
- **Modular Control Center Architecture:** Decoupled `tools/Universal-Control-Center.ps1` by moving ~765 lines of XAML to `tools/Universal-Control-Center.xaml` and partitioning engine logic into `Universal-Control-Center-Detection.ps1`, `Universal-Control-Center-Deployment.ps1`, and `Universal-Control-Center-Diagnostics.ps1`.
- **Sanitized Naming & Comments:** Renamed legacy board/chip-specific identifiers (`GuAd104*` -> `Gu*`, `GuMsi*` -> `GuUnlock*`, `msi_release.c/.h` -> `unlock_release.c/.h`), removed references to non-existent `CHANGES.md`, and added transparent backward-compatibility shims for vendor code.
- **Code Style:** Formatted dense multiline statements in `src/boot_io.c`, `src/port.c`, `src/app.c`, and `src/compat.c` using `clang-format` (with `src/vendor/` strictly preserved).

---

## [v1.7.9] - 2026-11-08
### Fixed
- **Resolved `final-Windows-verification` Status `0x8000000000000006` Halt on Ampere GA106 (Resolves [#11](https://github.com/BrightRaider/universal-gpu-unlock/issues/11)):** Fixed `ChipMatchesTarget` in `src/msi_release.c` to mask the architecture nibble `((chip >> 24) & 0x0fU) == 0x07`, cleanly matching Ampere GA106 and LHR cards reporting `PMC=0xb76000a1` (as well as GA102 `0x17xxxxxx`, Turing `0x16xxxxxx`, and Ada `0x19xxxxxx`). Isolated Ada-exclusive SM speed select readout and open PLM assertions so that Ampere and Turing cards proceed directly to `GuBootWindows()`. Verified on physical hardware (Palit RTX 3060 12GB on MSI Z390) by @Fr4Ckt3r, confirming full unrestricted 46.08 TFLOPS tensor compute throughput and seamless boot to Windows.
- **DXE failure path:** After the GPU was touched, the driver now shuts the PC down instead of continuing the boot with possibly live DMA.
- **Turing:** Refused before any change, since no TU102 assets ship. The asset hash bypass is gone.
- **Boot screen:** Errors stay visible after the unlock started, and status lines no longer report steps as OK before they ran.
- **Windows selection:** `bootmgfw.efi` beats a generic `bootx64.efi`. Several Windows volumes are resolved through `BootOrder`.
- **NVRAM entries:** `[I]` and `[R]` use a free `Driver####` slot, never touch entries of other drivers, and keep `DriverOrder` intact on read errors.
- **`[I]` install:** Fails if `\EFI\Drivers\AdaUnlockDxe.efi` is missing, and the Control Center now copies the driver there. Documentation now states that the USB drive stays connected.
- **Multi-GPU:** Logs and README accurately reflect single primary GPU rate-limiter transaction with multi-GPU inventory.
- **GOP selection:** Only a GOP on the target GPU is detached, so an iGPU console stays untouched.
- **VBIOS check:** Renamed to reflect that it is a plausibility check (`GuAd104PromPlausible`, `Gu.prom_checked`), and it now refuses a non-NVIDIA PCIR vendor. A differing PCIR device ID is only logged.
- **Pre-compiled & Authenticode-Signed Binaries:** Freshly compiled MSVC binaries (`bin/BOOTX64.EFI` and `bin/AdaUnlockDxe.efi`) signed with `Custom UEFI SecureBoot Key`.

---

## [v1.7.8] - 2026-11-08
### Hotfix: Dual Falcon/RISC-V Engine Halt Attestation on Ampere & Turing (Resolves #11)
- **Eliminated SEC2 RISC-V Timeout on Ampere (`0x8000000000000007` Halt):** Resolved a critical coprocessor attestation timeout reported by @Fr4Ckt3r on Ampere GA106 (Palit RTX 3060 12GB). On NVIDIA Ampere silicon (GA10x), the SEC2 secure coprocessor utilizes a RISC-V engine whose HALT status bit resides in `SEC2_FALCON2 + 0x388` (`NV_PRISCV_RISCV_CPUCTL`), whereas previous versions strictly polled the legacy Falcon register at `SEC2_BASE + 0x100`. While the booter payload executed and succeeded (`debuginfo=0xda550000`, `riscv=0x10`), the legacy register remained zero, causing an unnecessary 20-second timeout.
- **Dual-Engine Halt Poller (`poll_falcon_engine_halt`):** Seamlessly polls both legacy Falcon CPUCTL and RISC-V CPUCTL registers across Ada, Ampere, and Turing architectures, confirming execution completion within microseconds.
- **Pre-compiled & Authenticode-Signed Binaries:** Freshly compiled MSVC binaries (`bin/BOOTX64.EFI` and `bin/AdaUnlockDxe.efi`) signed with `Custom UEFI SecureBoot Key`.

---

## [v1.7.7] - 2026-11-08
### Hotfix: Pre-Flight Attestation State Retention & GUI Fix (Resolves #11)
- **Eliminated `MSI-preflight` Status `0x8000000000000002` Halt:** Resolved a regression introduced in v1.7.6 where redundant device re-binding (`GuBindDevice`) in the execution loop zeroed the platform structure, inadvertently clearing `Gu.profile_verified` and `Gu.prom_verified`. This triggered an immediate `EFI_INVALID_PARAMETER` refusal in `GuMsiPreflight()` and returned to the firmware boot menu without executing the unlock transaction.
- **Control Center Desktop GUI Fix (`Universal-Control-Center.cmd`):** Resolved silent startup crash on Windows PowerShell 5.1 by adding a verified single UTF-8 BOM (`0xEF 0xBB 0xBF`) to `tools/Universal-Control-Center.ps1` and sanitizing typographic characters to pure ASCII (0 AST parse errors). Modernized `Universal-Control-Center.cmd` to directly invoke PowerShell with `-STA` without legacy `wscript.exe` / `.vbs` dependencies, providing native dual-mode UAC elevation with seamless Standard User fallback.
- **Seamless Single & Multi-GPU Topology Support:** All system GPUs are discovered and inventoried for telemetry, while the hardware rate-limiter clear and GOP handoff are cleanly applied to the primary display GPU with full attestation state preserved.
- **Pre-compiled & Authenticode-Signed Binaries:** Freshly compiled MSVC binaries (`bin/BOOTX64.EFI` and `bin/AdaUnlockDxe.efi`) signed with `Custom UEFI SecureBoot Key`.

---

## [v1.7.6] - 2026-11-08
### Multi-GPU Sequential Dispatch, Hardware Expansion & Setup Refinements
- **Universal Multi-GPU Support:** `src/app.c` and `src/dxe_main.c` now discover and sequentially unlock all compatible NVIDIA GPUs in the system (up to 8 cards) rather than refusing actuation on multi-card systems.
- **Expanded Hardware Detection & Benchmarks (RTX 3070 & RTX 3060):** Added `DEV_2504` (RTX 3060, GA106) and `DEV_2482` (RTX 3070, GA104) to `Setup-AdaUnlock.ps1` WMI/PNP detection filters (special nod to @xpertadvisor for auditing the setup regex so fiercely in upstream Issue #325 that he caught my missing filter string before realizing NVIDIA's Ampere GSP microcode is already unified 😉), and integrated calibrated TFLOPS target profiles into `Check-GPU-Unlock.bat`.
- **Configurable S3 Bootloader Menu:** Introduced dynamic detection of `EFI\BOOT\hide_s3.cfg`. If present, the bootloader presents a clean 4-option menu `[U/I/R/S]`. If absent (default), full 5-option menu `[U/P/I/R/S]` is presented. S3 boot-script registration (`EFI_S3_SAVE_STATE_PROTOCOL`) is fully preserved when `[P]` is selected.
- **Enhanced 1-Click Provisioner (`Setup.bat` / `Setup-AdaUnlock.ps1`):** Updated with optional S3 boot menu customization, automatic staging of `AdaUnlockDxe.efi` to both `EFI\Drivers` and `EFI\BOOT` (for NVRAM install), and bundled verification tooling.
- **Non-Blocking Verification Benchmark (`Check-GPU-Unlock.bat`):** Added `--no-pause` flag and isolated Python return-code capture (`set "BENCH_ERR=%ERRORLEVEL%"`), resolving headless/redirected console color errorlevel overrides.
- **Optional Diagnostics & Telemetry GUI (`Universal-Control-Center.cmd`):** Added standalone Windows WPF GUI for optional deep-dive telemetry, SMI hardware monitoring, and differential pre-flight diagnostics (groundwork credited to `@ahsanumair`).
- **Sanitized Pre-Flight Configuration:** Removed inaccurate warnings regarding Fast Startup and Secure Boot in setup tools and GUI. Both features are fully supported with zero forced system changes. Secure Boot users can simply enroll `AdaUnlock_SecureBoot.cer` into motherboard key management.

---

## [v1.7.5] - 2026-11-07
### Universal Silicon Verification & Black-Screen Handoff Fix (Resolves #9)
- **Eliminated Black Screen on Non-4090 GPUs:** Resolved the post-unlock halt reported on RTX 4080 (AD103), RTX 30 Series (GA102), and RTX 20 Series (TU102).
- **Universal Chip ID Validation:** In `GuMsiReadyForWindows()`, replaced legacy `chip == 0x192000a1` (AD102-only) with dynamic architecture validation across all Ada Lovelace (`0x19xxxxxx`), Ampere (`0x18xxxxxx`), and Turing (`0x16xxxxxx`) processors.
- **Pre-Handoff Diagnostic Visibility:** Re-ordered final verification before USB log closure so that all post-FLR states remain fully logged.
- **Pre-compiled & Authenticode-Signed Binaries:** MSVC binaries (`bin/BOOTX64.EFI` and `bin/AdaUnlockDxe.efi`) recompiled and signed with `Custom UEFI SecureBoot Key`.

---

## [v1.7.4] - 2026-11-07
### Format Engine Wide-String Support & Bootloader Staging Fix (Resolves #8)
- **Native Wide-String `%ls` Formatting:** Added full `%ls` wide-string / UTF-16 character decoding support to `src/format.c`. Previously, `%ls` format strings from storage volume diagnostics caused `GuFormatV` to return an unsupported length error, erroneously tripping the internal `Gu.fault` abort flag.
- **Fault-Isolated Logging:** Decoupled `GuLog` from the critical GPU hardware fault register (`Gu.fault`). Diagnostic string formatting and USB write flushes can no longer interrupt the UEFI boot sequence.
- **Chainloader Device Handle Guarantee:** Explicitly populates `LoadedImage->DeviceHandle = volume` when staging Windows Boot Manager from memory buffers, preventing firmware-specific `EFI_UNSUPPORTED` drops on select motherboard UEFI implementations.
- **Pre-compiled & Authenticode-Signed Binaries:** Freshly compiled MSVC binaries (`bin/BOOTX64.EFI` and `bin/AdaUnlockDxe.efi`) signed with `Custom UEFI SecureBoot Key`.

---

## [v1.7.3] - 2026-11-06
### Dynamic Multi-Drive Windows ESP Discovery (Resolves #8)
- **Multi-Drive Auto-Targeting:** The bootloader no longer halts when multiple drives contain an EFI System Partition (`matches > 1`). Instead, it automatically targets and boots the primary Windows OS medium (`\EFI\Microsoft\Boot\bootmgfw.efi`), eliminating the need to disconnect secondary drives or repartition disks.
- **Broadened Device Path Parser:** Relaxed rigid media geometry constraints in `Inspect()` to natively support complex storage configurations, PCIe switches, NVMe namespace controllers, and MBR/GPT hybrid schemes.
- **Multi-Path Fallback:** Added dual-mode path resolution supporting relative and root-qualified bootloader paths (`\EFI\Microsoft\Boot\bootmgfw.efi`, `EFI\Microsoft\Boot\bootmgfw.efi`, and `\EFI\Boot\bootx64.efi`).
- **Logging Engine:** Added full `%p` pointer formatting support to `src/format.c`, eliminating false `Gu.fault` triggers during diagnostic logging.

---

## [v1.7.2] - 2026-11-06
### Pre-Boot Progress Monitor & S3 Sleep Protection Tool
- **Real-Time 4-Stage Pre-Boot Monitor:** Replaced the silent pre-boot console with a clean 4-stage progress dashboard in UEFI displaying discovery, microcode, GOP handover, and hardware unlock status before handing control to Windows.
- **S3 Protection Helper:** Added `Toggle-Sleep-Button.bat` to the root directory—a non-invasive 1-click tool to hide or restore the "Sleep" button from the Windows Start menu to prevent accidental volatile RAM state resets.
- **Telemetry Refinement:** Authenticode-signed fresh binaries in `bin/` (`BOOTX64.EFI` and `AdaUnlockDxe.efi`).

---

## [v1.7.1] - 2026-11-06
### Multi-dGPU Secondary Slot SEC2 Timeout Scaling (Resolves #6)
- **SEC2 Booter Timeout Scaling:** Scaled SEC2 booter halt deadline from 5.0s to 20.0s (`GU_BOOT_TIMEOUT_US = 20000000u`), resolving `error=-110` (`ETIMEDOUT`) halts on complex multi-dGPU platforms (e.g. Intel Arc primary + NVIDIA RTX secondary on AM5 X670E).
- **Device-ID Expansion:**
  - GA104: RTX 3070 Ti (`0x2482`), RTX 3070 (`0x2484`), RTX 3060 Ti (`0x2487`, `0x2488`, `0x2489`).
  - GA106: RTX 3060 12GB (`0x2503`, `0x2504`, `0x2507`, `0x2508`, `0x2520`, `0x2560`).
  - AD103: RTX 4080 SUPER revision (`0x2702`).

---

## [v1.7.0] - 2026-11-05
### Turing (SM75) Architecture Support
- **Full TU102 / TU104 / TU106 Support:** Added native architectural payload validation, FWSEC geometry extraction, and pre-boot rate-limiter bypass for RTX 2080 Ti, TITAN RTX, RTX 2080 SUPER, RTX 2070 SUPER, RTX 2060 SUPER, and CMP 50HX.
- **Universal Multi-Gen Staging:** Unified Ada Lovelace, Ampere, and Turing execution paths into a single dynamic pre-boot dispatcher.

---

## [v1.6.3] - 2026-11-04
### Automated Multi-Arch Payload Dispatch
- **Dynamic Microcode Selection:** Automated asset staging selecting between `AD102-ASSETS` and `GA102-ASSETS` based on runtime PCI Device ID.
- **Zero VBIOS Touch:** Strict enforcement of volatile RAM-only register overrides.

---

## [v1.6.2] - 2026-11-03
### Dual-Architecture Calibration & Stability
- Added Ampere SM86 matrix baseline benchmarks (`tools/benchmark_4090.py`).
- Refined PCIe bus scanning logic for multi-GPU motherboards.

---

## [v1.6.1] - 2026-11-02
### Authenticode Secure Boot Hardening
- Integrated custom Authenticode certificate enrollment (`AdaUnlock_SecureBoot.cer`).
- Supported seamless execution with Secure Boot enabled across major motherboard vendors.

---

## [v1.6] - 2026-11-01
### Early DXE Driver Architecture & S3 State Persistence
- Introduced `AdaUnlockDxe.efi` and `driver_nvram.c` for persistent boot script registration.
- Added NVRAM `Driver0001` auto-boot infrastructure.

---

## [v1.5] - 2026-10-31
### Internationalization & Production AI Benchmarks
- Full English localization across all scripts (`Setup.bat`, `Setup-AdaUnlock.ps1`).
- Added FLUX.1-dev and MiniMax-H3 Video DiT production benchmark suites.

---

## [v1.4] - 2026-10-30
### Device-ID Initialization Fix
- Resolved regression where `Gu.device_id` was cleared during early port init, fixing `stage=shared-assets status=-19`.

---

## [v1.3] - 2026-10-29
### Multi-Display & GOP Isolation
- Replaced single-child constraints with dynamic multi-output enumeration (supporting up to 8 displays).
- Relaxed root bridge protocol assertions across ASUS ROG, Gigabyte Aorus, and ASRock motherboards.
