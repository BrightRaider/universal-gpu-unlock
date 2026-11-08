# Changelog - Universal Multi-Gen GPU Unlock

All notable changes, architectural improvements, and device expansions for Universal GPU Unlock are documented here.

For packaged binaries and downloads, visit [GitHub Releases](https://github.com/BrightRaider/universal-gpu-unlock/releases).

---

## [v1.7.7] - 2026-11-08
### Hotfix: Pre-Flight Attestation State Retention (Resolves #11)
- **Eliminated `MSI-preflight` Status `0x8000000000000002` Halt:** Resolved a regression introduced in v1.7.6 where redundant device re-binding (`GuBindDevice`) in the execution loop zeroed the platform structure, inadvertently clearing `Gu.profile_verified` and `Gu.prom_verified`. This triggered an immediate `EFI_INVALID_PARAMETER` refusal in `GuMsiPreflight()` and returned to the firmware boot menu without executing the unlock transaction.
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
