/* SPDX-License-Identifier: GPL-2.0-only */

#include "port.h"
#include "version.h"

#include "checks.h"
#include "gpu_discovery.h"

#include "profile.h"

#include "unlock_release.h"

#include "boot_io.h"

#include "compat/compat.h"

#include "vendor/unlock_610.h"

#include "s3_save.h"

#include "driver_nvram.h"

static EFI_STATUS GuRebootSystem(EFI_SYSTEM_TABLE *t, EFI_STATUS s) {
  (VOID) GuBootClose();
  if (t->BootServices->Stall)
    t->BootServices->Stall(3000000);
  if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {
    t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
  }
  return s;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {

  if (!image || !t || !t->BootServices || !t->RuntimeServices)
    return EFI_INVALID_PARAMETER;

  EFI_STATUS s = GuBootOpen(image, t);
  if (s != EFI_SUCCESS)
    return s;

  GuLog("=== Universal Multi-Gen GPU Unlock %s (BOOTX64.EFI) ===\n", GU_VERSION_TAG);

  const char *stage = "discover";
  CHAR16 choice = L'S';

  GU_DISCOVERED_DEVICE devices[GU_MAX_DISCOVERED_DEVICES];
  UINTN device_count = 0;

  s = GuDiscoverAllGpus(image, t, devices, GU_MAX_DISCOVERED_DEVICES,
                        &device_count);
  if (s != EFI_SUCCESS || device_count == 0)
    goto failed;

  /* Bind primary GPU for initial profile logs & asset checks */
  GuBindDevice(t->BootServices, image, &devices[0]);

  GuLog("PCI Multi-GPU Inventory: Discovered %u supported NVIDIA GPU(s)\n",
        (unsigned)device_count);
  for (UINTN g = 0; g < device_count; g++) {
    GuLog("  GPU [%u/%u]: DevID 0x%04x (%s)\n", (unsigned)(g + 1),
          (unsigned)device_count, (unsigned)devices[g].device_id,
          devices[g].is_ampere
              ? "Ampere"
              : (devices[g].is_turing ? "Turing" : "Ada Lovelace"));
  }

  if (device_count > 1) {
    GuLog(
        "WARNING: only GPU [1/%u] is processed; the other GPU(s) stay stock\n",
        (unsigned)device_count);
    GuBootSay(L"\r\n[WARNING] More than one supported GPU found. Only the "
              L"first one is unlocked; the others stay stock.\r\n");
  }

  stage = "diagnostic-scan";
  (VOID)GuDiagnosticScan();

  stage = "stage-windows-image";
  s = GuBootLoadWindows();
  if (s != EFI_SUCCESS || Gu.fault)
    goto failed;

  stage = "manual-choice";
  s = GuBootChoose(&choice);
  if (s != EFI_SUCCESS || Gu.fault)
    goto failed;

  if (choice == L'I') {

    GuLog("Operator I: Requesting DXE Driver NVRAM installation\n");

    s = GuInstallDriverOption(image, t);

    if (s == EFI_SUCCESS) {

      GuLog("DXE Driver successfully installed to Driver#### / DriverOrder\n");

      GuBootSay(
          L"\r\n\r\n[OK] AdaUnlock DXE Driver installed to Internal NVMe-ESP & UEFI NVRAM.\r\n"

          L"Driver executes automatically during UEFI POST on every cold boot.\r\n"
          L"USB drive is NO LONGER required and can be safely removed!\r\n"

          L"Rebooting system in 3 seconds...\r\n");

    } else {

      GuLog("ERROR: Failed to install DXE Driver: %#llx\n",
            (unsigned long long)s);

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

  if (choice == L'R') {

    GuLog("Operator R: Requesting DXE Driver NVRAM removal\n");

    s = GuRemoveDriverOption(t);

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

  BOOLEAN unlock = (choice == L'U' || choice == L'P');

  BOOLEAN enable_s3 = (choice == L'P');

  if (!unlock) {

    GuLog("Operator skipped: no GPU transaction, no driver release; stock "
          "Windows\n");

    if (Gu.fault || GuBootClose() != EFI_SUCCESS)
      return EFI_DEVICE_ERROR;

    return GuBootWindows();
  }

  stage = "assets";
  s = GuBootLoadAssets();
  if (s != EFI_SUCCESS || Gu.fault)
    goto failed;

  stage = "read-only-cold-profile";
  s = GuPrepare();
  if (s != EFI_SUCCESS || Gu.fault)
    goto failed;

  GuBootSay(
      L"\r\n=======================================================\r\n"
      L" UNIVERSAL GPU UNLOCK " GU_VERSION_TAG_WIDE L" - Status Monitor\r\n"
      L"=======================================================\r\n"
      L" [1/4] GPU and PCIe Topology Discovery ..... [ OK ]\r\n"
      L" [2/4] Firmware Hashes and VBIOS Parse ..... [ OK ]\r\n"
      L" [3/4] Display Handover .................... [ PENDING ]\r\n"
      L" [4/4] Hardware SEC2 and MSR Unlock ........ [ PENDING ]\r\n\r\n"
      L" --> Starting GPU unlock...\r\n");

  GuBootSilence();

  GuLog("--- Processing Primary GPU [1/%u]: DevID 0x%04x ---\n",
        (unsigned)device_count, (unsigned)devices[0].device_id);

  s = GuExecuteUnlock(t, image, &stage);
  if (s != EFI_SUCCESS || Gu.fault)
    goto failed;

  if (enable_s3) {
    (VOID) GuRegisterS3(t);
  }

  GuBootUnsilence();
  GuBootSay(L" [3/4] Display Handover .................... [ OK ]\r\n"
            L" [4/4] Hardware SEC2 and MSR Unlock ........ [ OK ]\r\n"
            L" Handing over control to Windows Boot Manager...\r\n\r\n");

  GuLog("TRANSACTION OK: GPU [1/%u] unlocked, hardware readbacks passed!\n",
        (unsigned)device_count);

  stage = "final-Windows-verification";
  if (!GuUnlockReadyForWindows()) {
    s = EFI_NOT_READY;
    goto failed;
  }

  stage = "close-USB-log";
  s = GuBootClose();
  if (s != EFI_SUCCESS || Gu.fault)
    goto failed;

  stage = "start-Windows";
  s = GuBootWindows();

  GuBootHalt();
  return s == EFI_SUCCESS ? EFI_ABORTED : s;

failed:

  GuBootUnsilence();

  if (s == EFI_SUCCESS)
    s = EFI_DEVICE_ERROR;

  GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\n", stage,
        (unsigned long long)s, GuUnlockMutated(), Gu.retained, Gu.fault);

  if (GuUnlockMutated() || Gu.started || Gu.retained) {

    if (Gu.started || Gu.master_attributes_pending)
      (VOID) GuClearMaster();

    (VOID) GuBootClose();
    GuBootHalt();
    return EFI_ABORTED;
  }

  GuBootSay(L"\r\nPreflight refused. No driver release or GPU transaction. Log "
            L"saved to USB.\r\nReturning to Boot Menu in 3 seconds...\r\n");

  (VOID) GuBootClose();

  if (t && t->BootServices && t->BootServices->Stall)
    t->BootServices->Stall(3000000);

  return s;
}
