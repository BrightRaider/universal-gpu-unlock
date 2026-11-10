/* SPDX-License-Identifier: GPL-2.0-only */
/* AdaUnlockDxe.c - Universal UEFI DXE Driver with S3 Boot Script Persistence */
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

/* Standard UEFI ReadyToBoot event group GUID:
 * 7ce88fb3-4bd7-4679-87a8-a8d8dee50d2b */
static const EFI_GUID ReadyToBootEventGroupGuid =
    { 0x7ce88fb3, 0x4bd7, 0x4679, { 0x87, 0xa8, 0xa8, 0xd8, 0xde, 0xe5, 0x0d, 0x2b } };

#define EVT_SIGNAL_READY_TO_BOOT 0x00000200
#define EVT_NOTIFY_SIGNAL        0x00000200

static EFI_HANDLE mDxeImage = NULL;
static EFI_SYSTEM_TABLE *mDxeTable = NULL;
static EFI_EVENT mReadyToBootEvent = NULL;

static VOID EFIAPI AdaUnlockOnReadyToBoot(EFI_EVENT event, VOID *context) {
  (VOID)context;

  if (event && mDxeTable && mDxeTable->BootServices && mDxeTable->BootServices->CloseEvent) {
    mDxeTable->BootServices->CloseEvent(event);
    mReadyToBootEvent = NULL;
  }

  if (!mDxeImage || !mDxeTable || !mDxeTable->BootServices) return;

  EFI_STATUS s = GuBootOpen(mDxeImage, mDxeTable);
  if (s != EFI_SUCCESS && s != EFI_ALREADY_STARTED) return;

  GuLog("=== Universal Multi-Gen GPU Unlock %s (AdaUnlockDxe.efi) ===\n", GU_VERSION_TAG);

  const char *stage = "dxe-discover";
  GU_DISCOVERED_DEVICE devices[GU_MAX_DISCOVERED_DEVICES];
  UINTN device_count = 0;

  s = GuDiscoverAllGpus(mDxeImage, mDxeTable, devices, GU_MAX_DISCOVERED_DEVICES, &device_count);
  if (s != EFI_SUCCESS || device_count == 0) goto finish;

  /* Bind primary GPU for initial assets & cold profile */
  GuBindDevice(mDxeTable->BootServices, mDxeImage, &devices[0]);

  GuLog("DXE-DRIVER: Multi-GPU Inventory found %u supported NVIDIA GPU(s)\n", (unsigned)device_count);
  for (UINTN g = 0; g < device_count; g++) {
    GuLog("DXE-DRIVER:   GPU [%u/%u]: DevID 0x%04x (%s)\n",
          (unsigned)(g + 1), (unsigned)device_count, (unsigned)devices[g].device_id,
          devices[g].is_ampere ? "Ampere" : (devices[g].is_turing ? "Turing" : "Ada Lovelace"));
  }
  if (device_count > 1)
    GuLog("DXE-DRIVER: WARNING: only GPU [1/%u] is processed; the other GPU(s) stay stock\n", (unsigned)device_count);

  stage = "dxe-assets";
  s = GuBootLoadAssets();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-cold-profile";
  s = GuPrepare();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  GuLog("DXE-DRIVER: Priming Primary GPU [1/%u]: DevID 0x%04x\n",
        (unsigned)device_count, (unsigned)devices[0].device_id);

  GuBootSay(L"[Universal GPU Unlock] Performing pre-boot hardware unlock...\r\n");

  s = GuExecuteUnlock(mDxeTable, mDxeImage, &stage);
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-finish";
  GuLog("DXE-DRIVER: TRANSACTION OK: hardware readbacks passed!\n");

  /* Register S3 Boot Script Table via EFI_S3_SAVE_STATE_PROTOCOL */
  (VOID)GuRegisterS3(mDxeTable);

finish:
  if (s == EFI_SUCCESS && Gu.fault) s = EFI_DEVICE_ERROR;
  if (s != EFI_SUCCESS) {
    GuLog("DXE-DRIVER: STOP at stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",
          stage, (unsigned long long)s, GuUnlockMutated(), Gu.retained, Gu.fault);
    if (Gu.started || Gu.master_attributes_pending) (VOID)GuClearMaster();
    if (GuUnlockMutated() || Gu.started || Gu.retained) {
      /* The GPU may still own DMA buffers: the OS must never inherit that memory.
       * Only a full power-off ends it. Removing the USB drive skips this driver. */
      (VOID)GuBootClose();
      GuBootSay(L"\r\n[ERROR] GPU unlock failed. Shutting down in 10 seconds.\r\n"
                L"Remove the USB drive to boot without the unlock.\r\n");
      if (mDxeTable->BootServices->Stall) mDxeTable->BootServices->Stall(10000000);
      if (mDxeTable->RuntimeServices && mDxeTable->RuntimeServices->ResetSystem)
        mDxeTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
      GuBootHalt();
      return;
    }
  } else {
    GuLog("DXE-DRIVER: Succeeded for GPU [1/%u]. Handing over to OS loader.\n",
          (unsigned)device_count);

    for (UINTN g = 0; g < device_count; g++) {
      if (mDxeTable && mDxeTable->BootServices && mDxeTable->BootServices->ConnectController && devices[g].handle) {
        (VOID) mDxeTable->BootServices->ConnectController(devices[g].handle, NULL, NULL, TRUE);
      }
    }
    if (mDxeTable && mDxeTable->BootServices && mDxeTable->BootServices->Stall) {
      mDxeTable->BootServices->Stall(200000);
    }
    if (mDxeTable && mDxeTable->ConOut && mDxeTable->ConOut->Reset) {
      (VOID) mDxeTable->ConOut->Reset(mDxeTable->ConOut, FALSE);
    }
  }
  (VOID)GuBootClose();
}

EFI_STATUS EFIAPI AdaUnlockDriverEntryPoint(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
  if (!image || !t || !t->BootServices) return EFI_INVALID_PARAMETER;

  mDxeImage = image;
  mDxeTable = t;

  /* Defer execution to ReadyToBoot event:
   * Early BDS / Driver#### dispatch occurs before ConsoleOut and GOP bindings
   * are connected on AMD TRX40 / Threadripper, multi-GPU and workstation boards.
   * Hooking ReadyToBoot guarantees all PCIe root complexes, GOP display drivers,
   * and console endpoints are 100% stabilized immediately prior to OS boot. */
  EFI_STATUS s = EFI_UNSUPPORTED;
  if (t->BootServices->CreateEventEx != NULL) {
    s = t->BootServices->CreateEventEx(
        EVT_NOTIFY_SIGNAL,
        TPL_CALLBACK,
        AdaUnlockOnReadyToBoot,
        NULL,
        &ReadyToBootEventGroupGuid,
        &mReadyToBootEvent
    );
  }
  if (s != EFI_SUCCESS && t->BootServices->CreateEvent != NULL) {
    s = t->BootServices->CreateEvent(
        EVT_SIGNAL_READY_TO_BOOT,
        TPL_CALLBACK,
        AdaUnlockOnReadyToBoot,
        NULL,
        &mReadyToBootEvent
    );
  }

  if (s != EFI_SUCCESS) {
    /* Fallback: execute directly if the UEFI environment refuses event creation */
    AdaUnlockOnReadyToBoot(NULL, NULL);
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}
