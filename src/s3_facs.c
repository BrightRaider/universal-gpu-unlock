/* SPDX-License-Identifier: GPL-2.0-only */
#include "s3_facs.h"
#include "port.h"
#include "compat/compat.h"

typedef char gu_s3_ctx_size_assert[(sizeof(GU_S3_RESUME_CONTEXT) == 108) ? 1 : -1];

static const EFI_GUID mAcpi20TableGuid = GU_EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID mAcpi10TableGuid = GU_EFI_ACPI_10_TABLE_GUID;

static inline BOOLEAN GuCompareGuid(const EFI_GUID *g1, const EFI_GUID *g2) {
  const UINT32 *a = (const UINT32 *)g1;
  const UINT32 *b = (const UINT32 *)g2;
  return (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

typedef struct {
  BOOLEAN installed;
  BOOLEAN applied;
  VOID *orig_rsdp;
  VOID *shadow_rsdp;
  VOID *high_acpi_page;
  EFI_PHYSICAL_ADDRESS high_acpi_phys;
  EFI_PHYSICAL_ADDRESS telemetry_page_phys;
  VOID *low_trampoline;
  EFI_SYSTEM_TABLE *system_table;
  EFI_EXIT_BOOT_SERVICES orig_exit_boot_services;
  BOOLEAN hook_active;
  EFI_ACPI_S3_SAVE_PROTOCOL *acpi_s3_save_protocol;
  EFI_ACPI_S3_SAVE orig_s3_save;
  BOOLEAN s3_save_hook_active;
  UINT16 s3_save_call_count;
  UINT32 orig_waking_vector;
  UINT64 orig_xfirmware_waking_vector;
} GU_ACPI_SHADOW_CHAIN_STATE;

static GU_ACPI_SHADOW_CHAIN_STATE mShadowState = {
  FALSE, FALSE, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL, FALSE, NULL, NULL, FALSE, 0, 0, 0
};

UINT32 GuAcpiApplyShadowConfigTable(VOID) {
  if (!mShadowState.installed || !mShadowState.system_table || !mShadowState.shadow_rsdp) return 0;
  EFI_SYSTEM_TABLE *st = mShadowState.system_table;
  UINT32 count = 0;
  for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
    if (GuCompareGuid(&st->ConfigurationTable[i].VendorGuid, &mAcpi20TableGuid) ||
        GuCompareGuid(&st->ConfigurationTable[i].VendorGuid, &mAcpi10TableGuid)) {
      st->ConfigurationTable[i].VendorTable = mShadowState.shadow_rsdp;
      count++;
    }
  }
  if (count > 0) {
    mShadowState.applied = TRUE;
  }
  return count;
}

UINT32 GuAcpiRestoreConfigTable(VOID) {
  if (!mShadowState.installed || !mShadowState.system_table || !mShadowState.orig_rsdp) return 0;
  EFI_SYSTEM_TABLE *st = mShadowState.system_table;
  UINT32 count = 0;
  for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
    if (GuCompareGuid(&st->ConfigurationTable[i].VendorGuid, &mAcpi20TableGuid) ||
        GuCompareGuid(&st->ConfigurationTable[i].VendorGuid, &mAcpi10TableGuid)) {
      st->ConfigurationTable[i].VendorTable = mShadowState.orig_rsdp;
      count++;
    }
  }
  mShadowState.applied = FALSE;
  return count;
}

EFI_STATUS EFIAPI GuHookedS3Save(
    IN EFI_ACPI_S3_SAVE_PROTOCOL *This,
    IN VOID *Context
) {
  mShadowState.s3_save_call_count++;

  if (mShadowState.low_trampoline != NULL) {
    volatile UINT16 *words = (volatile UINT16 *)((UINT8 *)mShadowState.low_trampoline + GU_TRAMP_RESTORED_COUNT_OFF);
    words[1] = mShadowState.s3_save_call_count; /* +0x502: S3SaveCount */
  }

  /*
   * PHASE 1 (Sandwich Pre-Call):
   * Revert SystemTable ConfigurationTable to RealRSDP so that AcpiS3SaveDxe
   * and SMM LockBox observe clean, genuine BIOS ACPI tables!
   */
  BOOLEAN was_applied = mShadowState.applied;
  GuAcpiRestoreConfigTable();

  EFI_STATUS Status = EFI_SUCCESS;
  if (mShadowState.orig_s3_save != NULL) {
    Status = mShadowState.orig_s3_save(This, Context);
  }

  /*
   * PHASE 2 (Sandwich Post-Call):
   * Only re-apply ShadowRSDP if it was previously applied!
   */
  if (was_applied) {
    GuAcpiApplyShadowConfigTable();
  }

  return Status;
}

EFI_STATUS EFIAPI GuHookedExitBootServices(
    IN EFI_HANDLE ImageHandle,
    IN UINTN MapKey
) {
  /*
   * PHASE 1 (Sandwich Pre-Call):
   * Revert SystemTable ConfigurationTable to RealRSDP so that firmware drivers,
   * SMM runtime handlers, and BIOS S3 state save observe clean, unmodified ACPI tables!
   * MANDATORY: ABSOLUTELY NO BootServices calls inside here (no Print, no CalculateCrc32, no Logging).
   */
  UINT32 restored_count = GuAcpiRestoreConfigTable();

  /* Record pre-call diagnostics in LowTrampoline buffer */
  if (mShadowState.low_trampoline != NULL) {
    volatile UINT16 *words = (volatile UINT16 *)((UINT8 *)mShadowState.low_trampoline + GU_TRAMP_RESTORED_COUNT_OFF);
    words[0] = (UINT16)restored_count; /* +0x500: RestoredCount (lower 16 bits) */
    /* words[1] (+0x502) preserves S3SaveCallCount */

    volatile UINT32 *dwords = (volatile UINT32 *)((UINT8 *)mShadowState.low_trampoline + GU_TRAMP_RESTORED_COUNT_OFF);
    dwords[1] = (UINT32)(UINTN)mShadowState.orig_rsdp; /* +0x504: Lower 32 bits of restored VendorTable ptr */

    volatile UINT16 *ebs_words = (volatile UINT16 *)((UINT8 *)mShadowState.low_trampoline + GU_TRAMP_EBS_CALL_COUNT_OFF);
    ebs_words[0]++; /* +0x508: EbsCallCount (lower 16 bits of +0x508) */
  }

  /* Call original firmware ExitBootServices */
  EFI_STATUS Status = EFI_SUCCESS;
  if (mShadowState.orig_exit_boot_services != NULL) {
    Status = mShadowState.orig_exit_boot_services(ImageHandle, MapKey);
  }

  /* Record post-call diagnostics in LowTrampoline buffer */
  if (mShadowState.low_trampoline != NULL) {
    volatile UINT16 *ebs_words = (volatile UINT16 *)((UINT8 *)mShadowState.low_trampoline + GU_TRAMP_EBS_CALL_COUNT_OFF);
    if (!EFI_ERROR(Status)) {
      ebs_words[1]++; /* +0x50A: EbsSuccessCount (upper 16 bits of +0x508) */
    }
    volatile UINT32 *dwords = (volatile UINT32 *)((UINT8 *)mShadowState.low_trampoline + GU_TRAMP_RESTORED_COUNT_OFF);
    dwords[3] = (UINT32)Status; /* +0x50C: EbsLastStatus */
  }

  /*
   * PHASE 2 (Sandwich Post-Call):
   * In v1.8.44: ONLY re-apply ShadowRSDP if ExitBootServices failed (e.g.
   * EFI_INVALID_PARAMETER retry by winload.efi after MapKey update).
   * If Status == EFI_SUCCESS, RealRSDP stays permanently in ConfigurationTable!
   * Windows winload.efi already captured ShadowRSDP prior to ExitBootServices.
   */
  if (EFI_ERROR(Status)) {
    GuAcpiApplyShadowConfigTable();
  }

  return Status;
}

EFI_STATUS GuAcpiInstallEbsHook(IN EFI_SYSTEM_TABLE *SystemTable) {
  if (!SystemTable || !SystemTable->BootServices) return EFI_INVALID_PARAMETER;
  if (!mShadowState.installed || mShadowState.hook_active) return EFI_SUCCESS;

  mShadowState.orig_exit_boot_services = SystemTable->BootServices->ExitBootServices;
  SystemTable->BootServices->ExitBootServices = GuHookedExitBootServices;

  if (SystemTable->BootServices->CalculateCrc32 != NULL) {
    SystemTable->BootServices->Hdr.CRC32 = 0;
    SystemTable->BootServices->CalculateCrc32(
        SystemTable->BootServices,
        SystemTable->BootServices->Hdr.HeaderSize,
        &SystemTable->BootServices->Hdr.CRC32
    );
  }

  mShadowState.hook_active = TRUE;
  GuLog("S3 Plan C: ExitBootServices Sandwich Hook installed (Orig=%#llx Hook=%#llx)\n",
        (unsigned long long)(UINTN)mShadowState.orig_exit_boot_services,
        (unsigned long long)(UINTN)GuHookedExitBootServices);
  return EFI_SUCCESS;
}

EFI_STATUS GuAcpiUninstallEbsHook(IN EFI_SYSTEM_TABLE *SystemTable) {
  if (mShadowState.s3_save_hook_active && mShadowState.acpi_s3_save_protocol != NULL) {
    mShadowState.acpi_s3_save_protocol->S3Save = mShadowState.orig_s3_save;
    mShadowState.s3_save_hook_active = FALSE;
  }

  if (mShadowState.hook_active && SystemTable && SystemTable->BootServices) {
    if (mShadowState.orig_exit_boot_services != NULL) {
      SystemTable->BootServices->ExitBootServices = mShadowState.orig_exit_boot_services;
      if (SystemTable->BootServices->CalculateCrc32 != NULL) {
        SystemTable->BootServices->Hdr.CRC32 = 0;
        SystemTable->BootServices->CalculateCrc32(
            SystemTable->BootServices,
            SystemTable->BootServices->Hdr.HeaderSize,
            &SystemTable->BootServices->Hdr.CRC32
        );
      }
      mShadowState.orig_exit_boot_services = NULL;
    }
    mShadowState.hook_active = FALSE;
    GuLog("S3 Plan C: ExitBootServices Sandwich Hook uninstalled\n");
  }

  GuAcpiRestoreConfigTable();
  return EFI_SUCCESS;
}

static BOOLEAN GuidMatch(const EFI_GUID *a, const EFI_GUID *b) {
  const UINT8 *p1 = (const UINT8 *)a;
  const UINT8 *p2 = (const UINT8 *)b;
  for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
    if (p1[i] != p2[i]) return FALSE;
  }
  return TRUE;
}

/*
 * Host-Test & 64-bit safety helper:
 * In UEFI firmware, physical addresses are <= 4GB. In 64-bit host unit tests,
 * stack/heap pointers exceed 32 bits (>4GB). If addr32 is >= 1MB (not low trampoline memory)
 * and anchor pointer exceeds 4GB, merge upper 32 bits from anchor so pointers remain valid.
 */
static inline VOID *GuAcpiPaddrToPtr(UINT32 addr32, const VOID *anchor) {
  if (addr32 == 0) return NULL;
  UINTN addr = (UINTN)addr32;
#if defined(_WIN64) || defined(__x86_64__)
  if (addr32 >= 0x00100000ULL && anchor != NULL && (UINTN)anchor > 0xFFFFFFFFULL) {
    addr |= ((UINTN)anchor & ~0xFFFFFFFFULL);
  }
#endif
  return (VOID *)addr;
}

EFI_STATUS GuAcpiLocateFadtAndFacs(
    IN EFI_SYSTEM_TABLE *SystemTable,
    OUT GU_ACPI_FACS_INFO *Info
) {
  if (!SystemTable || !Info) return EFI_INVALID_PARAMETER;
  memset(Info, 0, sizeof(GU_ACPI_FACS_INFO));

  static const EFI_GUID Acpi20Guid = GU_EFI_ACPI_20_TABLE_GUID;
  static const EFI_GUID Acpi10Guid = GU_EFI_ACPI_10_TABLE_GUID;

  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *rsdp = NULL;

  /* 1. Search UEFI System Configuration Table for RSDP */
  Info->config_table_acpi20_idx = -1;
  Info->config_table_acpi10_idx = -1;

  for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
    if (GuidMatch(&SystemTable->ConfigurationTable[i].VendorGuid, &Acpi20Guid)) {
      if (Info->config_table_acpi20_idx < 0) {
        Info->config_table_acpi20_idx = (INTN)i;
        rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)SystemTable->ConfigurationTable[i].VendorTable;
      }
    } else if (GuidMatch(&SystemTable->ConfigurationTable[i].VendorGuid, &Acpi10Guid)) {
      if (Info->config_table_acpi10_idx < 0) {
        Info->config_table_acpi10_idx = (INTN)i;
        if (!rsdp) {
          rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)SystemTable->ConfigurationTable[i].VendorTable;
        }
      }
    }
  }

  /* Warm-Reset / Re-Entry Detection: If ConfigurationTable was already pointing to ShadowRSDP */
  if (rsdp != NULL && ((mShadowState.installed && (VOID *)rsdp == mShadowState.shadow_rsdp) ||
                       (mShadowState.installed && mShadowState.orig_rsdp != NULL && (VOID *)rsdp != mShadowState.orig_rsdp))) {
    if (mShadowState.installed && mShadowState.orig_rsdp != NULL) {
      GuLog("GuAcpiLocateFadtAndFacs: Existing Shadow RSDP detected at %#llx; recovered Real RSDP at %#llx\n",
            (unsigned long long)(UINTN)rsdp, (unsigned long long)(UINTN)mShadowState.orig_rsdp);
      rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)mShadowState.orig_rsdp;
    }
  }

  if (!rsdp) {
    GuLog("GuAcpiLocateFadtAndFacs: ACPI RSDP not found in ConfigurationTable\n");
    return EFI_NOT_FOUND;
  }

  if (rsdp->Signature != GU_ACPI_RSDP_SIGNATURE) {
    GuLog("GuAcpiLocateFadtAndFacs: Invalid RSDP signature (%#llx)\n", (unsigned long long)rsdp->Signature);
    return EFI_UNSUPPORTED;
  }

  Info->rsdp = rsdp;
  Info->xsdt_fadt_index = -1;
  Info->rsdt_fadt_index = -1;

  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *xsdt_fadt = NULL;
  EFI_PHYSICAL_ADDRESS xsdt_fadt_phys = 0;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *rsdt_fadt = NULL;
  EFI_PHYSICAL_ADDRESS rsdt_fadt_phys = 0;

  /* 2. Traverse XSDT (64-bit table pointers) if ACPI 2.0+ */
  if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
    EFI_ACPI_DESCRIPTION_HEADER *xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)rsdp->XsdtAddress;
    if (xsdt && xsdt->Length >= sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
      Info->xsdt = xsdt;
      Info->xsdt_phys = (EFI_PHYSICAL_ADDRESS)rsdp->XsdtAddress;
      Info->xsdt_original_checksum = xsdt->Checksum;
      UINTN count = (xsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT64);
      UINT8 *entry_ptr = ((UINT8 *)xsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
      for (UINTN i = 0; i < count; i++) {
        UINT64 table_phys = 0;
        memcpy(&table_phys, entry_ptr + i * sizeof(UINT64), sizeof(UINT64));
        EFI_ACPI_DESCRIPTION_HEADER *tbl = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)table_phys;
        if (tbl && tbl->Signature == GU_ACPI_FADT_SIGNATURE) {
          xsdt_fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)tbl;
          xsdt_fadt_phys = (EFI_PHYSICAL_ADDRESS)table_phys;
          Info->xsdt_fadt_index = (INTN)i;
          break;
        }
      }
    }
  }

  /* 3. Traverse RSDT (32-bit table pointers) */
  if (rsdp->RsdtAddress != 0) {
    EFI_ACPI_DESCRIPTION_HEADER *rsdt = (EFI_ACPI_DESCRIPTION_HEADER *)GuAcpiPaddrToPtr(rsdp->RsdtAddress, rsdp);
    if (rsdt && rsdt->Length >= sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
      Info->rsdt = rsdt;
      Info->rsdt_phys = (EFI_PHYSICAL_ADDRESS)(UINTN)rsdt;
      Info->rsdt_original_checksum = rsdt->Checksum;
      UINTN count = (rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);
      UINT8 *entry_ptr = ((UINT8 *)rsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
      for (UINTN i = 0; i < count; i++) {
        UINT32 table_phys = 0;
        memcpy(&table_phys, entry_ptr + i * sizeof(UINT32), sizeof(UINT32));
        EFI_ACPI_DESCRIPTION_HEADER *tbl = (EFI_ACPI_DESCRIPTION_HEADER *)GuAcpiPaddrToPtr(table_phys, rsdp);
        if (tbl && tbl->Signature == GU_ACPI_FADT_SIGNATURE) {
          rsdt_fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)tbl;
          rsdt_fadt_phys = (EFI_PHYSICAL_ADDRESS)(UINTN)tbl;
          Info->rsdt_fadt_index = (INTN)i;
          break;
        }
      }
    }
  }

  /*
   * Plan B Warm-Reset / Re-Entry Detection:
   * If XSDT already points to a low-memory table (<1MB, e.g. Shadow FADT at 0x9E400),
   * or differs from RSDT's FADT, recover the true original Real FADT from RSDT!
   */
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *fadt = NULL;
  EFI_PHYSICAL_ADDRESS fadt_phys = 0;

  if (xsdt_fadt_phys != 0 && xsdt_fadt_phys < 0x00100000ULL && rsdt_fadt != NULL) {
    GuLog("GuAcpiLocateFadtAndFacs: Existing Shadow FADT detected in XSDT at %#llx; recovered Real FADT from RSDT at %#llx\n",
          (unsigned long long)xsdt_fadt_phys, (unsigned long long)rsdt_fadt_phys);
    fadt = rsdt_fadt;
    fadt_phys = rsdt_fadt_phys;
  } else if (rsdt_fadt != NULL) {
    /* Prefer RSDT's FADT since 32-bit BIOS always follows RSDT */
    fadt = rsdt_fadt;
    fadt_phys = rsdt_fadt_phys;
  } else if (xsdt_fadt != NULL) {
    fadt = xsdt_fadt;
    fadt_phys = xsdt_fadt_phys;
  }

  if (!fadt) {
    GuLog("GuAcpiLocateFadtAndFacs: FADT ('FACP') table not found in XSDT/RSDT\n");
    return EFI_NOT_FOUND;
  }

  Info->fadt = fadt;
  Info->fadt_phys = fadt_phys;
  Info->fadt_original_checksum = fadt->Header.Checksum;

  /*
   * 4. Resolve active Real FACS address:
   * In normal boot, FirmwareCtrl points to Real FACS (e.g. 0xDC8BB000).
   * If XFirmwareCtrl is set and >= 0x00100000ULL, it could also point to Real FACS.
   * But if XFirmwareCtrl points to low memory (<1MB, e.g. 0x9E200 Shadow FACS from v1.8.37),
   * prefer FirmwareCtrl if it points to high memory (>= 1MB).
   */
  EFI_PHYSICAL_ADDRESS facs_phys = 0;
  if (fadt->FirmwareCtrl != 0 && (EFI_PHYSICAL_ADDRESS)fadt->FirmwareCtrl >= 0x00100000ULL) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)(UINTN)GuAcpiPaddrToPtr(fadt->FirmwareCtrl, rsdp);
  } else if (fadt->Header.Length >= 140 && fadt->XFirmwareCtrl >= 0x00100000ULL) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)fadt->XFirmwareCtrl;
  } else if (fadt->FirmwareCtrl != 0) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)(UINTN)GuAcpiPaddrToPtr(fadt->FirmwareCtrl, rsdp);
  } else if (fadt->Header.Length >= 140 && fadt->XFirmwareCtrl != 0) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)fadt->XFirmwareCtrl;
  }

  if (facs_phys == 0) {
    GuLog("GuAcpiLocateFadtAndFacs: FADT has NULL FirmwareCtrl / XFirmwareCtrl\n");
    return EFI_NOT_FOUND;
  }

  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(UINTN)facs_phys;

  if (facs->Signature != GU_ACPI_FACS_SIGNATURE) {
    GuLog("GuAcpiLocateFadtAndFacs: Invalid FACS signature at %#llx (%#x)\n",
          (unsigned long long)facs_phys, (unsigned)facs->Signature);
    return EFI_UNSUPPORTED;
  }

  /* Check if this FACS table was created by our driver during a previous warm boot */
  if (facs->Length >= 64) {
    UINT32 shadow_magic = 0;
    memcpy(&shadow_magic, facs->Reserved, sizeof(UINT32));
    if (shadow_magic == GU_SHADOW_FACS_SIGNATURE) {
      UINT64 orig_phys = 0;
      memcpy(&orig_phys, &facs->Reserved[4], sizeof(UINT64));
      if (orig_phys != 0) {
        EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *true_real =
            (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(UINTN)orig_phys;
        if (true_real && true_real->Signature == GU_ACPI_FACS_SIGNATURE) {
          GuLog("GuAcpiLocateFadtAndFacs: Existing Shadow FACS detected at %#llx; recovered Real FACS at %#llx\n",
                (unsigned long long)facs_phys, (unsigned long long)orig_phys);
          facs = true_real;
          facs_phys = orig_phys;
        }
      }
    }
  }

  Info->real_facs = facs;
  Info->real_facs_phys = facs_phys;
  if (mShadowState.installed && mShadowState.orig_waking_vector != 0 &&
      facs->FirmwareWakingVector == (UINT32)(UINTN)mShadowState.low_trampoline) {
    Info->original_firmware_waking_vector = mShadowState.orig_waking_vector;
    Info->original_xfirmware_waking_vector = mShadowState.orig_xfirmware_waking_vector;
  } else {
    Info->original_firmware_waking_vector = facs->FirmwareWakingVector;
    if (facs->Length >= 32) {
      Info->original_xfirmware_waking_vector = facs->XFirmwareWakingVector;
    } else {
      Info->original_xfirmware_waking_vector = 0;
    }
  }
  Info->found = TRUE;

  return EFI_SUCCESS;
}

UINT32 GuAcpiBuildTrampolineCode(
    OUT UINT8 *Buffer,
    IN UINT32 BufferSize,
    IN EFI_PHYSICAL_ADDRESS TrampolinePhys,
    IN EFI_PHYSICAL_ADDRESS ContextPhys,
    IN EFI_PHYSICAL_ADDRESS ShadowFacsPhys,
    IN EFI_PHYSICAL_ADDRESS TelemetryPhys,
    IN EFI_PHYSICAL_ADDRESS GpuBar0Phys,
    IN UINT32 RootPortLinkReg,
    IN UINT32 GpuPmcsrReg
) {
  if (!Buffer || BufferSize < 1024) return 0;
  /* Derive High ACPI base and target addresses */
  EFI_PHYSICAL_ADDRESS high_acpi_phys = (ShadowFacsPhys >= GU_HIGH_SHADOW_FACS_OFF) ?
      (ShadowFacsPhys - GU_HIGH_SHADOW_FACS_OFF) : ShadowFacsPhys;
  UINT32 shadow_wake_phys = (UINT32)(ShadowFacsPhys + 0x0C);
  UINT32 high_telem_dest = TelemetryPhys ? (UINT32)TelemetryPhys : (UINT32)(high_acpi_phys + 0x0500);
  UINT32 bar0_target = GpuBar0Phys ? (UINT32)GpuBar0Phys : 0xFB000000U;
  UINT32 booter_dma = ContextPhys ? (UINT32)(ContextPhys + 0x01000U) : 0x00021000U;
  UINT32 meta_dma = ContextPhys ? (UINT32)(ContextPhys + 0x2A000U) : 0x0004A000U;
  UINT32 booter_size = (GuAssets[0].firmware.size > 0) ? (UINT32)GuAssets[0].firmware.size : 57672U;
  UINT32 booter_sz_param = (booter_size & ~0xFFU) | 0x10U;
  (VOID)booter_size;
  (VOID)booter_sz_param;

  /* Pre-populate PCI probe addresses at +0x0520 (8 dwords = 32 bytes) */
  UINT32 *probe_addrs = (UINT32 *)(Buffer + 0x0520);
  probe_addrs[0] = 0x80001900U; /* Bridge 0:3:1 DevID (Expected 0x14831022) */
  probe_addrs[1] = 0x80001904U; /* Bridge 0:3:1 Cmd/Stat */
  probe_addrs[2] = 0x80001918U; /* Bridge 0:3:1 Bus Numbers (Pri, Sec, Sub) */
  probe_addrs[3] = RootPortLinkReg ? RootPortLinkReg : 0x80001960U; /* Bridge 0:3:1 Link Ctrl/Status (Cap 0x10) */
  probe_addrs[4] = 0x802D0000U; /* GPU 45:0:0 DevID (Expected 0x268410de if up) */
  probe_addrs[5] = 0x802D0004U; /* GPU 45:0:0 Cmd/Stat */
  probe_addrs[6] = 0x802D0010U; /* GPU 45:0:0 BAR0 */
  probe_addrs[7] = GpuPmcsrReg ? GpuPmcsrReg : 0x802D0064U; /* GPU 45:0:0 PMCSR (Cap 0x01) */

  /* Pre-populate GDT & IDTR descriptors at Buffer + GU_TRAMP_RM_IDTR_OFF (0x0600) (v1.8.53) */
  /* +0x0600: RM_IDTR (6 bytes) */
  Buffer[GU_TRAMP_RM_IDTR_OFF + 0] = 0xFF; Buffer[GU_TRAMP_RM_IDTR_OFF + 1] = 0x03; /* Limit = 0x03FF */
  Buffer[GU_TRAMP_RM_IDTR_OFF + 2] = 0x00; Buffer[GU_TRAMP_RM_IDTR_OFF + 3] = 0x00;
  Buffer[GU_TRAMP_RM_IDTR_OFF + 4] = 0x00; Buffer[GU_TRAMP_RM_IDTR_OFF + 5] = 0x00; /* Base = 0x00000000 */

  /* +0x0608: PM_GDTR (6 bytes) */
  *(UINT16 *)(Buffer + GU_TRAMP_PM_GDTR_OFF) = (UINT16)((5 * 8) - 1); /* Limit = 39 (0x0027) */
  *(UINT32 *)(Buffer + GU_TRAMP_PM_GDTR_OFF + 2) = (UINT32)(TrampolinePhys + GU_TRAMP_GDT_BASE_OFF); /* Base = TrampolinePhys + 0x0610 */

  /* +0x0610: GDT (5 descriptors = 40 bytes) */
  memset(Buffer + GU_TRAMP_GDT_BASE_OFF, 0, 8); /* Entry 0: 0x00 Null */

  /* Entry 1: 0x08 32-bit Flat Code (Base=0, Limit=4GB, G=1, D=1, P=1, DPL=0, Type=0x9A) */
  UINT8 *desc = Buffer + GU_TRAMP_GDT_BASE_OFF + 0x08;
  desc[0] = 0xFF; desc[1] = 0xFF; desc[2] = 0x00; desc[3] = 0x00;
  desc[4] = 0x00; desc[5] = 0x9A; desc[6] = 0xCF; desc[7] = 0x00;

  /* Entry 2: 0x10 32-bit Flat Data (Base=0, Limit=4GB, G=1, D=1, P=1, DPL=0, Type=0x92) */
  desc = Buffer + GU_TRAMP_GDT_BASE_OFF + 0x10;
  desc[0] = 0xFF; desc[1] = 0xFF; desc[2] = 0x00; desc[3] = 0x00;
  desc[4] = 0x00; desc[5] = 0x92; desc[6] = 0xCF; desc[7] = 0x00;

  /* Entry 3: 0x18 16-bit Code (Base=TrampolinePhys&0xFFFF0000, Limit=64KB, G=0, D=0, Type=0x9A) */
  UINT32 tramp_base16 = (UINT32)(TrampolinePhys & 0xFFFF0000ULL);
  desc = Buffer + GU_TRAMP_GDT_BASE_OFF + 0x18;
  desc[0] = 0xFF; desc[1] = 0xFF;
  desc[2] = (UINT8)(tramp_base16 & 0xFF);
  desc[3] = (UINT8)((tramp_base16 >> 8) & 0xFF);
  desc[4] = (UINT8)((tramp_base16 >> 16) & 0xFF);
  desc[5] = 0x9A; desc[6] = 0x00;
  desc[7] = (UINT8)((tramp_base16 >> 24) & 0xFF);

  /* Entry 4: 0x20 16-bit Data (Base=TrampolinePhys&0xFFFF0000, Limit=64KB, G=0, D=0, Type=0x92) */
  desc = Buffer + GU_TRAMP_GDT_BASE_OFF + 0x20;
  desc[0] = 0xFF; desc[1] = 0xFF;
  desc[2] = (UINT8)(tramp_base16 & 0xFF);
  desc[3] = (UINT8)((tramp_base16 >> 8) & 0xFF);
  desc[4] = (UINT8)((tramp_base16 >> 16) & 0xFF);
  desc[5] = 0x92; desc[6] = 0x00;
  desc[7] = (UINT8)((tramp_base16 >> 24) & 0xFF);

  UINT8 *e = Buffer + 0x000;

  *e++ = 0xFA; /* cli */
  *e++ = 0xFC; /* cld */
  *e++ = 0x8C; *e++ = 0xC8; /* mov ax, cs */
  *e++ = 0x8E; *e++ = 0xD8; /* mov ds, ax */
  *e++ = 0x89; *e++ = 0x26; *e++ = 0x70; *e++ = 0x05; /* mov [0x0570], sp */
  *e++ = 0x8C; *e++ = 0x16; *e++ = 0x72; *e++ = 0x05; /* mov [0x0572], ss */

  /* Set Telemetry Magic: mov dword [0x051C], 0x53335F54 ('S3_T') */
  *e++ = 0x66; *e++ = 0xC7; *e++ = 0x06; *e++ = 0x1C; *e++ = 0x05;
  *e++ = 0x54; *e++ = 0x5F; *e++ = 0x33; *e++ = 0x53;

  /* Telemetry Entry Checkpoint: mov word [0x0510], 0xAA01 */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x10; *e++ = 0x05; *e++ = 0x01; *e++ = 0xAA;

  /* Probe loop: si=0x0520, di=0x0540, cx=8 */
  *e++ = 0xBE; *e++ = 0x20; *e++ = 0x05; /* mov si, 0x0520 */
  *e++ = 0xBF; *e++ = 0x40; *e++ = 0x05; /* mov di, 0x0540 */
  *e++ = 0xB9; *e++ = 0x08; *e++ = 0x00; /* mov cx, 8 */
  UINT8 *pci_loop_start = e;
  *e++ = 0x66; *e++ = 0x8B; *e++ = 0x04; /* mov eax, [si] */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; /* mov dx, 0x0CF8 */
  *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; /* mov dx, 0x0CFC */
  *e++ = 0x66; *e++ = 0xED;             /* in eax, dx */
  *e++ = 0x66; *e++ = 0x89; *e++ = 0x05; /* mov [di], eax */
  *e++ = 0x83; *e++ = 0xC6; *e++ = 0x04; /* add si, 4 */
  *e++ = 0x83; *e++ = 0xC7; *e++ = 0x04; /* add di, 4 */
  *e++ = 0xE2;                           /* loop */
  INT8 loop_disp = (INT8)(pci_loop_start - (e + 1));
  *e++ = (UINT8)loop_disp;

  /* Check if Bridge DevID is AMD (Vendor ID 0x1022 at [0x0540]) */
  *e++ = 0xA1; *e++ = 0x40; *e++ = 0x05; /* mov ax, [0x0540] */
  *e++ = 0x3D; *e++ = 0x22; *e++ = 0x10; /* cmp ax, 0x1022 */
  *e++ = 0x0F; *e++ = 0x85;              /* jne near .skip_bridge (4 bytes) */
  UINT8 *skip_bridge_disp = e;
  *e++ = 0x00; *e++ = 0x00;              /* placeholder rel16 */

  /*
   * 1. Load original Bus Numbers from [0x0548], preserve Latency Timer (bits 31:24),
   * set Pri=0, Sec=0x2D, Sub=0x2D:
   */
  *e++ = 0x66; *e++ = 0xA1; *e++ = 0x48; *e++ = 0x05;             /* mov eax, [0x0548] */
  *e++ = 0x66; *e++ = 0x25; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; *e++ = 0xFF; /* and eax, 0xFF000000 */
  *e++ = 0x66; *e++ = 0x0D; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x2D; *e++ = 0x00; /* or eax, 0x002D2D00 */

  /* Write new bus numbers to Root Port 0:3:1 offset 0x18 (0x80001918) */
  *e++ = 0x66; *e++ = 0x89; *e++ = 0xC1;                         /* mov ecx, eax (save in ECX, 100% stack-free) */
  *e++ = 0x66; *e++ = 0xB8; *e++ = 0x18; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80; /* mov eax, 0x80001918 */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C;                         /* mov dx, 0x0CF8 */
  *e++ = 0x66; *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0x66; *e++ = 0x89; *e++ = 0xC8;                         /* mov eax, ecx (restore from ECX, 100% stack-free) */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C;                         /* mov dx, 0x0CFC */
  *e++ = 0x66; *e++ = 0xEF;                                       /* out dx, eax */

  /* 2. Compact loop: probe GPU registers 45:0:0 into [0x0560..0x056C] */
  *e++ = 0xBE; *e++ = 0x30; *e++ = 0x05; /* mov si, 0x0530 (probe_addrs[4..7]) */
  *e++ = 0xBF; *e++ = 0x60; *e++ = 0x05; /* mov di, 0x0560 */
  *e++ = 0xB9; *e++ = 0x04; *e++ = 0x00; /* mov cx, 4 */
  UINT8 *pci_loop2_start = e;
  *e++ = 0x66; *e++ = 0x8B; *e++ = 0x04; /* mov eax, [si] */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; /* mov dx, 0x0CF8 */
  *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; /* mov dx, 0x0CFC */
  *e++ = 0x66; *e++ = 0xED;             /* in eax, dx */
  *e++ = 0x66; *e++ = 0x89; *e++ = 0x05; /* mov [di], eax */
  *e++ = 0x83; *e++ = 0xC6; *e++ = 0x04; /* add si, 4 */
  *e++ = 0x83; *e++ = 0xC7; *e++ = 0x04; /* add di, 4 */
  *e++ = 0xE2;                           /* loop pci_loop2_start */
  INT8 loop2_disp = (INT8)(pci_loop2_start - (e + 1));
  *e++ = (UINT8)loop2_disp;

  /*
   * NOTE: Root Port offset 0x18 remains configured as Sec=0x2D, Sub=0x2D
   * throughout 32-bit protected mode. It is restored to original [0x0548]
   * during 32-bit PCIe teardown before dropping back to 16-bit real mode.
   */

  /* Patch .skip_bridge near jne displacement (relative to end of 4-byte instruction) */
  INT32 skip_disp = (INT32)(e - (skip_bridge_disp + 2));
  skip_bridge_disp[0] = (UINT8)(skip_disp & 0xFF);
  skip_bridge_disp[1] = (UINT8)((skip_disp >> 8) & 0xFF);

  /*
   * 4. 16->32->16-Bit Flat Mode Bridge (v1.8.46 / v1.8.47):
   * Enables access to High Memory (HighAcpiPage > 1MB) for ShadowFACS waking vector
   * reading, 128-byte telemetry mirroring, and temporary BAR0 hardware rate-limiter unlock.
   */

  /* In 16-bit Real Mode: Load GDTR from CS:0x0608 (TrampolinePhys + GU_TRAMP_PM_GDTR_OFF) (v1.8.53) */
  *e++ = 0x2E; *e++ = 0x0F; *e++ = 0x01; *e++ = 0x16; *e++ = 0x08; *e++ = 0x06; /* lgdt [cs:0x0608] */

  /* Enter Protected Mode: CR0.PE = 1 */
  *e++ = 0x66; *e++ = 0x0F; *e++ = 0x20; *e++ = 0xC0; /* mov eax, cr0 */
  *e++ = 0x0C; *e++ = 0x01;                         /* or al, 1 */
  *e++ = 0x66; *e++ = 0x0F; *e++ = 0x22; *e++ = 0xC0; /* mov cr0, eax */

  /* Far jump to 32-bit flat code segment (CS=0x0008) */
  *e++ = 0x66; *e++ = 0xEA;                         /* jmp ptr16:32 */
  UINT8 *pm32_jmp_target = e;
  *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; /* placeholder dword offset */
  *e++ = 0x08; *e++ = 0x00;                         /* Selector 0x0008 */

  /* ==================== 32-BIT FLAT PROTECTED MODE (BITS 32) ==================== */
  UINT8 *pm32_start = e;
  UINT32 pm32_linear = (UINT32)(TrampolinePhys + (UINT32)(pm32_start - Buffer));
  *(UINT32 *)pm32_jmp_target = pm32_linear;

  /* Load 32-bit flat data selectors (0x0010): DS, ES, FS, GS, SS */
  *e++ = 0x66; *e++ = 0xB8; *e++ = 0x10; *e++ = 0x00; /* mov ax, 0x0010 */
  *e++ = 0x8E; *e++ = 0xD8;                         /* mov ds, ax */
  *e++ = 0x8E; *e++ = 0xC0;                         /* mov es, ax */
  *e++ = 0x8E; *e++ = 0xE0;                         /* mov fs, ax */
  *e++ = 0x8E; *e++ = 0xE8;                         /* mov gs, ax */
  *e++ = 0x8E; *e++ = 0xD0;                         /* mov ss, ax */

  /* Diagnostic Marker 0x324B4F21 ('!OK2') directly to HighAcpiPage + 0x500 (v1.8.46b) */
  *e++ = 0xC7; *e++ = 0x05;                         /* mov dword [disp32], imm32 */
  *(UINT32 *)e = high_telem_dest; e += 4;
  *e++ = 0x21; *e++ = 0x4F; *e++ = 0x4B; *e++ = 0x32; /* 0x324B4F21 ('!OK2') */

  /* Load Real-Mode IDTR (Limit=0x3FF, Base=0) while in 32-bit mode (v1.8.53: TrampolinePhys + GU_TRAMP_RM_IDTR_OFF) */
  *e++ = 0x0F; *e++ = 0x01; *e++ = 0x1D;             /* lidt [disp32] */
  *(UINT32 *)e = (UINT32)(TrampolinePhys + GU_TRAMP_RM_IDTR_OFF); e += 4; /* TrampolinePhys + 0x0600 */

  /* Task (a): Read real Windows waking vector from ShadowFACS + 0x0C */
  *e++ = 0x8B; *e++ = 0x35;                         /* mov esi, [disp32] */
  *(UINT32 *)e = shadow_wake_phys; e += 4;

  /* Check for zero vector and fallback to HalpRMStub (0x00001000) */
  *e++ = 0x85; *e++ = 0xF6;                         /* test esi, esi */
  *e++ = 0x75; *e++ = 0x05;                         /* jnz .have_vec32 */
  *e++ = 0xBE; *e++ = 0x00; *e++ = 0x10; *e++ = 0x00; *e++ = 0x00; /* mov esi, 0x00001000 */

  /* Save captured vector to TrampolinePhys + 0x0514 */
  *e++ = 0x89; *e++ = 0x35;                         /* mov [disp32], esi */
  *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0514); e += 4;

  /* Convert linear ESI into Segment:Offset */
  *e++ = 0x89; *e++ = 0xF0;                         /* mov eax, esi */
  *e++ = 0x83; *e++ = 0xE0; *e++ = 0x0F;             /* and eax, 0x0F (Offset = AX & 0x0F) */
  *e++ = 0xC1; *e++ = 0xEE; *e++ = 0x04;             /* shr esi, 4    (Segment = ESI >> 4) */

  /* Pack Segment:Offset into EAX: Offset in low 16, Segment in high 16 */
  *e++ = 0xC1; *e++ = 0xE6; *e++ = 0x10;             /* shl esi, 16 */
  *e++ = 0x09; *e++ = 0xF0;                         /* or eax, esi */

  /* Store far pointer into TrampolinePhys + 0x0500 and Telemetry + 0x0518 */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0500); e += 4; /* mov [TrampolinePhys+0x0500], eax */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0518); e += 4; /* mov [TrampolinePhys+0x0518], eax */

  /* Write Exit Checkpoint 0xAA02 to TrampolinePhys + 0x0512 BEFORE telemetry copy */
  *e++ = 0x66; *e++ = 0xC7; *e++ = 0x05;             /* mov word [disp32], 0xAA02 */
  *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0512); e += 4;
  *e++ = 0x02; *e++ = 0xAA;

  /* Task (b): Copy 31 DWORDs (+0x0504..+0x057F) to HighAcpiPage + 0x504, preserving Marker at +0x500 */
  *e++ = 0xBE;                                       /* mov esi, imm32 */
  *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0504); e += 4;
  *e++ = 0xBF;                                       /* mov edi, imm32 */
  *(UINT32 *)e = (UINT32)(high_telem_dest + 0x04); e += 4;
  *e++ = 0xB9; *e++ = 0x1F; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; /* mov ecx, 31 (124 bytes) */
  *e++ = 0xFC;                                       /* cld */
  *e++ = 0xF3; *e++ = 0xA5;                         /* rep movsd */

  /* ==================== S3 WAKE HARDWARE UNLOCK VIA TEMPORARY BAR0 (v1.8.47) ==================== */
  /* Record target BAR0 address at high_telem_dest + 0x8C */
  *e++ = 0xC7; *e++ = 0x05;
  *(UINT32 *)e = (UINT32)(high_telem_dest + 0x8C); e += 4;
  *(UINT32 *)e = bar0_target; e += 4;

  /* 1. Read Root Port 0:3:1 Command register (0x80001904) and enable MSE (bit 1) + BME (bit 2) */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; *e++ = 0x00; *e++ = 0x00; /* mov edx, 0x0CF8 */
  *e++ = 0xB8; *e++ = 0x04; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80; /* mov eax, 0x80001904 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; *e++ = 0x00; *e++ = 0x00; /* mov edx, 0x0CFC */
  *e++ = 0xED;                                       /* in eax, dx */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xB0); e += 4; /* mov [dest+0xB0], eax (save rp_cmd) */
  *e++ = 0x0C; *e++ = 0x06;                         /* or al, 0x06 (MSE | BME) */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* 2. Read Root Port 0:3:1 Memory Base/Limit (0x80001920) and assign temporary window 0xFC00FB00 */
  *e++ = 0xB2; *e++ = 0xF8;                         /* mov dl, 0xF8 */
  *e++ = 0xB8; *e++ = 0x20; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80; /* mov eax, 0x80001920 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xED;                                       /* in eax, dx */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xB4); e += 4; /* mov [dest+0xB4], eax (save rp_mem) */
  *e++ = 0xB8; *e++ = 0x00; *e++ = 0xFB; *e++ = 0x00; *e++ = 0xFC; /* mov eax, 0xFC00FB00 */
  *e++ = 0xEF;                                       /* out dx, eax (unconditionally set window 0xFB00..0xFC00) */

  /* 3. Assign GPU BAR0 = bar0_target (0x802D0010) */
  *e++ = 0xB2; *e++ = 0xF8;                         /* mov dl, 0xF8 */
  *e++ = 0xB8; *e++ = 0x10; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x80; /* mov eax, 0x802D0010 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xED;                                       /* in eax, dx */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xBC); e += 4; /* mov [dest+0xBC], eax (save gpu_bar0) */
  *e++ = 0xB8; *(UINT32 *)e = bar0_target; e += 4;   /* mov eax, bar0_target */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* 4. Enable GPU Command Memory Space Enable (bit 1) + Bus Master Enable (bit 2) (0x802D0004) */
  *e++ = 0xB2; *e++ = 0xF8;                         /* mov dl, 0xF8 */
  *e++ = 0xB8; *e++ = 0x04; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x80; /* mov eax, 0x802D0004 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xED;                                       /* in eax, dx */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xB8); e += 4; /* mov [dest+0xB8], eax (save gpu_cmd) */
  *e++ = 0x0C; *e++ = 0x06;                         /* or al, 0x06 (MSE | BME) */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* 5. MMIO Operations via Base in EBX */
  *e++ = 0xBB; *(UINT32 *)e = bar0_target; e += 4;   /* mov ebx, bar0_target */

  /* Read Chip ID from BAR0 + 0x00 */
  *e++ = 0x8B; *e++ = 0x03;                         /* mov eax, [ebx] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x88); e += 4; /* mov [dest+0x88], eax */

  /* Check for unmapped MMIO (0xFFFFFFFF or 0xBADFxxxx) */
  *e++ = 0x83; *e++ = 0xF8; *e++ = 0xFF;             /* cmp eax, -1 (0xFFFFFFFF) */
  UINT8 *fault_jmp1 = e;
  *e++ = 0x0F; *e++ = 0x84; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; /* jz near .fault */

  *e++ = 0x89; *e++ = 0xC2;                         /* mov edx, eax */
  *e++ = 0xC1; *e++ = 0xEA; *e++ = 0x10;             /* shr edx, 16 */
  *e++ = 0x81; *e++ = 0xFA; *e++ = 0xDF; *e++ = 0xBA; *e++ = 0x00; *e++ = 0x00; /* cmp edx, 0x0000BADF */
  UINT8 *fault_jmp2 = e;
  *e++ = 0x0F; *e++ = 0x84; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; /* jz near .fault */

  /* ==================== SEC2 DIAGNOSTICS & RESET CYCLE (v1.8.51) ==================== */
  /* 1. Capture initial telemetry starting at +0xC0 */
  /* +0xC0: PMC_ENABLE (BAR0 + 0x00000200) */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00000200U; e += 4; /* mov eax, [ebx + 0x00000200] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xC0); e += 4; /* mov [dest+0xC0], eax */

  /* +0xC4: SEC2_RESET (BAR0 + 0x008403C0) */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov eax, [ebx + 0x008403C0] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xC4); e += 4; /* mov [dest+0xC4], eax */

  /* +0xC8: SEC2_HWCFG2 (BAR0 + 0x008400F4) */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008400F4U; e += 4; /* mov eax, [ebx + 0x008400F4] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xC8); e += 4; /* mov [dest+0xC8], eax */

  /* +0xCC: FALCON2_BCR (BAR0 + 0x00841668) */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00841668U; e += 4; /* mov eax, [ebx + 0x00841668] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xCC); e += 4; /* mov [dest+0xCC], eax */

  /* 2. Execute Falcon Reset Cycle (subroutine with return pointer in EBP) */
  *e++ = 0xBD; /* mov ebp, imm32 */
  UINT8 *first_ret_target = e;
  *(UINT32 *)e = 0; e += 4;

  /* ==================== SEC2 ENGINE RESET SUBROUTINE ==================== */
  /* Stack-free register-linked subroutine: called via `mov ebp, ret_target; jmp near .sec2_sub_start` */
  UINT8 *sec2_sub_start = e;
  /* 1. Assert reset: Read 0x008403C0, assert reset (or al, 1), write back */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov eax, [ebx + 0x008403C0] */
  *e++ = 0x0C; *e++ = 0x01;                                     /* or al, 0x01 */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov [ebx + 0x008403C0], eax */

  /* 2. 10 dummy reads of 0x008403C0 for signal propagation (unlock_610.c:1630) */
  *e++ = 0x31; *e++ = 0xC9;                         /* xor ecx, ecx */
  *e++ = 0xB1; *e++ = 0x0A;                         /* mov cl, 10 */
  UINT8 *dummy_loop1 = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov eax, [ebx + 0x008403C0] */
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz dummy_loop1 */
  *e++ = (UINT8)(dummy_loop1 - (e + 1));

  /* 3. Poll reset assert: [ebx + 0x008403C0] & 1 == 1 (unlock_610.c:1642) */
  *e++ = 0xB1; *e++ = 0x64;                         /* mov cl, 100 */
  UINT8 *assert_poll_start = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov eax, [ebx + 0x008403C0] */
  *e++ = 0xA8; *e++ = 0x01;                                     /* test al, 0x01 */
  *e++ = 0x75;                                                 /* jnz .assert_done */
  UINT8 *assert_break = e++;
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz assert_poll_start */
  *e++ = (UINT8)(assert_poll_start - (e + 1));
  *assert_break = (UINT8)(e - (assert_break + 1));

  /* 4. Release reset: read 0x008403C0, and al, ~1, write back (unlock_610.c:1651) */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov eax, [ebx + 0x008403C0] */
  *e++ = 0x24; *e++ = 0xFE;                                     /* and al, 0xFE */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov [ebx + 0x008403C0], eax */

  /* 5. 10 dummy reads of 0x008403C0 after reset release (unlock_610.c:1652) */
  *e++ = 0xB1; *e++ = 0x0A;                         /* mov cl, 10 */
  UINT8 *dummy_loop2 = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov eax, [ebx + 0x008403C0] */
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz dummy_loop2 */
  *e++ = (UINT8)(dummy_loop2 - (e + 1));

  /* 6. Poll reset release: [ebx + 0x008403C0] & 1 == 0 (unlock_610.c:1660) */
  *e++ = 0xB1; *e++ = 0x64;                         /* mov cl, 100 */
  UINT8 *rel_poll_start = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008403C0U; e += 4; /* mov eax, [ebx + 0x008403C0] */
  *e++ = 0xA8; *e++ = 0x01;                                     /* test al, 0x01 */
  *e++ = 0x74;                                                 /* jz .rel_done */
  UINT8 *rel_break = e++;
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz rel_poll_start */
  *e++ = (UINT8)(rel_poll_start - (e + 1));
  *rel_break = (UINT8)(e - (rel_break + 1));

  /* 7. Poll 0x008400F4 Bit 12 (Memory Scrubbing Active) == 0 with bounded timeout (unlock_610.c:1666) */
  *e++ = 0xB9; *e++ = 0x50; *e++ = 0xC3; *e++ = 0x00; *e++ = 0x00; /* mov ecx, 50000 */
  UINT8 *scrub_poll_start = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x008400F4U; e += 4; /* mov eax, [ebx + 0x008400F4] */
  *e++ = 0xA9; *e++ = 0x00; *e++ = 0x10; *e++ = 0x00; *e++ = 0x00; /* test eax, 0x00001000 */
  *e++ = 0x74;                                                 /* jz .scrub_done */
  UINT8 *scrub_break = e++;
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz scrub_poll_start */
  *e++ = (UINT8)(scrub_poll_start - (e + 1));
  *scrub_break = (UINT8)(e - (scrub_break + 1));

  /* Return to caller via EBP */
  *e++ = 0xFF; *e++ = 0xE5;                                     /* jmp ebp */

  /* .after_first_reset: */
  *(UINT32 *)first_ret_target = (UINT32)(TrampolinePhys + (UINT32)(e - Buffer));

  /* 3. Switch Falcon out of Boot-ROM mode: write 0 to FALCON2_BCR (BAR0 + 0x00841668) (v1.8.52) */
  *e++ = 0x31; *e++ = 0xC0;                                     /* xor eax, eax */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00841668U; e += 4; /* mov [ebx + 0x00841668], eax */

  /* Poll [ebx + 0x00841668] & 0x11 == 1 with bounded timeout (50,000 iterations) (v1.8.52) */
  *e++ = 0xB9; *e++ = 0x50; *e++ = 0xC3; *e++ = 0x00; *e++ = 0x00; /* mov ecx, 50000 */
  UINT8 *bcr_poll_start = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00841668U; e += 4; /* mov eax, [ebx + 0x00841668] */
  *e++ = 0x89; *e++ = 0xC2;                                     /* mov edx, eax */
  *e++ = 0x80; *e++ = 0xE2; *e++ = 0x11;                         /* and dl, 0x11 */
  *e++ = 0x80; *e++ = 0xFA; *e++ = 0x01;                         /* cmp dl, 0x01 */
  *e++ = 0x74;                                                 /* jz .bcr_done */
  UINT8 *bcr_break = e++;
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz bcr_poll_start */
  *e++ = (UINT8)(bcr_poll_start - (e + 1));
  *bcr_break = (UINT8)(e - (bcr_break + 1));

  /* Save post-switch FALCON2_BCR readback value to +0xD4 (v1.8.52) */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xD4); e += 4; /* mov [dest+0xD4], eax */

  /* Write Chip-ID to 0x00840084 */
  *e++ = 0x8B; *e++ = 0x03;                                     /* mov eax, [ebx] */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00840084U; e += 4; /* mov [ebx + 0x00840084], eax */

  /* Read 0x00840100 (CPUCTL) after BCR switch & reset cycle and save to +0xD0 */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840100U; e += 4; /* mov eax, [ebx + 0x00840100] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xD0); e += 4; /* mov [dest+0xD0], eax */

  /* 4. FBIF & Booter Chunked DMA Transfers, Falcon2 Config, CPU Start and Bounded Polling (v1.8.54) */
  /* FBIF Trans Enable: [ebx + 0x00840624] |= 0x80 */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840624U; e += 4; /* mov eax, [ebx + 0x00840624] */
  *e++ = 0x0C; *e++ = 0x80;                                     /* or al, 0x80 */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00840624U; e += 4; /* mov [ebx + 0x00840624], eax */

  /* Clear Falcon IRQ Status: [ebx + 0x0084010C] = 0 */
  *e++ = 0x31; *e++ = 0xC0;                                     /* xor eax, eax */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x0084010CU; e += 4; /* mov [ebx + 0x0084010C], eax */

  /* FBIF Trans Mode Setup: [ebx + 0x00840600] = ([ebx + 0x00840600] & ~7) | 5 */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840600U; e += 4; /* mov eax, [ebx + 0x00840600] */
  *e++ = 0x24; *e++ = 0xF8;                                     /* and al, 0xF8 */
  *e++ = 0x0C; *e++ = 0x05;                                     /* or al, 0x05 */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00840600U; e += 4; /* mov [ebx + 0x00840600], eax */

  /* Pass 1 Setup (IMEM):
   * [ebx + 0x00840110] = booter_dma >> 8
   * EDI = 0 (Dest inside IMEM)
   * EDX = 256 (Offset in buffer ab 256)
   * EBP = 125 (125 Chunks = 32,000 Bytes)
   * ESI = 0x0614 (Cmd IMEM)
   */
  *e++ = 0xB8; *(UINT32 *)e = (UINT32)(booter_dma >> 8); e += 4; /* mov eax, booter_dma >> 8 */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00840110U; e += 4; /* mov [ebx + 0x00840110], eax */

  /* Booter DMA address high: [ebx + 0x00840128] = 0 */
  *e++ = 0x31; *e++ = 0xC0;                                     /* xor eax, eax */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00840128U; e += 4; /* mov [ebx + 0x00840128], eax */

  *e++ = 0x31; *e++ = 0xFF;                                     /* xor edi, edi */
  *e++ = 0xBA; *(UINT32 *)e = 256U; e += 4;                     /* mov edx, 256 */
  *e++ = 0xBD; *(UINT32 *)e = 125U; e += 4;                     /* mov ebp, 125 */
  *e++ = 0xBE; *(UINT32 *)e = 0x0614U; e += 4;                  /* mov esi, 0x0614 */

  /* .dma_loop_entry: */
  UINT8 *dma_loop_entry = e;

  /* Poll DMA Idle: [ebx + 0x00840118] & 1 == 0 (with bounded timeout 50,000) */
  *e++ = 0xB9; *e++ = 0x50; *e++ = 0xC3; *e++ = 0x00; *e++ = 0x00; /* mov ecx, 50000 */
  UINT8 *dma_idle_poll = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840118U; e += 4; /* mov eax, [ebx + 0x00840118] */
  *e++ = 0xA8; *e++ = 0x01;                                     /* test al, 0x01 */
  *e++ = 0x74;                                                 /* jz .dma_idle_ready */
  UINT8 *dma_idle_break = e++;
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz dma_idle_poll */
  *e++ = (UINT8)(dma_idle_poll - (e + 1));
  *dma_idle_break = (UINT8)(e - (dma_idle_break + 1));

  /* Write Dest, Offset, Cmd */
  *e++ = 0x89; *e++ = 0xBB; *(UINT32 *)e = 0x00840114U; e += 4; /* mov [ebx + 0x00840114], edi */
  *e++ = 0x89; *e++ = 0x93; *(UINT32 *)e = 0x0084011CU; e += 4; /* mov [ebx + 0x0084011C], edx */
  *e++ = 0x89; *e++ = 0xB3; *(UINT32 *)e = 0x00840118U; e += 4; /* mov [ebx + 0x00840118], esi */

  /* Advance pointers by 256 bytes per chunk */
  *e++ = 0x81; *e++ = 0xC7; *(UINT32 *)e = 256U; e += 4;        /* add edi, 256 */
  *e++ = 0x81; *e++ = 0xC2; *(UINT32 *)e = 256U; e += 4;        /* add edx, 256 */
  *e++ = 0x4D;                                                 /* dec ebp */
  *e++ = 0x75;                                                 /* jnz .dma_loop_entry */
  *e++ = (UINT8)(dma_loop_entry - (e + 1));

  /* Poll DMA Complete: [ebx + 0x00840118] & 2 == 2 (bounded timeout 50,000) (analog to unlock_610.c:556) */
  *e++ = 0xB9; *e++ = 0x50; *e++ = 0xC3; *e++ = 0x00; *e++ = 0x00; /* mov ecx, 50000 */
  UINT8 *dma_comp_poll = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840118U; e += 4; /* mov eax, [ebx + 0x00840118] */
  *e++ = 0xA8; *e++ = 0x02;                                     /* test al, 0x02 */
  *e++ = 0x75;                                                 /* jnz .dma_comp_done */
  UINT8 *dma_comp_break = e++;
  *e++ = 0x49;                                                 /* dec ecx */
  *e++ = 0x75;                                                 /* jnz dma_comp_poll */
  *e++ = (UINT8)(dma_comp_poll - (e + 1));
  *dma_comp_break = (UINT8)(e - (dma_comp_break + 1));

  /* Check if IMEM completed (ESI == 0x0614). If so, reload for DMEM (Count=96, Offset=0, Cmd=0x0600) */
  *e++ = 0x66; *e++ = 0x81; *e++ = 0xFE; *e++ = 0x14; *e++ = 0x06; /* cmp si, 0x0614 */
  *e++ = 0x75;                                                 /* jne .dma_transfers_done */
  UINT8 *dma_done_jmp = e++;
  *e++ = 0xB8; *(UINT32 *)e = (UINT32)((booter_dma + 32256U) >> 8); e += 4; /* mov eax, (booter_dma + 32256) >> 8 */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00840110U; e += 4; /* mov [ebx + 0x00840110], eax */
  *e++ = 0x31; *e++ = 0xFF;                                     /* xor edi, edi */
  *e++ = 0x31; *e++ = 0xD2;                                     /* xor edx, edx */
  *e++ = 0xBD; *(UINT32 *)e = 96U; e += 4;                      /* mov ebp, 96 */
  *e++ = 0xBE; *(UINT32 *)e = 0x0600U; e += 4;                  /* mov esi, 0x0600 */
  *e++ = 0xEB;                                                 /* jmp short .dma_loop_entry */
  *e++ = (UINT8)(dma_loop_entry - (e + 1));
  *dma_done_jmp = (UINT8)(e - (dma_done_jmp + 1));

  /* Falcon2 Registers */
  /* [ebx + 0x00841210] = 16 (sig_dmem) */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x00841210U; e += 4;
  *(UINT32 *)e = 16U; e += 4;

  /* [ebx + 0x0084119C] = 1 (engine_mask) */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x0084119CU; e += 4;
  *(UINT32 *)e = 1U; e += 4;

  /* [ebx + 0x00841198] = 3 (ucode_id) */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x00841198U; e += 4;
  *(UINT32 *)e = 3U; e += 4;

  /* [ebx + 0x00841180] = 1 */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x00841180U; e += 4;
  *(UINT32 *)e = 1U; e += 4;

  /* Entry Point Vector: [ebx + 0x00840104] = 256 (code_va) */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x00840104U; e += 4;
  *(UINT32 *)e = 256U; e += 4;

  /* Mailbox 0: [ebx + 0x00840040] = meta_dma */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x00840040U; e += 4;
  *(UINT32 *)e = meta_dma; e += 4;

  /* Mailbox 1: [ebx + 0x00840044] = 0 */
  *e++ = 0x31; *e++ = 0xC0;                                     /* xor eax, eax */
  *e++ = 0x89; *e++ = 0x83; *(UINT32 *)e = 0x00840044U; e += 4; /* mov [ebx + 0x00840044], eax */

  /* Start Falcon CPU: [ebx + 0x00840100] = 2 */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x00840100U; e += 4; /* mov dword [ebx + 0x00840100], 2 */
  *(UINT32 *)e = 2U; e += 4;

  /* Bounded Polling on Mailbox 0 == 0x67b00001 (GU_AD102_MARKER) (100,000 iterations)
   * Only sample CPUCTL Halt (Bit 4) if Mailbox 0 != meta_dma to avoid premature abort on stale Halt bit
   */
  *e++ = 0xB9; *e++ = 0xA0; *e++ = 0x86; *e++ = 0x01; *e++ = 0x00; /* mov ecx, 100000 */
  UINT8 *halt_poll_start = e;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840040U; e += 4;   /* mov eax, [ebx + 0x00840040] (Mailbox 0) */
  *e++ = 0x3D; *(UINT32 *)e = 0x67B00001U; e += 4;               /* cmp eax, 0x67B00001 */
  *e++ = 0x74;                                                   /* jz .halt_done */
  UINT8 *halt_break1 = e++;
  *e++ = 0x3D; *(UINT32 *)e = meta_dma; e += 4;                   /* cmp eax, meta_dma */
  *e++ = 0x74;                                                   /* jz .continue_poll */
  UINT8 *halt_continue = e++;
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840100U; e += 4;   /* mov eax, [ebx + 0x00840100] (CPUCTL) */
  *e++ = 0xA8; *e++ = 0x10;                                       /* test al, 0x10 */
  *e++ = 0x75;                                                   /* jnz .halt_done */
  UINT8 *halt_break2 = e++;
  *halt_continue = (UINT8)(e - (halt_continue + 1));              /* .continue_poll target */
  *e++ = 0x49;                                                   /* dec ecx */
  *e++ = 0x75;                                                   /* jnz halt_poll_start */
  *e++ = (UINT8)(halt_poll_start - (e + 1));
  *halt_break1 = (UINT8)(e - (halt_break1 + 1));                  /* .halt_done target */
  *halt_break2 = (UINT8)(e - (halt_break2 + 1));

  /* Save post-booter SEC2_CPUCTL to +0xD8 */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840100U; e += 4;   /* mov eax, [ebx + 0x00840100] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + GU_TELEM_BOOTER_CPUCTL_POST_OFF); e += 4; /* mov [dest+0xD8], eax */

  /* Save post-booter Mailbox 0 / Marker to +0xDC (v1.8.54) */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00840040U; e += 4;   /* mov eax, [ebx + 0x00840040] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + GU_TELEM_BOOTER_MARKER_POST_OFF); e += 4; /* mov [dest+0xDC], eax */

  /* Read pre-unlock registers: SS0 (0x82381C), SS1 (0x823820), PLM (0x823804), Readout1 (0x823818) */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x0082381CU; e += 4; /* mov eax, [ebx + 0x0082381C] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x90); e += 4; /* mov [dest+0x90], eax */

  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00823820U; e += 4; /* mov eax, [ebx + 0x00823820] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x94); e += 4; /* mov [dest+0x94], eax */

  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00823804U; e += 4; /* mov eax, [ebx + 0x00823804] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x98); e += 4; /* mov [dest+0x98], eax */

  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00823818U; e += 4; /* mov eax, [ebx + 0x00823818] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x9C); e += 4; /* mov [dest+0x9C], eax */

  /* Perform Rate-Limiter Unlock Writes */
  /* mov dword [ebx + 0x0082381C], 0x88888888 */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x0082381CU; e += 4;
  *e++ = 0x88; *e++ = 0x88; *e++ = 0x88; *e++ = 0x88;

  /* mov dword [ebx + 0x00823820], 0x00000008 */
  *e++ = 0xC7; *e++ = 0x83; *(UINT32 *)e = 0x00823820U; e += 4;
  *e++ = 0x08; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00;

  /* Read back post-unlock registers */
  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x0082381CU; e += 4; /* mov eax, [ebx + 0x0082381C] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xA0); e += 4; /* mov [dest+0xA0], eax */
  *e++ = 0x89; *e++ = 0xC1;                         /* mov ecx, eax */

  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00823820U; e += 4; /* mov eax, [ebx + 0x00823820] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xA4); e += 4; /* mov [dest+0xA4], eax */
  *e++ = 0x89; *e++ = 0xC2;                         /* mov edx, eax */

  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00823818U; e += 4; /* mov eax, [ebx + 0x00823818] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xA8); e += 4; /* mov [dest+0xA8], eax */

  *e++ = 0x8B; *e++ = 0x83; *(UINT32 *)e = 0x00823804U; e += 4; /* mov eax, [ebx + 0x00823804] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xAC); e += 4; /* mov [dest+0xAC], eax */

  /* Evaluate Status: SS0 == 0x88888888 && (SS1 & 0xF) == 0x8 -> Status = 1, else 2 */
  *e++ = 0xB8; *e++ = 0x02; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; /* mov eax, 2 (Mismatch) */
  *e++ = 0x81; *e++ = 0xF9; *e++ = 0x88; *e++ = 0x88; *e++ = 0x88; *e++ = 0x88; /* cmp ecx, 0x88888888 */
  UINT8 *stat_mismatch1 = e;
  *e++ = 0x75; *e++ = 0x00;                         /* jne .stat_done */
  *e++ = 0x83; *e++ = 0xE2; *e++ = 0x0F;             /* and edx, 0x0F */
  *e++ = 0x83; *e++ = 0xFA; *e++ = 0x08;             /* cmp edx, 0x08 */
  UINT8 *stat_mismatch2 = e;
  *e++ = 0x75; *e++ = 0x00;                         /* jne .stat_done */
  *e++ = 0x48;                                       /* dec eax (Status = 1 Success) */

  /* .stat_done: */
  stat_mismatch1[1] = (UINT8)(e - (stat_mismatch1 + 2));
  stat_mismatch2[1] = (UINT8)(e - (stat_mismatch2 + 2));
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x84); e += 4; /* mov [dest+0x84], eax (status) */

  /* ==================== POST-UNLOCK SEC2 RESET & QUIESCE ==================== */
  /* Call .sec2_reset_cycle subroutine */
  *e++ = 0xBD; /* mov ebp, imm32 */
  UINT8 *second_ret_target = e;
  *(UINT32 *)e = 0; e += 4;

  *e++ = 0xE9; /* jmp near .sec2_sub_start */
  UINT8 *sec2_call_jmp = e;
  *(INT32 *)e = 0; e += 4;
  INT32 call2_disp = (INT32)(sec2_sub_start - (sec2_call_jmp + 4));
  *(INT32 *)sec2_call_jmp = call2_disp;

  /* .after_second_reset: */
  *(UINT32 *)second_ret_target = (UINT32)(TrampolinePhys + (UINT32)(e - Buffer));

  /* Jump over fault handler to common teardown */
  UINT8 *skip_fault_jmp = e;
  *e++ = 0xEB; *e++ = 0x00;                         /* jmp short .common_teardown */

  /* .fault: */
  INT32 fdisp1 = (INT32)(e - (fault_jmp1 + 6));
  *(INT32 *)(fault_jmp1 + 2) = fdisp1;
  INT32 fdisp2 = (INT32)(e - (fault_jmp2 + 6));
  *(INT32 *)(fault_jmp2 + 2) = fdisp2;

  *e++ = 0xC7; *e++ = 0x05; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x84); e += 4;
  *e++ = 0x03; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; /* mov dword [dest+0x84], 3 (Fault) */

  /* .common_teardown: Restore GPU and Bridge PCI config to pre-trampoline state */
  skip_fault_jmp[1] = (UINT8)(e - (skip_fault_jmp + 2));
  *e++ = 0xC7; *e++ = 0x05; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x80); e += 4;
  *e++ = 0x4B; *e++ = 0x4C; *e++ = 0x4E; *e++ = 0x55; /* mov dword [dest+0x80], 0x554E4C4B ('UNLK') */

  /* Assert that telemetry mirror (+0x0580..+0x05DF) ends strictly before RM_IDTR (0x0600) */
  if ((0x0580U + (24U * sizeof(UINT32))) > GU_TRAMP_RM_IDTR_OFF) {
    return 0;
  }

  /* Mirror 24 DWORDs (96 bytes, +0x80..+0xDF) from high_telem_dest + 0x80 to TrampolinePhys + 0x0580 (v1.8.58) */
  *e++ = 0xBE; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x80); e += 4; /* mov esi, high_telem_dest + 0x80 */
  *e++ = 0xBF; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0580); e += 4; /* mov edi, TrampolinePhys + 0x0580 */
  *e++ = 0xB9; *e++ = 0x18; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00;     /* mov ecx, 24 */
  *e++ = 0xFC;                                                           /* cld */
  *e++ = 0xF3; *e++ = 0xA5;                                             /* rep movsd */

  /* ==================== 5-STAGE PCIE TEARDOWN ==================== */
  /* Stage 1: Restore GPU Command register */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; *e++ = 0x00; *e++ = 0x00; /* mov edx, 0x0CF8 */
  *e++ = 0xB8; *e++ = 0x04; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x80; /* mov eax, 0x802D0004 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xB8); e += 4; /* mov eax, [dest+0xB8] */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* Stage 2: Restore GPU BAR0 */
  *e++ = 0xB2; *e++ = 0xF8;                         /* mov dl, 0xF8 */
  *e++ = 0xB8; *e++ = 0x10; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x80; /* mov eax, 0x802D0010 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xBC); e += 4; /* mov eax, [dest+0xBC] */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* Stage 3: Restore Root Port Memory Base/Limit register */
  *e++ = 0xB2; *e++ = 0xF8;                         /* mov dl, 0xF8 */
  *e++ = 0xB8; *e++ = 0x20; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80; /* mov eax, 0x80001920 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xB4); e += 4; /* mov eax, [dest+0xB4] */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* Stage 4: Restore Root Port Command register */
  *e++ = 0xB2; *e++ = 0xF8;                         /* mov dl, 0xF8 */
  *e++ = 0xB8; *e++ = 0x04; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80; /* mov eax, 0x80001904 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(high_telem_dest + 0xB0); e += 4; /* mov eax, [dest+0xB0] */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* Stage 5: Restore Root Port Offset 0x18 (Bus Numbers) - Restored LAST */
  *e++ = 0xB2; *e++ = 0xF8;                         /* mov dl, 0xF8 */
  *e++ = 0xB8; *e++ = 0x18; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80; /* mov eax, 0x80001918 */
  *e++ = 0xEF;                                       /* out dx, eax */
  *e++ = 0xB2; *e++ = 0xFC;                         /* mov dl, 0xFC */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(high_telem_dest + 0x48); e += 4; /* mov eax, [dest+0x48] */
  *e++ = 0xEF;                                       /* out dx, eax */

  /* Transition to 16-Bit Protected Mode: jmp far 0x0018:pm16_offset */
  *e++ = 0xEA;                                       /* jmp ptr16:32 */
  UINT8 *pm16_jmp_target = e;
  *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; /* placeholder dword offset */
  *e++ = 0x18; *e++ = 0x00;                         /* Selector 0x0018 */

  /* ==================== 16-BIT PROTECTED MODE (BITS 16) ==================== */
  UINT8 *pm16_start = e;
  UINT32 pm16_offset_in_seg = (UINT32)((TrampolinePhys & 0xFFFFULL) + (UINT32)(pm16_start - Buffer));
  *(UINT32 *)pm16_jmp_target = pm16_offset_in_seg;

  /* Load 16-bit data segment selector 0x0020 */
  *e++ = 0xB8; *e++ = 0x20; *e++ = 0x00;             /* mov ax, 0x0020 */
  *e++ = 0x8E; *e++ = 0xD8;                         /* mov ds, ax */
  *e++ = 0x8E; *e++ = 0xC0;                         /* mov es, ax */
  *e++ = 0x8E; *e++ = 0xE0;                         /* mov fs, ax */
  *e++ = 0x8E; *e++ = 0xE8;                         /* mov gs, ax */
  *e++ = 0x8E; *e++ = 0xD0;                         /* mov ss, ax */

  /* Clear CR0.PE (Bit 0) to return to Real Mode */
  *e++ = 0x66; *e++ = 0x0F; *e++ = 0x20; *e++ = 0xC0; /* mov eax, cr0 */
  *e++ = 0x24; *e++ = 0xFE;                         /* and al, 0xFE */
  *e++ = 0x66; *e++ = 0x0F; *e++ = 0x22; *e++ = 0xC0; /* mov cr0, eax */

  /* Far jump to 16-Bit Real Mode (0x9E00:rm_exit_off) */
  *e++ = 0xEA;                                       /* jmp ptr16:16 */
  UINT8 *rm_jmp_target = e;
  *e++ = 0x00; *e++ = 0x00;                         /* placeholder word offset */
  UINT16 rm_seg = (UINT16)(TrampolinePhys >> 4);
  *e++ = (UINT8)(rm_seg & 0xFF);
  *e++ = (UINT8)((rm_seg >> 8) & 0xFF);             /* Real Mode Segment (0x9E00) */

  /* ==================== 16-BIT REAL MODE (BITS 16) ==================== */
  UINT8 *rm_exit = e;
  UINT16 rm_exit_off = (UINT16)(rm_exit - Buffer);
  *(UINT16 *)rm_jmp_target = rm_exit_off;

  /* Reload real-mode data segment registers with CS (0x9E00) */
  *e++ = 0x8C; *e++ = 0xC8;                         /* mov ax, cs */
  *e++ = 0x8E; *e++ = 0xD8;                         /* mov ds, ax */
  *e++ = 0x8E; *e++ = 0xC0;                         /* mov es, ax */

  /* Restore BIOS SP / SS saved at entry [0x0570] / [0x0572] */
  *e++ = 0x8E; *e++ = 0x16; *e++ = 0x72; *e++ = 0x05; /* mov ss, [0x0572] */
  *e++ = 0x8B; *e++ = 0x26; *e++ = 0x70; *e++ = 0x05; /* mov sp, [0x0570] */

  /* Zero general purpose registers for clean Windows handoff */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC0;             /* xor eax, eax */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xDB;             /* xor ebx, ebx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC9;             /* xor ecx, ecx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xD2;             /* xor edx, edx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xF6;             /* xor esi, esi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xFF;             /* xor edi, edi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xED;             /* xor ebp, ebp */

  /* Far indirect jump directly to Windows HalpRMStub */
  *e++ = 0x2E; *e++ = 0xFF; *e++ = 0x2E; *e++ = 0x00; *e++ = 0x05; /* jmp far ptr cs:[0x0500] */

  if ((UINT32)(e - Buffer) > 0x0500U) {
    /* Machine code exceeded 1280 bytes (would overwrite telemetry area at 0x0500) */
    return 0;
  }

  return (UINT32)(e - Buffer);
}

/*
 * Discovers the PCIe Link Control & Status Register on the upstream bridge for target_bus.
 */
static UINT32 GuFindRootPortLinkReg(EFI_BOOT_SERVICES *bs, UINT8 target_bus) {
  UINT32 fallback_reg = 0x80001960U; /* Default AMD Root Port 0:3:1 Link Ctrl/Status (cap=0x50+0x10) */
  if (!bs || !bs->LocateHandleBuffer || !bs->HandleProtocol || !bs->FreePool) return fallback_reg;

  EFI_GUID pci_guid = { 0x4cf5b200, 0x68b8, 0x41fb, { 0x88, 0xae, 0xb2, 0xec, 0x17, 0x93, 0xac, 0x5d } };
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;
  EFI_STATUS s = bs->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &count, &handles);
  if (EFI_ERROR(s) || !handles) return fallback_reg;

  UINT32 fallback_match_reg = 0;

  for (UINTN i = 0; i < count; i++) {
    EFI_PCI_IO_PROTOCOL *io = NULL;
    if (EFI_ERROR(bs->HandleProtocol(handles[i], &pci_guid, (VOID **)&io)) || !io) continue;

    UINT8 hdr_type = 0, sec_bus = 0;
    (VOID)io->Pci.Read(io, EfiPciIoWidthUint8, 0x0E, 1, &hdr_type);
    if ((hdr_type & 0x7FU) == 0x01) { /* PCI-to-PCI Bridge */
      (VOID)io->Pci.Read(io, EfiPciIoWidthUint8, 0x19, 1, &sec_bus);
      UINTN seg = 0, bus = 0, dev = 0, func = 0;
      if (io->GetLocation) {
        (VOID)io->GetLocation(io, &seg, &bus, &dev, &func);
      }
      BOOLEAN is_target = (sec_bus == target_bus);
      BOOLEAN is_default_bdf = (bus == 0 && dev == 3 && func == 1);
      if (is_target || (is_default_bdf && fallback_match_reg == 0)) {
        UINT8 cap_ptr = 0;
        (VOID)io->Pci.Read(io, EfiPciIoWidthUint8, 0x34, 1, &cap_ptr);
        UINTN steps = 0;
        while (cap_ptr >= 0x40 && cap_ptr <= 0xFC && (cap_ptr & 3) == 0 && ++steps < 48) {
          UINT8 cap_id = 0, next_ptr = 0;
          (VOID)io->Pci.Read(io, EfiPciIoWidthUint8, cap_ptr, 1, &cap_id);
          (VOID)io->Pci.Read(io, EfiPciIoWidthUint8, cap_ptr + 1, 1, &next_ptr);
          if (cap_id == 0x10) { /* PCI Express Capability */
            UINT32 reg_off = (UINT32)(cap_ptr + 0x10);
            UINT32 pci_addr = 0x80000000U | ((UINT32)bus << 16) | ((UINT32)dev << 11) | ((UINT32)func << 8) | (reg_off & ~3U);
            if (is_target) {
              bs->FreePool(handles);
              return pci_addr;
            }
            fallback_match_reg = pci_addr;
            break;
          }
          cap_ptr = next_ptr;
        }
      }
    }
  }
  bs->FreePool(handles);
  return fallback_match_reg ? fallback_match_reg : fallback_reg;
}

/*
 * Discovers the Power Management Control/Status Register (PMCSR) on the GPU via PCI Cap 0x01.
 */
static UINT32 GuFindGpuPmcsrReg(EFI_BOOT_SERVICES *bs, EFI_PCI_IO_PROTOCOL *gpu_io, UINT8 gpu_bus) {
  UINT32 fallback_reg = 0x80000000U | ((UINT32)gpu_bus << 16) | 0x64U; /* Default RTX 4090 pm_cap=0x60 + 0x04 */
  if (Gu.pm_cap >= 0x40 && Gu.pm_cap <= 0xFC) {
    UINT32 reg_off = (UINT32)(Gu.pm_cap + 0x04);
    return 0x80000000U | ((UINT32)gpu_bus << 16) | (reg_off & ~3U);
  }

  if (!gpu_io && bs && bs->LocateHandleBuffer && bs->HandleProtocol && bs->FreePool) {
    EFI_GUID pci_guid = { 0x4cf5b200, 0x68b8, 0x41fb, { 0x88, 0xae, 0xb2, 0xec, 0x17, 0x93, 0xac, 0x5d } };
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    if (!EFI_ERROR(bs->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &count, &handles)) && handles) {
      for (UINTN i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL *io = NULL;
        if (!EFI_ERROR(bs->HandleProtocol(handles[i], &pci_guid, (VOID **)&io)) && io) {
          UINTN seg = 0, bus = 0, dev = 0, func = 0;
          if (io->GetLocation && !EFI_ERROR(io->GetLocation(io, &seg, &bus, &dev, &func))) {
            if (bus == gpu_bus && dev == 0 && func == 0) {
              gpu_io = io;
              break;
            }
          }
        }
      }
      bs->FreePool(handles);
    }
  }

  if (gpu_io) {
    UINT8 cap_ptr = 0;
    (VOID)gpu_io->Pci.Read(gpu_io, EfiPciIoWidthUint8, 0x34, 1, &cap_ptr);
    UINTN steps = 0;
    while (cap_ptr >= 0x40 && cap_ptr <= 0xFC && (cap_ptr & 3) == 0 && ++steps < 48) {
      UINT8 cap_id = 0, next_ptr = 0;
      (VOID)gpu_io->Pci.Read(gpu_io, EfiPciIoWidthUint8, cap_ptr, 1, &cap_id);
      (VOID)gpu_io->Pci.Read(gpu_io, EfiPciIoWidthUint8, cap_ptr + 1, 1, &next_ptr);
      if (cap_id == 0x01) { /* PCI Power Management Capability */
        UINT32 reg_off = (UINT32)(cap_ptr + 0x04);
        UINTN seg = 0, bus = gpu_bus, dev = 0, func = 0;
        if (gpu_io->GetLocation) {
          (VOID)gpu_io->GetLocation(gpu_io, &seg, &bus, &dev, &func);
        }
        UINT32 pci_addr = 0x80000000U | ((UINT32)bus << 16) | ((UINT32)dev << 11) | ((UINT32)func << 8) | (reg_off & ~3U);
        return pci_addr;
      }
      cap_ptr = next_ptr;
    }
  }

  return fallback_reg;
}

/*
 * Allocates a dedicated 4KB telemetry page of EfiRuntimeServicesData (or EfiReservedMemoryType)
 * strictly OUTSIDE the 27MB NVS zone (0xDADDA000..0xDC8DA000) so that nt!HalpRestoreNvsArea
 * does NOT overwrite it during S3 wake (v1.8.46c).
 */
static EFI_PHYSICAL_ADDRESS GuAcpiAllocateTelemetryPage(
    IN EFI_BOOT_SERVICES *bs
) {
  if (!bs || !bs->AllocatePages) return 0;

  EFI_PHYSICAL_ADDRESS telem_phys = 0;
  EFI_STATUS s;

  /* Strategy 1: AllocateMaxAddress <= 0xDADD9000 (below 27MB NVS block) as EfiRuntimeServicesData */
  telem_phys = 0xDADD9000ULL;
  s = bs->AllocatePages(AllocateMaxAddress, EfiRuntimeServicesData, 1, &telem_phys);
  if (!EFI_ERROR(s) && telem_phys >= 0x00100000ULL && (telem_phys < 0xDADDA000ULL || telem_phys >= 0xDC8DA000ULL)) {
    memset((VOID *)(UINTN)telem_phys, 0, 4096);
    return telem_phys;
  }

  /* Strategy 2: AllocateMaxAddress <= 0xDADD9000 as EfiReservedMemoryType */
  telem_phys = 0xDADD9000ULL;
  s = bs->AllocatePages(AllocateMaxAddress, EfiReservedMemoryType, 1, &telem_phys);
  if (!EFI_ERROR(s) && telem_phys >= 0x00100000ULL && (telem_phys < 0xDADDA000ULL || telem_phys >= 0xDC8DA000ULL)) {
    memset((VOID *)(UINTN)telem_phys, 0, 4096);
    return telem_phys;
  }

  /* Strategy 3: Try candidate physical addresses outside the NVS zone */
  static const EFI_PHYSICAL_ADDRESS candidate_addrs[] = {
    0xDA000000ULL, 0xD9000000ULL, 0xD8000000ULL,
    0xDD000000ULL, 0xDE000000ULL, 0xDF000000ULL
  };
  for (UINTN i = 0; i < sizeof(candidate_addrs)/sizeof(candidate_addrs[0]); i++) {
    telem_phys = candidate_addrs[i];
    s = bs->AllocatePages(AllocateAddress, EfiRuntimeServicesData, 1, &telem_phys);
    if (!EFI_ERROR(s)) {
      memset((VOID *)(UINTN)telem_phys, 0, 4096);
      return telem_phys;
    }
  }

  /* Strategy 4: AllocateMaxAddress 4GB with NVS bounds check */
  telem_phys = 0xFFFFFFFFULL;
  s = bs->AllocatePages(AllocateMaxAddress, EfiRuntimeServicesData, 1, &telem_phys);
  if (!EFI_ERROR(s)) {
    if (telem_phys < 0xDADDA000ULL || telem_phys >= 0xDC8DA000ULL) {
      memset((VOID *)(UINTN)telem_phys, 0, 4096);
      return telem_phys;
    }
    if (bs->FreePages) {
      bs->FreePages(telem_phys, 1);
    }
  }

  return 0;
}

EFI_STATUS GuAcpiInstallShadowFacs(
    IN EFI_SYSTEM_TABLE *SystemTable,
    IN GU_S3_RESUME_CONTEXT *Context,
    IN VOID *NvsBuffer,
    IN EFI_PHYSICAL_ADDRESS NvsPhys,
    IN VOID *LowTrampBuffer,
    IN EFI_PHYSICAL_ADDRESS LowTrampPhys,
    OUT GU_ACPI_FACS_INFO *Info
) {
  if (!SystemTable || !Context || !NvsBuffer || !NvsPhys || !LowTrampBuffer || !LowTrampPhys || !Info) {
    return EFI_INVALID_PARAMETER;
  }

  /* 1. Discover FADT and Real FACS */
  EFI_STATUS s = GuAcpiLocateFadtAndFacs(SystemTable, Info);
  if (EFI_ERROR(s) || !Info->found || !Info->fadt || !Info->real_facs) {
    GuLog("GuAcpiInstallShadowFacs: Failed to locate ACPI tables (%#llx)\n", (unsigned long long)s);
    return s;
  }

  /* Check for prior wake telemetry in low trampoline memory or low pages (0x90000..0x9F000) */
  UINT8 *low_base = (UINT8 *)LowTrampBuffer;
  UINT8 *telemetry_page = NULL;
  EFI_PHYSICAL_ADDRESS telemetry_phys = 0;

  if (*(UINT32 *)(low_base + GU_TRAMP_MAGIC_OFF) == GU_TRAMP_TELEMETRY_MAGIC) {
    UINT16 entry_chk = *(UINT16 *)(low_base + GU_TRAMP_ENTRY_STATUS_OFF);
    if (entry_chk == 0xAA01 || entry_chk == 0xDEAD) {
      telemetry_page = low_base;
      telemetry_phys = LowTrampPhys;
    }
  }

#ifndef GU_S3_HOST_TEST
  if (!telemetry_page && LowTrampPhys < 0x00100000ULL) {
    /* Scan 16 low-memory pages from 0x00090000 to 0x0009F000 (4-KB steps) */
    for (EFI_PHYSICAL_ADDRESS page_addr = 0x00090000ULL; page_addr <= 0x0009F000ULL; page_addr += 4096) {
      UINT8 *candidate = (UINT8 *)(UINTN)page_addr;
      if (*(UINT32 *)(candidate + GU_TRAMP_MAGIC_OFF) == GU_TRAMP_TELEMETRY_MAGIC) {
        UINT16 entry_chk = *(UINT16 *)(candidate + GU_TRAMP_ENTRY_STATUS_OFF);
        if (entry_chk == 0xAA01 || entry_chk == 0xDEAD) {
          telemetry_page = candidate;
          telemetry_phys = page_addr;
          break;
        }
      }
    }
  }
#endif

  if (telemetry_page != NULL) {
    UINT16 entry_status = *(UINT16 *)(telemetry_page + GU_TRAMP_ENTRY_STATUS_OFF);
    UINT16 exit_status  = *(UINT16 *)(telemetry_page + GU_TRAMP_EXIT_STATUS_OFF);
    UINT32 full_vec     = *(UINT32 *)(telemetry_page + GU_TRAMP_CAPTURED_VEC_OFF);
    UINT16 tgt_off      = *(UINT16 *)(telemetry_page + GU_TRAMP_TARGET_PTR_OFF);
    UINT16 tgt_seg      = *(UINT16 *)(telemetry_page + GU_TRAMP_TARGET_PTR_OFF + 2);
    GuLog("S3 Prior Wake Telemetry (Page %#llx): Magic=S3_T Entry=%#04x Vector=%#08x Target=%04x:%04x Exit=%#04x\n",
          (unsigned long long)telemetry_phys,
          (unsigned)entry_status, (unsigned)full_vec, (unsigned)tgt_seg, (unsigned)tgt_off, (unsigned)exit_status);

    UINT32 br_id   = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x00);
    UINT32 br_cmd  = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x04);
    UINT32 br_bus  = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x08);
    UINT32 br_lnk  = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x0C);
    UINT32 gpu_id  = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x10);
    UINT32 gpu_cmd = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x14);
    UINT32 gpu_bar = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x18);
    UINT32 gpu_pm  = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x1C);
    UINT32 post_id = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_POST_OFF + 0x00);
    UINT32 post_cmd= *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_POST_OFF + 0x04);
    UINT32 post_bar= *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_POST_OFF + 0x08);
    UINT32 post_pm = *(UINT32 *)(telemetry_page + GU_TRAMP_PCI_PROBE_POST_OFF + 0x0C);

    UINT16 lnk_status = (UINT16)(br_lnk >> 16);
    BOOLEAN dllla = (lnk_status & 0x2000U) != 0;
    BOOLEAN training = (lnk_status & 0x0800U) != 0;
    UINT8 speed = (UINT8)(lnk_status & 0x000FU);
    UINT8 width = (UINT8)((lnk_status >> 4) & 0x003FU);

    GuLog("S3 Wake Pre-OS Hardware Telemetry (Raw Before Config):\n"
          "  Root Port (0:3:1) -> DevID=%#08x Cmd/Stat=%#08x BusNum=%#08x\n"
          "  PCIe Link Status  -> Raw=%#04x (DLLLA=%u Training=%u Speed=Gen%u Width=x%u)\n"
          "  GPU       (45:0:0)-> DevID=%#08x Cmd/Stat=%#08x BAR0=%#08x PMCSR=%#08x\n"
          "S3 Wake Pre-OS Hardware Telemetry (After Temporary Bridge Arming):\n"
          "  GPU       (45:0:0)-> DevID=%#08x Cmd/Stat=%#08x BAR0=%#08x PMCSR=%#08x\n"
          "  Root Port (0:3:1) -> BusNum restored to original (%#08x)\n",
          (unsigned)br_id, (unsigned)br_cmd, (unsigned)br_bus,
          (unsigned)lnk_status, dllla ? 1 : 0, training ? 1 : 0, (unsigned)speed, (unsigned)width,
          (unsigned)gpu_id, (unsigned)gpu_cmd, (unsigned)gpu_bar, (unsigned)gpu_pm,
          (unsigned)post_id, (unsigned)post_cmd, (unsigned)post_bar, (unsigned)post_pm,
          (unsigned)br_bus);

    /* Check for S3 Wake Hardware Unlock Telemetry block at +0x80 ('UNLK' = 0x554E4C4B) (v1.8.47 / v1.8.58) */
    UINT8 *unlk_page = telemetry_page;
    if (*(UINT32 *)(telemetry_page + 0x0580) == GU_TELEM_UNLOCK_MAGIC) {
      unlk_page = telemetry_page + 0x0500;
      GuLog("S3 Prior Wake Telemetry: Located mirrored hardware unlock telemetry at low offset +0x0580\n");
    } else if (*(UINT32 *)(unlk_page + GU_TELEM_UNLOCK_MAGIC_OFF) != GU_TELEM_UNLOCK_MAGIC) {
#ifndef GU_S3_HOST_TEST
      /* Scan candidate dedicated telemetry pages outside NVS for UNLK block */
      static const EFI_PHYSICAL_ADDRESS candidate_high_pages[] = {
        0xDADD9000ULL, 0xDA000000ULL, 0xD9000000ULL, 0xD8000000ULL,
        0xDD000000ULL, 0xDE000000ULL, 0xDF000000ULL
      };
      for (UINTN i = 0; i < sizeof(candidate_high_pages)/sizeof(candidate_high_pages[0]); i++) {
        UINT8 *cand = (UINT8 *)(UINTN)candidate_high_pages[i];
        if (*(UINT32 *)(cand + GU_TELEM_UNLOCK_MAGIC_OFF) == GU_TELEM_UNLOCK_MAGIC) {
          unlk_page = cand;
          GuLog("S3 Prior Wake Telemetry: Located high hardware unlock telemetry at %#llx\n",
                (unsigned long long)candidate_high_pages[i]);
          break;
        }
      }
#endif
    }

    if (*(UINT32 *)(unlk_page + GU_TELEM_UNLOCK_MAGIC_OFF) == GU_TELEM_UNLOCK_MAGIC) {
      UINT32 unlk_status = *(UINT32 *)(unlk_page + GU_TELEM_UNLOCK_STATUS_OFF);
      UINT32 unlk_chip   = *(UINT32 *)(unlk_page + GU_TELEM_CHIP_ID_OFF);
      UINT32 unlk_bar0   = *(UINT32 *)(unlk_page + GU_TELEM_BAR0_ASSIGNED_OFF);
      UINT32 bcr_post      = *(UINT32 *)(unlk_page + GU_TELEM_FALCON2_BCR_POST_OFF);
      UINT32 cpuctl_post   = *(UINT32 *)(unlk_page + GU_TELEM_SEC2_CPUCTL_POST_OFF);
      UINT32 booter_cpuctl = *(UINT32 *)(unlk_page + GU_TELEM_BOOTER_CPUCTL_POST_OFF);
      UINT32 ss0_before    = *(UINT32 *)(unlk_page + GU_TELEM_SS0_BEFORE_OFF);
      UINT32 ss1_before    = *(UINT32 *)(unlk_page + GU_TELEM_SS1_BEFORE_OFF);
      UINT32 ss0_after     = *(UINT32 *)(unlk_page + GU_TELEM_SS0_AFTER_OFF);
      UINT32 ss1_after     = *(UINT32 *)(unlk_page + GU_TELEM_SS1_AFTER_OFF);
      UINT32 ro_after      = *(UINT32 *)(unlk_page + GU_TELEM_READOUT_AFTER_OFF);
      GuLog("S3 Wake Hardware Unlock Telemetry: Magic=UNLK Status=%u Chip=%#08x AssignedBAR0=%#08x\n"
            "  BCR_Post: %#08x CPUCTL_Post: %#08x Booter_CPUCTL: %#08x\n"
            "  SS0: %#08x -> %#08x (Target 0x88888888)\n"
            "  SS1: %#08x -> %#08x (Target 0x00000008)\n"
            "  Readout1: %#08x (Full-Rate = 0x00000000)\n",
            (unsigned)unlk_status, (unsigned)unlk_chip, (unsigned)unlk_bar0,
            (unsigned)bcr_post, (unsigned)cpuctl_post, (unsigned)booter_cpuctl,
            (unsigned)ss0_before, (unsigned)ss0_after,
            (unsigned)ss1_before, (unsigned)ss1_after,
            (unsigned)ro_after);
      if (unlk_page != telemetry_page) {
        *(UINT32 *)(unlk_page + GU_TELEM_UNLOCK_MAGIC_OFF) = 0;
      }
    }

    /* Zero out the telemetry magic on the found page after reading */
    *(UINT32 *)(telemetry_page + GU_TRAMP_MAGIC_OFF) = 0;
  } else {
    GuLog("S3 Prior Wake Telemetry: none found\n");
  }

  /*
   * Dedicated High (>1MB, <4GB) ACPI NVS Allocation:
   * Holds Shadow ACPI tables: ShadowRSDP (+0x000), ShadowXSDT (+0x080),
   * ShadowFADT (+0x0200), and ShadowFACS (+0x0400).
   * This guarantees ZERO bytes of "RSD PTR " or "FACS" reside below 1MB!
   */
  EFI_PHYSICAL_ADDRESS high_acpi_phys = 0xFFFFFFFFULL;
  UINT8 *high_acpi_base = NULL;
  if (SystemTable->BootServices && SystemTable->BootServices->AllocatePages) {
    EFI_STATUS as = SystemTable->BootServices->AllocatePages(
        AllocateMaxAddress,
        EfiACPIMemoryNVS,
        1,
        &high_acpi_phys
    );
    if (!EFI_ERROR(as) && high_acpi_phys >= 0x00100000ULL) {
      high_acpi_base = (UINT8 *)(UINTN)high_acpi_phys;
      memset(high_acpi_base, 0, 4096);
    }
  }

  if (high_acpi_base == NULL) {
    /* Fallback if high allocation unavailable: use NvsBuffer (>1MB) */
    high_acpi_base = (UINT8 *)NvsBuffer + 0x30000;
    high_acpi_phys = NvsPhys + 0x30000;
    memset(high_acpi_base, 0, 4096);
  }

  Info->high_acpi_phys = high_acpi_phys;

  /*
   * 2. Setup Shadow FACS at HighAcpiPage + GU_HIGH_SHADOW_FACS_OFF (0x0400, 64-byte aligned):
   */
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = high_acpi_phys + GU_HIGH_SHADOW_FACS_OFF;
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(high_acpi_base + GU_HIGH_SHADOW_FACS_OFF);

  /* Clone Real FACS into Shadow FACS */
  memcpy(shadow_facs, Info->real_facs, sizeof(EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE));
  shadow_facs->FirmwareWakingVector = 0;
  shadow_facs->XFirmwareWakingVector = 0;
  shadow_facs->Flags = 0;
  UINT8 *sf_raw = (UINT8 *)shadow_facs;
  *(UINT32 *)(sf_raw + 0x20) = 0; /* Version at +0x20 */
  *(UINT32 *)(sf_raw + 0x24) = 0; /* OspmFlags at +0x24 */

  /* Tag Shadow FACS with signature and real FACS address for warm reset recovery */
  *(UINT32 *)(sf_raw + 0x28) = GU_SHADOW_FACS_SIGNATURE;
  memcpy(sf_raw + 0x2C, &Info->real_facs_phys, sizeof(UINT64));

  /* Clear all 64-bit wake flags in both tables for rock-solid 16-bit real mode wake */
  Info->real_facs->Flags = 0;
  Info->real_facs->XFirmwareWakingVector = 0;
  if (Info->real_facs->Length >= 64) {
    UINT8 *rf_raw = (UINT8 *)Info->real_facs;
    *(UINT32 *)(rf_raw + 0x20) = 0; /* Version at +0x20 */
    *(UINT32 *)(rf_raw + 0x24) = 0; /* OspmFlags at +0x24 */
  }

  /*
   * 3. Low (<1MB) ACPI NVS Allocation (4096 bytes):
   * +0x00000: Pure 16-bit Real Mode S3 Resume Trampoline Machine Code (<380 bytes)
   * +0x00200..0x0027F: Zero-initialized (NO FACS signature below 1MB!)
   * +0x00280: Real FACS Vector Mirror (32-bit at 0x280, 64-bit at 0x288)
   * +0x00300: Telemetry Checkpoints & Waking Vector capture
   * +0x00320: PCI Probe Addresses Table (8 dwords)
   * +0x00340: PCI Probe Raw Results (8 dwords)
   * +0x00360: PCI Probe Post-Arm Results (4 dwords)
   * +0x00370: Saved BIOS SP / SS
   */
  EFI_PHYSICAL_ADDRESS trampoline_phys = LowTrampPhys;

  /* Clear the entire low trampoline memory page */
  memset(low_base, 0, 4096);

  /* Store Real FACS original vectors as backup mirror in low memory at +0x0580 / +0x0588 */
  *(UINT32 *)(low_base + GU_TRAMP_REAL_FACS_MIRROR_OFF) = Info->original_firmware_waking_vector;
  *(UINT64 *)(low_base + GU_TRAMP_REAL_FACS_MIRROR_OFF + 8) = Info->original_xfirmware_waking_vector;

  /* Dynamically discover Root Port Link Status and GPU PMCSR register addresses */
  UINT8 target_gpu_bus = (Context && Context->bus) ? Context->bus : 45U;
  UINT32 root_port_link_reg = GuFindRootPortLinkReg(SystemTable->BootServices, target_gpu_bus);
  UINT32 gpu_pmcsr_reg = GuFindGpuPmcsrReg(SystemTable->BootServices, Gu.pci, target_gpu_bus);

  if (Context) {
    Context->root_port_link_reg = root_port_link_reg;
    Context->gpu_pmcsr_reg = gpu_pmcsr_reg;
  }
  GuLog("S3 Hardware Recon Config: Root Port LinkReg=%#08x (Cap 0x10+0x10), GPU PMCSR=%#08x (Cap 0x01+0x04)\n",
        (unsigned)root_port_link_reg, (unsigned)gpu_pmcsr_reg);

  /* Allocate dedicated 4KB telemetry page outside the 27MB NVS zone (v1.8.46c) */
  EFI_PHYSICAL_ADDRESS telem_page_phys = GuAcpiAllocateTelemetryPage(SystemTable->BootServices);
  Info->telemetry_page_phys = telem_page_phys;
  mShadowState.telemetry_page_phys = telem_page_phys;
  if (telem_page_phys != 0) {
    GuLog("S3 Dedicated Telemetry Page: Allocated at %#llx (outside NVS 0xdadda000..0xdc8da000)\n",
          (unsigned long long)telem_page_phys);
  } else {
    GuLog("S3 Dedicated Telemetry Page: Allocation failed, falling back to HighAcpiPage+0x500\n");
  }

  /* 4. Build Pure 16-bit Real Mode S3 Resume Trampoline machine code in low memory (<1MB) */
  EFI_PHYSICAL_ADDRESS gpu_bar0_phys = (Context && Context->bar_base) ? Context->bar_base : 0xFB000000ULL;
  UINT32 tramp_bytes = GuAcpiBuildTrampolineCode(
      low_base,
      4096,
      trampoline_phys,
      NvsPhys,
      shadow_facs_phys,
      telem_page_phys,
      gpu_bar0_phys,
      root_port_link_reg,
      gpu_pmcsr_reg
  );

  if (tramp_bytes == 0) {
    GuLog("GuAcpiInstallShadowFacs: Failed to build trampoline machine code\n");
    return EFI_DEVICE_ERROR;
  }

  Info->shadow_facs_phys = shadow_facs_phys;
  Info->trampoline_phys = trampoline_phys;

  EFI_PHYSICAL_ADDRESS shadow_fadt_phys = high_acpi_phys + GU_HIGH_SHADOW_FADT_OFF;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *shadow_fadt =
      (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)(high_acpi_base + GU_HIGH_SHADOW_FADT_OFF);
  Info->shadow_fadt = shadow_fadt;
  Info->shadow_fadt_phys = shadow_fadt_phys;

  /*
   * Plan C: Complete Shadow Chain Architecture (v1.8.45)
   * Real ACPI tables (RealRSDP, RealRSDT, RealXSDT, RealFADT) remain 100% UNTOUCHED byte-for-byte.
   * Complete shadow hierarchy is constructed in high EfiACPIMemoryNVS:
   * ShadowRSDP -> ShadowXSDT -> ShadowFADT -> ShadowFACS (high_acpi_phys + 0x400)
   */
  GuLog("S3 Plan C Pre-Build: RealRSDP=%#llx RealXSDT=%#llx (Chk=%#02x) RealFADT=%#llx (Len=%u Chk=%#02x)\n",
        (unsigned long long)(UINTN)Info->rsdp,
        (unsigned long long)Info->xsdt_phys, (unsigned)Info->xsdt_original_checksum,
        (unsigned long long)Info->fadt_phys, (unsigned)Info->fadt->Header.Length,
        (unsigned)Info->fadt_original_checksum);

  /*
   * 5. Populate ShadowFADT at HighAcpiPage + 0x200:
   *    Copy full Header.Length bytes from original RealFADT.
   *    Set FirmwareCtrl = ShadowFACS (high_acpi_phys + 0x400), XFirmwareCtrl = 0.
   *    Recalculate ShadowFADT->Header.Checksum.
   */
  UINT32 fadt_len = Info->fadt->Header.Length;
  if (fadt_len > (4096 - GU_HIGH_SHADOW_FADT_OFF)) {
    fadt_len = 4096 - GU_HIGH_SHADOW_FADT_OFF;
  }
  memcpy(shadow_fadt, Info->fadt, fadt_len);
  shadow_fadt->FirmwareCtrl = (UINT32)shadow_facs_phys;
  if (shadow_fadt->Header.Length >= 140) {
    shadow_fadt->XFirmwareCtrl = 0;
  }
  shadow_fadt->Header.Checksum = 0;
  UINT8 sf_sum = 0;
  UINT8 *sf_bytes = (UINT8 *)shadow_fadt;
  for (UINT32 i = 0; i < shadow_fadt->Header.Length; i++) {
    sf_sum = (UINT8)(sf_sum + sf_bytes[i]);
  }
  shadow_fadt->Header.Checksum = (UINT8)(0 - sf_sum);

  /*
   * 6. Populate ShadowXSDT at HighAcpiPage + 0x080:
   *    Copy RealXSDT, update FACP/FADT entry to point to ShadowFADT, recalculate Checksum.
   */
  EFI_ACPI_DESCRIPTION_HEADER *shadow_xsdt = NULL;
  EFI_PHYSICAL_ADDRESS shadow_xsdt_phys = 0;

  if (Info->xsdt != NULL) {
    UINT32 max_inline_xsdt = GU_HIGH_SHADOW_FADT_OFF - GU_HIGH_SHADOW_XSDT_OFF; /* 0x180 = 384 bytes */
    if (Info->xsdt->Length <= max_inline_xsdt) {
      shadow_xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(high_acpi_base + GU_HIGH_SHADOW_XSDT_OFF);
      shadow_xsdt_phys = high_acpi_phys + GU_HIGH_SHADOW_XSDT_OFF;
    } else if (SystemTable->BootServices && SystemTable->BootServices->AllocatePages) {
      EFI_PHYSICAL_ADDRESS alloc_phys = 0xFFFFFFFFULL;
      EFI_STATUS as = SystemTable->BootServices->AllocatePages(
          AllocateMaxAddress,
          EfiACPIMemoryNVS,
          1,
          &alloc_phys
      );
      if (!EFI_ERROR(as)) {
        shadow_xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)alloc_phys;
        shadow_xsdt_phys = alloc_phys;
      }
    }
  }

  if (shadow_xsdt != NULL && Info->xsdt != NULL) {
    memcpy(shadow_xsdt, Info->xsdt, Info->xsdt->Length);
    if (Info->xsdt_fadt_index >= 0) {
      UINT8 *entry_ptr = ((UINT8 *)shadow_xsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
      UINT64 sf_phys64 = (UINT64)shadow_fadt_phys;
      memcpy(entry_ptr + Info->xsdt_fadt_index * sizeof(UINT64), &sf_phys64, sizeof(UINT64));
    }
    shadow_xsdt->Checksum = 0;
    UINT8 sxsdt_sum = 0;
    UINT8 *sxsdt_bytes = (UINT8 *)shadow_xsdt;
    for (UINT32 i = 0; i < shadow_xsdt->Length; i++) {
      sxsdt_sum = (UINT8)(sxsdt_sum + sxsdt_bytes[i]);
    }
    shadow_xsdt->Checksum = (UINT8)(0 - sxsdt_sum);
  }

  /*
   * 7. Populate ShadowRSDP at HighAcpiPage + 0x000:
   *    Copy RealRSDP (36 bytes for Revision >= 2).
   *    Set XsdtAddress = ShadowXSDT.
   *    Leave RsdtAddress = RealRSDP->RsdtAddress untouched.
   *    Recalculate 20-byte legacy Checksum (Offset 8), THEN 36-byte ExtendedChecksum (Offset 32).
   */
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *shadow_rsdp =
      (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)(high_acpi_base + GU_HIGH_SHADOW_RSDP_OFF);
  EFI_PHYSICAL_ADDRESS shadow_rsdp_phys = high_acpi_phys + GU_HIGH_SHADOW_RSDP_OFF;

  UINT32 rsdp_len = (Info->rsdp->Revision >= 2 && Info->rsdp->Length >= 36) ? Info->rsdp->Length : 36;
  memcpy(shadow_rsdp, Info->rsdp, rsdp_len);
  shadow_rsdp->XsdtAddress = (UINT64)shadow_xsdt_phys;
  shadow_rsdp->RsdtAddress = Info->rsdp->RsdtAddress; /* Untouched */

  /* Checksum 1: 20-byte legacy Checksum (Offset 8) */
  shadow_rsdp->Checksum = 0;
  UINT8 rsdp_sum20 = 0;
  UINT8 *rsdp_bytes = (UINT8 *)shadow_rsdp;
  for (UINT32 i = 0; i < 20; i++) {
    rsdp_sum20 = (UINT8)(rsdp_sum20 + rsdp_bytes[i]);
  }
  shadow_rsdp->Checksum = (UINT8)(0 - rsdp_sum20);

  /* Checksum 2: 36-byte ExtendedChecksum (Offset 32) */
  shadow_rsdp->ExtendedChecksum = 0;
  UINT8 rsdp_sum36 = 0;
  for (UINT32 i = 0; i < shadow_rsdp->Length; i++) {
    rsdp_sum36 = (UINT8)(rsdp_sum36 + rsdp_bytes[i]);
  }
  shadow_rsdp->ExtendedChecksum = (UINT8)(0 - rsdp_sum36);

  /*
   * 8. State registration:
   */
  mShadowState.installed = TRUE;
  mShadowState.applied = FALSE;
  mShadowState.orig_rsdp = (VOID *)(UINTN)Info->rsdp;
  mShadowState.shadow_rsdp = (VOID *)(UINTN)shadow_rsdp_phys;
  mShadowState.high_acpi_page = (VOID *)high_acpi_base;
  mShadowState.high_acpi_phys = high_acpi_phys;
  mShadowState.low_trampoline = (VOID *)low_base;
  mShadowState.system_table = SystemTable;
  mShadowState.orig_exit_boot_services = NULL;
  mShadowState.hook_active = FALSE;
  mShadowState.orig_waking_vector = Info->original_firmware_waking_vector;
  mShadowState.orig_xfirmware_waking_vector = Info->original_xfirmware_waking_vector;
  if (!mShadowState.s3_save_hook_active) {
    mShadowState.acpi_s3_save_protocol = NULL;
    mShadowState.orig_s3_save = NULL;
    mShadowState.s3_save_call_count = 0;
  }

  /*
   * NOTE: GuAcpiApplyShadowConfigTable() is deliberately DEFERRED until
   * GuBootWindows() immediately before StartImage(Windows)!
   * ConfigurationTable stays 100% on RealRSDP throughout driver execution.
   */

  /*
   * 9. Hook Real FACS waking vector to point to Low Trampoline (<1MB)
   *    RealFACS is what AMI Aptio V PEI wakes to!
   *    RealFADT and RealXSDT are 100% UNTOUCHED!
   */
  Info->real_facs->FirmwareWakingVector = (UINT32)trampoline_phys;
  Info->real_facs->XFirmwareWakingVector = 0;
  Info->real_facs->Flags = 0;
  if (Info->real_facs->Length >= 64) {
    UINT8 *rf_raw = (UINT8 *)Info->real_facs;
    *(UINT32 *)(rf_raw + 0x20) = 0; /* Version at +0x20 */
    *(UINT32 *)(rf_raw + 0x24) = 0; /* OspmFlags at +0x24 */
  }

  /*
   * 10. LEAK 3: Detect and sandwich EFI_ACPI_S3_SAVE_PROTOCOL (EDK2 AcpiS3SaveDxe)
   */
  static EFI_GUID s3_save_guid = EFI_ACPI_S3_SAVE_PROTOCOL_GUID;
  EFI_ACPI_S3_SAVE_PROTOCOL *acpi_s3_save = NULL;
  EFI_STATUS s_s3save = EFI_NOT_FOUND;
  if (SystemTable->BootServices && SystemTable->BootServices->LocateProtocol) {
    s_s3save = SystemTable->BootServices->LocateProtocol(&s3_save_guid, NULL, (VOID **)&acpi_s3_save);
  }

  if (!EFI_ERROR(s_s3save) && acpi_s3_save != NULL && acpi_s3_save->S3Save != NULL) {
    GuLog("S3 Plan C: EFI_ACPI_S3_SAVE_PROTOCOL found at %#llx (S3Save=%#llx)\n",
          (unsigned long long)(UINTN)acpi_s3_save,
          (unsigned long long)(UINTN)acpi_s3_save->S3Save);
    mShadowState.acpi_s3_save_protocol = acpi_s3_save;
    if (acpi_s3_save->S3Save != GuHookedS3Save) {
      mShadowState.orig_s3_save = acpi_s3_save->S3Save;
      acpi_s3_save->S3Save = GuHookedS3Save;
    }
    mShadowState.s3_save_hook_active = TRUE;
    Info->s3_save_hook_installed = TRUE;
  } else {
    GuLog("S3 Plan C: EFI_ACPI_S3_SAVE_PROTOCOL not found (%#llx)\n", (unsigned long long)s_s3save);
    Info->s3_save_hook_installed = FALSE;
  }

  /* Read-Only Forensics Logging */
  GuLog("RealFADT Forensic: FirmwareCtrl=%#08x XFirmwareCtrl=%#llx (Len=%u)\n",
        (unsigned)Info->fadt->FirmwareCtrl,
        (Info->fadt->Header.Length >= 140) ? (unsigned long long)Info->fadt->XFirmwareCtrl : 0ULL,
        (unsigned)Info->fadt->Header.Length);

  GuLog("UEFI ConfigurationTable Forensic (%u entries):\n", (unsigned)SystemTable->NumberOfTableEntries);
  for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
    const EFI_GUID *g = &SystemTable->ConfigurationTable[i].VendorGuid;
    GuLog("  [%02u] GUID={%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} Table=%#llx\n",
          (unsigned)i,
          (unsigned)g->Data1, (unsigned)g->Data2, (unsigned)g->Data3,
          (unsigned)g->Data4[0], (unsigned)g->Data4[1], (unsigned)g->Data4[2], (unsigned)g->Data4[3],
          (unsigned)g->Data4[4], (unsigned)g->Data4[5], (unsigned)g->Data4[6], (unsigned)g->Data4[7],
          (unsigned long long)(UINTN)SystemTable->ConfigurationTable[i].VendorTable);
  }

  VOID *acpi10_ptr = NULL;
  if (Info->config_table_acpi10_idx >= 0 && (UINTN)Info->config_table_acpi10_idx < SystemTable->NumberOfTableEntries) {
    acpi10_ptr = SystemTable->ConfigurationTable[Info->config_table_acpi10_idx].VendorTable;
  }
  if (acpi10_ptr != NULL) {
    const UINT8 *r10 = (const UINT8 *)acpi10_ptr;
    UINT32 rsdt_addr = *(const UINT32 *)(r10 + 0x10);
    UINT32 rsdt_entry0 = 0;
    if (Info->rsdt != NULL && Info->rsdt->Length >= (sizeof(EFI_ACPI_DESCRIPTION_HEADER) + sizeof(UINT32))) {
      rsdt_entry0 = *(UINT32 *)((UINT8 *)Info->rsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
    } else if (rsdt_addr != 0) {
      EFI_ACPI_DESCRIPTION_HEADER *rsdt_ptr = (EFI_ACPI_DESCRIPTION_HEADER *)GuAcpiPaddrToPtr(rsdt_addr, acpi10_ptr);
      if (rsdt_ptr != NULL && rsdt_ptr->Length >= (sizeof(EFI_ACPI_DESCRIPTION_HEADER) + sizeof(UINT32))) {
        rsdt_entry0 = *(UINT32 *)((UINT8 *)rsdt_ptr + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
      }
    }
    GuLog("ACPI 1.0 RSDP Forensic: Ptr=%#llx RsdtAddress=%#08x RSDT.Entry[0]=%#08x\n",
          (unsigned long long)(UINTN)acpi10_ptr, (unsigned)rsdt_addr, (unsigned)rsdt_entry0);
    GuLog("  RSDP10 Bytes (20b): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
          r10[0], r10[1], r10[2], r10[3], r10[4], r10[5], r10[6], r10[7],
          r10[8], r10[9], r10[10], r10[11], r10[12], r10[13], r10[14], r10[15],
          r10[16], r10[17], r10[18], r10[19]);
  }

  if (mShadowState.orig_s3_save != NULL) {
    const UINT8 *fn_bytes = (const UINT8 *)(UINTN)mShadowState.orig_s3_save;
    GuLog("AcpiS3Save->S3Save Code Hex Dump (256 bytes at %#llx):\n", (unsigned long long)(UINTN)fn_bytes);
    for (UINTN row = 0; row < 16; row++) {
      UINTN off = row * 16;
      GuLog("  +%03x: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
            (unsigned)off,
            fn_bytes[off+0], fn_bytes[off+1], fn_bytes[off+2], fn_bytes[off+3],
            fn_bytes[off+4], fn_bytes[off+5], fn_bytes[off+6], fn_bytes[off+7],
            fn_bytes[off+8], fn_bytes[off+9], fn_bytes[off+10], fn_bytes[off+11],
            fn_bytes[off+12], fn_bytes[off+13], fn_bytes[off+14], fn_bytes[off+15]);
    }
  }

  /* Record pointers and checksums into Info for telemetry & tests */
  Info->shadow_fadt = shadow_fadt;
  Info->shadow_fadt_phys = shadow_fadt_phys;
  Info->shadow_xsdt = shadow_xsdt;
  Info->shadow_xsdt_phys = shadow_xsdt_phys;
  Info->shadow_rsdp = shadow_rsdp;
  Info->shadow_rsdp_phys = shadow_rsdp_phys;
  Info->shadow_rsdp_checksum = shadow_rsdp->Checksum;
  Info->shadow_rsdp_extended_checksum = shadow_rsdp->ExtendedChecksum;
  Info->shadow_xsdt_checksum = shadow_xsdt ? shadow_xsdt->Checksum : 0;
  Info->trampoline_phys = trampoline_phys;
  Info->ebs_hook_installed = FALSE;

  GuLog("S3 Plan C Complete Shadow Chain Armed:\n"
        "  RealRSDP=%#llx RealXSDT=%#llx RealFADT=%#llx RealFACS=%#llx (WakingVector=%#08x)\n"
        "  ShadowRSDP=%#llx (Chk20=%#02x ExtChk36=%#02x) -> ShadowXSDT=%#llx (Chk=%#02x)\n"
        "  ShadowXSDT [idx=%d] -> ShadowFADT=%#llx (FirmwareCtrl=%#08x XFirmwareCtrl=0 Chk=%#02x)\n"
        "  ShadowFACS=%#llx LowTrampoline=%#llx HighAcpiPage=%#llx\n"
        "  TelemetryPage=%#llx (outside 0xdadda000..0xdc8da000)\n"
        "  ConfigTable In-Place Patching: DEFERRED to GuBootWindows()\n",
        (unsigned long long)(UINTN)Info->rsdp, (unsigned long long)Info->xsdt_phys,
        (unsigned long long)Info->fadt_phys, (unsigned long long)Info->real_facs_phys,
        (unsigned)Info->real_facs->FirmwareWakingVector,
        (unsigned long long)shadow_rsdp_phys, (unsigned)shadow_rsdp->Checksum, (unsigned)shadow_rsdp->ExtendedChecksum,
        (unsigned long long)shadow_xsdt_phys, shadow_xsdt ? (unsigned)shadow_xsdt->Checksum : 0,
        (int)Info->xsdt_fadt_index, (unsigned long long)shadow_fadt_phys,
        (unsigned)shadow_fadt->FirmwareCtrl, (unsigned)shadow_fadt->Header.Checksum,
        (unsigned long long)shadow_facs_phys, (unsigned long long)trampoline_phys,
        (unsigned long long)high_acpi_phys,
        (unsigned long long)telem_page_phys);

  return EFI_SUCCESS;
}
