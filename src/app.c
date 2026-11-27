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
  (VOID) GuClearSafeguardFlag();

  GuBootSay(GuBootUiStr(GU_STR_FAIL_HDR));
  GuBootSay(GuBootGetLogPath());
  GuBootSay(GuBootUiStr(GU_STR_FAIL_FTR));

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
    GuBootSay(GuBootUiStr(GU_STR_INST_ESP_OK));
  } else if (s == EFI_SUCCESS) {
    GuLog("DXE Driver registered on the USB drive (Driver#### / DriverOrder)\n");
    GuBootSay(GuBootUiStr(GU_STR_INST_USB_OK));
  } else {
    GuLog("ERROR: Failed to install DXE Driver: %#llx\n", (unsigned long long)s);
    GuBootSay(GuBootUiStr(GU_STR_INST_ERR));
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

  GuBootSay(GuBootUiStr(
      s == EFI_SUCCESS     ? GU_STR_REM_OK
      : s == EFI_NOT_FOUND ? GU_STR_REM_NOTFOUND
                           : GU_STR_REM_ERR));

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

  GuBootSay(GuBootUiStr(GU_STR_STATUS_HDR));
  GuBootSay(GuBootUiStr(GU_STR_ST_STEP12));
  if (Gu.already_unlocked) {
    GuBootSay(GuBootUiStr(GU_STR_ST_ALREADY3));
    GuBootSay(GuBootUiStr(enable_s3 ? GU_STR_ST_S3_REG : GU_STR_ST_S3_SKIP));
  } else {
    GuBootSay(GuBootUiStr(GU_STR_ST_PEND34));
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
  GuBootSay(GuBootUiStr(GU_STR_ST_DISP_OK));
  if (enable_s3) {
    GuBootSay(GuBootUiStr(Gu.s3_registered ? GU_STR_ST_S3_DONE : GU_STR_ST_S3_FAIL));
  } else {
    GuBootSay(GuBootUiStr(GU_STR_ST_UNLOCK_OK));
  }

  if (GuUnlockIsCsmVideo()) {
    GuBootSay(GuBootUiStr(GU_STR_WARN_CSM_POST));
  }

  if (enable_s3 && Gu.s3_s0ix_warning) {
    GuBootSay(GuBootUiStr(GU_STR_WARN_S0IX));
  }

  GuBootSay(GuBootUiStr(GU_STR_HANDOFF_WIN));
  t->BootServices->Stall(3000000);

  GuLog("TRANSACTION OK: %u of %u GPU(s) unlocked, hardware readbacks passed!\n",
        (unsigned)unlocked_count, (unsigned)device_count);

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

  GuBootEnableLargeConsole();

  GuLog("=== Universal Multi-Gen GPU Unlock %s (BOOTX64.EFI) ===\n", GU_VERSION_TAG);

  /* MindAllik v1.3.3 / v1.3.4 Safeguard check: If previous attempt did not complete,
   * bypass unlock immediately to prevent system boot loops. */
  if (GuCheckSafeguardFlag()) {
    GuLog("[SAFEGUARD ACTIVE] Previous unlock attempt detected (ATTEMPT.FLG present)!\n");
    GuLog("[SAFEGUARD ACTIVE] Bypassing hardware unlock to prevent boot loop. Chainloading stock Windows.\n");
    GuBootSay(GuBootUiStr(GU_STR_SAFEGUARD));
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
    GuBootSay(GuBootUiStr(GU_STR_MULTIGPU_INFO));
  }

  s = GuBootLoadWindows();
  if (s != EFI_SUCCESS || Gu.fault)
    return RunFailure(t, image, gpu, "stage-windows-image", s);

  GuBootSetCsmStatus(GuProbePreMenuCsmStatus(t));

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
