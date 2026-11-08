/* SPDX-License-Identifier: GPL-2.0-only */
/* AdaUnlockDxe.c - Universal UEFI DXE Driver with S3 Boot Script Persistence */
#include "port.h"
#include "checks.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "msi_release.h"
#include "boot_io.h"
#include "compat/compat.h"
#include "vendor/unlock_610.h"
#include "s3_save.h"

EFI_STATUS EFIAPI AdaUnlockDriverEntryPoint(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
  if (!image || !t || !t->BootServices) return EFI_INVALID_PARAMETER;

  EFI_STATUS s = GuBootOpen(image, t);
  if (s != EFI_SUCCESS) return s;

  const char *stage = "dxe-discover";
  GU_DISCOVERED_DEVICE devices[GU_MAX_DISCOVERED_DEVICES];
  UINTN device_count = 0;

  s = GuDiscoverAllGpus(image, t, devices, GU_MAX_DISCOVERED_DEVICES, &device_count);
  if (s != EFI_SUCCESS || device_count == 0) goto finish;

  /* Bind primary GPU for initial assets & cold profile */
  GuBindDevice(t->BootServices, image, &devices[0]);

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
  s = GuAd104Prepare();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  GuLog("DXE-DRIVER: Priming Primary GPU [1/%u]: DevID 0x%04x\n",
        (unsigned)device_count, (unsigned)devices[0].device_id);

  s = GuExecuteUnlock(t, image, &stage);
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-finish";
  GuLog("DXE-DRIVER: TRANSACTION OK: hardware readbacks passed!\n");

  /* Register S3 Boot Script Table via EFI_S3_SAVE_STATE_PROTOCOL */
  (VOID)GuRegisterS3(t);

finish:
  if (s == EFI_SUCCESS && Gu.fault) s = EFI_DEVICE_ERROR;
  if (s != EFI_SUCCESS) {
    GuLog("DXE-DRIVER: STOP at stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",
          stage, (unsigned long long)s, GuMsiMutated(), Gu.retained, Gu.fault);
    if (Gu.started || Gu.master_attributes_pending) (VOID)GuClearMaster();
    if (GuMsiMutated() || Gu.started || Gu.retained) {
      /* The GPU may still own DMA buffers: the OS must never inherit that memory.
       * Only a full power-off ends it. Removing the USB drive skips this driver. */
      (VOID)GuBootClose();
      GuBootSay(L"\r\n[ERROR] GPU unlock failed. Shutting down in 10 seconds.\r\n"
                L"Remove the USB drive to boot without the unlock.\r\n");
      if (t->BootServices->Stall) t->BootServices->Stall(10000000);
      if (t->RuntimeServices && t->RuntimeServices->ResetSystem)
        t->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
      GuBootHalt();
      return EFI_ABORTED;
    }
  } else {
    GuLog("DXE-DRIVER: Succeeded for GPU [1/%u]. Returning EFI_SUCCESS to DXE Dispatcher.\n",
          (unsigned)device_count);
  }
  (VOID)GuBootClose();
  return s;
}
