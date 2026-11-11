# Operational Architecture, Persistence & Secure Boot Telemetry

This document details the real-world operational findings gathered across **44 supervised hardware execution cycles** (`RUN0001` through `RUN0044`) on one reference GeForce RTX 4090 test bench, alongside guidance for NVRAM persistence and Secure Boot. Results below are observations from that bench and the linked issues, not guarantees for other systems.

---

## 1. Testbench Telemetry: 44 Supervised Execution Cycles

Across multiple days of continuous hardware validation, the dynamic UEFI unlock runtime has undergone 44 monitored boot cycles under diverse hardware cold states, memory training events, and firmware configurations:

| Telemetry Range | Architecture Stage | Status | Key Milestones & Hardware Findings |
| :--- | :--- | :--- | :--- |
| **RUN0001 – RUN0013** | Initial Pre-Flight Prototyping | Isolated | Refined strict UEFI handle database bounds and relaxed single-child GOP assertions. |
| **RUN0014 – RUN0026** | GOP Detach & Claim Engine | Verified | Perfected multi-stage console display decoupling without tripping host bus agents. |
| **RUN0027 – RUN0033** | Dynamic Selector Discovery | All 7 passed | Automated detection of varying cold-state selectors across cold boots. |
| **RUN0034 – RUN0044** | DXE NVRAM Autopilot & Production | All 11 passed | Autonomous execution completed under both `SS0=0x0, SS1=0x0` and `SS0=0x24145006, SS1=0x3` cold states. |

### Canonical Hardware Execution Trace (from `RUN0044`):
```text
DXE-DRIVER: Ada Lovelace GPU detected: Device ID 0x2684
GuAd104Prepare: start
Registers: cold state verified (PMC=0x192000a1, SS0=0x0, SS1=0)
FWSEC descriptor: offset=0x43e3c ROM_bytes=1048576 header=0x32c0301
FWSEC geometry: size=0x10000 engine=0x400 signatures=2 versions=0x3 bytes=768
MSI driver detach return: stage=1 status=0, stage=2 status=0, stage=3 status=0
MSI exclusive claim/decode result: status=0
asset request: gpu-unlock/610.43.03/gsp_ga10x.bin bytes=84277400 error=0
Falcon start: base=0x840000 cpuctl=0x10 code_va=0x100 sig_dmem=0x10 engine_mask=0x1 ucode=3
AD102 payload result: boot=0 halt=0 marker=0x67b00001 expected=0x67b00001 marker_ok=1
selector write: SS0=0x88888888 -> matched=1
selector write: SS1=0x8        -> matched=1
AD102 selectors-applied: readout1=0 fields=0 0 0 0 0 0 0 0 0 valid=1 full=1
PCIe FLR returned: result=0
post-FLR SEC2 reset released, GSP halted ... SS0=88888888 SS1=00000008
DXE-DRIVER: TRANSACTION OK: hardware readbacks passed!
DXE-DRIVER: Succeeded! Returning EFI_SUCCESS to DXE Dispatcher.
```

Observed in these runs on the RTX 4090 bench:
- **VBIOS untouched:** The unlock state was gone after every full power-off, and the card booted stock afterwards. The repository has no automated test for this.
- **Readout check:** `readout1=0 (valid=1 full=1)` was logged after the selector writes. The runtime treats this value as the rate limiter being disengaged.
- **Benchmarks:** FP32 shader throughput and GDDR6X memory clocks stayed at stock, and the FP16 tensor GEMM reached **>212 TFLOPS**. Other cards are listed with their own results in the README.

---

## 2. The Two Operational Modes

### Mode A: Portable USB One-Shot (Session-Based)
*Best for users who want to selectively unlock compute for heavy AI rendering or research sessions without touching their motherboard's permanent boot configuration.*
1. Provision a FAT32 USB flash drive with `Setup.bat`.
2. Reboot, press `F8` / `F11` / `F12` to open the UEFI Boot Menu, and boot from the USB drive.
3. Press **`[U]`** for Standard Unlock. The runtime executes in ~300 ms, displays the 4-stage progress dashboard, and immediately hands control over to Windows Boot Manager (`bootmgfw.efi`).
4. **Behavior:** The GPU remains unlocked for the entire duration of your Windows session. Once you shut down, the volatile RAM state resets, and the system boots stock on the next boot without the USB drive.

### Mode B: NVRAM Autopilot (UEFI Driver)
*For systems that should unlock on every cold boot without opening the boot menu.*
1. Boot from the USB drive and press **`[I]` (Install DXE Driver to NVRAM)**.
2. **Internal ESP install:** The runtime looks for the internal Windows ESP, a non-USB volume with `\EFI\Microsoft\Boot\bootmgfw.efi`. If it finds one, it copies `AdaUnlockDxe.efi` to `\EFI\Drivers\` on it, copies the firmware assets of the detected GPU architecture, and registers a `Driver####` entry that points to the ESP. The USB drive can then be removed.
3. **USB fallback:** If no internal ESP is found or the copy fails, the entry points to `\EFI\Drivers\AdaUnlockDxe.efi` on the USB drive, which then has to stay connected. An older copy on the ESP is deleted before the new one is written, and an incomplete write counts as a failed copy.
4. **Execution Model:** On every power-on, the firmware runs the driver before Windows Boot Manager starts.
5. **On failure:** If the unlock fails after the GPU was touched, the driver first takes its own entry out of `DriverOrder` and then shuts the PC down, because only a power-off guarantees the GPU is no longer using system memory. The next boot runs without the unlock, for both the internal ESP install and the USB fallback. The entry and its files stay in place. To enable the driver again, boot the USB drive and press **`[I]`**. Press **`[R]`** to remove it. If the driver cannot change `DriverOrder`, the message says so; removing the USB drive then only helps with the USB fallback.
6. **NVRAM safety:** `[I]` uses the first free `Driver####` slot or reuses its own entry. `[R]` removes only entries whose description and file path belong to AdaUnlock, reports when none exist, and deletes `\EFI\Drivers\AdaUnlockDxe.efi` from the internal ESP. The copied firmware assets stay on the ESP. If `DriverOrder` cannot be read, both options stop without changing NVRAM, and `[I]` copies nothing to the ESP.

### Boot Behavior
- **Windows selection:** A volume with `bootmgfw.efi` always beats a generic `bootx64.efi`. If several qualify, the first active matching entry in the firmware's `BootOrder` wins. USB volumes are never selected.
- **Multiple GPUs:** Only the first supported GPU is processed, and a warning is shown. If it does not own an active GOP, the run stops before anything is changed.
- **Display:** Only a GOP on the target GPU is detached, so a monitor on the integrated GPU keeps working.
- **VBIOS:** The check only confirms that the ROM is readable and plausible. It does not compare against a per-card hash.
- **Errors:** Messages stay enabled after the unlock starts. Once the NVIDIA display driver is detached, they are only visible on another display, so check the `RUNxxxx.TXT` log on the USB drive.
- **Verification status from the issues:** Ampere is confirmed on an RTX 3060 ([#11](https://github.com/BrightRaider/universal-gpu-unlock/issues/11)), RTX 3080 Ti ([#12](https://github.com/BrightRaider/universal-gpu-unlock/issues/12), [#16](https://github.com/BrightRaider/universal-gpu-unlock/issues/16)) and RTX 3090 ([#13](https://github.com/BrightRaider/universal-gpu-unlock/issues/13)). An RTX 3080 that stops at `stage=discover` is still open ([#17](https://github.com/BrightRaider/universal-gpu-unlock/issues/17)). On Turing, the transaction completes on an RTX 2080 Ti, but the throughput gain is not confirmed yet ([#15](https://github.com/BrightRaider/universal-gpu-unlock/issues/15)).

---

## 3. Secure Boot & Competitive Anti-Cheat Coexistence

A frequent concern among enthusiasts is whether unlocking the GPU compromises UEFI Secure Boot or triggers competitive kernel anti-cheat engines (such as **Riot Vanguard** for *Valorant*, **Easy Anti-Cheat**, **BattlEye**, or **Ricochet**).

**The short answer:** If you enroll the image or certificate in `db` (see below), Secure Boot stays enabled. Whether a specific anti-cheat accepts the system is decided by its vendor, and this was not tested systematically.

### What Anti-Cheat Engines Commonly Check (vendors do not publish their full criteria):
Kernel-level anti-cheat drivers query the Windows Security Subsystem (specifically `HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State` and UEFI runtime variables):
- **Criterion 1:** Is `UEFISecureBootEnabled == 1`?
- **Criterion 2:** Is the TPM 2.0 PCR-7 measurement chain intact and Windows hypervisor-enforced code integrity (HVCI) operational?

Anti-cheat engines do **not** forbid users from owning and administering their motherboard's authorized signature database (`db`). The UEFI specification explicitly provides the `db` variable so hardware vendors and users can authorize proprietary hardware option ROMs, storage controllers, and custom drivers.

### Authorizing the Driver While Keeping Secure Boot Active:
You do **not** need to disable Secure Boot:
1. **Method 1 (Enroll Hash / Image):** In your BIOS Secure Boot menu, select **"Enroll EFI Image"** (or *"Select an UEFI file as trusted for executing"*), browse to `BOOTX64.EFI` / `AdaUnlockDxe.efi`, and confirm. The BIOS computes its SHA-256 hash and appends it to `db`.
2. **Method 2 (Enroll Certificate):** In BIOS Key Management, append `AdaUnlock_SecureBoot.cer` to the `db` database.
3. **Outcome:** In Windows, `Confirm-SecureBootUEFI` and `UEFISecureBootEnabled` report `1` (True). Behavior of individual anti-cheat engines and games was not tested systematically.

---

## 4. Power Management & Cold-Boot Best Practices

Because the GPU unlock is strictly **volatile (RAM-only)**, internal GPU register states interact directly with Windows ACPI power states:

### 1. Windows Fast Startup ("Hiberboot" / Hybrid S4)
By default, Windows 10/11 "Shut Down" does not actually power off the system cleanly; it saves the Windows kernel session to disk in a hybrid hibernation state (`hiberfil.sys`). Upon powering back on, the motherboard may skip early DXE driver dispatching to reduce POST time, or the GPU may not enter a true cold state.
* **Best Practice:** Disable Windows Fast Startup to ensure every power cycle performs a genuine hardware cold POST:
  ```powershell
  # Run in elevated Administrator PowerShell or Command Prompt:
  powercfg /h off
  ```
  *(Alternatively: Control Panel -> Power Options -> Choose what the power buttons do -> Uncheck "Turn on fast startup").*

### 2. Standby / Sleep (ACPI S3 / Modern Standby)
Entering S3 Sleep cuts power to the GPU execution clusters. Upon waking, `nvlddmkm.sys` resets the graphics card from the physical onboard VBIOS, restoring factory rate-limiters.
* **Best Practice:** Use the included **`Toggle-Sleep-Button.bat`** tool in the repository root to conveniently hide the "Sleep" option from your Windows Start menu, preventing accidental loss of the unlocked volatile state during the day.
