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
  s = GuDiscoverSingleGpu(image, t);
  if (s != EFI_SUCCESS) goto finish;

  if (Gu.is_ampere) {
    GuLog("DXE-DRIVER: Ampere GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);
  } else if (Gu.is_turing) {
    GuLog("DXE-DRIVER: Turing GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);
  } else {
    GuLog("DXE-DRIVER: Ada Lovelace GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);
  }

  stage = "dxe-assets";
  s = GuBootLoadAssets();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-cold-profile";
  s = GuAd104Prepare();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-preflight";
  s = GuMsiPreflight(t, image, Gu.handle);
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-release";
  s = GuMsiRelease();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  if (!GuMsiReleased() || !GuAd104ColdEntry()) { s = EFI_NOT_READY; goto finish; }
  Gu.display_check = GuMsiReleased;
  Gu.display_ok = TRUE;
  Gu.armed = TRUE;

  UINT64 mask = 0xffffffffffffULL;
  struct pci_dev dev = {
    .vendor = 0x10de,
    .device = Gu.device_id,
    .dev = { .dma_mask = &mask, .coherent_dma_mask = 0xfffffffffULL },
    .current_state = PCI_D0,
    .pm_cap = Gu.pm_cap,
    .refs = 1
  };
  struct gpu_unlock_610_shared shared = { 0 };

  stage = "dxe-shared-assets";
  int result = gpu_unlock_610_load_shared(&dev, &shared);
  if (!result) {
    stage = "dxe-firmware-transaction";
    result = gpu_unlock_610_unlock(&dev, &shared);
  }
  gpu_unlock_610_release_shared(&shared);

  GuLog("DXE-DRIVER: Transaction result=%d safe=%u retained=%u flrs=%u\n",
        result, GuSafeToReturn(), Gu.retained, Gu.flrs);

  if (result || Gu.fault || !GuSafeToReturn()) {
    s = EFI_DEVICE_ERROR;
    goto finish;
  }

  stage = "dxe-finish";
  s = GuMsiFinish();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  GuLog("DXE-DRIVER: TRANSACTION OK: hardware readbacks passed!\n");

  /* Register S3 Boot Script Table via EFI_S3_SAVE_STATE_PROTOCOL */
  {
    static EFI_GUID S3SaveStateGuid = EFI_S3_SAVE_STATE_PROTOCOL_GUID;
    EFI_S3_SAVE_STATE_PROTOCOL *s3 = NULL;
    EFI_STATUS s3_status = t->BootServices->LocateProtocol(&S3SaveStateGuid, NULL, (VOID **)&s3);
    GuLog("DXE-DRIVER: LocateProtocol(S3SaveState) status=%#llx proto=%p\n",
          (unsigned long long)s3_status, s3);

    if (s3_status == EFI_SUCCESS && s3 && s3->Write) {
      UINT32 val_ss0 = 0x88888888;
      UINT32 val_ss1 = 0x00000008;
      UINT64 addr_ss0 = (UINT64)Gu.bar_base + 0x0082381cULL;
      UINT64 addr_ss1 = (UINT64)Gu.bar_base + 0x00823820ULL;

      EFI_STATUS w0 = s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                                EfiBootScriptWidthUint32, addr_ss0, (UINTN)1, &val_ss0);
      EFI_STATUS w1 = s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                                EfiBootScriptWidthUint32, addr_ss1, (UINTN)1, &val_ss1);

      GuLog("DXE-DRIVER: Registered S3 Boot Script SS0 status=%#llx, SS1 status=%#llx\n",
            (unsigned long long)w0, (unsigned long long)w1);
    } else {
      GuLog("DXE-DRIVER: S3SaveState protocol not available even in early DXE\n");
    }
  }

finish:
  if (s != EFI_SUCCESS) {
    GuLog("DXE-DRIVER: STOP at stage=%s status=%#llx fault=%u\n",
          stage, (unsigned long long)s, Gu.fault);
    if (Gu.started || Gu.master_attributes_pending) (VOID)GuClearMaster();
  } else {
    GuLog("DXE-DRIVER: Succeeded! Returning EFI_SUCCESS to DXE Dispatcher.\n");
  }
  (VOID)GuBootClose();
  return s;
}
