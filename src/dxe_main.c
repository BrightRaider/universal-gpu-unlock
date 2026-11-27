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
#include <Protocol/LoadedImage.h>

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
static BOOLEAN mReadyToBootFired = FALSE;
static EFI_IMAGE_START mOrigStartImage = NULL;
static BOOLEAN mStartImageHookActive = FALSE;
static UINTN mStartImageDepth = 0;
static UINTN mStartImageLogCount = 0;

static EFI_STATUS AdaUnlockRegisterReadyToBoot(EFI_SYSTEM_TABLE *t,
                                               EFI_EVENT_NOTIFY callback,
                                               EFI_EVENT *out_event) {
  if (!t || !t->BootServices || !callback || !out_event) return EFI_INVALID_PARAMETER;
  if (t->BootServices->CreateEventEx != NULL) {
    return t->BootServices->CreateEventEx(
        EVT_NOTIFY_SIGNAL,
        TPL_CALLBACK,
        callback,
        NULL,
        &ReadyToBootEventGroupGuid,
        out_event
    );
  }
  if (t->BootServices->CreateEvent != NULL) {
    return t->BootServices->CreateEvent(
        EVT_SIGNAL_READY_TO_BOOT,
        TPL_CALLBACK,
        callback,
        NULL,
        out_event
    );
  }
  return EFI_UNSUPPORTED;
}

static VOID AdaUnlockUpdateBootServicesCrc32(EFI_SYSTEM_TABLE *t) {
  if (!t || !t->BootServices || !t->BootServices->CalculateCrc32) return;
  t->BootServices->Hdr.CRC32 = 0;
  (VOID)t->BootServices->CalculateCrc32(
      t->BootServices,
      t->BootServices->Hdr.HeaderSize,
      &t->BootServices->Hdr.CRC32
  );
}

static VOID *AdaUnlockGetAcpiTablePtr(CONST EFI_SYSTEM_TABLE *t) {
  if (!t || !t->ConfigurationTable) return NULL;
  static const EFI_GUID acpi20 = GU_EFI_ACPI_20_TABLE_GUID;
  static const EFI_GUID acpi10 = GU_EFI_ACPI_10_TABLE_GUID;
  VOID *fallback10 = NULL;
  for (UINTN i = 0; i < t->NumberOfTableEntries; i++) {
    if (memcmp(&t->ConfigurationTable[i].VendorGuid, &acpi20, sizeof(EFI_GUID)) == 0) {
      return t->ConfigurationTable[i].VendorTable;
    }
    if (!fallback10 &&
        memcmp(&t->ConfigurationTable[i].VendorGuid, &acpi10, sizeof(EFI_GUID)) == 0) {
      fallback10 = t->ConfigurationTable[i].VendorTable;
    }
  }
  return fallback10;
}

static BOOLEAN AdaUnlockIsOsLoaderImage(EFI_HANDLE ImageHandle, EFI_MEMORY_TYPE *out_type) {
  if (out_type) *out_type = (EFI_MEMORY_TYPE)0x7fffffffU;
  if (!ImageHandle || !mDxeTable || !mDxeTable->BootServices || !mDxeTable->BootServices->HandleProtocol) {
    return FALSE;
  }
  static EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
  EFI_STATUS s = mDxeTable->BootServices->HandleProtocol(ImageHandle, &loaded_guid, (VOID **)&loaded);
  if (EFI_ERROR(s) || !loaded) return FALSE;
  if (out_type) *out_type = loaded->ImageCodeType;
  return loaded->ImageCodeType == EfiLoaderCode;
}

static EFI_STATUS EFIAPI AdaUnlockHookedStartImage(EFI_HANDLE ImageHandle,
                                                   UINTN *ExitDataSize,
                                                   CHAR16 **ExitData);

static EFI_STATUS AdaUnlockInstallStartImageHook(EFI_SYSTEM_TABLE *t) {
  if (!t || !t->BootServices || !t->BootServices->StartImage) return EFI_INVALID_PARAMETER;
  if (mStartImageHookActive) return EFI_SUCCESS;
  if (t->BootServices->StartImage == AdaUnlockHookedStartImage) {
    mStartImageHookActive = TRUE;
    return EFI_SUCCESS;
  }
  mOrigStartImage = t->BootServices->StartImage;
  t->BootServices->StartImage = AdaUnlockHookedStartImage;
  AdaUnlockUpdateBootServicesCrc32(t);
  mStartImageHookActive = TRUE;
  return EFI_SUCCESS;
}

/* Refresh Shadow ACPI tables from live RSDP/XSDT/FADT and enforce Plan C hooks. */
static VOID AdaUnlockApplyPlanCNow(EFI_STATUS *out_refresh,
                                   GU_ACPI_SHADOW_REFRESH_INFO *out_info,
                                   EFI_STATUS *out_ebs,
                                   UINT32 *out_patched) {
  GU_ACPI_SHADOW_REFRESH_INFO local_info;
  memset(&local_info, 0, sizeof(local_info));
  EFI_STATUS ref_s = GuAcpiRefreshShadowChain(&local_info);
  EFI_STATUS ebs_s = GuAcpiInstallEbsHook(mDxeTable);
  UINT32 patched = GuAcpiApplyShadowConfigTable();
  (VOID)AdaUnlockInstallStartImageHook(mDxeTable);
  mPlanCActivated = TRUE;
  if (out_refresh) *out_refresh = ref_s;
  if (out_info) *out_info = local_info;
  if (out_ebs) *out_ebs = ebs_s;
  if (out_patched) *out_patched = patched;
}

/* Activate Plan C Shadow ACPI ConfigurationTable and ExitBootServices hook right
 * before the OS loader starts, keeping RealRSDP exposed during early BDS. */
static VOID AdaUnlockActivatePlanC(BOOLEAN can_log) {
  if (!mDxeTable || !Gu.s3_registered || mPlanCActivated) return;
  UINT32 patched = 0;
  AdaUnlockApplyPlanCNow(NULL, NULL, NULL, &patched);
  if (can_log && patched > 0) {
    GuLog("DXE-DRIVER: Plan C Shadow ACPI (%u table(s)) + ExitBootServices hook activated.\n",
          (unsigned)patched);
  }
}

static EFI_STATUS EFIAPI AdaUnlockHookedStartImage(EFI_HANDLE ImageHandle,
                                                   UINTN *ExitDataSize,
                                                   CHAR16 **ExitData) {
  if (!mOrigStartImage) return EFI_NOT_READY;

  EFI_MEMORY_TYPE code_type = (EFI_MEMORY_TYPE)0x7fffffffU;
  BOOLEAN is_os_loader = AdaUnlockIsOsLoaderImage(ImageHandle, &code_type);

  if (Gu.s3_registered && (is_os_loader || mReadyToBootFired || mPlanCActivated)) {
    VOID *pre_acpi = AdaUnlockGetAcpiTablePtr(mDxeTable);
    mStartImageDepth++;
    EFI_STATUS ref_s = EFI_SUCCESS;
    GU_ACPI_SHADOW_REFRESH_INFO ref_info;
    EFI_STATUS ebs_s = EFI_SUCCESS;
    UINT32 patched = 0;
    AdaUnlockApplyPlanCNow(&ref_s, &ref_info, &ebs_s, &patched);
    VOID *post_acpi = AdaUnlockGetAcpiTablePtr(mDxeTable);

    if (mStartImageLogCount < 2) {
      mStartImageLogCount++;
      GuBootAppendLog(
          "DXE-DRIVER: Plan C StartImage(depth=%u type=%u rtb=%u): ref=%#llx RealXSDT=0x%02x(%u) RealFADT=0x%02x(%u) -> ShadowXSDT=0x%02x ShadowFADT=0x%02x rearm=%u acpi=%p->%p patched=%u ebs=%#llx\n",
          (unsigned)mStartImageDepth, (unsigned)code_type,
          (unsigned)mReadyToBootFired, (unsigned long long)ref_s,
          (unsigned)ref_info.real_xsdt_checksum, (unsigned)ref_info.real_xsdt_length,
          (unsigned)ref_info.real_fadt_checksum, (unsigned)ref_info.real_fadt_length,
          (unsigned)ref_info.shadow_xsdt_checksum, (unsigned)ref_info.shadow_fadt_checksum,
          (unsigned)ref_info.real_facs_rearmed,
          pre_acpi, post_acpi,
          (unsigned)patched, (unsigned long long)ebs_s);
    }

    EFI_STATUS s = mOrigStartImage(ImageHandle, ExitDataSize, ExitData);

    mStartImageDepth--;
    if (mStartImageDepth == 0) {
      (VOID)GuAcpiRestoreConfigTable();
      mPlanCActivated = FALSE;
    }
    return s;
  }

  return mOrigStartImage(ImageHandle, ExitDataSize, ExitData);
}

static VOID EFIAPI AdaUnlockOnPlanCReadyToBoot(EFI_EVENT event, VOID *context) {
  (VOID)context;

  if (event && mDxeTable && mDxeTable->BootServices && mDxeTable->BootServices->CloseEvent) {
    mDxeTable->BootServices->CloseEvent(event);
    mPlanCReadyToBootEvent = NULL;
  }

  if (!mDriverExecuted || mReadyToBootFired) return;
  mReadyToBootFired = TRUE;

  VOID *pre_acpi = AdaUnlockGetAcpiTablePtr(mDxeTable);
  AdaUnlockActivatePlanC(FALSE);
  VOID *post_acpi = AdaUnlockGetAcpiTablePtr(mDxeTable);

  GuBootAppendLog("DXE-DRIVER: Plan C ReadyToBoot fired: acpi=%p->%p activated=%u\n",
                  pre_acpi, post_acpi, (unsigned)mPlanCActivated);
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
    if (!GuUnlockMutated() && !Gu.started && !Gu.retained) {
      (VOID)GuClearSafeguardFlag();
    }
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
      mReadyToBootFired = TRUE;
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

  /* 2. Register deferred Plan C activation hooks:
   * - Retry full unlock at ReadyToBoot if PCI devices were not ready during early BDS.
   * - Otherwise, if Plan C S3 is armed, defer ConfigurationTable / EBS hook activation
   *   until ReadyToBoot AND wrap BootServices->StartImage so ShadowRSDP is guaranteed
   *   active when the OS loader (EfiLoaderCode) executes. */
  if (!mDriverExecuted) {
    (VOID)AdaUnlockRegisterReadyToBoot(t, AdaUnlockOnReadyToBoot, &mReadyToBootEvent);
  } else if (Gu.s3_registered && !mPlanCActivated) {
    EFI_STATUS rtb_s = AdaUnlockRegisterReadyToBoot(t, AdaUnlockOnPlanCReadyToBoot, &mPlanCReadyToBootEvent);
    EFI_STATUS start_s = AdaUnlockInstallStartImageHook(t);
    GuBootAppendLog(
        "DXE-DRIVER: Plan C deferred hooks armed (ReadyToBoot=%#llx ev=%p, StartImageHook=%#llx orig=%#llx)\n",
        (unsigned long long)rtb_s, mPlanCReadyToBootEvent,
        (unsigned long long)start_s, (unsigned long long)(UINTN)mOrigStartImage);
  }

  return EFI_SUCCESS;
}
