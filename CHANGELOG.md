# Changelog - Universal Multi-Gen GPU Unlock

All notable changes, architectural improvements, and device expansions for Universal GPU Unlock are documented here.

For packaged binaries and downloads, visit [GitHub Releases](https://github.com/BrightRaider/universal-gpu-unlock/releases).

---

## [Unreleased]
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
