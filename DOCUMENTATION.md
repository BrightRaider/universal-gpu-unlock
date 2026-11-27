# Technical Documentation: Universal GPU Unlock (Multi-Gen)

> **Tool:** Universal GPU Unlock  
> **Version:** v2.1.0  
> **Architectures:** NVIDIA Ada Lovelace (RTX 40), Ampere (RTX 30), Turing (RTX 20 / CMP)  
> **Supported Operating Systems:** Windows 10, Windows 11, WSL2 / Ubuntu CUDA Containers  

---

## 1. Introduction & How It Works

### 1.1 What Is Universal GPU Unlock?
NVIDIA restricts consumer and mining graphics cards in firmware via an artificial rate-limiter on FP16 and BF16 Tensor Core accumulation (`GEMM`), capping them to a fraction of their physical silicon throughput. *Universal GPU Unlock* lifts this clamp at the UEFI pre-boot level in volatile RAM before the Windows kernel and display driver load. This unlocks full enterprise-grade compute throughput for AI, deep learning, and LLM workloads (PyTorch, Stable Diffusion, ComfyUI, vLLM).

### 1.2 Limits & Gaming Behavior
- **No Gaming Boost:** The unlock does not alter shader, rasterizer, or DLSS performance in PC games. In pure gaming workloads, the performance delta is 0%.
- **100% RAM-Only / Reversible:** No permanent VBIOS flashing takes place. After a full power-off (cold boot), the GPU reverts cleanly to factory stock.
- **Secure Boot & Anti-Cheat Compatible:** With Authenticode signing, Secure Boot stays enabled; Riot Vanguard (*Valorant* / *League of Legends*) works cleanly via option **`[I]`** (`AdaUnlockDxe.efi`) when booting normally through the Windows Boot Manager (`bootmgfw.efi`) (see [Section 4](#4-secure-boot-configuration)).

---

## 2. Bootloader Modes & Options

When booting from the USB flash drive, a 30-second countdown menu appears:

| Key | Mode | Description |
| :---: | :--- | :--- |
| **`[U]`** | **Standard Unlock** | Proven, robust volatile unlock in RAM. Reverts cleanly to factory stock on cold power-off. |
| **`[P]`** | **S3 Persistence Unlock** | Verified S3 persistence architecture (Tier 1 Boot Script + Tier 2 Universal ACPI FACS Shadow Trampoline & Arena) that keeps the unlock alive across classic ACPI S3 sleep/wake cycles. |
| **`[I]`** | **Install to NVRAM** | Copies `AdaUnlockDxe.efi` + architecture assets onto the best internal FAT32 volume (`v1.9.5`/`v1.9.6` checks free bytes via `EFI_FILE_SYSTEM_INFO` across primary ESP, `GPU_UNLOCK`/`UNLOCK_ESP`, and small FAT32 volumes `<= 2 GiB`, creates directories recursively, streams in `1 MiB` chunks with rollback on `VOLUME_FULL`) and registers the driver in UEFI NVRAM `DriverOrder` (including `ReadyToBoot`, `StartImage`, and live ACPI refresh Plan-C activation). |
| **`[R]`** | **Remove DXE Driver** | Cleanly removes the DXE driver from UEFI NVRAM and all internal FAT32 partitions. |
| **`[S]`** | **Stock Windows** | Performs hardware diagnostic scans and logs to the USB drive, skips the unlock, and boots stock Windows. |
| **`[Space]`** | **Pause Countdown** | Pauses the 30-second menu timer so you can read and select options without time pressure. |

### 2.1 EFI System Partition (ESP) Space Requirements for Option `[I]`
By default, Windows Setup creates an EFI System Partition (ESP) of only 100 MB, of which Windows itself already consumes 30–50 MB.
* **Firmware Asset Sizes:**
  * **Ada Lovelace (RTX 40) & Ampere (RTX 30):** Require NVIDIA's signed GSP firmware `gsp_ga10x.bin` (**80.37 MB**). A standard 100 MB ESP usually lacks enough free space. Starting with `v1.9.5`/`v1.9.6`, `[I]` inspects free space upfront via `EFI_FILE_SYSTEM_INFO`, automatically falls back to a dedicated internal FAT32 partition (`GPU_UNLOCK` / `UNLOCK_ESP` or `<= 2 GiB`), creates subdirectories recursively (`OpenOrCreateDirRecursive()`), streams in `1 MiB` blocks, rolls back partial writes cleanly on error, and only falls back to USB as a last resort.
  * **Turing (RTX 20 / CMP):** Requires only `TU102-ASSETS` (**28.1 MB**). Because the unified release bootloader detects the installed GPU architecture automatically, option `[I]` on Turing cards copies only the compact Turing assets—fitting directly onto standard 100 MB ESPs without manual partitioning.
* **Solutions & Best Practices:**
  1. **Option `[P]` (S3 Sleep Persistence - Recommended):** Users who put their PC to sleep (ACPI S3 Suspend-to-RAM) rather than shutting down do not need `[I]` at all. Unlock once from USB—the ACPI NVS dispatcher restores the unlock on wake in under 3 seconds with zero ESP footprint.
  2. **Dedicated FAT32 Partition (1-Click Tool `Setup.bat [3]` / `tools\Create-Unlock-Partition.bat`):** `Setup.bat` option `[3]` (`tools/Create-Unlock-Partition.ps1 -TargetDrive C -Hidden`, also accessible in Universal Control Center) shrinks an internal SSD by 512 MB and formats it directly via `Format-Volume -Partition` as FAT32 labeled `GPU_UNLOCK` (eliminating premature Windows format popups). With `-Hidden` (`NoDefaultDriveLetter` without a drive letter), the partition stays hidden in Windows Explorer while UEFI mounts it at boot and option `[I]` (`v1.9.6+`) automatically deploys the DXE driver and assets there (the USB stick can then be unplugged).
  3. **Expand ESP:** Enlarge the existing 100 MB ESP to ~500 MB using a partition manager.
* **Automatic Detection in Setup & Control Center:** Both `Setup.bat` (`tools\Setup-AdaUnlock.ps1` — in `v2.0.0` including BitLocker, Secure Boot, and S0ix pre-flight checks) and Universal Control Center inspect the Windows ESP size automatically and recommend option `[P]` or `GPU_UNLOCK` when a 100 MB ESP is detected.

### 2.2 Architecture Note: Stickless `[I]` with S3 Included vs. Optional No-S3 Opt-Out

By design, the boot menu stays fixed at five keys (`[U / P / I / R / S]`) with option **`[I]` (`AdaUnlockDxe.efi`)** passing `GU_UNLOCK_REGISTER_S3` (`src/dxe_main.c`):

- **Why S3 stays enabled in `[I]` by default:**
  - Reboot persistence and ACPI S3 sleep persistence work together without requiring separate menu branches or user choices.
  - On boards where `GuRegisterS3()` returns an error, `GuUnlockAllGpus()` (`src/multi_gpu.c`) logs `[!] S3 Persistence registration returned ...` without aborting the GPU unlock itself.
  - Pre-mutation early-BDS issues (such as `#24`) fail in `GuUnlockPreflight()` (`src/unlock_release.c`) *before* any GPU or S3 step executes, so disabling S3 would not prevent them.
- **What `GU_UNLOCK_REGISTER_S3` adds compared to a hypothetical S3-free DXE build:**
  - Allocates persistent `EfiACPIMemoryNVS`, an `EfiReservedMemoryType` DMA arena, and a `< 0xA0000` low-memory trampoline (`src/s3_resume.c`).
  - When Tier 1 (`EFI_S3_SAVE_STATE_PROTOCOL`) is locked down by SMM LockBox, Tier 2 (`ShadowFACS` / Plan C) clones `RSDP`/`XSDT`/`FADT`/`FACS`, hooks `BootServices->StartImage` & `ExitBootServices`, and recalculates `BootServices->Hdr.CRC32` (`src/dxe_main.c`, `src/s3_facs.c`).
- **Planned Escape Hatch (Future Opt-Out Without Menu Bloat):** Should an ACPI/BIOS edge case, dual-boot loader (`GRUB`/`rEFInd`), S0ix/S4 conflict, or third-party anti-cheat specifically collide with Plan-C ACPI tables or hooks on a system that never uses S3 sleep, the clean solution is a silent flag file (e.g. `\GPU-UNLOCK\NO_S3.FLG`) or install option passed in NVRAM `LoadOptions` checked in `src/dxe_main.c` (`0U` instead of `GU_UNLOCK_REGISTER_S3`), keeping the `[U / P / I / R / S]` menu clean.

---

## 3. Headless Compute & Secondary GPUs (e.g., NVIDIA CMP 40HX)

### 3.1 The Uninitialized PCI Command Register Problem
Compute accelerators without physical display outputs (such as the TU106-based NVIDIA CMP 40HX) or secondary GPUs in systems running primary display on integrated Intel/AMD graphics (iGPU) are frequently left uninitialized by motherboard UEFI POST. The firmware leaves the PCI Command register at `0x0000` (Memory Space decode disabled, `rom=0x0`).

### 3.2 The Solution (from v1.8.27)
1. **Relaxed Pre-Check (`GuCheckConfig` in `src/checks.c`):** Uninitialized compute accelerators are no longer rejected solely because the Memory Decode bit in the PCI Command register is still cleared at entry.
2. **Dynamic PCI Memory Space Activation (`src/gpu_discovery.c`):** Before reading MMIO registers (`BAR0 + 0x300000` VBIOS PROM), the bootloader dynamically enables Memory Space decoding (`EFI_PCI_IO_ATTRIBUTE_MEMORY`) via `EFI_PCI_IO_PROTOCOL` and updates the PCI Command register directly when required.

---

## 4. Secure Boot Configuration

The UEFI binaries (`BOOTX64.EFI` and `AdaUnlockDxe.efi`) are signed with a dedicated Authenticode certificate.

- **Option A (Without Anti-Cheat, easiest):** Temporarily set **Secure Boot** to **Disabled** in BIOS.
- **Option B (Keep Secure Boot Enabled):**
  1. In BIOS Setup navigate to **Security ➔ Secure Boot ➔ Key Management** (or *Custom Key Management*).
  2. Under **Authorized Signatures (`db`)**, select **Append Key** / **Enroll Signature** (*never* delete or replace existing Microsoft/OEM factory keys!).
  3. Browse to the USB drive and select **`AdaUnlock_SecureBoot.cer`**.
  4. Save with **F10** and reboot.

### 4.1 Riot Vanguard (*Valorant* / *League of Legends*)
1. **Keep Secure Boot Enabled** (`Standard` mode with factory keys intact; if your BIOS blocks the USB drive or `AdaUnlockDxe.efi`, **append `AdaUnlock_SecureBoot.cer`** to `db` — never clear factory keys).
2. **Install `[I]` Once:** Boot from USB once (`F8` / `F11` / `F12`), press **`[I]`** (`AdaUnlockDxe.efi` is registered in UEFI NVRAM), and let it reboot.
3. **Boot Windows Normally (do not boot from USB):** Afterward always start Windows normally via **Windows Boot Manager (`bootmgfw.efi`)** *(if your 100 MB ESP is too small for Ada/Ampere assets, leave the USB drive plugged in as storage or create a hidden 512 MB FAT32 `GPU_UNLOCK` partition via `Setup.bat` -> `[3]` / `tools\Create-Unlock-Partition.bat` — always booting through the internal Windows Boot Manager)*.
4. **Stickless S3 Sleep with `[I]` (`v1.9.4`–`v2.0.0`, live-verified):** `AdaUnlockDxe.efi` (`v1.9.4+`) calls `GuAcpiRefreshShadowChain()` inside the `BootServices->StartImage` guard (`EfiLoaderCode`) immediately before `bootmgfw.efi` / `winload.efi`, cleanly re-cloning `ShadowFADT`, `ShadowXSDT`, and `ShadowRSDP` from the final post-BDS tables (`RealXSDT`/`RealFADT`). In `RUN0105.TXT` (`v1.9.4`, USB storage) and stickless from `v1.9.6` / `v2.0.0` (`RUN0110.TXT` on hidden 512 MB `GPU_UNLOCK`, `Confirm-SecureBootUEFI: True`, `FirmwareCtrl=0xDC637400`), Riot Vanguard (`vgk`/`vgtray`/`Riot Client`) + RTX 4090 unlock ran across both stickless cold/warm reboots (**226.64** TFLOPS) and ACPI S3 sleep/wake (**234.19** TFLOPS `FP16` / **768.22 GB/s** VRAM) with zero USB drive attached.

---

## 5. Troubleshooting & Best Practices

- **Disable CSM:** CSM (Compatibility Support Module) must be set to **Disabled** (Pure UEFI Mode) to avoid legacy VGA conflicts (Error Code 43).
- **Above 4G Decoding & Resizable BAR:** Enable in BIOS to eliminate the 256 MB PCIe BAR1 bottleneck (+10% to +15% throughput gain).
- **Turing / CMP Cards:** Run `tools\Toggle-Turing-GSP.bat` once as Administrator in Windows to bind the NVIDIA driver to GSP firmware mode.
- **GTX 16xx (`TU116` / `TU117`) Unsupported:** Excluded intentionally from `GU_TURING_DEVICE_IDS` (`src/gpu_device_ids.h`). These GPUs have no Tensor Cores and fail Windows `nvlddmkm` `RmInitAdapter` with **Error Code 43** even when pre-boot writes succeed.
- **USB4 / Thunderbolt eGPUs (`ASM2464PD`):** Unsupported/experimental compared to direct internal PCIe slots. Tunneled hosts can hang before `ExitBootServices()` inside platform ACPI power-dependency chains (`_PR0` / `D3Cold`), and OS USB4 Connection Managers (`Usb4DeviceRouter` / Linux `thunderbolt`) perform a domain reset during tunnel setup.
- **Laptop Hybrid / Optimus vs. dGPU-Direct:** On laptops (`RTX 4090`/`4080`/`4070`/`4060` Laptop), switching BIOS MUX to **dGPU-Direct** avoids Optimus idle D3 power-down resets. On Hybrid-only machines, run `tools\Setup-FastResume.bat` (`RMDisableRtd3` + ASPM off).
- **Workload Scaling (`Q4` LLM Decode vs. Training / DiT / Prefill):** Single-batch quantized **`Q4` / `Q4_K_M` LLM decode** is physically VRAM-bandwidth-bound and sees minimal gain (~0–5%); maximum speedups occur in Tensor-Compute-bound **BF16/FP16 training**, **ComfyUI / FLUX / Video DiT**, and **long-context LLM prefill**.
- **Windows Fast Startup (`HiberbootEnabled = 1`) & Vanguard (`0xC00000D4`):** Standard Fast Startup works normally during routine boots; however, after an unexpected power loss/hard reset (`Kernel-Power 41`) or partition changes, the next Fast Startup resume can fail with `Kernel-Boot ID 29` (`0xC00000D4`), causing `vgk.sys` to fail once with Code `31` (`restartRequired=true` in Riot Client). Performing one normal Windows **Restart** (which bypasses `hiberfil.sys`) resolves this immediately; optionally, Fast Startup alone (`HiberbootEnabled = 0`) can be disabled permanently while keeping S3 sleep unaffected.
- **VeraCrypt System Encryption (`DcsBoot.efi` at `\EFI\Microsoft\Boot\bootmgfw.efi`, `main` / post-`v2.0.0`, `refs #23`):** VeraCrypt replaces Microsoft's `~1.6 MB` `bootmgfw.efi` with the `~24 KiB` `DcsBoot.efi` UEFI chainloader (preserving Microsoft's binary as `bootmgfw_ms.vc`). `ReadWindowsBinary()` (`src/boot_io.c`) validates full PE32+ x64 EFI Application headers (`>= 4 KiB` up to `32 MiB`) instead of enforcing an artificial `64 KiB` floor, ensuring `DcsBoot.efi` stages and launches cleanly so pre-boot decryption runs before `bootmgfw_ms.vc` and `winload.efi`.
- **Stickless `[I]` `EFI_UNSUPPORTED` (`0x8000000000000003`) on Fast Boot / Early-BDS Boards (`main` / post-`v2.0.0`, `refs #24`):** Boards where `ConIn`/`StdErr` are `NULL` or split during early `Driver####` dispatch now pass `GuUnlockPreflight()` safely, and `ReadyToBoot` fallback retries cleanly with `ATTEMPT.FLG` / `GuBootOpen()` cleanup if any pre-mutation check defers.
- **Accessing Logs on Hidden `GPU_UNLOCK` or Protected ESPs (`main` / post-`v2.0.0`):** `Launch-Dashboard.bat` and `Check-GPU-Unlock.bat` read hidden `GPU_UNLOCK` / `UNLOCK_ESP` partitions directly in Windows via Win32 `FindFirstVolumeW` (no drive letter or Admin required). For manual inspection or attaching logs to bug reports, click **`Export Boot Log`** in the Dashboard or run **`Setup.bat [5]`** (`tools\Export-Boot-Log.ps1`), which copies the newest `RUN*.TXT` files into `Boot-Logs\` (automatically using UAC + temporary `mountvol` cleanup if the logs live on a protected Windows ESP) and opens Explorer highlighting the file.

---

## 6. S3 Persistence Architecture & Boot Script Dispatch (`[P]` Mode)

### 6.1 The Hardware Challenge of S3 Sleep (Suspend-to-RAM)
When entering ACPI S3 sleep, the OS and motherboard power management remove power from the GPU (power gating). On resume (S3 wake), the GPU undergoes a hardware power-on reset:
- All internal GPU control and configuration registers revert to factory defaults.
- The hardware rate-limiter registers at `0x0082381c` (SS0) and `0x00823820` (SS1) become hardware-locked again.
- Plain MMIO writes (`EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE`) are ignored after reset because the NVIDIA SEC2 Falcon microcontroller must first complete cryptographic authentication of the V67 microcode payload.
- Without re-running this handshake, the display driver resumes with throttled Tensor Core performance.

### 6.2 ACPI NVS Boot Script Dispatch Architecture
To preserve the unlock across S3 standby cycles without rebooting, `[P]` mode implements a multi-stage architecture:

1. **Persistent Memory Staging (`EfiACPIMemoryNVS`):**
   - Standard UEFI Boot Services memory (`EfiBootServicesCode`, `EfiBootServicesData`) is reclaimed and overwritten by Windows after `ExitBootServices()`.
   - Therefore, I allocate 64 pages (256 KB) of persistent `EfiACPIMemoryNVS` (Non-Volatile Sleeping Memory), marked reserved in the ACPI memory map, kept powered during S3 self-refresh, and left untouched by Windows.
   - All required payloads (Booter ~60 KB, V67 microcode 64 KB, boot descriptor, metadata) and `GU_S3_RESUME_CONTEXT` are copied into this buffer.

2. **Boot Script Dispatch Registration (`EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE`):**
   - Registers the Dispatch-2 opcode into the motherboard boot script via `EFI_S3_SAVE_STATE_PROTOCOL`.
   - **Self-Contained PIC Code Mirroring:** Because regular executable code in `EfiBootServicesCode` or `EfiLoaderCode` is freed after `ExitBootServices()`, the position-independent machine code of `GuS3ResumeDispatch` (331 bytes, 0 relocations) is copied directly into the protected NVS buffer (`nvs_base + 0x00800`).
   - The Dispatch-2 entry targets `nvs_phys + 0x00800`, ensuring the resume routine is never overwritten even under full Windows memory pressure.
   - **Parameter Resilience:** Supports both standard UEFI PI invocation (`PeiServices` in `RCX`, `Context` in `RDX`) and BIOS implementations that pass `Context` directly in `RCX`.
   - Fallback MMIO writes for SS0 and SS1 are also queued in case a BIOS skips Dispatch-2 execution.

3. **Self-Contained Resume Engine (`GuS3ExecuteResumeUnlock`):**
   - Executed by the motherboard BIOS on S3 wake before handing control back to Windows.
   - Resets the SEC2 Falcon controller, configures the FBIF interface, runs the Booter via DMA, polls Falcon halt, and latches rate-limiter selectors `0x88888888` and `0x00000008`.
   - Leaves SEC2 clean and records status in the NVS header (`resume_status = 1`, incremented `resume_count`).

### 6.3 Two-Tier S3 Persistence: Tier 1 (Boot Script) & Tier 2 (Universal ACPI FACS Shadow Trampoline)
On modern AMD AM4/AM5 platforms with AMI Aptio V firmware (such as MSI MEG X570 ACE with AGESA / Sinkclose mitigations), platform firmware signals `DxeSmmReadyToLock` at the end of internal DXE before external `Driver####` drivers or bootloaders load. Every subsequent `EFI_S3_SAVE_STATE_PROTOCOL->Write()` call fails with `EFI_OUT_OF_RESOURCES` (`0x8000000000000009`).

To solve this universally across motherboard vendors, I implemented a two-tier persistence engine:
1. **Tier 1 (UEFI S3 Save State Protocol):** Used on platforms without early SMM lockdown (e.g., Intel Z690/Z790 and many workstation boards).
2. **Tier 2 (Universal ACPI FACS Shadow Trampoline, `src/s3_facs.c`):**
   - **ACPI Table Discovery:** Locates RSDP in `ConfigurationTable`, walks XSDT (supporting unaligned 64-bit pointers) and RSDT to find FADT (`FACP`), and resolves the physical FACS address.
   - **Shadow FACS Allocation:** Allocates a shadow copy of FACS inside persistent `EfiACPIMemoryNVS` and redirects `FADT->FirmwareCtrl` and `FADT->XFirmwareCtrl` to it.
   - **FADT Checksum Recalculation:** Recomputes the 8-bit FADT checksum (0 mod 256) so Windows `ACPI.sys` accepts the modified table cleanly.
   - **Hardware FACS Hooking:** Redirects `RealFACS->FirmwareWakingVector` to the machine-code S3 resume trampoline.
   - **Sleep Flow:** Windows writes its true waking address (`HalpRMStub`) into the shadow FACS while the real FACS remains intact for the BIOS.
   - **Wake Flow:** The BIOS reads the real FACS from LockBox and jumps straight into the trampoline.
   - **Dual-Mode Trampoline:** Starts in 16-bit Real Mode, loads an embedded 5-entry GDT, transitions to 32-bit Flat Protected Mode, resets SEC2 Falcon, re-authenticates the unlock payload, latches `SS0 = 0x88888888` and `SS1 = 0x8`, returns cleanly to 16-bit Real Mode, and jumps (`jmp far`) directly into Windows `HalpRMStub`.
   - **Result:** 100% volatile, vendor-agnostic S3 persistence with zero VBIOS flash and zero SMM dependency.

### 6.4 Advanced S3 Architecture: Teardown, Reserved Memory Arena & Live Telemetry (from v1.8.59)
For Ada Lovelace GPUs (RTX 4090 / AD102) using GSP firmware and WPR2 partitioning, the S3 trampoline implements a multi-stage hardware control flow:
1. **4-Stage Hardware Teardown & PCIe FLR (v1.8.59):**
   - Isolates and resets GPU engines cleanly before SEC2 execution:
     - Root Port `0:3:1` upstream bridge routing (`Offset 0x18`) is kept active (`0x002D2D00`: `Sec=0x2D`, `Sub=0x2D`) for Windows handoff.
     - PCIe Function-Level Reset (FLR, PCIe Device Control Bit 15) resets all Falcon and DMA engines synchronously.
     - Bounded polling on BAR0 Boot0 (`0x192000A1`) verifies full link readiness after FLR.
2. **Single-Descriptor Reserved Memory Arena & DMA Unmapping (v1.8.61):**
   - **Retention:** The ~84 MB GSP firmware image and 3-level radix page table are reserved at cold boot inside a single contiguous 82 MB arena in `EfiReservedMemoryType` below 4 GB (`is_arena = TRUE`), adding exactly 1 descriptor to the UEFI Memory Map.
   - **DMA Unmapping (`DMA_live = 0`):** Prior to `StartImage(bootmgfw.efi)`, all UEFI DMA mappings are unmapped via `PciIo->Unmap()`, while physical DRAM retention persists across S3.
3. **EfiRuntimeServicesData Live Telemetry (v1.8.62):**
   - A 4 KB telemetry page (`TelemetryPage`) is allocated in `EfiRuntimeServicesData` so Windows (`kldbgdrv.sys` / `kd.exe -kl`) maps and inspects wake telemetry immediately after S3 wake (`!dd <telemetry_phys> L20` or `dd poi(nt!HalpAcpiFacsMapping)+100 L20`).
   - A dynamic 512 KB downward scan in `GuAcpiInstallShadowFacs()` detects prior-wake telemetry across warm reboots.
4. **Pre-Booter Hardware Snapshot (v1.8.63):**
   - Immediately prior to SEC2 Booter kickoff, the trampoline saves 6 key MMIO registers into the telemetry page (inspectable via WinDbg `!dd <Addr> L50`):
     * `+0xF0`: Pre-Booter `WPR2_LO` (`BAR0 + 0x001fa824`)
     * `+0xF4`: Pre-Booter `WPR2_HI` (`BAR0 + 0x001fa828`)
     * `+0xF8`: `NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE` (`BAR0 + 0x00100ce0`)
     * `+0xFC`: GFW Boot Completion (`BAR0 + 0x00118234`, `GU_GFW_BOOT_OFFSET`)
     * `+0x100`: `GSP_HWCFG2` (`BAR0 + 0x001100f4`)
     * `+0x104`: `GSP_CPUCTL` (`BAR0 + 0x00110100`)
5. **GSP FWSEC-FRTS WPR2 Partitioning Sequence (v1.8.64):**
   - Before SEC2 starts, the 32-bit trampoline executes the complete 1:1 FWSEC-FRTS sequence on GSP (`0x00110000`):
     * GSP Reset & Scrub (`0x001103C0`, `0x001100F4` Bit 12 == 0)
     * GSP BCR switch to non-secure mode (`0x00111668 = 0`, Poll `& 0x11 == 1`)
     * GSP FBIF & chunked DMA (IMEM 242 chunks of 256 bytes `cmd=0x0614`; DMEM 14 chunks of 256 bytes `cmd=0x0600`) from `EfiReservedMemoryType` (`Gu.s3_fwsec_dma`)
     * GSP Falcon2 config (`sig_dmem = 0x0B24`, `engine_mask = 0x0400`, `ucode_id = 9`, `config = 1`, `code_va = 0`, Mailboxes = 0)
     * GSP Start CPU (`0x00110100 = 2`) and bounded poll on halt bit (`0x00110100 & 0x10 == 0x10`)
     * Post-FWSEC telemetry snapshot (`+0x108..+0x118`: `WPR2_LO`, `WPR2_HI`, `GSP_CPUCTL`, `GSP_MAILBOX0`, Scratch `0x00001438`)
   - Properly partitions WPR2 (`low = 0x5ff2000`, `high = 0x5ff2e00`) before SEC2 runs, eliminating Booter error `0x91`.
6. **Consolidation of All 6 GPU DMA Buffers in Reserved Arena & `DMA_live = 0` (v1.8.65):**
   - Consolidates Radix table, GSP firmware image, FWSEC image, Booter, V67 payload, Boot image, and untouched WPR metadata into one contiguous 82 MB `EfiReservedMemoryType` arena (20,662 of 21,000 pages used).
   - `free_coherent()` ensures `GuDmaFree()` unmaps every DMA handle before `StartImage` (`DMA_live = 0`).
   - **IOMMU / Non-Identity DXE Mapping Guard (`refs #25`):** When `Gu.pci->Map()` returns non-identity DXE IOVAs (e.g., `IoMmuDxe` on MSI X670E), `s3_repack_arena_physical()` converts all persisted `Gu.s3_*_dma` globals, `Gu.s3_meta_cpu`, and 3-level GSP radix table pages back to `<4 GB` CPU-physical DRAM addresses after FLR quiescence and before `free_coherent()` unmaps DMA handles.
   - `publish_v67()` preserves an untouched copy of WPR metadata (`Gu.s3_meta_cpu` / `Gu.s3_meta_dma`) before cold-boot `unlock_transaction_signature()` mutates `meta + 72`.
   - Captures SEC2 Mailbox 1 (`BAR0 + 0x00840044`) at `+0x11C`.
7. **GSP HALT Gate, `0x0A00` Code Envelope & Diagnostic Snapshots `+0x120..+0x148` (v1.8.66):**
   - Port-`0x80`-paced GSP halt gate (~100 warm-up `in al, 0x80` reads, then up to 1,000,000 iterations) waits reliably for `(BAR0 + 0x00110100) & 0x10 != 0`.
   - Expanded code envelope to `0x0A00` (2,560 bytes; low data `0x0A00..0x0AEF`, `RM_IDTR`/`PM_GDTR`/`GDT` `0x0B00..0x0B37`).
   - Extended telemetry (`+0x120..+0x148` — inspectable in WinDbg via `!dd <Addr> L58` up to `+0x15C`) logs post-booter SEC2 diagnostics (`SEC2_DEBUGINFO`, `SEC2_IRQSTAT`, `SEC2_TRACEPC`, `SEC2_PRIV_ERR`, `SEC2_HUB_ERR`, `SEC2_PRIV_INFO`, `SEC2_PRIV_ADDR`) plus `PMC_DEVICE_ENABLE`, `GSP_RISCV_CPUCTL`, `GSP_BCR`, and `GSP_DEBUGINFO`, matched against cold-boot reference snapshots (`0x200` / `0x600` / `0x1100F4` / `0x110100` / `0x111388` / `0x111668`) in `RUN*.TXT`.
8. **Port-`0x80` SEC2 Booter Poll & `[meta_dma]` Probe `+0x14C` (v1.8.67):**
   - Up to `6,000,000` port-`0x80`-paced iterations (~5–6 s budget) on `MAILBOX0 == 0x67b00001` or `(CPUCTL & 0x10) != 0 && MAILBOX0 != meta_dma`, plus post-marker `100,000`-iteration halt poll before writing `SS0`/`SS1`.
   - Probes `[meta_dma]` first DWORD (expected `0x371a60b3`, low DWORD of `GU_WPR_META_MAGIC`) into `+0x14C`.
   - Verified across consecutive S3 sleep/wake cycles on RTX 4090 (Windows 10, driver `621.14`, `290.51` TFLOPS FP16 / `291.99` TFLOPS BF16).
9. **Universal-S3 Phase 1 — Dynamic Bridges, Chipsets, VBIOS Geometry & Ada Universality (`v1.9.0`):**
   - `GuDiscoverUpstreamBridgeChain()` dynamically walks up to `4` upstream PCI-to-PCI bridges (`Root -> Leaf`), computing the 16 MB inclusive bridge memory window from `bar0_target` (`Base = (bar0 >> 16) & 0xFFF0`, `Limit = ((bar0 + 0x00FFFFFF) >> 16) & 0xFFF0`) and passing `ctx->pci_id`, `PMC_BOOT_0` (`ctx->chip_id`), and parsed FWSEC/SEC2 VBIOS geometries (`app_code_off`, `os_data_off`, `sig_dmem`, `engine_mask`, `ucode_id`) via `GU_S3_RESUME_CONTEXT` (`version = 2`, `216` bytes; `2,281 / 2,560` bytes code envelope).
   - Port-`0x80`-paced pre-FWSEC GFW boot/VRAM gate (`(0x118234 & 0xFF) == 0xFF && 0x100CE0 != 0`), strict `< 0x000A0000` low-RAM allocation, and `[P]` preflight warnings for Modern Standby (`FADT` Bit 21 `LOW_POWER_S0_IDLE_CAPABLE`) and non-zero `XFirmwareWakingVector` (`9,782` host checks passed).
10. **Universal-S3 `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` Primary Discovery (`v1.9.1`):**
    - Uses `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` (`SegmentNumber == target_seg`, `dev = 0..31`, multifunction `0x80` -> `func = 0..7`, filter `(HeaderType & 0x7F) == 0x01`, `sec > current_bus && sec <= target_gpu_bus <= sub`, up to 4 levels; logged as `S3 PCIe Bridge[i]: BDF=... (pri=.. sec=.. sub=..)`) to catch root ports without `EFI_PCI_IO_PROTOCOL` handles (e.g., AMI Aptio V `0:3:1`), falling back cleanly to corrected EDK2 `EFI_PCI_IO_PROTOCOL_GUID` and resolving `GuFindRootPortLinkReg()` via PCIe capability `0x10` (`LinkReg = CapOffset + 0x10` on `bridges[count - 1]`). Removes hardcoded `0:3:1` fallbacks and aborts cleanly with `EFI_NOT_FOUND` on `0` bridges for `target_gpu_bus > 0` (`9,798` checks).
11. **Plan-C S3 Hook Activation in `AdaUnlockDxe.efi` (`v1.9.2`):**
    - Defers `GuAcpiInstallEbsHook()` + `GuAcpiApplyShadowConfigTable()` from early BDS to `ReadyToBoot` via `AdaUnlockOnPlanCReadyToBoot()`.
12. **`gBS->StartImage` Guard (`EfiLoaderCode`) & Forensic Log Append (`v1.9.3`):**
    - `AdaUnlockHookedStartImage()` re-applies Plan C right before OS loaders (`EfiLoaderCode`, nested across `bootmgfw.efi -> winload.efi`) while leaving non-OS boot drivers on `RealRSDP`, logging pointer transitions via `GuBootAppendLog()`.
13. **Live ACPI Shadow Refresh `GuAcpiRefreshShadowChain()` Before OS Boot (`v1.9.4`):**
    - Re-clones `ShadowFADT`, `ShadowXSDT`, and `ShadowRSDP` live from post-BDS `RealXSDT`/`RealFADT` inside the `StartImage` guard (fixing early-kernel BSODs caused by late AMI BDS ACPI updates) and re-arms `RealFACS->FirmwareWakingVector`. Live-verified with Riot Vanguard + Secure Boot + ~9 h S3 sleep (`247.43` TFLOPS post-wake).
14. **Stickless `[I]` Multi-Volume Fallback & Hidden `GPU_UNLOCK` Partition (`v1.9.5`):**
    - Inspects free bytes via `EFI_FILE_SYSTEM_INFO` across primary ESP, `GPU_UNLOCK`/`UNLOCK_ESP`, and `<= 2 GiB` internal FAT32 volumes, with `VOLUME_FULL` rollback and `-Hidden` formatting in `tools/Create-Unlock-Partition.ps1`.
15. **Recursive Directory Creation & 1 MiB Chunked Streaming Copy (`v1.9.6`):**
    - `OpenOrCreateDirRecursive()` walks subdirectories step-by-step for UEFI `FatDxe` compatibility on empty partitions, and `CopyFileBetweenVolumes()` streams `1 MiB` blocks (`GU_COPY_CHUNK_BYTES`). Live-verified completely USB-free on hidden `512 MB` `GPU_UNLOCK` (`226.64` TFLOPS cold reboot, `234.19` TFLOPS / `768.22 GB/s` after S3 sleep/wake, Secure Boot + Riot Vanguard active).
16. **Cockpit UX, `S3 ARMED` Telemetry, Stepless Power-Limit Slider & Root Consolidation (`v2.0.0`):**
    - Live `S3 ARMED` dashboard status (`GetPwrCapabilities()` + live `FADT` + `RUN*.TXT`), stepless `1-W` power-limit slider with `Eco`/`Stock`/`Max` snap markers, direct `ShellExecuteExW` UAC elevation, optional `Re-apply PL on S3 Wake` (`UniversalGpuUnlock_ReapplyPL` / `--reapply-pl`), baseline delta readout, BitLocker/Secure Boot/S0ix pre-flight checks in `tools/Setup-AdaUnlock.ps1`, root cleanup to 3 entrypoints (`Setup.bat`, `Launch-Dashboard.bat`, `Check-GPU-Unlock.bat`), and archival of old changelog entries into `docs/CHANGELOG_ARCHIVE.md`.

---

## 7. Security Architecture & Containment (Code Review Summary)

### 7.1 Two-Stage Hardware Containment
- **Zero Mutation (`!GuUnlockMutated()`):** On unsupported cards or pre-flight refusal, the GPU remains completely untouched. The bootloader displays the reason and returns cleanly to the BIOS/boot menu after 5 seconds.
- **Active Mutation (`GuUnlockMutated()`):** If a failure occurs mid-transaction, the bootloader immediately clears Bus Master Enable (`GuClearMaster()`), restores PCI attributes, and executes a controlled halt (`GuBootHalt()`) so Windows never inherits an inconsistent DMA state.

### 7.2 CSM Video Isolation & GOP Recovery
- Detects legacy CSM video adapters (`pci_attributes & VGA_MEMORY/VGA_IO`).
- Prevents calling `ConnectController` on legacy VGA handles.
- **Layer 1:** Stages and binds the native UEFI GOP driver from the VBIOS Option ROM.
- **Layer 2:** Installs a safe fallback GOP protocol onto the EFI ConsoleOut handle when required.

### 7.3 DXE Self-Disable Safeguard (`AdaUnlockDxe.efi`)
- If a failure occurs during permanent DXE execution, the driver disables its own `Driver####` NVRAM entry before shutting down.
- This guarantees the system boots cleanly into stock Windows on the next power-on without boot loops.

### 7.4 Host Test Coverage (MSVC Host Suites)
All core components are validated against mocked firmware services (`tests/Run-MsvcHostTests.ps1`):
1. `test_driver_nvram`: NVRAM load options, `DriverOrder`, recursive directory creation, and chunked copying.
2. `test_boot_select`: Countdown timer and keyboard selection.
3. `test_csm_handover`: CSM detection and two-layer GOP recovery.
4. `test_gpu_inventory`: Multi-GPU inventory parser (57 checks).
5. `test_gpu_topology`: Negative topology isolation (44 checks).
6. `test_gpu_discovery`: PCI I/O adapter and Memory Space activation (21 checks).
7. `test_turing_local`: Ada/Ampere/Turing device IDs (including Ada Laptop IDs and `TU11x` exclusion), `GU_FEAT_PLM` / `SS0` / `SS1` decoder unit checks (`src/diag_decode.h`), and asset validation.
8. `test_s3_resume`: Universal S3 Resume engine, ACPI FACS shadow intercept, and trampoline checks (9,809+ checks).
9. `test_dxe_main`: Early-BDS deferral, `ReadyToBoot` / `StartImage` guards, and live ACPI refresh.
10. `test_multi_gpu_second_preflight` & `test_already_unlocked`: Multi-GPU plan reset and post-POST `already_unlocked` detection.
11. `test_s3_non_identity`: Non-identity `PciIo->Map()` S3 arena tracking, warning deduplication, `<4 GB` CPU-physical radix/WPR metadata repack, and clean DMA unmap.

---

## 8. Multi-GPU Sequential Unlock Engine (from v1.8.28)

### 8.1 Multi-GPU Challenge
In workstations and multi-GPU rigs (e.g., Dual RTX 4090 or RTX 4090 + RTX 3090), older bootloader versions processed only the primary display adapter (`gpu[0]`), leaving secondary GPUs capped at factory stock.

### 8.2 Sequential Unlock Loop
Starting in `v1.8.28`, both the bootloader (`src/app.c`) and DXE driver (`src/dxe_main.c`) share `GuUnlockAllGpus` (`src/multi_gpu.c`):
1. **Sequential Binding:** Iterates through all discovered supported NVIDIA GPUs (`0 .. device_count - 1`) via `GuBindDevice`.
2. **Dynamic Asset Reload:** Detects architecture changes (`is_ampere` / `is_turing`) on heterogeneous rigs (e.g., Ada + Ampere) and reloads matching firmware payloads (`AD102-ASSETS` vs `GA102-ASSETS`).
3. **Fault Handling:** If read-only asset loading or `GuPrepare` fails on a secondary card, that card is skipped and left stock. If an active unlock transaction fails mid-flight, the standard containment path aborts safely.
4. **S3 Boot Script Registration:** Registers S3 persistence after each successfully unlocked GPU (`[P]` / `[I]` modes).
5. **Plan Reset Between GPUs (`GuUnlockPrepareNextGpu`):** Resets static `Plan` state and `GuHardwareVerified` between cards once the preceding GPU is either completely verified or untouched (preventing `EFI_ALREADY_STARTED` `0x8000000000000014`, refs `#19`).
6. **Headless Secondary Ordering:** Secondary headless cards require an active GOP framebuffer on another adapter during preflight; therefore, the primary display GPU is rotated to the end of the list (`devices[device_count - 1]`).
7. **Console Reconnect:** Reconnects the primary display controller only after all GPU transactions succeed.

---

## 9. The 60-Second Quick-Start Principle & Root Cleanup

### 9.1 Eliminating Script Sprawl
A core goal of this project is a **60-Second Quick Start**: going from download to unlocked GPU within one minute. Accumulating standalone wrapper scripts (`Setup-FastResume.bat`, `Toggle-Sleep-Button.bat`, `Toggle-Turing-GSP.bat`, `Universal-Control-Center.cmd`) in the repository root caused unnecessary clutter.

### 9.2 Consolidated Structure (v2.0.0)
As of `v2.0.0`, the root directory contains strictly **3 user entrypoints**:
- **`Setup.bat`:** Unified setup menu (`[1]` new USB setup with BitLocker/SecureBoot/S0ix pre-flight checks, `[2]` update existing USB via `tools\Update-USB.bat`, `[3]` create hidden 512 MB `GPU_UNLOCK` partition via `tools\Create-Unlock-Partition.bat`, `[4]` launch Dashboard).
- **`Launch-Dashboard.bat`:** 1-click Cockpit GUI (`tools/gpu_dashboard.py` + `tools/dashboard_i18n.py` — live `EN` / `简体中文` / `DE` language switcher persisted in `config.json`, `S3 ARMED` indicator, stepless 1-W power-limit slider, SM clock-cap `-lgc` controls with S3-wake re-apply, baseline comparison, multi-GPU selector, benchmark, and quick utility launchers).
- **`Check-GPU-Unlock.bat`:** 1-click CLI verification (`tools/quick_check.py` — TFLOPS, `FMLA32/16`, S3 Persistence, Power Limit, CUDA ratio, ReBAR, PCIe link, and last wake time).

All helper utilities (`Setup-AdaUnlock.ps1`, `Update-USB.bat`, `Create-Unlock-Partition.*`, `Setup-FastResume.bat`, `Toggle-Sleep-Button.bat`, `Toggle-Turing-GSP.bat`, `Universal-Control-Center.*`) live cleanly under `tools/`, the Cockpit screenshot sits at `docs/images/dashboard-v2.0.0.png`, and `.gitattributes` (`export-ignore`) + `src/Build-Release-Zip.ps1` (`git archive HEAD`) ensure clean release archives without dev/meta files.

---

## 10. Turing Architecture & Rate-Limiter Reality (Issue #15 & #20 Analysis)

### 10.1 Analysis of `TU102-ASSETS`
Inside `TU102-ASSETS/`, `v67-tu102-610.43.03.bin` is byte-identical (same SHA-256) to `GA102-ASSETS/v67-ga102-610.43.03.bin`.

### 10.2 Hardware Findings
1. **No `SS0`/`SS1` MSR Registers on Turing:**
   Speed-select override registers at `0x0082381c` (`SS0`) and `0x00823820` (`SS1`) do not physically exist on Turing (`BAR0` reads return `0xbadf5040`). NVIDIA introduced those PLM-guarded speed-select registers starting with Ampere (`GA102`) and Ada Lovelace (`AD102`).
2. **Explanation for Issue #15:**
   On TU102 (RTX 2080 Ti / TITAN RTX), the SEC2 Falcon handshake runs cleanly and exits with success, yet GEMM stays at stock ~54 TFLOPS without GSP mode because those specific Ampere MSRs are absent.
3. **Actual Turing Compute Boost (GSP Firmware Mode):**
   On Turing, unlocking full Tensor throughput (>85 to 105 TFLOPS) relies primarily on enabling NVIDIA's GSP firmware mode in the Windows driver:
   ```cmd
   reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000" /v EnableGpuFirmware /t REG_DWORD /d 1 /f
   ```
   This binds `nvlddmkm.sys` to the onboard RISC-V GSP co-processor instead of the slower CPU RM path.

### 10.4 Turing Threshold Calibration & BF16 Clarification (Issue #20)
1. **cuBLAS FP32 Accumulation on Turing (`sm_75`):**
   In PyTorch (`torch.matmul`), FP16 matrices default to FP32 accumulation (`CUBLAS_COMPUTE_32F`) to preserve precision, which runs at half the issue rate of FP16-accumulate on 2nd-gen Tensor Cores.
   For an RTX 2060 (30 SMs) at 1680 MHz boost, the theoretical physical peak under FP32 accumulation is:
   $$30 \text{ SMs} \times 512 \text{ Ops/clock} \times 1.68 \text{ GHz} = 25.8 \text{ TFLOPS}$$
   The **25.82 TFLOPS** measured by `@resYuto` in Issue #20 (with `v1.8.31`, a `4.21x` multiplier over the 6.13 TFLOPS FP32 CUDA baseline) matches **100% of theoretical hardware capacity**.
2. **Threshold Calibration (`gu_system_info.py`):**
   Earlier checks expected 35 TFLOPS on RTX 2060 (derived from FP16-accumulate math) and produced false stock warnings. All Turing profiles (`RTX 2060` through `TITAN RTX`) were recalibrated for realistic dense FP32-accumulate GEMM thresholds (RTX 2060: threshold 18 TFLOPS, `min_unlocked` 20 TFLOPS).
3. **No Hardware BF16 on Turing:**
   Hardware BF16 Tensor Cores arrived with Ampere (`sm_80` / `sm_86`). On Turing (`sm_75`), BF16 falls back to slow CUDA emulation (~3.5 TFLOPS); the verification tools explicitly note this.
4. **Driver GSP Version Independence:**
   The Windows driver's GSP firmware version does **not** need to match the `610.43.03` pre-boot assets. UEFI pre-boot initialization is volatile; Windows loads its own GSP firmware normally (`560.xx`, `570.xx`, `572.xx+` all work).
5. **ReBAR on Turing (ReBarUEFI) & Strict No-Support Disclaimer:**
   Turing GPUs ship without ReBAR-enabled VBIOS firmware (`BAR1` = 256 MB). Enthusiasts can optionally test the external open-source ReBarUEFI project (xCuri0).
   > [!CAUTION]
   > **STRICT SUPPORT DISCLAIMER (NO SUPPORT FOR REBARUEFI):**
   > * The Universal GPU Unlock project provides **NO TECHNICAL SUPPORT, ASSISTANCE, OR WARRANTY** for ReBarUEFI or modifying/flashing motherboard BIOS ROMs!
   > * The GPU unlock operates via BAR0 and achieves **100% of physical tensor compute** without ReBAR (e.g., 25.82 TFLOPS on RTX 2060). ReBAR only optimizes PCIe transfers and does not increase raw Tensor Core compute throughput.
   > * **S4 Hibernate Warning:** Experimental pre-boot ReBAR overrides (`0x610`/`0xBC0` or `ReBarDxe`) combined with Windows S4 Hibernation (`Setup-FastResume.bat`) trigger hard `Kernel-Power 41` hangs, and forced `ConnectController` GOP reconnects during S4 resume cause TDR reboots. Universal GPU Unlock leaves BAR1 untouched across S3/S4 resume (pure SS unlock without ReBAR mutations or resume GOP reconnects).
   > * An optional experimental guide lives in `tools/optional-turing-rebar/`.

---

## 11. Silicon Detection for Already-Unlocked GPUs & Boot Freeze Protection (from v1.8.31)

### 11.1 The Pre-Installed DXE Driver Conflict
When `AdaUnlockDxe.efi` is permanently installed via `[I]` (`Driver####`), UEFI runs it early in POST before the USB boot menu. By the time `BOOTX64.EFI` launches, the GPU is already unlocked (`SS0=0x88888888, SS1=0x8`). Earlier versions aborted with `PREFLIGHT REFUSED` when `[P]` or `[U]` was selected afterward because `Registers()` expected cold-state values, causing some motherboards (notably MSI Click BIOS) to freeze on the splash screen when an error status returned to UEFI.

### 11.2 The v1.8.31 Fix
1. **Live Silicon Detection (`GuIsUnlocked`):** `BOOTX64.EFI` inspects `BAR0 + 0x82381c` and `0x823820`. If already unlocked in POST, the menu displays `[OK] GPU Silicon Status: ALREADY UNLOCKED (by DXE Driver in POST)`.
2. **Seamless Forwarding:** Selecting `[P]` or `[U]` skips the redundant hardware unlock, registers S3 persistence if `[P]` was chosen, and chains cleanly into Windows.
3. **Freeze Protection in `RunFailure`:** If preflight fails before any hardware mutation, the bootloader avoids handing an unhandled error status back to BIOS and safely boots stock Windows after a 5-second pause.
4. **S3 Protocol Fallback:** `GuS3PrepareAndRegister` checks both `EFI_S3_SAVE_STATE_PROTOCOL_GUID` (PI 1.2+) and `EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID` (Framework) and logs clear diagnostics if ACPI S3 is disabled in BIOS.

---

## 12. Ecosystem Enhancements & Hardware Stability (from v1.8.32)

### 12.1 12 MiB / 32 MiB GFW Usable-Memory Delta Tolerance (Zagitta PR #361)
On 24 GB+ cards (RTX 3090 Ti and RTX 4090), GPU firmware (GFW) reports **24,564 MiB** instead of 24,576 MiB pre-driver due to a 12 MiB aperture reservation delta (similarly 8 MiB on RTX 4080, 6 MiB on RTX 4070 SUPER). Strict byte-equality checks caused `-ERANGE` (`-34`) aborts. `src/vendor/unlock_610.c` now allows up to a 32 MiB delta (`delta_mb <= 32`) while validating that `mmu_lock_low` sits safely inside the upper pre-scrubbed 256 MiB window.

### 12.2 `ATTEMPT.FLG` Boot-Loop Safeguard (MindAllik v1.3.3 / v1.3.4)
To guarantee recovery if a hardware mutation freezes mid-flight:
1. Prior to the first hardware write, the loader creates `\GPU-UNLOCK\ATTEMPT.FLG` on ESP/USB.
2. The flag is removed only after transaction verification and display handover complete.
3. If `BOOTX64.EFI` or `AdaUnlockDxe.efi` sees an existing `ATTEMPT.FLG` on the next POST, it enters `[SAFEGUARD ACTIVE]`, logs a warning, clears the flag, and chainloads stock Windows immediately.

### 12.3 `SLOT-REVIVE` via Secondary Bus Reset & GOP Decouple (physicsdolphin v2.1.12-rc6)
On OEM systems (Dell/HP) or fast warm reboots, PCIe slots can stall out of `D0` or report `0xffff` / `0xffffffff`. Triggering a Secondary Bus Reset (SBR) while UEFI GOP is still bound causes fatal MMIO aborts, so `GuSlotRevive` (`src/gpu_discovery.c`) executes a strict 5-step sequence:
1. **GOP Decouple:** `DisconnectController()` releases active display/GOP bindings.
2. **PMCSR D0:** Forces power state bits 1:0 to D0 (`pmcsr & ~3U`) with 10 ms settle delay.
3. **SBR Pulse:** Pulses Secondary Bus Reset (offset `0x3E`, bit 6) on the upstream bridge for 2 ms, clears it, and waits 100 ms for link retraining.
4. **Bookkeeping Restore:** Restores BAR0/BAR1 and re-enables Memory Decode & Bus Master in PCI Command.
5. **Config Refresh:** Re-reads PCI headers for clean discovery.

### 12.4 Turing (TU102) `BCR_CTRL` Polling Skip (MindAllik v1.3.3)
On Turing GPUs (RTX 2080 Ti, TITAN RTX), polling `BCR_CTRL` (`falcon2 + 0x668`) triggers timeout `-110` (`poll timeout: reg=0x111668`). `src/vendor/unlock_610.c` skips this poll cleanly for `GU_ARCH_TURING` while keeping it active for Ampere and Ada.

### 12.5 Hardware Database Expansion (Mobile & Laptop GPUs)
`src/gpu_device_ids.h` added mobile Ampere IDs including `0x25ec` (RTX 3050 6GB Laptop, GA107), `0x25ed` (RTX 2050 Mobile, GA107), and `0x2560` (RTX 3060 Mobile / FrankenCard, GA106), matched in `tools/gu_system_info.py` and `tests/test_turing_local.c`.

### 12.6 S3 Persistence Protocol & Opcode Fixes (UEFI PI Standard)
Earlier revisions in `src/s3_save.h` used non-standard GUIDs (returning `EFI_NOT_FOUND` on MSI MEG X570 ACE / AMI Aptio V) and swapped opcodes (`MEM_WRITE` vs `IO_WRITE`, `DISPATCH_2` vs `STALL`). These were corrected to official UEFI PI definitions (`EFI_S3_SAVE_STATE_PROTOCOL_GUID` `{e857caf6-...}`, `EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID` `{470e1529-...}`, `MEM_WRITE = 0x02`, `DISPATCH_2 = 0x09`), paired with position-independent `GuS3ResumeDispatch` mirroring in `src/s3_resume.c` and honest pre-boot `[ OK ]` / `[ FAILED ]` telemetry.

---

## 13. Universal ACPI FACS Shadow Trampoline Architecture for S3 Standby (from v1.8.33 / v1.8.34)

### 13.1 The Problem: Sealed SMM LockBox on AMI Aptio V for AMD AM4/AM5
On modern AMD AM4 and AM5 motherboards (such as MSI MEG X570 ACE with AGESA 1.2.0.Cb / Sinkclose security updates), firmware signals `gEfiDxeSmmReadyToLockProtocolGuid` before BDS (Boot Device Selection) executes.
This irrevocably seals the SMRAM S3 Save State table. Any subsequent third-party bootloader or DXE driver call to `s3->Write()` is rejected with `EFI_OUT_OF_RESOURCES` (`0x8000000000000009`).
Earlier approaches lost the GPU unlock on S3 wake or had to rely solely on S4 hibernation.

### 13.2 The FACS Shadowing Principle
To bypass a sealed SMM LockBox completely, Universal GPU Unlock (`v1.8.33+`) installs a universal ACPI FACS shadow intercept:
1. **FADT Discovery:** Walks the UEFI Configuration Table for RSDP, traverses XSDT/RSDT to find `FACP` (Fixed ACPI Description Table), and resolves the real FACS address.
2. **Shadow FACS Allocation:** Allocates a shadow copy of FACS inside persistent `EfiACPIMemoryNVS` DRAM.
3. **FADT Redirection:** Updates FADT `FirmwareCtrl` and `XFirmwareCtrl` to point to the shadow FACS and recalculates the 8-bit FADT checksum (`0 mod 256`).
4. **Waking Vector Intercept:** Windows (`hal.dll` / `ACPI.sys`) writes its waking vector (`HalpRMStub`) into the shadow FACS while real FACS stays untouched.
5. **Real FACS Hook:** `RealFACS->FirmwareWakingVector` points to the machine-code S3 resume trampoline.

### 13.3 The v1.8.34 Low-Memory (< 1 MB) Trampoline Solution
Early `v1.8.33` tests woke from S3 to a black screen (requiring reset and hybrid-sleep recovery). Forensic analysis of EDK2/AMI Aptio V (`BootScriptExecutorDxe`) exposed the root cause:
Firmware invokes the S3 waking vector in **16-bit Real Mode** using a segment:offset far jump:
```nasm
mov ecx, [ebp + 8]   ; FirmwareWakingVector
shrd ebx, ecx, 20
and ecx, 0xf
mov bx, cx
mov [@jmp_addr + 1], ebx
retf
BITS 16
@jmp_addr: jmp 0x0:0x0 ; 16-bit far jump
```
Whenever the trampoline sat in high NVS (`> 1 MB`, e.g., `0xDC638300`), the segment truncated to 16 bits (`0x3830:0x0000 = 0x00038300`), crashing in conventional RAM.

In **v1.8.34**, I redesigned the low trampoline:
1. **Low-Memory Allocation (< 1 MB):** `AllocatePages(AllocateMaxAddress, EfiACPIMemoryNVS, 1, &low_tramp_phys)` places the trampoline below 640 KB (`< 0x000A0000`, fallback `< 0x00100000`), guaranteeing `Segment <= 0x9000` fits 16 bits without truncation.
2. **PCI Command Restoration (MSE + BME):** Re-enables Memory Space Enable (bit 1) and Bus Master Enable (bit 2) via `0x0CF8`/`0x0CFC` before BAR0/DMA access.
3. **Struct Offsets & Rel32 Branches:** Aligned `GU_S3_RESUME_CONTEXT` fields (`booter_dma` `0x20`, `booter_size` `0x28`, `val_ss0` `0x50`, `val_ss1` `0x54`, `resume_status` `0x58`, `resume_count` `0x5C`, `last_sec2_marker` `0x60`) verified by `TestGuS3ResumeContextOffsets`, with `0x0F 0x84 rel32` forward jumps.
4. **Clean Real-Mode Return & OS Handoff:** Switches back via a 16-bit PM descriptor (`Base = low_tramp_phys`), clears `CR0.PE`, restores BIOS `SS:SP`, zeroes GPRs, and executes `jmp far ptr cs:[0x30]` to `HalpRMStub`.
5. **Warm-Boot Resilience (Shadow FACS Tagging):** Tags shadow FACS `Reserved[0..3]` with `GU_SHADOW_FACS_SIGNATURE` (`0x53554753U` `"SGUS"`) and stores the original `RealFACS` pointer in `Reserved[4..11]` so warm restarts recover cleanly instead of nesting shadows.
6. **32-Bit Wake Vector Enforcement:** Clears bit 1 in FACS `Flags` and zeroes `XFirmwareWakingVector` so Windows uses the 32-bit Real-Mode vector.

### 13.4 The v1.8.35 Pure 16-Bit Real-Mode Passthrough & Low-Memory Shadow FACS
1. **Root Cause of v1.8.34 Black Screen:** Forensic checks showed that on S3 wake the discrete GPU was not yet link-trained or bridge-routed by `pci.sys`, so unprotected BAR0 MMIO access at `0xFB8403C0` triggered an unrecoverable PCIe Master Abort / Machine Check stall.
2. **Pure 16-Bit Real-Mode Passthrough (No MMIO):** Executed purely in 16-bit Real Mode without GDT/CR0 transitions.
3. **Co-Located Shadow FACS (`0x9E200`):** Placed 64-byte shadow FACS on the same low NVS page at `+0x0200`, read directly via `DS:[0x020C]` / `DS:[0x0218]`.
4. **4-Stage Vector Resolution:** Resolves shadow/backup vectors safely without crashing to `0`.
5. **Exact Segment:Offset Math & Telemetry:** Converts vector (`Offset = AX & 0x0F`, `Segment = (AX >> 4) | (DX << 12)`) and hands off to `HalpRMStub` cleanly (< 1.7s wake).

### 13.5 The v1.8.36 Risk-Free Pre-OS Config-Space Recon, Bridge Restore & PCIe Link Status (`DLLLA`)
1. **No `MSE/BME` (`0x0006`) on Root Port `0:3:1`:** Removed writing `0x0006` into bridge offset `0x04` during passive recon; when offset `0x20` reads `0x00000000` (`Base=0, Limit=0x000FFFFF`), enabling `MSE` covers `< 1 MB` DRAM and cuts the CPU off from `0x9E000` and `HalpRMStub`. Plain `0xCF8`/`0xCFC` config cycles require neither `MSE` nor `BME`.
2. **Temporary Bus Routing & Mandatory Bridge Restore:** Saves Root Port `0:3:1` offset `0x18` (preserving Latency Timer bits 31:24) in `[0x0348]`, sets `Pri=0x00, Sec=0x2D, Sub=0x2D` to probe Bus 45, and immediately restores offset `0x18` before jumping to `HalpRMStub`.
3. **PCIe Link Status (`DLLLA`) & Dynamic PMCSR Discovery:** Dynamically locates PCIe Capability `0x10` (`CapPtr + 0x10`) on the Root Port and PM Capability `0x01` (`CapPtr + 0x04`) on the GPU for warm-wake telemetry logging (`DLLLA` bit 13, Training bit 11, link speed/width).
4. **Strict Code/Data Separation & Host Verification:** Code (`0x0000..0x015B`, 347 bytes), ACPI tables (`0x0200`/`0x0280`), and telemetry (`0x0300..0x036F`) verified across 106 MSVC host tests (asserting `0x66` operand-size overrides on all 32-bit `in`/`out` instructions and no writes to `0x80001904`).

### 13.6 The v1.8.37 Split-FACS Experiment (Historical)
Live `kd.exe` kernel measurements during S3 confirmed that if both `FirmwareCtrl` and `XFirmwareCtrl` point to `ShadowFACS`, AMI BIOS re-reads FADT on wake and jumps directly to Windows' vector in `ShadowFACS`, skipping `RealFACS`. `v1.8.37` attempted splitting `FirmwareCtrl = RealFACS` vs `XFirmwareCtrl = ShadowFACS`.

### 13.8 The v1.8.39 Exact FADT Field Mapping (Windows vs. AMI Aptio V)
1. **Live Kernel Disassembly (`ntoskrnl.exe`):** Disassembling `nt!HalpAcpiGetFacsMapping` proved:
   ```asm
   nt!HalpAcpiGetFacsMapping:
   fffff801`085bb087 mov edx, dword ptr [rax+24h] ; Reads offset 0x24 = FirmwareCtrl!
   fffff801`085bb0b1 call nt!HalpAcpiCheckAndMapTable
   ```
   64-bit Windows reads exclusively 32-bit `FirmwareCtrl` (`0x24`) and ignores `XFirmwareCtrl`.
2. **Firmware Behavior (AMI Aptio V / EDK2 `S3Resume2Pei`):** Follows `if (FADT->Header.Length >= 140 && FADT->XFirmwareCtrl != 0) Facs = FADT->XFirmwareCtrl; else Facs = FADT->FirmwareCtrl;`.
3. **`v1.8.39` Mapping:** `RSDT` pointed to original FADT, while `ShadowFADT` (in `XSDT`) set `FirmwareCtrl = ShadowFACS` and `XFirmwareCtrl = RealFACS`. Verified across 106 host tests (`MSVC_HOST_TESTS=PASS`).

### 13.9 The v1.8.40 Plan-C Full Shadow Chain & `ExitBootServices` Sandwich Hook
1. **Live S3 Observation (`RUN0048.TXT` & WinDbg):** AMI `S3Resume2Pei` ignored `XFirmwareCtrl` for 32-bit Real-Mode resume and also read `FirmwareCtrl` (`0x9E200`), skipping `RealFACS` whenever FADT `FirmwareCtrl` was modified in place.
2. **Plan-C Architecture (Complete Shadow Chain):**
   - **Original Tables 100% Untouched:** `RealRSDP`, `RealRSDT`, `RealXSDT`, and `RealFADT` stay byte-identical; only `RealFACS + 0x0C` points to `LowTrampoline` (`0x9E000`).
   - **Complete Shadow Chain in `EfiACPIMemoryNVS`:** `ShadowFACS` (`0x9E200`) -> `ShadowFADT` (`0x9E400`, `FirmwareCtrl = ShadowFACS`, `XFirmwareCtrl = 0`) -> `ShadowXSDT` (`0x9E640`) -> `ShadowRSDP` (`0x9E600`, recomputed 20-byte + 36-byte checksums).
   - **In-Place `ConfigurationTable` Patch:** Switches `gEfiAcpi20TableGuid` / `gEfiAcpiTableGuid` to `ShadowRSDP`.
3. **Minimal `ExitBootServices` Sandwich Hook:**
   - **Pre-Call:** Restores `ConfigurationTable` to `RealRSDP` immediately before calling `OrigExitBootServices()` so firmware/SMM S3-save handlers capture genuine OEM ACPI tables (zero BootServices calls inside hook).
   - **Post-Call:** Restores `ShadowRSDP` on retry (`winload.efi` `EFI_INVALID_PARAMETER`).
   - Verified across 137 tests in 10 suites (`MSVC_HOST_TESTS=PASS`).

### 13.10 The v1.8.40b EBS Conditional Retry & Low-Trampoline Diagnostic Counters
1. **Kernel Forensics (`HalpSetupRealModeResume`):** Confirmed Windows writes `0x00001000` (`HalpLowStubPhysicalAddress`) into `ShadowFACS + 0x0C` and leaves `XFirmwareWakingVector` (`+0x18`) untouched.
2. **Conditional Phase 2:** Restores `ShadowRSDP` on return from `OrigExitBootServices()` **only** when `EFI_ERROR(Status)` (retry). On `EFI_SUCCESS`, `RealRSDP` stays installed cleanly since `winload.efi` already consumed the shadow chain.
3. **BootServices-Free Counters:** Records `EbsCallCount` (`+0x304`), `EbsSuccessCount` (`+0x308`), and `EbsLastStatus` (`+0x30C`) (145 host tests passing).

### 13.11 The v1.8.41 32-Bit Flat Protected Mode Trampoline via `XFirmwareWakingVector` (`+0x18`)
Explored ACPI 2.0+ vector priority (`XFirmwareWakingVector != 0`, `OspmFlags = 0` -> EDK2 `AsmTransferControl32` 32-bit PM entry) using an embedded 5-entry GDT (`+0x0180`, dynamic 16-bit descriptors with `Base = LowTrampoline` avoiding `#GP(0)` 64 KB limit faults) and 32-bit PM -> 16-bit PM -> Real Mode teardown (`164` tests passing).

### 13.12 The v1.8.42 64-Bit Long Mode S3 Resume Trampoline via `XFirmwareWakingVector` (`+0x18`)
Implemented a 4-stage `64-bit Long Mode -> 32-bit Compat Mode -> 32-bit Flat PM -> 16-bit PM -> 16-bit Real Mode` transition using indirect `FF 2D <disp32>` far jump (`0xEA` is invalid `#UD` in 64-bit mode), 10-byte `GDTR64`, RIP-relative addressing, and full opcode-stepper verification (`169` host tests passing).

### 13.13 The v1.8.43b Full Long-Mode Teardown (`EFER`/`CR3`/`CR4`), GDT Shift (`0x01C0`) & `OspmFlags |= 0x01`
Re-enabled `OspmFlags |= 0x01` + `Flags |= 0x02` + `Version >= 2`, implemented a stack-free **polyglot dual-mode preamble** (`FA FC 31 C0 40 0F 84 <disp32>`, branching via `0x40` acting as `inc eax` in 32-bit vs silent REX prefix in 64-bit), cleared `CR0.PG`, `IA32_EFER.LME/NXE` (`0xC0000080`), `CR3`, and `CR4.PAE`, and shifted tables to `0x01C0` (`187` host checks passing).

---

### 13.14 The v1.8.44 Closure of 3 ACPI Leak Vectors & Return to Stable 16-Bit Real Mode

1. **Hardware Finding on 64-Bit Wake (`RUN0057.TXT` & WinDbg):**
   - The empirical 22-byte `0xCF9` reset probe (`v1.8.43c-probe`) at `0x9E000` was never reached when `OspmFlags |= 0x01` was set: AMI Aptio V on AMD AM4 stalled inside BIOS PEI before jumping to any waking vector.
   - The 64-bit wake (`XFirmwareWakingVector` `+0x18`) was permanently retired in favor of the reliable 16-bit Real-Mode path (`RealFACS + 0x0C = LowTrampoline = 0x9E000`).

2. **LEAK 1 Closed (`ShadowRSDP`/`ShadowXSDT`/`ShadowFADT` in High Memory `> 1 MB, < 4 GB`):**
   - Moved `ShadowRSDP` (`+0x000`), `ShadowXSDT` (`+0x080`), and `ShadowFADT` (`+0x200`) onto a dedicated high NVS page (`AllocateMaxAddress = 0xFFFFFFFF`), removing every `"RSD PTR "` signature below 1 MB.

3. **LEAK 2 Closed (Dynamic Live GUID Scan & `CRC32` Update):**
   - `GuAcpiApplyShadowConfigTable()` and `GuAcpiRestoreConfigTable()` scan `gST->ConfigurationTable` dynamically on every call and recompute `gST->Hdr.CRC32`.

4. **LEAK 3 Closed (`EFI_ACPI_S3_SAVE_PROTOCOL` Sandwich Hook):**
   - Hooks EDK2 `AcpiS3SaveDxe` (`125f2de1-fb85-440c-a54c-4d99358a8d38`) so SMM LockBox saves genuine OEM `RealRSDP`/`RealFACS` before swapping `ShadowRSDP` back in for Windows.

5. **Pure 16-Bit Real-Mode Trampoline (< 380 Bytes):** Saves BIOS `SS:SP`, writes `'S3_T'` + `0xAA01`, probes 8 PCIe DWORDs with clean bridge restore, resolves waking vector, restores stack, writes `0xAA02`, clears GPRs, and executes `jmp far ptr cs:[0x0300]` (`MSVC_HOST_TESTS=PASS`).

---

### 13.15 The v1.8.45 Sub-1MB Signature Cleanup, High-Memory `ShadowFACS`, Late `ConfigurationTable` Apply & S3Save Forensics

1. **Banning `ShadowFACS` from `< 1 MB` (`+0x0400`):** Moved `ShadowFACS` to `HighAcpiPage + 0x0400` (`ShadowFADT->FirmwareCtrl = HighAcpiPage + 0x0400`) and zeroed `0x9E200..0x9E2FF`. Below 1 MB neither `"RSD PTR "` nor `"FACS"` exists.
2. **Collision-Free Stack Save (`+0x0370`/`+0x0372`):** Shifted BIOS `SS:SP` save away from EBS diagnostic fields (`+0x0304`), with a clean `0x1000` (`0100:0000`) fallback if `[0x020C] == 0`.
3. **Deferred `ConfigurationTable` Patching:** Applies `ShadowRSDP` only right before `gBS->StartImage(Windows)` in `GuBootWindows()` after display reconnect and USB log close, rolling back cleanly on error.
4. **Forensic Read-Only Logging & Re-Entrancy Guard:** Logs `RealFADT` vectors, `ConfigurationTable` entries, and 256-byte `S3Save` dump (`8,474` host checks passing).

### 13.16 The v1.8.46 16->32->16-Bit Flat Mode Bridge, High-Memory Vector Read & Telemetry Mirroring

1. **Empirical Confirmation of 16-Bit Real-Mode Entry (`v1.8.45b-probe`):** A 20-byte `0xCF9` hard-reset probe at `0x0009E000` triggered on AMI Aptio V S3 wake (`Kernel-Power 41`), proving 100% firmware entry into Real Mode at `0x9E000`.
2. **Solving High-Memory Access (`> 1 MB`) & WinDbg Visibility:** Built a stack-free 16->32->16-bit PM bridge (`410` bytes inside `512` bytes) using a 5-entry GDT at `+0x0200..0x0237` (16-bit code/data base `0x00090000`) to read `ShadowFACS + 0x0C` at `HighAcpiPhys + 0x040C` and mirror 32 DWORDs of telemetry (`0x9E300..0x9E37F`) to `HighAcpiPhys + 0x0500` (`8,431` checks passing).

---

### 13.17 The v1.8.46b Diagnostic Marker `0x324B4F21` (`'!OK2'`) & Aligned 31-DWORD Mirror

Writes `0x324B4F21` (`'!OK2'`) immediately at `+0x0500` upon entering 32-bit PM, followed by copying 31 DWORDs (`0x0009E304 -> +0x0504`) (`420` bytes code, `8,432` host checks passing).

---

### 13.18 The v1.8.46c Dedicated Non-NVS Telemetry Page Bypassing `nt!HalpRestoreNvsArea`

1. **HAL Telemetry Wipe Diagnosis:** `kdX64.exe -kl` inspection of `nt!HalpNvsRegionData` proved `HighAcpiPage` (`0xDC617000`) resided inside a 27 MB BIOS NVS region (`0xDADDA000..0xDC8DA000`). On wake, `nt!HalpRestoreNvsArea` overwrote all 27 MB with the pre-sleep snapshot, wiping `0x324B4F21` back to zero.
2. **Non-NVS Page Fix:** `GuAcpiAllocateTelemetryPage()` allocates a dedicated 4 KB page below `0xDADD9000` as `EfiRuntimeServicesData` (not restored by `nt!HalpRestoreNvsArea`, yet virtual-mapped by `winload.efi` for live WinDbg `!dd <phys> L20` inspection; `8,435` host checks passing).

---

### 13.19 The v1.8.47 32-Bit Flat-Mode Temporary BAR0 MMIO Hardware Unlock on S3 Wake

1. **Telemetry Proof from `v1.8.46c` (`0xd8261000`):** Proved clean execution through the bridge (`324B4F21`, `0xAA01`, `0xAA02`, 1603 ms wake) and showed GPU (`0x2D:0:0`) in D0 with `BAR0 = 0x00000000` and `MSE` cleared.
2. **4 KB Low-Trampoline Memory Layout (`0x0009E000`):**
   - `+0x0000..+0x03FF` (1024 bytes): Executable machine code (`791` bytes used, `233` bytes guard).
   - `+0x0400..+0x0437` (56 bytes): `RM_IDTR` (`+0x0400`), `PM_GDTR` (`+0x0408`), 5 GDT descriptors (`+0x0410`).
   - `+0x0438..+0x04FF` (200 bytes): Zero-initialized guard region.
   - `+0x0500..+0x057F` (128 bytes): Telemetry & EBS-sandwich diagnostics (`0x0500` far jump target, `0x0504` `RealRSDP` low32, `0x0508` EBS counters, `0x0510` `0xAA01`, `0x0512` `0xAA02`, `0x0514` vector, `0x051C` `'S3_T'`, `0x0520` probes, `0x0540` raw, `0x0560` post-arm, `0x0570`/`0x0572` saved `SP`/`SS`).
   - `+0x0580..+0x058F` (16 bytes): Real FACS original vector mirror (`+0x0580` 32-bit, `+0x0588` 64-bit); `+0x0590..+0x0FFF` guard.
3. **32-Bit Unlock Flow & Non-NVS Telemetry Reference (`telemetry_phys + 0x80..0xBC`):**
   Temporarily routes Root Port `0:3:1` memory window (`0xFC00FB00`), assigns GPU `BAR0 = 0xFB000000` + `MSE`, reads Chip ID (`0x192000a1`, aborts to teardown on `0xFFFFFFFF` / `0xBADFxxxx`), writes `SS0 = 0x88888888` / `SS1 = 0x00000008`, and restores all PCI config registers cleanly before returning to Real Mode (`8,811` host checks):

   | Offset | Field | Description / Expected Value |
   | :---: | :--- | :--- |
   | `+0x80` | `Magic` | `0x554E4C4B` (`'UNLK'`) |
   | `+0x84` | `Status` | `1` = Success, `2` = Mismatch, `3` = Fault / Bad Chip |
   | `+0x88` | `ChipID` | GPU Chip ID (`0x192000a1` on `AD102`) |
   | `+0x8C` | `AssignedBAR0` | Temporary BAR0 aperture (`0xFB000000`) |
   | `+0x90` | `SS0_Before` | `SS0` (`+0x82381C`) prior to unlock |
   | `+0x94` | `SS1_Before` | `SS1` (`+0x823820`) prior to unlock |
   | `+0x98` | `Readout1_Before` | SM speed status (`+0x823818`) prior to unlock |
   | `+0x9C` | `PLM_Before` | Privilege Level Mask (`+0x823804`) prior to unlock |
   | `+0xA0` | `SS0_After` | `SS0` after unlock (`0x88888888`) |
   | `+0xA4` | `SS1_After` | `SS1` after unlock (`0x00000008`, bits 3:0 == `0x8`) |
   | `+0xA8` | `Readout1_After` | SM speed status after unlock (`0x00000000` = Full Rate) |
   | `+0xAC` | `PLM_After` | Privilege Level Mask after unlock (`0xFFFFFFFF`) |
   | `+0xB0` | `OrigRootPortCmd` | Pre-trampoline Root Port Command register |
   | `+0xB4` | `OrigRootPortMem` | Pre-trampoline Root Port Memory Base/Limit |
   | `+0xB8` | `OrigGpuCmd` | Pre-trampoline GPU Command register (`0x00000000`) |
   | `+0xBC` | `OrigGpuBar0` | Pre-trampoline GPU BAR0 register (`0x00000000`) |

---

### 13.20 The v1.8.48 32-Bit Port I/O Opcode Fix & Complete PCIe Bridge Teardown

1. **Root Cause 1 (32-Bit Immediate Phase Shift):** Fixed 2-byte `mov edx` port opcodes (`BA F8 0C`) emitted in 32-bit PM mode (which caused the CPU to consume subsequent instruction bytes as 4-byte immediates and throw PCIe Master Abort) to proper 5-byte `mov edx, imm32` (`BA F8 0C 00 00` / `BA FC 0C 00 00`) and 6-byte `cmp edx, 0x0000BADF`.
2. **Root Cause 2 & 4-Stage Teardown:** Unconditionally routes `0xFC00FB00` after saving `+0xB4`, then restores GPU Command -> GPU BAR0 -> Root Port Memory Base/Limit -> Root Port Command in reverse order (`839` bytes code, `8,815` host checks passing).

---

### 13.21 The v1.8.49 PLM Privilege Level Mask Unlatch Write in 32-Bit Flat Mode

Confirmed real `AD102` Chip ID `0x192000a1` readback in Windows (`Status = 2` mismatch) and exposed `PLM_Before = 0xFFFFFF8F` (`0x00823804`), testing direct host MMIO PLM unlock write (`0xFFFFFFFF`) ahead of `SS0`/`SS1` (`849` bytes code, `8,816` checks passing).

---

### 13.22 The v1.8.50 SEC2 Falcon Booter Handshake & PCIe Bus Master DMA in 32-Bit Flat Mode on S3 Wake

1. **Forensic Proof from `v1.8.49`:** Confirmed AD102 ignores host MMIO writes to `0x00823804` (`PLM` stayed `0xFFFFFF8F`, proving PLM can only be opened internally by executing the SEC2 Falcon Booter).
2. **SEC2 Falcon Handshake & PCIe `BME` (`0x04`) + `MSE` (`0x02`):** Enabled `MSE | BME` (`or al, 0x06`) on Root Port `0x80001904` and GPU `0x802D0004`, performed SEC2 setup (`0x00840624 |= 0x80`, `0x0084010C = 0`, FBIF mode `( & ~7) | 5`, DMA base/size, `CPUCTL = 2`), bounded halt poll (`50,000` iters), and logged `CPUCTL` (`+0xC0`) & marker (`+0xC4`) (`973` bytes code, `8,695` host checks).

---

### 13.23 The v1.8.51 SEC2 Falcon Hardware Diagnostics & Deterministic Reset Cycle in 32-Bit Flat Mode

1. **Diagnosis (`0xBADF5620`):** `+0xC0` read `0xBADF5620` (`SYS_PRI_ERROR_CODE` — SEC2 still reset/clock-gated/scrubbing).
2. **Telemetry Expansion (`+0xC0..+0xD0`):** Captures `PMC_ENABLE` (`0x00000200`), `SEC2_RESET` (`0x008403C0`), `SEC2_HWCFG2` (`0x008400F4`), `FALCON2_BCR` (`0x00841668`), and post-reset `CPUCTL` (`+0xD0`).
3. **RM-Exact Falcon Reset Cycle:** Assert bit 0 on `0x008403C0`, 10 dummy reads, release bit 0, 10 dummy reads, bounded scrub poll on `0x008400F4` bit 12 == 0, and write Chip ID to `0x00840084` (`979` bytes, `8,697` host checks).

---

### 13.24 The v1.8.52 SEC2 Falcon BCR Mode Switch & Boot-ROM Unmask in 32-Bit Flat Mode

1. **Hardware Root Cause from `v1.8.51` (`kdX64.exe -kl`):**
   ```text
   +0xC0: 40000000 (PMC_ENABLE, BAR0 + 0x00000200) -> Device Enable active
   +0xC4: 00000000 (SEC2_RESET, BAR0 + 0x008403C0) -> Engine reset released
   +0xC8: 000067B7 (SEC2_HWCFG2, BAR0 + 0x008400F4) -> Bit 12 = 0 (scrub idle), Bit 10 = 1 (BCR required)
   +0xCC: 00000110 (FALCON2_BCR, BAR0 + 0x00841668) -> Bit 4 set (Boot-ROM mode active!)
   +0xD0: BADF5620 (SEC2_CPUCTL, BAR0 + 0x00840100) -> SYS_PRI_ERROR_CODE
   ```
   Bit 4 (`0x10`) in `FALCON2_BCR` traps the RISC-V core in Boot-ROM mode and blocks PRI CPU register reads with `0xBADF5620`.
2. **RM BCR Mode Switch (`0x00841668 = 0`, poll `& 0x11 == 1`):** Clears Boot-ROM mode deterministically (`50,000` iter bound), records `+0xD4` (`FALCON2_BCR_POST`), and unlocks `CPUCTL` (`1,016` bytes code, `8,700` checks).

---

### 13.25 The v1.8.53 SEC2 FBIF & Booter DMA Handshake & Falcon Execution

1. **Breakthrough Confirmed (`kdX64.exe -kl`):** `+0xD4` transitioned `0x00000110 -> 0x00000001`, `+0xD0` (`SEC2_CPUCTL_Post`) became **`0x00000010`** (`0xBADF5620` gone completely!).
2. **Expanded Envelope:** Relocated `RM_IDTR`/`PM_GDTR`/`GDT` to `+0x0600..0x0637` past telemetry (`0x0500..0x05BF`), expanding code envelope from 1024 to 1280 bytes (`1,115` bytes used, `8,709` host checks).

---

### 13.26 The v1.8.54 AD102 Chunked 256-Byte SEC2 DMA (IMEM/DMEM), Falcon2 Registers & Mailbox Booter Execution

Completed chunked 256-byte RISC-V SEC2 DMA inside the 32-bit trampoline:
1. **Finding from `v1.8.53` (`RUN0070.TXT`):** Single legacy `0x110/0x118` writes left `+0xD8 = 0` because AD102 Falcon 2 requires 256-byte chunked IMEM/DMEM DMA plus Falcon2 registers and `MAILBOX0`.
2. **Chunked DMA Engine (`src/s3_facs.c`):**
   - FBIF setup (`0x00840624 |= 0x80`, `0x0084010C = 0`, `0x00840600 = ( & ~7) | 5`, `0x00840110 = booter_dma >> 8`, `0x00840128 = 0`).
   - IMEM Transfer: 125 chunks (32,000 bytes microcode from offset `256`, `cmd = 0x0614`).
   - DMEM Transfer: 96 chunks (24,576 bytes data from offset `32,256`, `cmd = 0x0600`).
3. **Falcon2 Config & Mailbox (`0x00840104 = 256`, `0x00841210 = 16`, `0x0084119C = 1`, `0x00841198 = 3`, `0x00841180 = 1`, `MAILBOX0 = meta_dma`, `CPUCTL = 2`):** Polls `0x67b00001` (`GU_AD102_MARKER`) or halt bit, writes `SS0`/`SS1`, and stays at `1,275` bytes (`< 1,280` bytes limit; `8,714` checks).

---

### 13.28 The v1.8.56 Falcon DMA Base-Reload, Completion Poll & `meta_dma` Guard Hardening

1. **DMEM Base Reload & Dual Completion Polls (`src/s3_facs.c`):** Reloads `0x00840110 = (booter_dma + 32256) >> 8` for offset `0` in DMEM pass and enforces DMA completion polling (`(0x00840118 & 2) == 2`) after both IMEM and DMEM transfers.
2. **Polling Guard:** Ignores stale reset Halt bit while `MAILBOX0 == meta_dma` (`1,272` bytes code, `8,714` host checks).

---

### 13.29 The v1.8.57 Booter Container Header Unpacking & Dynamic Fuse Signature Patching in ACPI NVS

1. **72-Byte Booter Container Root Cause (`booter_load_ad102.bin`):** `unlock_610.c` unpacks the executable image past its 72-byte `BTL1` (`0x314C5442`) header (`image_size = 56,832`) and patches the GPU fuse signature at `patch_loc = 32,272`. Prior S3 prep copied the raw container, shifting IMEM/DMEM DMA by 72 bytes and leaving RISC-V trapped at Boot-ROM (`0x00841380 = 0x00080000`).
2. **S3 Prepare Fix (`src/s3_resume.c`):** Validates `GU_BOOTER_MAGIC`, extracts `data + 72` (`56,832` bytes) to `nvs_base + 0x01000`, patches `Gu.fuse_version` via `gpu_unlock_booter_patch()`, and sets `ctx->booter_size = 56,832` (`8,721` host checks).

---

### 13.31 The v1.8.58 1:1 SEC2 Engine Reset Cycle, 5-Stage PCIe Teardown & Wake Crash-Dump Forensics

1. **1:1 `hold_sec2_reset()` Harmonization (`src/s3_facs.c` vs. `src/vendor/unlock_610.c:1614-1674`):**
   - SEC2 must exit reset with `RESET_FALSE` (`SEC2_BASE + 0x3c0` bit 0 = 0); leaving `RESET_TRUE` causes subsequent host reads to return `FECS_PRI_RESET` (`0xbadf12` in bits 31:8), stalling driver scrub waits indefinitely. `0x00841668` (`FALCON2_BCR`) is strictly pre-bootrom only (`falcon_reset()`) and is never re-written in post-unlock cleanup.
   - Stack-free `.sec2_reset_cycle` (`jmp ebp`): (1) `0x008403C0 |= 1`, (2) 10 dummy reads, (3) bounded poll `& 1 == 1` (`1000 us`), (4) `0x008403C0 &= ~1`, (5) 10 dummy reads, (6) bounded poll `& 1 == 0` (`1000 us`), (7) bounded poll `0x008400F4` bit 12 (`0x1000`) == 0 (`1000000 us`). Invoked both pre-booter and post-selector write.
2. **Crash-Dump Forensics (`C:\Windows\MEMORY.DMP`):**
   - `BugCheck 0x0000007E` (`SYSTEM_THREAD_EXCEPTION_NOT_HANDLED`), `P1 = 0xFFFFFFFF80000003` (`STATUS_BREAKPOINT`), `P2 = nvlddmkm.sys + 0x25BF4DE` (driver `621.14 / 32.0.16.2114`) firing fail-fast `int 3`.
   - Root cause: discrete desktop GPUs reside in `D3cold` across S3 wake (onboard VRAM unpowered until `nvlddmkm.sys` devinit); if Root Port bridge routing stays disabled or SEC2 scrub is incomplete, the driver triggers fail-fast `int 3` (`black screen`).
3. **5-Stage PCIe Teardown & Telemetry Bounds (`src/s3_facs.c`):**
   - Reverse restore sequence: Stage 1 GPU Command (`0x802D0004`), Stage 2 GPU BAR0 (`0x802D0010`), Stage 3 Root Port Memory Base/Limit (`0x80001920`), Stage 4 Root Port Command (`0x80001904`), Stage 5 Root Port Sec/Sub Bus (`0x80001918`).
   - Telemetry copy (`rep movsd` of 24 DWORDs = 96 bytes to `+0x0580..+0x05E0`) stays asserted strictly below `RM_IDTR` (`+0x0600`) (`1,277` bytes code `< 1,280`; `8,720` host checks).

---

### 13.32 The v1.8.59 True PCIe FLR, Preserved Bridge Bus Routing & 2 KB Code Envelope

1. **Remaining S3-Wake Black Screen Root Causes Fixed:**
   - **Windows Confirmed Alive:** Kernel ran cleanly without `BugCheck 0x7E`, proving `v1.8.58` `hold_sec2_reset()` cured the driver crash.
   - **Removed Stage 5 Bus Zeroing:** `v1.8.58` Stage 5 wrote `0x00000000` back to Root Port `0:3:1` offset `0x18` (`0x80001918`), cutting bus `0x2D` off from Windows. Removing Stage 5 preserves `0x002D2D00` (`Pri=0, Sec=0x2D, Sub=0x2D`) active through the 4-stage teardown (`0x802D0004`, `0x802D0010`, `0x80001920`, `0x80001904`).
   - **Pre-Existing `WPR2` Lockout (`src/vendor/unlock_610.c:1835-1840`):** `kgspIsWpr2Up_TU102` rejects cold-D3cold resume if `(hi >> 4)` is non-zero (`"post-FLR WPR2 remains enabled ... stock driver requires HI.VAL=0"`, returning `-EBUSY`).
2. **8-Step True PCIe Function-Level Reset (`src/vendor/unlock_610.c:1750-1860`):**
   Immediately after `SS0`/`SS1` writes: (a) clear Bus Master `0x802D0004 &= ~0x04`, (b) assert FLR bit 15 (`0x8000`) at `Gu.pcie_cap + 0x08` (`0x802D0080`), (c) ~120 ms stall via `120,000` port-`0x80` reads, (d) poll `0x802D0000 == 0x268410DE` (`50,000` iters), (e) restore `BAR0 = 0xFB000000` & `Cmd = 0x06` (`MSE | BME`), (f) poll `[ebx] == 0x192000A1` (`50,000` iters), (g) run stack-free `.sec2_sub_start`, (h) log post-FLR `WPR2_HI` (`+0xE0` == `0x00000000`), `SS0` (`+0xE4`), `SS1` (`+0xE8`), and `PLM` (`+0xEC`).
3. **2048-Byte Code Envelope Map (`< 0x0800`):**
   - `0x0000..0x07FF`: Executable code (`1,424` bytes used = `0x0590`); `0x0800..0x087F`: Data/checkpoints; `0x0880..0x08EF`: Low FACS mirror (28 DWORDs = 112 bytes, compile/runtime asserted via `GU_TRAMP_REAL_FACS_MIRROR_OFF + 112 <= GU_TRAMP_RM_IDTR_OFF`); `0x0900..0x0905` `RM_IDTR`; `0x0908..0x090D` `PM_GDTR`; `0x0910..0x093F` GDT (`8,729` host checks).

---

### 13.33 The v1.8.60 Radix Table & GSP Firmware Retention in `EfiReservedMemoryType` Across S3 Sleep

1. **Booter `0x91` Root Cause (`!dd d81a3000 L10`):** `EfiBootServicesData` radix tables were reclaimed and zeroed by Windows after `ExitBootServices()`, causing SEC2 Booter `RADIX_TABLE_INVALID` / `PTE_LOOKUP_FAILED` (`0x00000091`) on S3 wake.
2. **Strict `EfiReservedMemoryType` Allocation (NEVER `EfiACPIMemoryNVS`):** Allocates the ~84 MB GSP firmware image (`0x05053000` bytes) + 41 radix pages (`20,604` pages = `84,393,984` bytes total) in `EfiReservedMemoryType` (`< 4 GB`), with `GuDmaAllocate()` falling back to `gBS->AllocatePages(AllocateMaxAddress, EfiReservedMemoryType, 0xFFFFFFFFULL)` when `PciIo->AllocateBuffer` restricts memory types. Using `EfiACPIMemoryNVS` for 85 MB would overflow `nt!HalpSaveNvsArea` during sleep/hibernate; `free_coherent()` / `GuDmaFree()` preserve allocations when `Gu.s3_persist_armed` is set (`wpr_meta` `0xdc642000` pointers: `radix_dma +0x10`, `boot_dma +0x20`, `signature_dma +0x48`; `8,743` host checks).

---

### 13.34 The v1.8.61 Contiguous Reserved Memory Arena & Clean DMA Unmapping (`DMA_live = 0`)

1. **Fixing `StartImage` Failure (`DMA_live = 20612`):** Splitting 20,604 single allocations fragmented UEFI Memory Map and left active DMA mappings blocking `bootmgfw.efi` (`PciBusDxe` UEFI Spec 13.4.2).
2. **Single-Descriptor 82 MB Arena (`GuS3AllocateArena()`):** Allocates 1 contiguous `21,000`-page `EfiReservedMemoryType` block (`< 4 GB`, +1 UEFI memory descriptor total), sub-allocates all GSP/radix entries linearly, and unmaps every `PciIo->Map()` handle in `GuDmaFree()` so `DMA_live = 0` strictly before OS handoff (`8,749` checks).

---

### 13.35 The v1.8.62 `EfiRuntimeServicesData` Telemetry Visibility & Dynamic Prior-Wake Scan

Switched `TelemetryPage` back to `EfiRuntimeServicesData` below `0xDADD9000` (so `kd.exe -kl` maps it via `SetVirtualAddressMap()` while keeping the 82 MB arena in `EfiReservedMemoryType`), plus added a 512 KB downward prior-wake scan (`0xDADD9000..0xDAB00000`) (`8,749` host checks).

---

### 13.36 The v1.8.63 Pre-Booter MMIO Diagnostic Snapshot

Added non-invasive 6-register pre-booter telemetry (`+0xF0..+0x104`: `WPR2_LO`, `WPR2_HI`, `0x00100ce0`, `0x00118234`, `0x001100f4`, `0x00110100`; `1,490` bytes trampoline, `8,755` host checks).

### 13.37 The v1.8.64 GSP FWSEC-FRTS WPR2 Partitioning in the S3 Trampoline

Added full 7-stage GSP (`0x00110000`) FWSEC-FRTS sequence ahead of SEC2 using persistent `Gu.s3_fwsec_dma` (`64 KB`, `EfiReservedMemoryType`, `DMA_live = 0`) + post-FWSEC telemetry (`+0x108..+0x118`; `1,954` bytes, `8,767` checks).

### 13.38 The v1.8.65 Consolidation of All 6 GPU DMA Buffers into the Arena

Consolidated all 6 DMA structures (`20,662` of `21,000` pages) into the single reserved descriptor (`DMA_live = 0`, zero ACPI-NVS image copies, untouched `Gu.s3_meta_cpu`/`Gu.s3_meta_dma`), adding Mailbox 1 (`+0x11C`) (`1,965` bytes, Test `[10]`, `8,778` checks).

### 13.39 The v1.8.66 Port-`0x80` GSP Halt Gate & `0x0A00` Envelope

Added port-`0x80`-paced `gsp_halt_poll` (`1,000,000` iterations on `0x00110100 & 0x10`), expanded code envelope to `0x0A00` (`2,560` bytes; data `0x0A00..0x0AEF`, `GDT/IDTR` `0x0B00..0x0B37`), and added `+0x120..+0x148` snapshots (`2,098` bytes, `8,791` checks).

### 13.40 The v1.8.67 Port-`0x80` SEC2 Poll, `[meta_dma]` Probe & Dual-S3 Verification

Added up to `6,000,000` port-`0x80` iterations on SEC2 Booter (`0x67b00001`), `100,000`-iter post-marker halt poll, and `[meta_dma]` probe (`+0x14C`, `2,140` bytes). Verified live across two consecutive S3 sleep/wake cycles on RTX 4090 (Windows 10, driver `621.14`: `290.51` TFLOPS FP16 / `291.99` TFLOPS BF16 `4096x4096`; `8,795` checks).

### 13.41 The v1.9.0 Universal-S3 Phase 1: Dynamic Bridges & VBIOS Geometry

Added dynamic upstream PCIe bridge path discovery (`GuDiscoverUpstreamBridgeChain()`, `GU_S3_MAX_BRIDGES = 4`, dynamic 16 MB memory windows, `GU_S3_RESUME_CONTEXT` `version = 2`, `216` bytes), dynamic Ada FWSEC/SEC2 VBIOS geometries (`2,281 / 2,560` bytes), pre-FWSEC GFW boot/VRAM gate, strict `< 0x000A0000` allocation, and S0ix/`XFirmwareWakingVector` preflight warnings (`9,782` checks incl. Test `[11]`).

### 13.42 The v1.9.1 `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` Path & Riot Vanguard Guide

Switched `GuDiscoverUpstreamBridgeChain()` to walk `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` as primary path (catching root ports without `PciIo` handles like AMI Aptio V `0:3:1`), corrected `EFI_PCI_IO_PROTOCOL_GUID` fallback, removed hardcoded `0:3:1` / bus-45 fallbacks in favor of clean `EFI_NOT_FOUND` abort (`9,798` checks), and documented verified Riot Vanguard usage via `[I]` + `bootmgfw.efi`.

### 13.43 The v1.9.2 Plan-C S3 Activation in `AdaUnlockDxe.efi` (`ReadyToBoot` + `ExitBootServices`)

Deferred Plan-C `ConfigurationTable` + `ExitBootServices` hook activation in `src/dxe_main.c` (`AdaUnlockDriverEntryPoint()`) from early BDS to `ReadyToBoot` via `AdaUnlockOnPlanCReadyToBoot()` (`GuAcpiInstallEbsHook()` + `GuAcpiApplyShadowConfigTable()`), keeping early BDS/SMM on `RealRSDP` (`test_dxe_main` passing).

### 13.44 The v1.9.3 `gBS->StartImage` Guard (`EfiLoaderCode`) & Log Append

Closed the gap observed in `RUN0099.TXT` (where AMI BDS reset `ConfigurationTable` back to `RealRSDP` between `ReadyToBoot` and `bootmgfw.efi`): `AdaUnlockInstallStartImageHook()` wraps `BootServices->StartImage` (recomputing `Hdr.CRC32`) to re-apply Plan C deterministically for `EfiLoaderCode` OS loaders (`bootmgfw.efi -> winload.efi` nested depth tracking) while preserving `RealRSDP` for early drivers, and `GuBootAppendLog()` records pointers into `RUNxxxx.TXT` (`test_dxe_main` passing).

### 13.45 The v1.9.4 Live ACPI Shadow Refresh `GuAcpiRefreshShadowChain()` Before OS Boot

Fixed the early-kernel BSOD in `v1.9.3` (`RUN0100.TXT`) caused by late AMI BDS updates to `RealXSDT` (`0xee -> 0x13`) and `RealFADT` (`0x77 -> 0x7a`) after `Driver####` ran: `GuAcpiRefreshShadowChain()` clones `ShadowFADT`, `ShadowXSDT`, and `ShadowRSDP` fresh from live post-BDS tables inside `StartImage(EfiLoaderCode)` and re-arms `RealFACS->FirmwareWakingVector`. Live-verified in `RUN0105.TXT` (`FirmwareCtrl=0xDC637400`, `Confirm-SecureBootUEFI: True`, *League of Legends* + Riot Vanguard across ~9 h S3 sleep, `247.43` TFLOPS post-wake).

### 13.46 The v1.9.5 / v1.9.6 Stickless `[I]` Multi-Volume, Recursive Directories & `1 MiB` Copy

`CollectInternalInstallCandidates()` (`src/driver_nvram.c`) selects internal FAT32 targets by actual free bytes via `EFI_FILE_SYSTEM_INFO` (`Primary ESP` -> `GPU_UNLOCK`/`UNLOCK_ESP` -> small FAT32 `<= 2 GiB`). `OpenOrCreateDirRecursive()` walks subdirectories step-by-step for UEFI `FatDxe` compatibility on fresh partitions, `CopyFileBetweenVolumes()` streams `1 MiB` chunks with partial rollback cleanup, and `tools/Create-Unlock-Partition.ps1 -Hidden` formats cleanly via `Format-Volume -Partition` with `NoDefaultDriveLetter = True`. Live-verified 100% USB-free on RTX 4090 (`RUN0110.TXT` on hidden `512 MB` `GPU_UNLOCK`: `226.64` TFLOPS stickless reboot, `234.19` TFLOPS / `768.22 GB/s` after stickless S3 sleep/wake, `Confirm-SecureBootUEFI: True`, Riot Vanguard `vgk` active).

### 13.47 The v2.0.0 Cockpit UX, Live `S3 ARMED` Status, Power-Limit Slider & Root Consolidation

1. **Live `S3 ARMED` Status Indicator (`tools/gu_system_info.py`, `tools/gpu_dashboard.py`, `tools/quick_check.py`):** Combines `powrprof.dll!GetPwrCapabilities()` (`SystemS3`), live ACPI FADT (`FirmwareCtrl` & `Flags` Bit 21 `S0ix`), and current-boot `RUN*.TXT` logs (`ARMED`, `S0IX_WARN`, `UNVERIFIED`, `NOT_ARMED`).
2. **Stepless 1-W Power-Limit Slider, UAC Elevation & S3-Wake Re-Apply:** `get_power_limit_info()` / `set_power_limit()` provide 1-W steps over `[min_w, max_w]` with `Eco (~78%)`/`Stock`/`Max` markers, direct `ShellExecuteExW` UAC elevation (`_pl_busy` thread guard), readback verification, optional S3-wake re-apply (`UniversalGpuUnlock_ReapplyPL` / `--reapply-pl`), and 1-click *Gaming/Quiet* + *AI/Compute* profiles.
3. **Baseline Comparison & Pre-Flight Audits:** Persistent local benchmark baseline delta readout (`vs. Baseline`); automatic **BitLocker (`C:`)**, **Secure Boot**, **ESP size**, and **ACPI S3 vs S0ix** pre-flight audits in `tools/Setup-AdaUnlock.ps1`.
4. **Root Cleanup & Archival:** Reduced root to `Setup.bat`, `Launch-Dashboard.bat`, and `Check-GPU-Unlock.bat`; moved helper utilities into `tools/`; archived `v1.5–v1.7.9` history into `docs/CHANGELOG_ARCHIVE.md`; added `docs/images/dashboard-v2.0.0.png`, `.gitattributes`, and `src/Build-Release-Zip.ps1`.

### 13.48 Hardening in `v2.0.0`: External-Load Guard (`refs #22`), FMLA Diagnostics & UEFI/S3/S4 Audit

1. **External-Load Detection & `FMLA32/FMLA16` Ratio (`tools/gu_system_info.py`, `tools/quick_check.py`, `tools/gpu_dashboard.py` — `refs #22`):**
   - `get_gpu_utilization()` checks `nvidia-smi` (`utilization.gpu`, `utilization.memory`) ahead of the benchmark. Under `> 15%` external GPU load, CLI and Dashboard warn explicitly instead of raising false `STOCK RATE-LIMITER ACTIVE` alarms (confirmed in `#22` on RTX 3060 Ti `10DE:2489` / MSI PRO Z790-A WIFI DDR4: idle `+104.4%` from `29.95` to `61.23 TFLOPS` FP16 / `61.97 TFLOPS` BF16; under `74%` Topaz Video AI load `BF16` remained unthrottled at `60.19 TFLOPS`).
   - `measure_fmla_ratio()` compares `FP16` with `allow_fp16_reduced_precision_reduction = True` vs `False` (`try...finally` restored). On Ampere (`sm_80`/`sm_86`), unlocked `FMLA32/FMLA16` sits near `~1.00x` vs `~0.50x` clamped; on Ada (`sm_89`, e.g. RTX 4090 `~0.56–0.63x`) and Turing (`sm_75`), `< 0.7x` under `False` reflects architecture behavior natively, handled properly by `classify_unlock_status()`.
2. **UEFI, OVMF & S4 Hibernate Audit (`src/gpu_discovery.c`, `src/unlock_release.c`, `src/s3_facs.c`, `src/s3_resume.c`):**
   - **OEM Zeroed Root Port Bus (`0xFFFF` Empty Slot):** `GuSlotRevive()` handles previously identified `0xFFFF` slots while `GuLogZeroedRootPortHint()` logs unintrusive read-only diagnostics on `discovered == 0` when OEM firmware zeroes bridge offset `0x18` (`sec=0/sub=0`) before discovery.
   - **Proxmox / OVMF (`virtio-fs`):** `GuReconnectDisplayAfterUnlock()` and `ConnectChildGopHandles()` (`src/unlock_release.c`) never blindly call `ConnectController` across `AllHandles`, targeting only isolated GPU/child/console handles.
   - **S4 Hibernate & Pre-Boot ReBAR Safety:** `src/s3_facs.c` and `src/s3_resume.c` enforce a pure SS0/SS1 unlock without mutating pre-boot ReBAR BAR1 registers (`0x610`/`0xBC0`) or forcing `ConnectController` during S3/S4 resume (preventing `Kernel-Power 41` hangs and TDR reboots).
3. **Final `v2.0.0` Hardware Acceptance (2026-11-25, Stickless `GPU_UNLOCK` + S3 + Riot Vanguard):**
   - Verified on stickless reboot (`USB_DRIVES_MOUNTED = 0`, `Confirm-SecureBootUEFI: True`, `FirmwareCtrl = 0xDC637400`, `vgk` Running + `vgtray`, **`224.52 TFLOPS`** `FP16`, `FMLA32/FMLA16 = 0.59x`, `S3 ARMED (ShadowFACS in FADT)`) and subsequent S3 sleep/wake (`vgk`, `vgtray`, `Riot Client`, `Confirm-SecureBootUEFI: True`, `ShadowFACS = 0xDC637400`, **`221.19 TFLOPS`** with live `16%` external-load notice, `4.12x` CUDA ratio, `FMLA32/FMLA16 = 0.64x`, `[OK] FULLY UNLOCKED!`).
4. **Desktop RTX 4060 Ti (`AD106`) Device IDs (`src/gpu_device_ids.h`, `refs #21`):**
   - Added desktop PCI IDs `0x2803` (8GB), `0x2805` (16GB), and `0x2807` alongside `0x2860` (Mobile) in `v2.0.0` (and temporary `test/v1.8.32-4060ti` patch). `#21` was closed as completed after 3 days without user response (external hardware `RUN*.TXT` / benchmark confirmation remains pending).

### 13.49 VeraCrypt DCS & Small EFI Chainloader PE32+ Validation (`v2.1.0`, `refs #23`)

- **Root Cause (`EFI_COMPROMISED_DATA` / `0x8000000000000021` at `stage-windows-image`):** Systems protected by VeraCrypt UEFI Full System Encryption replace `\EFI\Microsoft\Boot\bootmgfw.efi` with `DcsBoot.efi` (`~24,088` bytes) and back up the original Microsoft Boot Manager to `bootmgfw_ms.vc`. Previously, `ReadWindowsBinary()` (`src/boot_io.c`) passed a hardcoded `65536`-byte (`64 KiB`) floor to `ReadFileData()`, rejecting valid compact chainloaders before the boot menu opened.
- **Strict Structural Fix (`ValidateWindowsPeImage()`):** Lowered the minimum file bound in `ReadWindowsBinary()` to `4096` bytes (`4 KiB`) and replaced the `MZ`-only check with full bounds-checked little-endian PE32+ validation (`e_lfanew >= 0x40`, `"PE\0\0"`, `Machine == 0x8664`, `SizeOfOptionalHeader >= 72`, `Magic == 0x020B`, `Subsystem == 10` `EFI_APPLICATION`), keeping `bootmgfw.efi` (`DcsBoot.efi`) as the staged OS loader so VeraCrypt pre-boot authentication runs normally prior to `bootmgfw_ms.vc`. Verified via host unit tests in `tests/test_boot_select.c`.

### 13.50 Stickless `[I]` Early-BDS Console Preflight & `ReadyToBoot` Fallback Fix (`v2.1.0`, `refs #24`)

- **Root Cause (`STOP at stage=preflight status=0x8000000000000003` in `AdaUnlockDxe.efi` while USB `BOOTX64.EFI` worked):**
  1. During early `Driver####` dispatch before BDS finishes `ConSplitter` / USB keyboard connection (e.g. Gigabyte B760M D2H with Fast Boot), `t->ConIn`, `t->ConsoleInHandle`, or `t->StandardErrorHandle` can be `NULL` or distinct from `t->ConsoleOutHandle`. `GuUnlockPreflight()` (`src/unlock_release.c`) silently rejected non-unified handles or missing `ConIn` with `EFI_UNSUPPORTED` (`0x8000000000000003`) before any GPU/GOP diagnostic ran.
  2. Furthermore, the early-BDS failure left `ATTEMPT.FLG` on the ESP (`dxe_main.c` only cleared it after success/safeguard trigger, even when `mutated == 0`) and `GuBootClose()` kept `Table` non-`NULL` while zeroing `Root`/`Log`, causing `GuBootOpen()` to return `EFI_ALREADY_STARTED` with `Root == NULL` when the `ReadyToBoot` fallback fired (`GuBootLoadAssets()` failed silently with `EFI_NOT_READY`).
- **Fix & Tests (`src/unlock_release.c`, `src/dxe_main.c`, `src/boot_io.c`, `src/app.c`):**
  1. `GuUnlockPreflight()` and `Alive()` allow optional or split console handles (`ConsoleOutHandle`, `ConsoleInHandle`, `StandardErrorHandle`) while continuing to verify integrity of every populated handle/protocol, routing `InputGuid` checks via `ConsoleHost()`, explicitly logging all refusal reasons, and cleanly deferring if `ConsoleOutHandle` directly aliases `Plan.gop` without `ConSplitter`.
  2. Both `dxe_main.c` and `app.c` clear `ATTEMPT.FLG` immediately when preflight refuses without touching hardware (`!mutated && !started && !retained`); `GuBootOpen()` checks `if (Table && Root)` and `GuBootLoadAssets()` frees any stale asset buffer before reloading so `ReadyToBoot` retries cleanly in the same boot.
  3. Verified in `tests/test_multi_gpu_second_preflight.c` (no-`ConIn`, all-`NULL`, split handles, invalid `ConIn` vtable, GOP-alias deferral) and `tests/test_dxe_main.c` (pre-mutation refusal clearing `ATTEMPT.FLG` followed by `ReadyToBoot` Plan-C completion).
  4. **Hardware Verification (`@henry-richard7` in `#24`, closed `COMPLETED`):** Gigabyte GeForce RTX 3060 12GB (`10DE:2504`) on Gigabyte B760M D2H (Rev. 1.0, ReBAR `16384 MB` active, PCIe Gen4 x16) measured **24.2 ➔ 47.0 TFLOPS** (**+94.2%**) in USB mode (`BOOTX64.EFI`, `RUN0003.TXT`); Stickless `[I]` confirmed working with `test/v2.0.0-dxe-preflight` (`AdaUnlockDxe.efi` `RUN0001.TXT`: `Preflight console: non-unified handles (out=0x33340e98/... in=0/0 ...)`, `SS0=0x88888888`, `SS1=0x8`, Tier-2 `ShadowFACS` Plan C + `StartImage` `patched=2`).

### 13.51 Automatic UEFI Log Rotation (Max 50 Logs) & Hidden-Volume Log Access (`v2.1.0`)

- **UEFI Log Rotation (`src/boot_io.c`, `tests/test_boot_select.c`):** Replaced the `1..9999` linear `Open` loop inside `GuBootOpen()` with `OpenRotatedRunLog()` (`GU_MAX_RUN_LOGS = 50`). It scans `\GPU-UNLOCK` (`\AD102-UNLOCK`) via `dir->Read()`, prunes older `RUN*.TXT` files whenever 50 or more exist (multi-round batch pruning up to 64 files/round, preserving `ATTEMPT.FLG` and unrelated files), handles `9999 -> 1` wraparound cleanly via `RunLogOrderKey()`, and falls back to bounded linear probing only if `dir->Read()` is unavailable. This caps ESP usage at `< 1 MB` even on `100 MB` Turing ESPs and prevents boot slowdown after hundreds of boots. Verified via 4 mock filesystem rotation tests in `tests/test_boot_select.c`.
- **Hidden-Volume Log Discovery & 1-Click Export (`tools/gu_system_info.py`, `tools/gpu_dashboard.py`, `tools/Export-Boot-Log.ps1`, `Setup.bat [5]`):** Added Win32 `FindFirstVolumeW` / `GetVolumeInformationW` enumeration (`_enumerate_hidden_fat32_roots()`, 64-bit `wintypes.HANDLE` safe + `SEM_FAILCRITICALERRORS`) so both `gpu_dashboard.py` and `quick_check.py` read live `RUN*.TXT` files from unmounted `GPU_UNLOCK` / `UNLOCK_ESP` partitions without drive letters or UAC. Added **`Export Boot Log`** in the Dashboard and **`Setup.bat [5]`** (`tools/Export-Boot-Log.ps1`), copying the newest logs to `Boot-Logs\` (with UAC + `mountvol` cleanup fallback for ACL-protected Windows ESPs) and selecting the newest log in Windows Explorer.

### 13.52 Non-Identity `PciIo->Map()` S3 Arena Pointer Conversion (`v2.1.0`, `refs #25`)

- **Root Cause (`MSI MPG X670E Carbon WIFI` + RTX 4090 S3 Sleep Re-Lock despite `ShadowFACS` Armed):**
  - On UEFI platforms with `IoMmuDxe` / Pre-Boot DMA Protection enabled, `Gu.pci->Map(..., EfiPciIoOperationBusMasterCommonBuffer, ...)` remaps the `21,000`-page `<4 GB` `EfiReservedMemoryType` S3 arena (`Gu.s3_arena_phys`, e.g., `0x8c0f8000`) to temporary DXE IOVAs (e.g., `0x00204000..0x052bc000`).
  - While cold-boot unlock succeeds using those active IOVAs, `free_coherent()` unmaps every DMA mapping via `Gu.pci->Unmap()` before Windows handoff (`DMA_live = 0`). After `Unmap()` and S3 sleep/wake, the unmapped IOVAs stored in `Gu.s3_*_dma`, `Gu.s3_meta_cpu`, and every entry of the 3-level GSP radix page table no longer match physical DRAM addresses.
  - Additionally, `GuDmaAllocate()` (`src/port.c`) logged `WARNING: PciIo->Map non-identity mapping` on every arena allocation (`20,612` repetitive log lines).
- **Fix & Verification (`src/port.h`, `src/port.c`, `src/vendor/unlock_610.c`, `tests/test_s3_non_identity.c`):**
  1. Added `s3_arena_non_identity` and `s3_arena_non_identity_count` in `GU_PLATFORM`, logging the non-identity warning once per unlock transaction instead of `20,612` times.
  2. Extracted shared helpers `pack_radix_tables(work, use_cpu_phys)` and `pack_wpr_meta(work, dest, use_cpu_phys)` in `src/vendor/unlock_610.c` and preserved the allocation-time IOVA in `work->s3_meta_dma` so `free_coherent()` continues unmapping all `GU_DMA` handles cleanly.
  3. Added `s3_repack_arena_physical(work)` right after FLR quiescence (`work->quiesced`) and before `unlock_transaction_unwind(work)`: when `Gu.s3_arena_non_identity` is set, it validates every `<4 GB` `.cpu` buffer pointer (`<= 0xFFFFFFFF`), repacks all radix table pages (`work->pages[0..table_pages-1]`), updates `Gu.s3_meta_cpu` / `Gu.wpr_meta`, and converts `Gu.s3_fwsec_dma`, `Gu.s3_booter_dma`, `Gu.s3_v67_dma`, `Gu.s3_boot_dma`, `Gu.s3_meta_dma`, and `Gu.s3_radix_dma` to CPU-physical DRAM addresses.
  4. Verified cleanly in host unit suite `tests/test_s3_non_identity.c` (covering identity `dma == cpu`, synthetic IOMMU non-identity `0x204000..`, `>4 GB` bounds rejection `-ERANGE`, 3-level radix repacking, WPR metadata repack, single-warning deduplication, and `free_coherent()` unmap parity).
  5. **Hardware Verification (`@Lizardon1024` in `#25`, closed `COMPLETED`):** Confirmed on MSI MPG X670E Carbon WIFI (Ryzen 7 7800X3D iGPU, RTX 4090 `10DE:2684`, `255.0 TFLOPS`) with `test/v2.0.0-x670e-s3`: `[P]` unlock now survives ACPI S3 sleep/wake cleanly without needing to disable BIOS `IOMMU` (disabling BIOS `IOMMU` / `Pre-Boot DMA Protection` remains an optional fallback on boards whose PEI firmware also blocks physical wake DMA).

### 13.53 Ada (RTX 4090) Efficiency Sweetspot — Clock-Capping vs. Power Limiting

Controlled `25`-run measurements on an unlocked RTX 4090 (MiniMax-H3 Video DiT, `1184×896`, `INT8` UNet & Video VAE, `975 mV` undervolt curve, `+600 MHz` memory OC unless stated) explain why Ada workstation silicon such as the RTX 6000 Ada runs near `300 W` and why a fixed SM clock cap (`nvidia-smi -lgc`) outperforms raw power-limit throttling (`nvidia-smi -pl`):

1. **V/F Efficiency Cliff Above `~2,445 MHz` (`5.0s` / `125`-Frame & `10.1s` / `242`-Frame Sweeps):**
   - Above `~2,450 MHz`, dense AI sampling gains only `+1.4–3.1%` runtime (`396.35 s` at `2,450 MHz` vs. `384.38 s` at `450 W` full boost) while demanding `+40–60 W` extra board power (`370.8 W` vs. `411.4 W` overall average).
   - Pure `-pl` throttling (`315 W` / `70%`) keeps tripping `SW Power Cap`, causing the SM clock to oscillate between `1,965` and `2,775 MHz` (`2,108 MHz` average on `10.1s` renders, `462.48 s`, `39.44 Wh`). Below `65%` PL (`270 W`), total Wh/video rises again (`12.91 Wh` vs. `12.73 Wh` at `293 W` on `5.0s` renders) because VRAM and system idle overhead stay active for `+15 s` longer.
   - Locking SM clocks via **`nvidia-smi -lgc 210,2300`** with open `450 W` PL eliminates clock oscillation (`2,295 MHz` steady), completing the `242`-frame render **34.6 seconds faster** (`427.89 s` vs. `462.48 s`) while consuming **less total energy** (**`38.74 Wh`** vs. `39.44 Wh`, `-11.8%` vs. `450 W`). **`nvidia-smi -lgc 210,2450`** finishes in `396.35 s` (`+3.1%` vs. `450 W`, `66 s` faster than `70% PL`, `40.89 Wh` / `-6.9%` energy).
2. **CUDA `P2` State Memory OC & GDDR6X ECC Behavior:**
   - NVIDIA's driver ignores `-lmc` in CUDA `P2` compute state on GeForce cards (`10,851 MHz` remained locked across CLI attempts).
   - Over `+0` to `+900 MHz` memory offsets (Afterburner), **`+600` to `+700 MHz`** (`10,851–10,951 MHz` in `P2`) represents the clear plateau: `~3.5–4.5%` faster pure DiT sampling (`118.88–120.46 s` vs. `124.28 s` at `+0 MHz`) at unchanged energy, while **`+800` and `+900 MHz`** deliver zero extra speedup due to GDDR6X timing/ECC overhead.
3. **Gaming Coexistence:** Always reset with **`nvidia-smi -rgc`** after compute runs so games can use the full boost table normally.
4. **Cockpit & CLI Integration (`tools/gu_system_info.py`, `tools/gpu_dashboard.py`, `tools/quick_check.py`):**
   - `get_clock_cap_info()` queries `nvidia-smi -q -d SUPPORTED_CLOCKS`, live SM frequency, and `clocks_event_reasons.sw_power_cap`, computing architecture-aware snaps via `resolve_clock_cap_presets()` (snapped to discrete `15 MHz` bins, e.g., `2295` / `2445 MHz` on RTX 4090 vs. scaled `78%`/`85%` on Ampere/Turing).
   - `gpu_dashboard.py` provides a dedicated **SM CLOCK CAP (`-lgc`)** bar with `Max Eff`, `Sweetspot`, `Apply Cap`, and `Reset (-rgc)` plus 1-click **AI Efficiency** (open PL + `-lgc` in one UAC call) and **Gaming / Stock** (`Stock PL` + `-rgc`) presets persisted cleanly across S3 wake (`--reapply-pl`), alongside the trilingual `EN` / `简体中文` / `DE` switcher (`tools/dashboard_i18n.py`). Verified via mocked unit suite `tests/test_clock_cap.py`.

### 13.54 Ada Laptop Device IDs, PLM/Rate Log Decoding & Hardware Boundaries (`v2.1.0`)

1. **Ada Mobile PCI Device IDs & Profile Calibration (`src/gpu_device_ids.h`, `tools/gu_system_info.py`, `tests/test_turing_local.c`):**
   - Verified official consumer Ada Mobile IDs against `NVIDIA/open-gpu-kernel-modules` (`g_nv_name_released.h`) and added **`0x2717U` / `0x2757U`** (RTX 4090 Laptop, `AD103`), **`0x27a0U` / `0x27e0U`** (RTX 4080 Laptop, `AD104`), **`0x2820U`** (RTX 4070 Laptop, `AD106`), and **`0x28e0U`** (RTX 4060 Laptop, `AD107`), while correcting the `0x2860U` comment from `RTX 4060 Ti Mobile` to **`RTX 4070 Laptop` (`AD106`)**. All consumer Ada dies share the `AD102-ASSETS` transaction pipeline and `ChipMatchesTarget()` (`arch == 0x09U`) check.
   - Added laptop-specific entries (`"4090 laptop"`, `"4080 laptop"`, `"4070 laptop"`, `"4060 laptop"`) ahead of desktop keys in `CARD_PROFILES` (`tools/gu_system_info.py`) so lower-TGP mobile GPUs are not evaluated against desktop AD102/AD103 TFLOPS thresholds.
2. **PLM (`0x823804`) & Speed-Selector (`SS0` `0x82381c` / `SS1` `0x823820`) Plaintext Decoding (`src/diag_decode.h`, `src/profile.c`, `src/unlock_release.c`, `src/s3_facs.c`, `src/vendor/unlock_610.c`):**
   - Added header-only `GuPlmDecode()` (`bits[3:0]` = `L0..L3` read, `bits[7:4]` = `L0..L3` write: `0xffffff8f` `STOCK-LOCK(R:L0-3 W:L3)`, `0xffffffef` `RELOCK-L0(R:L0-3 W:L1,L2,L3)`, `0xffffffcf` `DEEP-LOCK(R:L0-3 W:L2,L3)`, `0xffffffff` `OPEN(R:L0-3 W:L0-3)`) and `GuRateSelectorDecode()` (`bit3` = override enable, `bits[2:0]` = `0:1x`, `1:1/2`, `2:1/4`, `3:1/8`, `0x0` = fuse default `fuse-def`, `0x8` = active `1x`).
   - Surfaced decoded diagnostics inline in `RUNxxxx.TXT` logs (cold entry `Selectors:`, `Hardware decode:`, `AD102 permission ...`, and `S3 Wake Hardware Unlock Telemetry`) and added `decode_plm_mask()` / `plm_summary` inspection to `tools/gu_system_info.py` + `tools/quick_check.py`.
3. **Pre-`StartImage` & Hardware Boundary Audit (`src/boot_io.c`, `src/dxe_main.c`, `src/unlock_release.c`):**
   - Confirmed `GuBootWindows()` (`src/boot_io.c`) and `AdaUnlockHookedStartImage()` (`src/dxe_main.c`) perform zero BAR0 scratch/canary writes prior to OS handoff `StartImage()` (avoiding post-`StartImage` hangs reported elsewhere on `28e0` laptops), mutating only in-RAM ACPI shadow pointers when `Gu.s3_registered` is active.
   - Kept `GTX 16xx` (`TU116` / `TU117`) excluded (no Tensor Cores + Windows `nvlddmkm` `RmInitAdapter` Code 43) and documented `USB4`/`Thunderbolt` eGPU (`ASM2464PD`) pre-EBS / Connection-Manager reset boundaries alongside `Q4` LLM decode bandwidth limits across `README.md` and `DOCUMENTATION.md`.

### 13.55 Scaled Trilingual GOP UEFI Console & Live Pre-Menu CSM Probe (`v2.1.0`)

1. **Unified 2x–3x Scrolling GOP Terminal (`src/gop_menu.h`, `src/boot_io.c`, `src/app.c`):**
   - `GuBootEnableLargeConsole()` (invoked exclusively by `BOOTX64.EFI` in `UefiMain()`, leaving `AdaUnlockDxe.efi` silent POST completely untouched) backs `GuBootSay()` and `GuBootChoose()` with a scrolling `76x20` GOP console (`GuGopCon`): `2x` (`16x32` px narrow / `32x32` px wide) on `1280x680`–`3199x1599` displays (HD/1080p/1440p) and `3x` (`24x48` px narrow / `48x48` px wide) on `>= 3200x1400` (4K) panels over a calm pure-black terminal palette (`~22 KiB` static `.bss`, zero heap allocations).
   - Both the `[U/P/I/R/S]` boot selection menu and all follow-up status screens (`[1/4]`–`[4/4]` unlock progress, DXE NVRAM install/removal, safeguard, and halt countdowns) share the same readable scale. `GuBootSilence()` detaches the pre-unlock `GOP` pointer prior to `DisconnectController` / FLR, and `GuBootUnsilence()` re-queries `ConsoleOutHandle` GOP to redraw preserved lines seamlessly; displays below `1280x680` (including Legacy CSM `1024x768` fallback GOP) use standard `ConOut->OutputString`.
2. **Trilingual Boot Menu (`EN` / `简体中文` / `DE`) & Preset Sync (`src/uefi_lang.h`, `tools/Deploy-UsbCommon.ps1`, `tools/gu_system_info.py`, `tools/dashboard_i18n.py`):**
   - Pressing **`[L]`** inside `GuBootChoose()` cycles `MenuLang` (`EN` ➔ `ZH` ➔ `DE` ➔ `EN`), immediately redraws the selection menu, and keeps the chosen language across follow-up `[1/4]`–`[4/4]` status, install/remove, safeguard, and halt screens.
   - `GuFont8x16German` provides crisp `8x16` German umlauts/eszett (`Ä`, `Ö`, `Ü`, `ß`, `ä`, `ö`, `ü`) and `GuFont16x16Wide` embeds curated sorted GNU Unifont `18.0.01` `16x16` (`GU_GOP_WIDE_TRAIL` 2-cell tracking with automatic column-75 wrap guard). When `!GopCon.gop` (`<1280x680` / serial ConOut), `GuBootActiveLang()` enforces pure English ASCII so BIOS ROMs without CJK fonts never render `□`. `RUNxxxx.TXT` logs via `GuLog()` always remain pure English ASCII.
   - `GuReadLangConfig()` reads `EFI\BOOT\lang.cfg` (`en` / `zh` / `de`) at boot menu entry; USB deployment (`tools/Deploy-UsbCommon.ps1`) and the Cockpit Dashboard (`tools/dashboard_i18n.py` ➔ `sync_boot_lang_cfg()`) keep `lang.cfg` synchronized across attached USB drives and hidden `GPU_UNLOCK` volumes. Verified in `tests/test_boot_select.c` and `tests/test_clock_cap.py`.
3. **Read-Only Pre-Menu BIOS CSM Probe (`src/unlock_release.c`, `src/unlock_release.h`, `tests/test_csm_handover.c`):**
   - Right before `GuBootChoose()`, `UefiMain()` calls `GuProbePreMenuCsmStatus(t)` which inspects `EFI_LEGACY_BIOS_PROTOCOL` (`0xdb9a1e3d-...`), target GPU PCI I/O decode attributes (`EfiPciIoAttributeOperationGet`), and bound `EFI_DRIVER_BINDING_PROTOCOL` / `EFI_LOADED_IMAGE_PROTOCOL` image metrics via `GuDetectCsmVideoDriver()` without mutating `Plan`.
   - Displays `[OK] BIOS CSM: OFF` (green), `[!] BIOS CSM: ENABLED` (amber), or `[!] LEGACY CSM VIDEO ACTIVE` (red, Code 43 warning) directly inside the boot menu in all three languages, covered by `TestProbePreMenuCsmStatus()` in `tests/test_csm_handover.c`.
