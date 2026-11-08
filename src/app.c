/* SPDX-License-Identifier: GPL-2.0-only */

#include "port.h"

#include "checks.h"
#include "gpu_discovery.h"

#include "profile.h"

#include "msi_release.h"

#include "boot_io.h"

#include "compat/compat.h"

#include "vendor/unlock_610.h"

#include "s3_save.h"

#include "driver_nvram.h"

static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {

  if(!image||!t||!t->BootServices||!t->RuntimeServices||!t->RuntimeServices->GetVariable)return EFI_INVALID_PARAMETER;

  UINT8 secure=1;UINTN n=1;UINT32 attrs=0;

  EFI_STATUS s=t->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,&attrs,&n,&secure);

  /* Authenticode signature verified by UEFI firmware against custom DB key. Allow SecureBoot. */

  (void)secure;

  s=GuBootOpen(image,t);if(s!=EFI_SUCCESS)return s;

  const char *stage="discover";CHAR16 choice=L'S';

  GU_DISCOVERED_DEVICE devices[GU_MAX_DISCOVERED_DEVICES];
  UINTN device_count = 0;

  s = GuDiscoverAllGpus(image, t, devices, GU_MAX_DISCOVERED_DEVICES, &device_count);
  if (s != EFI_SUCCESS || device_count == 0) goto failed;

  /* Bind primary GPU for initial profile logs & asset checks */
  GuBindDevice(t->BootServices, image, &devices[0]);

  GuLog("PCI Multi-GPU Inventory: Discovered %u supported NVIDIA GPU(s)\n", (unsigned)device_count);
  for (UINTN g = 0; g < device_count; g++) {
    GuLog("  GPU [%u/%u]: DevID 0x%04x (%s)\n",
          (unsigned)(g + 1), (unsigned)device_count, (unsigned)devices[g].device_id,
          devices[g].is_ampere ? "Ampere" : (devices[g].is_turing ? "Turing" : "Ada Lovelace"));
  }

  if (device_count > 1) {
    GuLog("WARNING: only GPU [1/%u] is processed; the other GPU(s) stay stock\n", (unsigned)device_count);
    GuBootSay(L"\r\n[WARNING] More than one supported GPU found. Only the first one is unlocked; the others stay stock.\r\n");
  }

  stage="assets";s=GuBootLoadAssets();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="read-only-cold-profile";s=GuAd104Prepare();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="stage-windows-image";s=GuBootLoadWindows();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="manual-choice";s=GuBootChoose(&choice);if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  if (choice == L'I') {

    GuLog("Operator I: Requesting DXE Driver NVRAM installation\n");

    s = GuInstallDriverOption(image, t);

    if (s == EFI_SUCCESS) {

      GuLog("DXE Driver successfully installed to Driver#### / DriverOrder\n");

      GuBootSay(L"\r\n\r\n[OK] AdaUnlock DXE Driver installed to UEFI NVRAM.\r\n"

                L"Driver runs during early boot on every startup while this USB drive is connected.\r\n"

                L"Rebooting system in 3 seconds...\r\n");

    } else {

      GuLog("ERROR: Failed to install DXE Driver: %#llx\n", (unsigned long long)s);

      GuBootSay(L"\r\n\r\n[ERROR] Failed to install DXE Driver to NVRAM! Nothing was registered.\r\n"

                L"Check that EFI\\Drivers\\AdaUnlockDxe.efi exists on this USB drive.\r\n"

                L"Rebooting in 5 seconds...\r\n");

      if (t->BootServices->Stall) t->BootServices->Stall(2000000);

    }

    (VOID)GuBootClose();

    if (t->BootServices->Stall) t->BootServices->Stall(3000000);

    if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {

      t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);

    }

    return s;

  }

  if (choice == L'R') {

    GuLog("Operator R: Requesting DXE Driver NVRAM removal\n");

    s = GuRemoveDriverOption(t);

    GuLog("DXE Driver removal from Driver#### / DriverOrder: status=%#llx\n", (unsigned long long)s);

    GuBootSay(s == EFI_SUCCESS ? L"\r\n\r\n[OK] AdaUnlock DXE Driver removed from UEFI NVRAM.\r\n" :
              s == EFI_NOT_FOUND ? L"\r\n\r\n[INFO] No AdaUnlock DXE Driver entry found. Nothing was changed.\r\n" :
              L"\r\n\r\n[ERROR] Could not remove the AdaUnlock DXE Driver. See the USB log.\r\n");

    GuBootSay(L"Rebooting system in 3 seconds...\r\n");

    (VOID)GuBootClose();

    if (t->BootServices->Stall) t->BootServices->Stall(3000000);

    if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {

      t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);

    }

    return s;

  }

  BOOLEAN unlock = (choice == L'U' || choice == L'P');

  BOOLEAN enable_s3 = (choice == L'P');

  if(!unlock) {

    GuLog("Operator skipped: no GPU transaction, no driver release; stock Windows\n");

    if(Gu.fault||GuBootClose()!=EFI_SUCCESS)return EFI_DEVICE_ERROR;

    return GuBootWindows();

  }

  GuBootSay(
    L"\r\n=======================================================\r\n"
    L" UNIVERSAL GPU UNLOCK v1.7.9 - Status Monitor\r\n"
    L"=======================================================\r\n"
    L" [1/4] GPU and PCIe Topology Discovery ..... [ OK ]\r\n"
    L" [2/4] Firmware Hashes and VBIOS Parse ..... [ OK ]\r\n"
    L" [3/4] Display Handover .................... [ PENDING ]\r\n"
    L" [4/4] Hardware SEC2 and MSR Unlock ........ [ PENDING ]\r\n\r\n"
    L" --> Starting GPU unlock...\r\n"
  );

  GuBootSilence();

  GuLog("--- Processing Primary GPU [1/%u]: DevID 0x%04x ---\n",
        (unsigned)device_count, (unsigned)devices[0].device_id);

  stage="MSI-preflight";s=GuMsiPreflight(t,image,Gu.handle);if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="MSI-release";s=GuMsiRelease();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  if(!GuMsiReleased()||!GuAd104ColdEntry()){s=EFI_NOT_READY;goto failed;}
  Gu.display_check=GuMsiReleased;Gu.display_ok=TRUE;Gu.armed=TRUE;

  UINT64 mask=0xffffffffffffULL;
  struct pci_dev dev={
    .vendor=0x10de,
    .device=Gu.device_id,
    .dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffULL},
    .current_state=PCI_D0,
    .pm_cap=Gu.pm_cap,
    .refs=1
  };
  struct gpu_unlock_610_shared shared={0};
  stage="shared-assets";int result=gpu_unlock_610_load_shared(&dev,&shared);
  if(!result){stage="firmware-transaction";result=gpu_unlock_610_unlock(&dev,&shared);}
  gpu_unlock_610_release_shared(&shared);
  GuLog("Transaction result=%d safe=%u retained=%u started=%u quiesced=%u flrs=%u DMA_live=%u\n",
        result, GuSafeToReturn(), Gu.retained, Gu.started, Gu.quiesced, Gu.flrs, Gu.dma_count);
  if(result||Gu.fault||!GuSafeToReturn()){s=EFI_DEVICE_ERROR;goto failed;}

  stage="release-exclusive-claim";s=GuMsiFinish();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  if (enable_s3) {
    static EFI_GUID S3SaveStateGuid = EFI_S3_SAVE_STATE_PROTOCOL_GUID;
    EFI_S3_SAVE_STATE_PROTOCOL *s3 = NULL;
    EFI_STATUS s3_status = t->BootServices->LocateProtocol(&S3SaveStateGuid, NULL, (VOID **)&s3);
    if (s3_status == EFI_SUCCESS && s3 && s3->Write) {
      UINT32 val_ss0 = 0x88888888;
      UINT32 val_ss1 = 0x00000008;
      UINT64 addr_ss0 = (UINT64)Gu.bar_base + 0x0082381cULL;
      UINT64 addr_ss1 = (UINT64)Gu.bar_base + 0x00823820ULL;
      (VOID)s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, addr_ss0, (UINTN)1, &val_ss0);
      (VOID)s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, addr_ss1, (UINTN)1, &val_ss1);
    }
  }

  GuBootUnsilence();
  GuBootSay(
    L" [3/4] Display Handover .................... [ OK ]\r\n"
    L" [4/4] Hardware SEC2 and MSR Unlock ........ [ OK ]\r\n"
    L" Handing over control to Windows Boot Manager...\r\n\r\n"
  );

  GuLog("TRANSACTION OK: GPU [1/%u] unlocked, hardware readbacks passed!\n", (unsigned)device_count);

  stage="final-Windows-verification";if(!GuMsiReadyForWindows()){s=EFI_NOT_READY;goto failed;}

  stage="close-USB-log";s=GuBootClose();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="start-Windows";s=GuBootWindows();

  GuBootHalt();return s==EFI_SUCCESS?EFI_ABORTED:s;

failed:

  GuBootUnsilence();

  if(s==EFI_SUCCESS)s=EFI_DEVICE_ERROR;

  GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",stage,(unsigned long long)s,GuMsiMutated(),Gu.retained,Gu.fault);

  if(GuMsiMutated()||Gu.started||Gu.retained) {

    if(Gu.started||Gu.master_attributes_pending)(VOID)GuClearMaster();

    (VOID)GuBootClose();GuBootHalt();return EFI_ABORTED;

  }

  GuBootSay(L"\r\nPreflight refused. No driver release or GPU transaction. Log saved to USB.\r\nReturning to Boot Menu in 3 seconds...\r\n");

  (VOID)GuBootClose();

  if(t&&t->BootServices&&t->BootServices->Stall)t->BootServices->Stall(3000000);

  return s;

}
