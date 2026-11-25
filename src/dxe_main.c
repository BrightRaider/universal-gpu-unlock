/* SPDX-License-Identifier: GPL-2.0-only */
/* AdaUnlockDxe.c - Universal UEFI DXE Driver with S3 Boot Script Persistence */
#include "port.h"
#include "version.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "unlock_release.h"
#include "boot_io.h"
#include "driver_nvram.h"
#include "multi_gpu.h"
#include "s3_facs.h"
#include "compat/compat.h"

/* Standard UEFI ReadyToBoot event group GUID:
 * 7ce88fb3-4bd7-4679-87a8-a8d8dee50d2b */
static const EFI_GUID ReadyToBootEventGroupGuid =
    { 0x7ce88fb3, 0x4bd7, 0x4679, { 0x87, 0xa8, 0xa8, 0xd8, 0xde, 0xe5, 0x0d, 0x2b } };

#define EVT_SIGNAL_READY_TO_BOOT 0x00000200
#define EVT_NOTIFY_SIGNAL        0x00000200

static EFI_HANDLE mDxeImage = NULL;
static EFI_SYSTEM_TABLE *mDxeTable = NULL;
static EFI_EVENT mReadyToBootEvent = NULL;
static EFI_EVENT mPlanCReadyToBootEvent = NULL;
static BOOLEAN mDriverExecuted = FALSE;
static BOOLEAN mPlanCActivated = FALSE;

static VOID AdaUnlockRegisterReadyToBoot(EFI_SYSTEM_TABLE *t,
                                         EFI_EVENT_NOTIFY callback,
                                         EFI_EVENT *out_event) {
  if (!t || !t->BootServices || !callback || !out_event) return;
  if (t->BootServices->CreateEventEx != NULL) {
    (VOID) t->BootServices->CreateEventEx(
        EVT_NOTIFY_SIGNAL,
        TPL_CALLBACK,
        callback,
        NULL,
        &ReadyToBootEventGroupGuid,
        out_event
    );
  } else if (t->BootServices->CreateEvent != NULL) {
    (VOID) t->BootServices->CreateEvent(
        EVT_SIGNAL_READY_TO_BOOT,
        TPL_CALLBACK,
        callback,
        NULL,
        out_event
    );
  }
}

/* Activate Plan C Shadow ACPI ConfigurationTable and ExitBootServices hook right
 * before the OS loader starts, keeping RealRSDP exposed during early BDS. */
static VOID AdaUnlockActivatePlanC(BOOLEAN can_log) {
  if (!mDxeTable || !Gu.s3_registered || mPlanCActivated) return;
  mPlanCActivated = TRUE;
  (VOID) GuAcpiInstallEbsHook(mDxeTable);
  UINT32 patched = GuAcpiApplyShadowConfigTable();
  if (can_log && patched > 0) {
    GuLog("DXE-DRIVER: Plan C Shadow ACPI (%u table(s)) + ExitBootServices hook activated.\n",
          (unsigned)patched);
  }
}

static VOID EFIAPI AdaUnlockOnPlanCReadyToBoot(EFI_EVENT event, VOID *context) {
  (VOID)context;

  if (event && mDxeTable && mDxeTable->BootServices && mDxeTable->BootServices->CloseEvent) {
    mDxeTable->BootServices->CloseEvent(event);
    mPlanCReadyToBootEvent = NULL;
  }

  if (!mDriverExecuted) return;
  AdaUnlockActivatePlanC(FALSE);
}

static VOID EFIAPI AdaUnlockOnReadyToBoot(EFI_EVENT event, VOID *context) {
  (VOID)context;

  if (event && mDxeTable && mDxeTable->BootServices && mDxeTable->BootServices->CloseEvent) {
    mDxeTable->BootServices->CloseEvent(event);
    mReadyToBootEvent = NULL;
  }

  if (mDriverExecuted) return;
  if (!mDxeImage || !mDxeTable || !mDxeTable->BootServices) return;

  EFI_STATUS s = GuBootOpen(mDxeImage, mDxeTable);
  if (s != EFI_SUCCESS && s != EFI_ALREADY_STARTED) return;

  GuLog("=== Universal Multi-Gen GPU Unlock %s (AdaUnlockDxe.efi) ===\n", GU_VERSION_TAG);

  /* MindAllik v1.3.3 / v1.3.4 Safeguard check: If previous attempt did not complete,
   * bypass unlock immediately to prevent system boot loops. */
  if (GuCheckSafeguardFlag()) {
    GuLog("DXE-DRIVER: [SAFEGUARD ACTIVE] ATTEMPT.FLG detected! Skipping unlock to prevent boot loop.\n");
    (VOID) GuClearSafeguardFlag();
    (VOID) GuBootSyncLog();
    (VOID) GuBootClose();
    return;
  }

  const char *stage = "dxe-discover";
  GU_DISCOVERED_DEVICE devices[GU_MAX_DISCOVERED_DEVICES];
  UINTN device_count = 0, unlocked_count = 0, failed = 0;

  s = GuDiscoverAllGpus(mDxeImage, mDxeTable, devices, GU_MAX_DISCOVERED_DEVICES, &device_count);
  if (s != EFI_SUCCESS || device_count == 0) goto finish;

  if (device_count > 1) {
    GU_DISCOVERED_DEVICE display = devices[0];
    for (UINTN i = 1; i < device_count; i++) devices[i - 1] = devices[i];
    devices[device_count - 1] = display;
  }

  /* Bind primary GPU for initial assets & cold profile */
  GuBindDevice(mDxeTable->BootServices, mDxeImage, &devices[0]);

  GuLogInventory("DXE-DRIVER: ", devices, device_count);

  stage = "dxe-assets";
  s = GuBootLoadAssets();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-cold-profile";
  s = GuPrepare();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  GuBootSay(L"[Universal GPU Unlock] Performing pre-boot hardware unlock...\r\n");

  (VOID) GuSetSafeguardFlag();

  Gu.s3_persist_armed = TRUE;
  s = GuUnlockAllGpus(mDxeTable, mDxeImage, devices, device_count, GU_UNLOCK_REGISTER_S3,
                      "DXE-DRIVER: ", &stage, &unlocked_count, &failed);
  if (s != EFI_SUCCESS) {
    GuLog("DXE-DRIVER: GPU [%u/%u] failed\n", (unsigned)(failed + 1), (unsigned)device_count);
    goto finish;
  }

  (VOID) GuClearSafeguardFlag();

  stage = "dxe-finish";
  GuLog("DXE-DRIVER: TRANSACTION OK: %u of %u GPU(s) unlocked, hardware readbacks passed!\n",
        (unsigned)unlocked_count, (unsigned)device_count);

finish:
  if (s == EFI_SUCCESS && Gu.fault) s = EFI_DEVICE_ERROR;
  if (s != EFI_SUCCESS) {
    GuLog("DXE-DRIVER: STOP at stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",
          stage, (unsigned long long)s, GuUnlockMutated(), Gu.retained, Gu.fault);
    if (Gu.started || Gu.master_attributes_pending) (VOID)GuClearMaster();
    GuReleaseStagedVbiosEfiImage();
    (VOID)GuBootSyncLog();
    if (GuUnlockMutated() || Gu.started || Gu.retained) {
      /* The GPU may still own DMA buffers: the OS must never inherit that memory,
       * and only a full power-off ends it. Leave DriverOrder first, so the next
       * boot runs stock instead of failing and shutting down again. This matters
       * for an install on the internal ESP, where removing the USB drive does not help. */
      EFI_STATUS disabled = GuDisableDriverOption(mDxeTable);
      GuLog("DXE-DRIVER: self-disable for next boot: status=%#llx\n", (unsigned long long)disabled);
      (VOID)GuBootClose();
      GuBootSay(disabled == EFI_SUCCESS
                    ? L"\r\n[ERROR] GPU unlock failed. Shutting down in 10 seconds.\r\n"
                      L"The AdaUnlock driver is disabled, so the next boot runs without the unlock.\r\n"
                      L"To enable it again, boot the USB drive and press [I]. Press [R] to remove it.\r\n"
                    : L"\r\n[ERROR] GPU unlock failed. Shutting down in 10 seconds.\r\n"
                      L"The AdaUnlock driver could not disable itself.\r\n"
                      L"Remove the USB drive to boot without the unlock.\r\n");
      if (mDxeTable->BootServices->Stall) mDxeTable->BootServices->Stall(10000000);
      if (mDxeTable->RuntimeServices && mDxeTable->RuntimeServices->ResetSystem)
        mDxeTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
      GuBootHalt();
      return;
    }
  } else {
    GuLog("DXE-DRIVER: Succeeded for %u of %u GPU(s). Handing over to OS loader.\n",
          (unsigned)unlocked_count, (unsigned)device_count);

    mDriverExecuted = TRUE;
    if (event != NULL) {
      AdaUnlockActivatePlanC(TRUE);
    }

    (VOID)GuBootSyncLog();

    if (devices[device_count - 1].handle) {
      (VOID)GuReconnectDisplayAfterUnlock(mDxeTable, mDxeImage, devices[device_count - 1].handle, TRUE);
    }
  }
  (VOID)GuBootClose();
}

EFI_STATUS EFIAPI AdaUnlockDriverEntryPoint(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
  if (!image || !t || !t->BootServices) return EFI_INVALID_PARAMETER;

  mDxeImage = image;
  mDxeTable = t;

  /* 1. Primary Execution: Execute immediately at DriverEntryPoint BEFORE DxeSmmReadyToLock.
   * Early BDS / Driver#### dispatch occurs when the S3 Save State Protocol table is wide open,
   * allowing S3 boot script registration on AMI Aptio V motherboards before SMRAM lockdown. */
  AdaUnlockOnReadyToBoot(NULL, NULL);

  /* 2. Register ReadyToBoot callback:
   * - Retry full unlock at ReadyToBoot if PCI devices were not ready during early BDS.
   * - Otherwise, if Plan C S3 is armed, defer ConfigurationTable / EBS hook activation
   *   until ReadyToBoot so early BDS keeps seeing RealRSDP. */
  if (!mDriverExecuted) {
    AdaUnlockRegisterReadyToBoot(t, AdaUnlockOnReadyToBoot, &mReadyToBootEvent);
  } else if (Gu.s3_registered && !mPlanCActivated) {
    AdaUnlockRegisterReadyToBoot(t, AdaUnlockOnPlanCReadyToBoot, &mPlanCReadyToBootEvent);
  }

  return EFI_SUCCESS;
}
