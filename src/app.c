/* SPDX-License-Identifier: GPL-2.0-only */

#include "port.h"
#include "version.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "unlock_release.h"
#include "boot_io.h"
#include "compat/compat.h"
#include "driver_nvram.h"
#include "multi_gpu.h"

static EFI_STATUS GuRebootSystem(EFI_SYSTEM_TABLE *t, EFI_STATUS s) {
  (VOID) GuBootClose();
  if (t->BootServices->Stall)
    t->BootServices->Stall(3000000);
  if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {
    t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
  }
  return s;
}

/* Common exit for every failed stage. After the GPU was touched it never
 * returns to the firmware: it reconnects the display if that is safe and halts. */
static EFI_STATUS RunFailure(EFI_SYSTEM_TABLE *t, EFI_HANDLE image,
                             const GU_DISCOVERED_DEVICE *gpu, const char *stage,
                             EFI_STATUS s) {
  GuBootUnsilence();

  if (s == EFI_SUCCESS)
    s = EFI_DEVICE_ERROR;

  GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\n", stage,
        (unsigned long long)s, GuUnlockMutated(), Gu.retained, Gu.fault);

  if (GuUnlockMutated() || Gu.started || Gu.retained) {

    if (Gu.started || Gu.master_attributes_pending)
      (VOID) GuClearMaster();

    (VOID) GuBootSyncLog();

    if (gpu && gpu->handle && !Gu.retained) {
      if (!GuUnlockIsCsmVideo() || Gu.flrs == 1) {
        (VOID) GuReconnectDisplayAfterUnlock(t, image, gpu->handle, FALSE);
      }
    }

    GuReleaseStagedVbiosEfiImage();

    (VOID) GuBootClose();
    GuBootHalt();
    return EFI_ABORTED;
  }

  GuReleaseStagedVbiosEfiImage();

  GuBootSay(L"\r\n=======================================================\r\n"
            L" UNIVERSAL GPU UNLOCK - PREFLIGHT REFUSED\r\n"
            L"=======================================================\r\n"
            L" Preflight check refused unlock. Hardware remains stock.\r\n"
            L" Diagnostic log saved to USB: ");
  GuBootSay(GuBootGetLogPath());
  GuBootSay(L"\r\n Please report this log on GitHub if unexpected:\r\n"
            L" https://github.com/BrightRaider/universal-gpu-unlock/issues\r\n\r\n"
            L" Booting Windows in stock mode in 5 seconds...\r\n");

  (VOID) GuBootClose();

  if (t && t->BootServices && t->BootServices->Stall)
    t->BootServices->Stall(5000000);

  return GuBootWindows();
}

/* [I]: register the DXE driver, say where it went, reboot. */
static EFI_STATUS RunInstall(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
  GuLog("Operator I: Requesting DXE Driver NVRAM installation\n");

  EFI_STATUS s = GuInstallDriverOption(image, t);

  if (s == EFI_SUCCESS && GuDriverInstalledOnEsp()) {
    GuLog("DXE Driver installed to the internal ESP (Driver#### / DriverOrder)\n");
    GuBootSay(L"\r\n\r\n[OK] AdaUnlock DXE Driver installed to the internal ESP and UEFI NVRAM.\r\n"
              L"It runs during UEFI POST on every cold boot. The USB drive can be removed.\r\n"
              L"Rebooting system in 3 seconds...\r\n");
  } else if (s == EFI_SUCCESS) {
    GuLog("DXE Driver registered on the USB drive (Driver#### / DriverOrder)\n");
    GuBootSay(L"\r\n\r\n[OK] AdaUnlock DXE Driver registered in UEFI NVRAM, on this USB drive.\r\n"
              L"[!] NOTE: The internal ESP was unavailable or had insufficient space for assets (~85MB).\r\n"
              L"Keep the USB drive connected (or use [P] S3 Sleep / create a 500MB FAT32 partition).\r\n"
              L"Rebooting system in 3 seconds...\r\n");
  } else {
    GuLog("ERROR: Failed to install DXE Driver: %#llx\n", (unsigned long long)s);
    GuBootSay(L"\r\n\r\n[ERROR] Failed to install DXE Driver to NVRAM! "
              L"Nothing was registered.\r\n"
              L"Check that EFI\\Drivers\\AdaUnlockDxe.efi exists on this USB "
              L"drive.\r\n"
              L"Rebooting in 5 seconds...\r\n");
    if (t->BootServices->Stall)
      t->BootServices->Stall(2000000);
  }

  return GuRebootSystem(t, s);
}

/* [R]: remove the DXE driver entry and its ESP copy, reboot. */
static EFI_STATUS RunRemove(EFI_SYSTEM_TABLE *t) {
  GuLog("Operator R: Requesting DXE Driver NVRAM removal\n");

  EFI_STATUS s = GuRemoveDriverOption(t);

  GuLog("DXE Driver removal from Driver#### / DriverOrder: status=%#llx\n",
        (unsigned long long)s);

  GuBootSay(
      s == EFI_SUCCESS
          ? L"\r\n\r\n[OK] AdaUnlock DXE Driver removed from UEFI NVRAM & Internal ESP.\r\n"
      : s == EFI_NOT_FOUND ? L"\r\n\r\n[INFO] No AdaUnlock DXE Driver entry "
                             L"found. Nothing was changed.\r\n"
                           : L"\r\n\r\n[ERROR] Could not remove the "
                             L"AdaUnlock DXE Driver. See the USB log.\r\n");

  GuBootSay(L"Rebooting system in 3 seconds...\r\n");

  return GuRebootSystem(t, s);
}

/* [S] or timeout: log a read-only diagnostic scan, then boot Windows unchanged.
 * The scan never touches the GPU state, so a read fault it latches must not
 * block the stock boot. */
static EFI_STATUS RunStock(VOID) {
  GuLog("Operator skipped: no GPU transaction, no driver release; stock "
        "Windows\n");

  BOOLEAN fault_before_scan = Gu.fault;
  (VOID)GuDiagnosticScan();
  if (Gu.fault && !fault_before_scan)
    GuLog("DiagnosticScan: read fault ignored for the stock boot\n");

  if (fault_before_scan || GuBootClose() != EFI_SUCCESS)
    return EFI_DEVICE_ERROR;

  return GuBootWindows();
}

/* [U] or [P]: run the unlock on every supported GPU and hand over to Windows. */
static EFI_STATUS RunUnlock(EFI_SYSTEM_TABLE *t, EFI_HANDLE image,
                            const GU_DISCOVERED_DEVICE *gpu, UINTN device_count,
                            BOOLEAN enable_s3) {
  if (enable_s3) {
    Gu.s3_persist_armed = TRUE;
  }
  const char *stage = "assets";
  EFI_STATUS s = GuBootLoadAssets();
  if (s != EFI_SUCCESS || Gu.fault)
    return RunFailure(t, image, gpu, stage, s);

  stage = "read-only-cold-profile";
  s = GuPrepare();
  if (s != EFI_SUCCESS || Gu.fault)
    return RunFailure(t, image, gpu, stage, s);

  if (Gu.already_unlocked) {
    GuBootSay(
        L"\r\n=======================================================\r\n"
        L" UNIVERSAL GPU UNLOCK " GU_VERSION_TAG_WIDE L" - Status Monitor\r\n"
        L"=======================================================\r\n"
        L" [1/4] GPU and PCIe Topology Discovery ..... [ OK ]\r\n"
        L" [2/4] Firmware Hashes and VBIOS Parse ..... [ OK ]\r\n"
        L" [3/4] Hardware Silicon Status ............. [ ALREADY UNLOCKED ]\r\n");
    if (enable_s3) {
      GuBootSay(L" [4/4] S3 Sleep Persistence ................ [ REGISTERING ]\r\n");
    } else {
      GuBootSay(L" [4/4] S3 Sleep Persistence ................ [ SKIPPED ]\r\n");
    }
  } else {
    GuBootSay(
        L"\r\n=======================================================\r\n"
        L" UNIVERSAL GPU UNLOCK " GU_VERSION_TAG_WIDE L" - Status Monitor\r\n"
        L"=======================================================\r\n"
        L" [1/4] GPU and PCIe Topology Discovery ..... [ OK ]\r\n"
        L" [2/4] Firmware Hashes and VBIOS Parse ..... [ OK ]\r\n"
        L" [3/4] Display Handover .................... [ PENDING ]\r\n"
        L" [4/4] Hardware SEC2 and MSR Unlock ........ [ PENDING ]\r\n\r\n"
        L" --> Starting GPU unlock...\r\n");
  }

  GuBootSilence();

  (VOID) GuSetSafeguardFlag();

  UINTN unlocked_count = 0, failed = 0;
  UINT32 options = (Gu.already_unlocked ? 0U : GU_UNLOCK_VERIFY_WINDOWS) | (enable_s3 ? GU_UNLOCK_REGISTER_S3 : 0U);
  s = GuUnlockAllGpus(t, image, gpu, device_count, options, "", &stage,
                      &unlocked_count, &failed);
  if (s != EFI_SUCCESS)
    return RunFailure(t, image, &gpu[failed], stage, s);

  (VOID) GuBootSyncLog();

  if (gpu[device_count - 1].handle) {
    (VOID) GuReconnectDisplayAfterUnlock(t, image, gpu[device_count - 1].handle, TRUE);
  }

  (VOID) GuClearSafeguardFlag();

  GuBootUnsilence();
  if (Gu.already_unlocked) {
    GuBootSay(L" [3/4] Display Handover .................... [ OK ]\r\n");
    if (enable_s3) {
      if (Gu.s3_registered) {
        GuBootSay(L" [4/4] Hardware & S3 Persistence Status .... [ OK ]\r\n");
      } else {
        GuBootSay(L" [4/4] Hardware [OK] | S3 Reg .............. [ FAILED ]\r\n");
      }
    } else {
      GuBootSay(L" [4/4] Hardware & S3 Persistence Status .... [ OK ]\r\n");
    }
  } else {
    GuBootSay(L" [3/4] Display Handover .................... [ OK ]\r\n");
    if (enable_s3) {
      if (Gu.s3_registered) {
        GuBootSay(L" [4/4] Hardware SEC2 and S3 Persistence .... [ OK ]\r\n");
      } else {
        GuBootSay(L" [4/4] Hardware SEC2 [OK] | S3 Reg ......... [ FAILED ]\r\n");
      }
    } else {
      GuBootSay(L" [4/4] Hardware SEC2 and MSR Unlock ........ [ OK ]\r\n");
    }
  }

  if (GuUnlockIsCsmVideo()) {
    GuBootSay(
        L"\r\n"
        L"  [!] BIOS WARNING: Legacy CSM Video Mode Detected!\r\n"
        L"      If Windows boots with low resolution or Code 43 in Device Manager,\r\n"
        L"      restart into BIOS and set CSM to [Disabled] (pure UEFI mode).\r\n"
        L"\r\n");
  }

  GuBootSay(L" Handing over control to Windows Boot Manager in 3 seconds...\r\n\r\n");
  t->BootServices->Stall(3000000);

  GuLog("TRANSACTION OK: %u of %u GPU(s) unlocked, hardware readbacks passed!\n",
        (unsigned)unlocked_count, (unsigned)device_count);

  stage = "close-USB-log";
  s = GuBootClose();
  if (s != EFI_SUCCESS || Gu.fault)
    return RunFailure(t, image, gpu, stage, s);

  s = GuBootWindows();

  GuBootHalt();
  return s == EFI_SUCCESS ? EFI_ABORTED : s;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {

  if (!image || !t || !t->BootServices || !t->RuntimeServices)
    return EFI_INVALID_PARAMETER;

  EFI_STATUS s = GuBootOpen(image, t);
  if (s != EFI_SUCCESS)
    return s;

  GuLog("=== Universal Multi-Gen GPU Unlock %s (BOOTX64.EFI) ===\n", GU_VERSION_TAG);

  /* MindAllik v1.3.3 / v1.3.4 Safeguard check: If previous attempt did not complete,
   * bypass unlock immediately to prevent system boot loops. */
  if (GuCheckSafeguardFlag()) {
    GuLog("[SAFEGUARD ACTIVE] Previous unlock attempt detected (ATTEMPT.FLG present)!\n");
    GuLog("[SAFEGUARD ACTIVE] Bypassing hardware unlock to prevent boot loop. Chainloading stock Windows.\n");
    GuBootSay(L"\r\n=======================================================\r\n"
              L" [SAFEGUARD ACTIVE] BOOT-LOOP PROTECTION TRIGGERED\r\n"
              L"=======================================================\r\n"
              L" An uncompleted GPU unlock attempt was detected (ATTEMPT.FLG).\r\n"
              L" Safeguard active: Skipping hardware unlock and clearing flag.\r\n"
              L" Chainloading Windows in safe factory mode in 3 seconds...\r\n");
    (VOID) GuClearSafeguardFlag();
    (VOID) GuBootSyncLog();
    if (t->BootServices && t->BootServices->Stall)
      t->BootServices->Stall(3000000);
    s = GuBootLoadWindows();
    if (s == EFI_SUCCESS) {
      (VOID) GuBootClose();
      return GuBootWindows();
    }
  }

  GU_DISCOVERED_DEVICE devices[GU_MAX_DISCOVERED_DEVICES];
  UINTN device_count = 0;

  s = GuDiscoverAllGpus(image, t, devices, GU_MAX_DISCOVERED_DEVICES,
                        &device_count);
  if (s != EFI_SUCCESS || device_count == 0)
    return RunFailure(t, image, NULL, "discover", s);

  if (device_count > 1) {
    GU_DISCOVERED_DEVICE display = devices[0];
    for (UINTN i = 1; i < device_count; i++) devices[i - 1] = devices[i];
    devices[device_count - 1] = display;
  }

  const GU_DISCOVERED_DEVICE *gpu = &devices[0];

  /* Bind primary GPU for initial profile logs & asset checks */
  GuBindDevice(t->BootServices, image, gpu);

  GuLogInventory("", devices, device_count);
  if (device_count > 1) {
    GuBootSay(L"\r\n[INFO] Multi-GPU system detected: All supported GPUs will be unlocked.\r\n");
  }

  s = GuBootLoadWindows();
  if (s != EFI_SUCCESS || Gu.fault)
    return RunFailure(t, image, gpu, "stage-windows-image", s);

  CHAR16 choice = L'S';
  s = GuBootChoose(&choice);
  if (s != EFI_SUCCESS || Gu.fault)
    return RunFailure(t, image, gpu, "manual-choice", s);

  switch (choice) {
  case L'I':
    return RunInstall(image, t);
  case L'R':
    return RunRemove(t);
  case L'U':
  case L'P':
    return RunUnlock(t, image, devices, device_count, choice == L'P');
  default:
    return RunStock();
  }
}
