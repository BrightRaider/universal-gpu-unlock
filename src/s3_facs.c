/* SPDX-License-Identifier: GPL-2.0-only */
#include "s3_facs.h"
#include "port.h"
#include "compat/compat.h"

typedef char gu_s3_ctx_size_assert[(sizeof(GU_S3_RESUME_CONTEXT) == 108) ? 1 : -1];

typedef struct {
  BOOLEAN installed;
  INTN acpi20_idx;
  INTN acpi10_idx;
  VOID *orig_acpi20_vendor_table;
  VOID *orig_acpi10_vendor_table;
  VOID *shadow_rsdp;
  VOID *low_trampoline;
  EFI_SYSTEM_TABLE *system_table;
  EFI_EXIT_BOOT_SERVICES orig_exit_boot_services;
  BOOLEAN hook_active;
} GU_ACPI_SHADOW_CHAIN_STATE;

static GU_ACPI_SHADOW_CHAIN_STATE mShadowState = {
  FALSE, -1, -1, NULL, NULL, NULL, NULL, NULL, NULL, FALSE
};

VOID GuAcpiApplyShadowConfigTable(VOID) {
  if (!mShadowState.installed || !mShadowState.system_table || !mShadowState.shadow_rsdp) return;
  if (mShadowState.acpi20_idx >= 0 && (UINTN)mShadowState.acpi20_idx < mShadowState.system_table->NumberOfTableEntries) {
    mShadowState.system_table->ConfigurationTable[mShadowState.acpi20_idx].VendorTable = mShadowState.shadow_rsdp;
  }
  if (mShadowState.acpi10_idx >= 0 && (UINTN)mShadowState.acpi10_idx < mShadowState.system_table->NumberOfTableEntries) {
    mShadowState.system_table->ConfigurationTable[mShadowState.acpi10_idx].VendorTable = mShadowState.shadow_rsdp;
  }
}

VOID GuAcpiRestoreConfigTable(VOID) {
  if (!mShadowState.installed || !mShadowState.system_table) return;
  if (mShadowState.acpi20_idx >= 0 && (UINTN)mShadowState.acpi20_idx < mShadowState.system_table->NumberOfTableEntries) {
    if (mShadowState.orig_acpi20_vendor_table != NULL) {
      mShadowState.system_table->ConfigurationTable[mShadowState.acpi20_idx].VendorTable = mShadowState.orig_acpi20_vendor_table;
    }
  }
  if (mShadowState.acpi10_idx >= 0 && (UINTN)mShadowState.acpi10_idx < mShadowState.system_table->NumberOfTableEntries) {
    if (mShadowState.orig_acpi10_vendor_table != NULL) {
      mShadowState.system_table->ConfigurationTable[mShadowState.acpi10_idx].VendorTable = mShadowState.orig_acpi10_vendor_table;
    }
  }
}

EFI_STATUS EFIAPI GuHookedExitBootServices(
    IN EFI_HANDLE ImageHandle,
    IN UINTN MapKey
) {
  /*
   * Diagnostic counter: record EBS call in LowTrampoline buffer without BootServices calls.
   * +0x304: EbsCallCount (UINT32)
   * +0x308: EbsSuccessCount (UINT32)
   * +0x30C: EbsLastStatus (UINT32)
   */
  if (mShadowState.low_trampoline != NULL) {
    volatile UINT32 *diag = (volatile UINT32 *)((UINT8 *)mShadowState.low_trampoline + 0x304);
    diag[0]++; /* EbsCallCount at +0x304 */
  }

  /*
   * PHASE 1 (Sandwich Pre-Call):
   * Revert SystemTable ConfigurationTable to RealRSDP so that firmware drivers,
   * SMM runtime handlers, and BIOS S3 state save observe clean, unmodified ACPI tables!
   * MANDATORY: ABSOLUTELY NO BootServices calls inside here (no Print, no CalculateCrc32, no Logging).
   */
  GuAcpiRestoreConfigTable();

  /* Call original firmware ExitBootServices */
  EFI_STATUS Status = EFI_SUCCESS;
  if (mShadowState.orig_exit_boot_services != NULL) {
    Status = mShadowState.orig_exit_boot_services(ImageHandle, MapKey);
  }

  /* Record post-call diagnostics in LowTrampoline buffer */
  if (mShadowState.low_trampoline != NULL) {
    volatile UINT32 *diag = (volatile UINT32 *)((UINT8 *)mShadowState.low_trampoline + 0x304);
    if (!EFI_ERROR(Status)) {
      diag[1]++; /* EbsSuccessCount at +0x308 */
    }
    diag[2] = (UINT32)Status; /* Last Status at +0x30C */
  }

  /*
   * PHASE 2 (Sandwich Post-Call):
   * In v1.8.40b: ONLY re-apply ShadowRSDP if ExitBootServices failed (e.g.
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
  if (!SystemTable || !SystemTable->BootServices) return EFI_INVALID_PARAMETER;
  if (!mShadowState.hook_active) return EFI_SUCCESS;

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

  GuAcpiRestoreConfigTable();
  mShadowState.hook_active = FALSE;
  GuLog("S3 Plan C: ExitBootServices Sandwich Hook uninstalled\n");
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

  /* Warm-Reset / Re-Entry Detection: If ConfigurationTable was already pointing to ShadowRSDP (< 1MB or active shadow state) */
  if (rsdp != NULL && ((EFI_PHYSICAL_ADDRESS)(UINTN)rsdp < 0x00100000ULL ||
                       (mShadowState.installed && (VOID *)rsdp == mShadowState.shadow_rsdp))) {
    if (mShadowState.installed && mShadowState.orig_acpi20_vendor_table != NULL) {
      GuLog("GuAcpiLocateFadtAndFacs: Existing Shadow RSDP detected at %#llx; recovered Real RSDP at %#llx\n",
            (unsigned long long)(UINTN)rsdp, (unsigned long long)(UINTN)mShadowState.orig_acpi20_vendor_table);
      rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)mShadowState.orig_acpi20_vendor_table;
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
    memcpy(&shadow_magic, (UINT8 *)facs + 0x28, sizeof(UINT32));
    if (shadow_magic != GU_SHADOW_FACS_SIGNATURE) {
      memcpy(&shadow_magic, facs->Reserved, sizeof(UINT32));
    }
    if (shadow_magic == GU_SHADOW_FACS_SIGNATURE) {
      UINT64 orig_phys = 0;
      memcpy(&orig_phys, (UINT8 *)facs + 0x2C, sizeof(UINT64));
      if (orig_phys == 0) {
        memcpy(&orig_phys, &facs->Reserved[4], sizeof(UINT64));
      }
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
  Info->original_firmware_waking_vector = facs->FirmwareWakingVector;
  if (facs->Length >= 32) {
    Info->original_xfirmware_waking_vector = facs->XFirmwareWakingVector;
  }
  Info->found = TRUE;

  return EFI_SUCCESS;
}

UINT32 GuAcpiBuildTrampolineCode(
    OUT UINT8 *Buffer,
    IN UINT32 BufferSize,
    IN EFI_PHYSICAL_ADDRESS TrampolinePhys,
    IN EFI_PHYSICAL_ADDRESS RealFacsPhys,
    IN EFI_PHYSICAL_ADDRESS ShadowFacsPhys,
    IN UINT32 RootPortLinkReg,
    IN UINT32 GpuPmcsrReg
) {
  if (!Buffer || BufferSize < 1024) return 0;

  /*
   * Layout within Low Trampoline buffer (<1MB, 4096 bytes):
   * +0x000: 32-bit Flat Protected Mode S3 Resume Trampoline Machine Code
   * +0x180: 3-Entry GDT (24 bytes, 8-byte aligned)
   * +0x198: GDTR (6 bytes: limit=0x0017, base=TrampolinePhys + 0x180)
   * +0x1A0: Real-Mode IDTR (6 bytes: limit=0x03FF, base=0x00000000)
   * +0x200: Shadow FACS (64 bytes, 512-byte aligned per ACPI spec)
   * +0x280: Real FACS Vector Mirror (32-bit at 0x280, 64-bit at 0x288)
   * +0x300: Far Jump Target: [0x300] = Offset (16-bit), [0x302] = Segment (16-bit)
   * +0x304: EBS Call Count / Saved BIOS SP
   * +0x308: EBS Success Count / Saved BIOS SS
   * +0x30C: EBS Last Status
   * +0x310: Telemetry Entry Status (0xAA01) or Fault (0xDEAD)
   * +0x312: Telemetry Exit Status (0xAA02)
   * +0x314: Telemetry Captured Vector (32-bit dword)
   * +0x318: Telemetry Target Offset (16-bit)
   * +0x31A: Telemetry Target Segment (16-bit)
   * +0x31C: Telemetry Magic (0x53335F54 "S3_T")
   * +0x320: PCI Probe Addresses Table (8 dwords = 32 bytes)
   * +0x340: PCI Probe Raw Results (8 dwords = 32 bytes)
   * +0x360: PCI Probe Post-Arm Results (4 dwords = 16 bytes)
   */

  /* Pre-populate PCI probe addresses at +0x0320 (8 dwords = 32 bytes) */
  UINT32 *probe_addrs = (UINT32 *)(Buffer + 0x0320);
  probe_addrs[0] = 0x80001900U; /* Bridge 0:3:1 DevID (Expected 0x14831022) */
  probe_addrs[1] = 0x80001904U; /* Bridge 0:3:1 Cmd/Stat */
  probe_addrs[2] = 0x80001918U; /* Bridge 0:3:1 Bus Numbers (Pri, Sec, Sub) */
  probe_addrs[3] = RootPortLinkReg ? RootPortLinkReg : 0x80001960U; /* Bridge 0:3:1 Link Ctrl/Status (Cap 0x10) */
  probe_addrs[4] = 0x802D0000U; /* GPU 45:0:0 DevID (Expected 0x268410de if up) */
  probe_addrs[5] = 0x802D0004U; /* GPU 45:0:0 Cmd/Stat */
  probe_addrs[6] = 0x802D0010U; /* GPU 45:0:0 BAR0 */
  probe_addrs[7] = GpuPmcsrReg ? GpuPmcsrReg : 0x802D0064U; /* GPU 45:0:0 PMCSR (Cap 0x01) */

  UINT8 *e = Buffer + 0x000;

  /* =========================================================================
   * SECTION 0: POLYGLOT PREAMBLE (Valid in both 32-bit PM and 64-bit Long Mode)
   * Bytes 0..10:
   *   cli                  (FA)
   *   cld                  (FC)
   *   xor eax, eax         (31 C0)
   *   inc eax              (40: in 32-bit: inc eax -> ZF=0; in 64-bit: REX prefix -> ZF=1)
   *   jz near .is_64bit    (0F 84 <disp32>: 32-bit: no jump; 64-bit: jump!)
   * ========================================================================= */
  *e++ = 0xFA; /* cli */
  *e++ = 0xFC; /* cld */
  *e++ = 0x31; *e++ = 0xC0; /* xor eax, eax */
  *e++ = 0x40;             /* inc eax (32-bit) / REX (64-bit) */
  *e++ = 0x0F; *e++ = 0x84; /* jz near .is_64bit */
  UINT8 *jz_64bit_disp = e; e += 4; /* placeholder for 32-bit displacement */

  /* =========================================================================
   * SECTION 1: 32-BIT FLAT PROTECTED MODE PATH (BITS 32, stack-free)
   * Entered when firmware invokes XFirmwareWakingVector in 32-bit PM (OspmFlags=0).
   * Paging is OFF, flat 4GB linear memory.
   * ========================================================================= */
  /* Checkpoint 1 (32-bit entry): mov dword ptr [TrampolinePhys + 0x0310], 0x00003201 */
  *e++ = 0xC7; *e++ = 0x05;
  *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0310); e += 4;
  *(UINT32 *)e = 0x00003201U; e += 4;

  /* Magic: mov dword ptr [TrampolinePhys + 0x031C], 0x53335F54 ('S3_T') */
  *e++ = 0xC7; *e++ = 0x05;
  *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x031C); e += 4;
  *(UINT32 *)e = 0x53335F54U; e += 4;

  UINT8 *sec1_common_start = e;

  /* 32-bit PCI Probe Loop (8 dwords): esi=probe_addrs, edi=raw_results, ecx=8, dx=0x0CF8 */
  *e++ = 0xBE; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0320); e += 4; /* mov esi, imm32 */
  *e++ = 0xBF; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0340); e += 4; /* mov edi, imm32 */
  *e++ = 0xB9; *(UINT32 *)e = 8; e += 4;                                 /* mov ecx, 8 */
  *e++ = 0x66; *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C;                   /* mov dx, 0x0CF8 */

  UINT8 *pci_loop32_start = e;
  *e++ = 0x8B; *e++ = 0x06;                         /* mov eax, [esi] */
  *e++ = 0xEF;                                     /* out dx, eax */
  *e++ = 0x80; *e++ = 0xC2; *e++ = 0x04;             /* add dl, 4 (0x0CFC) */
  *e++ = 0xED;                                     /* in eax, dx */
  *e++ = 0x80; *e++ = 0xEA; *e++ = 0x04;             /* sub dl, 4 (0x0CF8) */
  *e++ = 0x89; *e++ = 0x07;                         /* mov [rdi], eax */
  *e++ = 0x83; *e++ = 0xC6; *e++ = 0x04;             /* add esi, 4 */
  *e++ = 0x83; *e++ = 0xC7; *e++ = 0x04;             /* add edi, 4 */
  *e++ = 0xE2;                                     /* loop .pci_loop */
  *e++ = (UINT8)(pci_loop32_start - (e + 1));

  /* Check AMD Vendor ID 0x1022 at [TrampolinePhys + 0x0340] */
  *e++ = 0x66; *e++ = 0xA1; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0340); e += 4; /* mov ax, [0x9E340] */
  *e++ = 0x66; *e++ = 0x3D; *e++ = 0x22; *e++ = 0x10; /* cmp ax, 0x1022 */
  *e++ = 0x75; UINT8 *skip_bridge32_disp = e++;      /* jne short .skip_bridge32 */

  /* Bridge arming */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0348); e += 4; /* mov eax, [0x9E348] */
  *e++ = 0x25; *e++ = 0x00; *e++ = 0x00; *e++ = 0x00; *e++ = 0xFF;     /* and eax, 0xFF000000 */
  *e++ = 0x0D; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x2D; *e++ = 0x00;     /* or eax, 0x002D2D00 */
  *e++ = 0x89; *e++ = 0xC1;                                             /* mov ecx, eax */

  *e++ = 0xB8; *e++ = 0x18; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80;     /* mov eax, 0x80001918 */
  *e++ = 0xEF;                                                         /* out dx, eax */
  *e++ = 0x89; *e++ = 0xC8;                                             /* mov eax, ecx */
  *e++ = 0x80; *e++ = 0xC2; *e++ = 0x04;                               /* add dl, 4 */
  *e++ = 0xEF;                                                         /* out dx, eax */
  *e++ = 0x80; *e++ = 0xEA; *e++ = 0x04;                               /* sub dl, 4 */

  /* GPU Probe (4 dwords) */
  *e++ = 0xBE; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0330); e += 4;
  *e++ = 0xBF; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0360); e += 4;
  *e++ = 0xB9; *(UINT32 *)e = 4; e += 4;

  UINT8 *gpu_loop32_start = e;
  *e++ = 0x8B; *e++ = 0x06;
  *e++ = 0xEF;
  *e++ = 0x80; *e++ = 0xC2; *e++ = 0x04;
  *e++ = 0xED;
  *e++ = 0x80; *e++ = 0xEA; *e++ = 0x04;
  *e++ = 0x89; *e++ = 0x07;
  *e++ = 0x83; *e++ = 0xC6; *e++ = 0x04;
  *e++ = 0x83; *e++ = 0xC7; *e++ = 0x04;
  *e++ = 0xE2;
  *e++ = (UINT8)(gpu_loop32_start - (e + 1));

  /* Restore Bridge */
  *e++ = 0xB8; *e++ = 0x18; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80;
  *e++ = 0xEF;
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0348); e += 4;
  *e++ = 0x80; *e++ = 0xC2; *e++ = 0x04;
  *e++ = 0xEF;

  *skip_bridge32_disp = (UINT8)(e - (skip_bridge32_disp + 1));

  /* Read 32-bit waking vector from Shadow FACS at [ShadowFacsPhys + 0x0C] */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(ShadowFacsPhys + 0x0C); e += 4; /* mov eax, [ShadowFacsPhys + 0x0C] */
  *e++ = 0x85; *e++ = 0xC0;                                            /* test eax, eax */
  *e++ = 0x75; UINT8 *f_vec32_0 = e++;                                /* jnz .got_vec32 */

  /* Fallback: Read backup from Real FACS [RealFacsPhys + 0x0C] */
  *e++ = 0xA1; *(UINT32 *)e = (UINT32)(RealFacsPhys + 0x0C); e += 4;
  *e++ = 0x85; *e++ = 0xC0;
  *e++ = 0x75; UINT8 *f_vec32_1 = e++;

  /* Ultimate fallback: 0x00001000 */
  *e++ = 0xB8; *(UINT32 *)e = 0x00001000U; e += 4;

  *f_vec32_0 = (UINT8)(e - (f_vec32_0 + 1));
  *f_vec32_1 = (UINT8)(e - (f_vec32_1 + 1));

  /* Save captured vector to [TrampolinePhys + 0x0314] */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0314); e += 4;

  /* Calculate IP = EAX & 0x0F, CS = EAX >> 4, EAX = (CS << 16) | IP */
  *e++ = 0x89; *e++ = 0xC3;             /* mov ebx, eax */
  *e++ = 0x83; *e++ = 0xE3; *e++ = 0x0F; /* and ebx, 0x0F */
  *e++ = 0xC1; *e++ = 0xE8; *e++ = 0x04; /* shr eax, 4 */
  *e++ = 0xC1; *e++ = 0xE0; *e++ = 0x10; /* shl eax, 16 */
  *e++ = 0x09; *e++ = 0xD8;             /* or eax, ebx */
  *e++ = 0xA3; *(UINT32 *)e = (UINT32)(TrampolinePhys + 0x0318); e += 4; /* Telemetry at +0x0318 */

  /* Directly patch the real-mode tail's far jump target */
  *e++ = 0xA3;
  UINT8 *rm16_patch_dest32 = e; e += 4; /* mov [rm16_tail_patch], eax */

  /* Load GDTR from [TrampolinePhys + GU_TRAMP_GDTR_OFF] (loads 6-byte GDTR in 32-bit mode) */
  *e++ = 0x0F; *e++ = 0x01; *e++ = 0x15;
  *(UINT32 *)e = (UINT32)(TrampolinePhys + GU_TRAMP_GDTR_OFF); e += 4;

  /* Direct Far Jump to 16-bit Protected Mode: jmp far 0x0018:pm16_rel_off */
  *e++ = 0xEA;
  UINT8 *pm16_jmp_offset32 = e; e += 4;
  *e++ = 0x18; *e++ = 0x00; /* selector 0x0018 (16-bit Code Segment) */

  /* =========================================================================
   * SECTION 2: 64-BIT LONG MODE PATH (BITS 64, stack-free)
   * Entered when firmware jumps in 64-bit Long Mode.
   * Drops from 64-bit Long Mode into 32-bit Compatibility Mode, disables paging,
   * and branches directly to .sec1_common to execute native flat 32-bit recon & resume.
   * ========================================================================= */
  *(INT32 *)jz_64bit_disp = (INT32)(e - (jz_64bit_disp + 4));

  /* Checkpoint 1 (64-bit entry): mov dword ptr [rip + disp32], 0x00006401 */
  *e++ = 0xC7; *e++ = 0x05;
  UINT8 *chk64_disp = e; e += 4;
  *e++ = 0x01; *e++ = 0x64; *e++ = 0x00; *e++ = 0x00;
  *(INT32 *)chk64_disp = (INT32)(0x0310 - (e - Buffer));

  /* Magic: mov dword ptr [rip + disp32], 0x53335F54 ('S3_T') */
  *e++ = 0xC7; *e++ = 0x05;
  UINT8 *magic64_disp = e; e += 4;
  *e++ = 0x54; *e++ = 0x5F; *e++ = 0x33; *e++ = 0x53;
  *(INT32 *)magic64_disp = (INT32)(0x031C - (e - Buffer));

  /* Load GDTR64 (10-byte descriptor at GU_TRAMP_GDTR_OFF 0x01A8) */
  *e++ = 0x0F; *e++ = 0x01; *e++ = 0x15;
  UINT8 *lgdt64_disp = e; e += 4;
  *(INT32 *)lgdt64_disp = (INT32)(GU_TRAMP_GDTR_OFF - (e - Buffer));

  /* Indirect Far Jump into 32-bit Compatibility Mode: jmp fword ptr [rip + disp32] (FF 2D) */
  *e++ = 0xFF; *e++ = 0x2D;
  UINT8 *far_jmp32_disp = e; e += 4;
  *(INT32 *)far_jmp32_disp = (INT32)(GU_TRAMP_FARPTR32_OFF - (e - Buffer));

  /* .pm32_compat: In 32-bit Compatibility Mode (CS=0x0008, L=0, D=1): Disable paging */
  UINT8 *pm32_compat_start = e;
  UINT32 pm32_rel = (UINT32)(pm32_compat_start - Buffer);

  /* Load 32-bit Flat Data selector 0x0010 into DS, ES, FS, GS, SS */
  *e++ = 0x66; *e++ = 0xB8; *e++ = 0x10; *e++ = 0x00; /* mov ax, 0x0010 */
  *e++ = 0x8E; *e++ = 0xD8;                         /* mov ds, ax */
  *e++ = 0x8E; *e++ = 0xC0;                         /* mov es, ax */
  *e++ = 0x8E; *e++ = 0xE0;                         /* mov fs, ax */
  *e++ = 0x8E; *e++ = 0xE8;                         /* mov gs, ax */
  *e++ = 0x8E; *e++ = 0xD0;                         /* mov ss, ax */

  /* Disable Paging: mov eax, cr0; and eax, 0x7FFFFFFF; mov cr0, eax (deactivates IA-32e mode!) */
  *e++ = 0x0F; *e++ = 0x20; *e++ = 0xC0;                             /* mov eax, cr0 */
  *e++ = 0x25; *e++ = 0xFF; *e++ = 0xFF; *e++ = 0xFF; *e++ = 0x7F; /* and eax, 0x7FFFFFFF */
  *e++ = 0x0F; *e++ = 0x22; *e++ = 0xC0;                             /* mov cr0, eax */

  /* Disable Long Mode in IA32_EFER MSR (0xC0000080): rdmsr; and eax, 0xFFFFF6FF; wrmsr */
  *e++ = 0xB9; *e++ = 0x80; *e++ = 0x00; *e++ = 0x00; *e++ = 0xC0; /* mov ecx, 0xC0000080 */
  *e++ = 0x0F; *e++ = 0x32;                                         /* rdmsr */
  *e++ = 0x25; *e++ = 0xFF; *e++ = 0xF6; *e++ = 0xFF; *e++ = 0xFF; /* and eax, 0xFFFFF6FF (clears LME bit 8, NXE bit 11) */
  *e++ = 0x0F; *e++ = 0x30;                                         /* wrmsr */

  /* Clear CR3: xor eax, eax; mov cr3, eax */
  *e++ = 0x31; *e++ = 0xC0;                                         /* xor eax, eax */
  *e++ = 0x0F; *e++ = 0x22; *e++ = 0xD8;                             /* mov cr3, eax */

  /* Disable PAE in CR4: mov eax, cr4; and eax, 0xFFFFFFDF; mov cr4, eax */
  *e++ = 0x0F; *e++ = 0x20; *e++ = 0xE0;                             /* mov eax, cr4 */
  *e++ = 0x25; *e++ = 0xDF; *e++ = 0xFF; *e++ = 0xFF; *e++ = 0xFF; /* and eax, 0xFFFFFFDF (clears PAE bit 5) */
  *e++ = 0x0F; *e++ = 0x22; *e++ = 0xE0;                             /* mov cr4, eax */

  /* Jump to common 32-bit reconnaissance & resume path: jmp rel32 .sec1_common */
  *e++ = 0xE9;
  UINT8 *jmp_sec1_disp = e; e += 4;
  *(INT32 *)jmp_sec1_disp = (INT32)(sec1_common_start - (jmp_sec1_disp + 4));

  /* =========================================================================
   * SECTION 3: 16-BIT PROTECTED MODE (BITS 16, stack-free)
   * Entered via direct far jump from Section 1 (CS = Selector 0x0018).
   * Base of Selector 0x0018 is TrampolinePhys (<1MB).
   * ========================================================================= */
  UINT8 *pm16_start = e;
  UINT32 pm16_rel = (UINT32)(pm16_start - Buffer);
  *(UINT32 *)pm16_jmp_offset32 = pm16_rel;

  /* Load data segment registers with 16-bit data selector 0x0020 (Base = TrampolinePhys) */
  *e++ = 0xB8; *e++ = 0x20; *e++ = 0x00;             /* mov ax, 0x0020 */
  *e++ = 0x8E; *e++ = 0xD8;                         /* mov ds, ax */
  *e++ = 0x8E; *e++ = 0xC0;                         /* mov es, ax */
  *e++ = 0x8E; *e++ = 0xE0;                         /* mov fs, ax */
  *e++ = 0x8E; *e++ = 0xE8;                         /* mov gs, ax */
  *e++ = 0x8E; *e++ = 0xD0;                         /* mov ss, ax */

  /* lidt [0x01B4] (Real-Mode IDTR relative to DS Base = TrampolinePhys) */
  *e++ = 0x0F; *e++ = 0x01; *e++ = 0x1E; *e++ = 0xB4; *e++ = 0x01;

  /* Clear PE bit in CR0 to switch into Real Mode */
  *e++ = 0x0F; *e++ = 0x20; *e++ = 0xC0;             /* mov eax, cr0 */
  *e++ = 0x24; *e++ = 0xFE;                         /* and al, 0xFE */
  *e++ = 0x0F; *e++ = 0x22; *e++ = 0xC0;             /* mov cr0, eax */

  /* Far jump to serialize and flush pipeline: jmp far (TrampolinePhys >> 4):rm16_rel_off */
  *e++ = 0xEA;
  UINT8 *rm16_jmp_offset = e; e += 2;               /* 16-bit offset placeholder */
  UINT16 rm_cs = (UINT16)(TrampolinePhys >> 4);
  *e++ = (UINT8)(rm_cs & 0xFF);
  *e++ = (UINT8)((rm_cs >> 8) & 0xFF);

  /* =========================================================================
   * SECTION 4: 16-BIT REAL MODE (BITS 16, stack-free)
   * Entered via direct far jump from Section 3 (CS = TrampolinePhys >> 4).
   * ========================================================================= */
  UINT8 *rm16_start = e;
  UINT16 rm16_rel = (UINT16)(rm16_start - Buffer);
  *(UINT16 *)rm16_jmp_offset = rm16_rel;

  /* Set DS = CS */
  *e++ = 0x8C; *e++ = 0xC8;                         /* mov ax, cs */
  *e++ = 0x8E; *e++ = 0xD8;                         /* mov ds, ax */

  /* Telemetry Checkpoint 2 (proves survived Real-Mode transition!): mov word [0x0312], 0xAA02 */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x12; *e++ = 0x03; *e++ = 0x02; *e++ = 0xAA;

  /* Zero general purpose registers for clean Windows handoff (EAX=0 also zeroes AX) */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC0;             /* xor eax, eax */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xDB;             /* xor ebx, ebx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC9;             /* xor ecx, ecx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xD2;             /* xor edx, edx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xF6;             /* xor esi, esi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xFF;             /* xor edi, edi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xED;             /* xor ebp, ebp */

  /* Zero data segments using AX = 0 */
  *e++ = 0x8E; *e++ = 0xD8;                         /* mov ds, ax */
  *e++ = 0x8E; *e++ = 0xC0;                         /* mov es, ax */
  *e++ = 0x8E; *e++ = 0xE0;                         /* mov fs, ax */
  *e++ = 0x8E; *e++ = 0xE8;                         /* mov gs, ax */
  *e++ = 0x8E; *e++ = 0xD0;                         /* mov ss, ax */

  /* Directly patched far jump to Windows HalpRMStub (EA ip_lo ip_hi cs_lo cs_hi) */
  UINT8 *far_jump_inst = e;
  *e++ = 0xEA;
  *e++ = 0x00; *e++ = 0x00;                         /* ip placeholder */
  *e++ = 0x00; *e++ = 0x01;                         /* cs placeholder (0x0100) */

  /* Fix up patch displacements from Section 1 */
  *(UINT32 *)rm16_patch_dest32 = (UINT32)(TrampolinePhys + (far_jump_inst + 1 - Buffer));

  /*
   * =========================================================================
   * SECTION 5: EMBED 5-ENTRY GDT, 10-BYTE GDTR64, RM_IDTR, AND FARPTR32
   * =========================================================================
   */
  /* GDT Entry 0 (Selector 0x0000): Null Descriptor */
  memset(Buffer + GU_TRAMP_GDT_OFF, 0, 8);

  /* GDT Entry 1 (Selector 0x0008): 32-bit Compatibility Code Descriptor (Base=0, Limit=4GB, Access=0x9A, Flags=0xCF) */
  UINT8 *c32_desc = Buffer + GU_TRAMP_GDT_OFF + 8;
  c32_desc[0] = 0xFF; c32_desc[1] = 0xFF;
  c32_desc[2] = 0x00; c32_desc[3] = 0x00;
  c32_desc[4] = 0x00; c32_desc[5] = 0x9A;
  c32_desc[6] = 0xCF; c32_desc[7] = 0x00;

  /* GDT Entry 2 (Selector 0x0010): 32-bit Flat Data Descriptor (Base=0, Limit=4GB, Access=0x92, Flags=0xCF) */
  UINT8 *d32_desc = Buffer + GU_TRAMP_GDT_OFF + 16;
  d32_desc[0] = 0xFF; d32_desc[1] = 0xFF;
  d32_desc[2] = 0x00; d32_desc[3] = 0x00;
  d32_desc[4] = 0x00; d32_desc[5] = 0x92;
  d32_desc[6] = 0xCF; d32_desc[7] = 0x00;

  /* GDT Entry 3 (Selector 0x0018): 16-bit Code Descriptor (Base=TrampolinePhys, Limit=64KB, Access=0x9A, Flags=0x00) */
  UINT8 *c16_desc = Buffer + GU_TRAMP_GDT_OFF + 24;
  c16_desc[0] = 0xFF; c16_desc[1] = 0xFF;
  c16_desc[2] = (UINT8)(TrampolinePhys & 0xFF);
  c16_desc[3] = (UINT8)((TrampolinePhys >> 8) & 0xFF);
  c16_desc[4] = (UINT8)((TrampolinePhys >> 16) & 0xFF);
  c16_desc[5] = 0x9A;
  c16_desc[6] = 0x00;
  c16_desc[7] = (UINT8)((TrampolinePhys >> 24) & 0xFF);

  /* GDT Entry 4 (Selector 0x0020): 16-bit Data Descriptor (Base=TrampolinePhys, Limit=64KB, Access=0x92, Flags=0x00) */
  UINT8 *d16_desc = Buffer + GU_TRAMP_GDT_OFF + 32;
  d16_desc[0] = 0xFF; d16_desc[1] = 0xFF;
  d16_desc[2] = (UINT8)(TrampolinePhys & 0xFF);
  d16_desc[3] = (UINT8)((TrampolinePhys >> 8) & 0xFF);
  d16_desc[4] = (UINT8)((TrampolinePhys >> 16) & 0xFF);
  d16_desc[5] = 0x92;
  d16_desc[6] = 0x00;
  d16_desc[7] = (UINT8)((TrampolinePhys >> 24) & 0xFF);

  /* GDTR64 at GU_TRAMP_GDTR_OFF (0x01A8): 10 bytes packed (UINT16 Limit=0x0027, UINT64 Base=TrampolinePhys + 0x0180) */
  *(UINT16 *)(Buffer + GU_TRAMP_GDTR_OFF) = 0x0027; /* 5 descriptors * 8 bytes - 1 = 39 */
  *(UINT64 *)(Buffer + GU_TRAMP_GDTR_OFF + 2) = (UINT64)(TrampolinePhys + GU_TRAMP_GDT_OFF);

  /* Real-Mode IDTR at GU_TRAMP_RM_IDTR_OFF (0x01B4): 6 bytes packed (UINT16 Limit=0x03FF, UINT32 Base=0x00000000) */
  *(UINT16 *)(Buffer + GU_TRAMP_RM_IDTR_OFF) = 0x03FF;
  *(UINT32 *)(Buffer + GU_TRAMP_RM_IDTR_OFF + 2) = 0x00000000;

  /* FARPTR32 at GU_TRAMP_FARPTR32_OFF (0x01C0): 6 bytes packed (UINT32 Offset=TrampolinePhys + pm32_rel, UINT16 Selector=0x0008) */
  *(UINT32 *)(Buffer + GU_TRAMP_FARPTR32_OFF) = (UINT32)(TrampolinePhys + pm32_rel);
  *(UINT16 *)(Buffer + GU_TRAMP_FARPTR32_OFF + 4) = 0x0008;

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
    if (entry_chk == 0x3201 || entry_chk == 0x6401 || entry_chk == 0xAA01 || entry_chk == 0xDEAD) {
      telemetry_page = low_base;
      telemetry_phys = LowTrampPhys;
    }
  }

  if (!telemetry_page && LowTrampPhys < 0x00100000ULL) {
    /* Scan 16 low-memory pages from 0x00090000 to 0x0009F000 (4-KB steps) */
    for (EFI_PHYSICAL_ADDRESS page_addr = 0x00090000ULL; page_addr <= 0x0009F000ULL; page_addr += 4096) {
      UINT8 *candidate = (UINT8 *)(UINTN)page_addr;
      if (*(UINT32 *)(candidate + GU_TRAMP_MAGIC_OFF) == GU_TRAMP_TELEMETRY_MAGIC) {
        UINT16 entry_chk = *(UINT16 *)(candidate + GU_TRAMP_ENTRY_STATUS_OFF);
        if (entry_chk == 0x3201 || entry_chk == 0x6401 || entry_chk == 0xAA01 || entry_chk == 0xDEAD) {
          telemetry_page = candidate;
          telemetry_phys = page_addr;
          break;
        }
      }
    }
  }

  if (telemetry_page != NULL) {
    UINT16 entry_status = *(UINT16 *)(telemetry_page + GU_TRAMP_ENTRY_STATUS_OFF);
    UINT16 exit_status  = *(UINT16 *)(telemetry_page + GU_TRAMP_EXIT_STATUS_OFF);
    UINT32 full_vec     = *(UINT32 *)(telemetry_page + GU_TRAMP_CAPTURED_VEC_OFF);
    UINT16 tgt_off      = *(UINT16 *)(telemetry_page + GU_TRAMP_TARGET_PTR_OFF);
    UINT16 tgt_seg      = *(UINT16 *)(telemetry_page + GU_TRAMP_TARGET_PTR_OFF + 2);
    const char *mode_str = "Unknown";
    if (entry_status == 0x3201) mode_str = "32-Bit Flat PM";
    else if (entry_status == 0x6401) mode_str = "64-Bit Long Mode";
    else if (entry_status == 0xAA01) mode_str = "Legacy PM/LM";
    GuLog("S3 Prior Wake Telemetry (Page %#llx): Magic=S3_T Mode=%a Entry=%#04x Vector=%#08x Target=%04x:%04x Exit=%#04x\n",
          (unsigned long long)telemetry_phys, mode_str,
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

    /* Zero out the telemetry magic on the found page after reading */
    *(UINT32 *)(telemetry_page + GU_TRAMP_MAGIC_OFF) = 0;
  } else {
    GuLog("S3 Prior Wake Telemetry: none found\n");
  }

  /*
   * Low (<1MB) ACPI NVS Allocation (4096 bytes):
   * +0x00000: Pure 16-bit Real Mode S3 Resume Trampoline Machine Code (<380 bytes)
   * +0x00200: Shadow FACS (64 bytes, 512-byte aligned per ACPI spec)
   * +0x00280: Real FACS Vector Mirror (32-bit at 0x280, 64-bit at 0x288)
   * +0x00300: Telemetry Checkpoints & Waking Vector capture
   * +0x00320: PCI Probe Addresses Table (8 dwords)
   * +0x00340: PCI Probe Raw Results (8 dwords)
   * +0x00360: PCI Probe Post-Arm Results (4 dwords)
   */
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = LowTrampPhys + 0x00200;
  EFI_PHYSICAL_ADDRESS trampoline_phys  = LowTrampPhys;

  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(low_base + 0x00200);

  /* Clear the low trampoline memory page */
  memset(low_base, 0, 4096);

  /* 2. Clone Real FACS into Shadow FACS */
  memcpy(shadow_facs, Info->real_facs, sizeof(EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE));
  if (shadow_facs->Version < 2) shadow_facs->Version = 2;
  if (Info->real_facs->Version < 2) Info->real_facs->Version = 2;

  shadow_facs->FirmwareWakingVector = (UINT32)trampoline_phys;
  shadow_facs->XFirmwareWakingVector = (UINT64)trampoline_phys;
  shadow_facs->Flags |= 0x00000002U; /* 64-bit wake supported */
  *(UINT32 *)((UINT8 *)shadow_facs + 0x24) |= 0x00000001U; /* OspmFlags: 64-bit wake enabled */

  /* Tag Shadow FACS with signature and real FACS address at +0x28 / +0x2C (Reserved1) for warm reset recovery */
  *(UINT32 *)((UINT8 *)shadow_facs + 0x28) = GU_SHADOW_FACS_SIGNATURE;
  memcpy((UINT8 *)shadow_facs + 0x2C, &Info->real_facs_phys, sizeof(UINT64));

  /* Set 64-bit wake in Real FACS as well so EDK2 S3Resume2Pei enters via XFirmwareWakingVector */
  Info->real_facs->Flags |= 0x00000002U;
  *(UINT32 *)((UINT8 *)Info->real_facs + 0x24) |= 0x00000001U;
  Info->real_facs->XFirmwareWakingVector = (UINT64)trampoline_phys;
  Info->real_facs->FirmwareWakingVector = (UINT32)trampoline_phys;

  /* Store Real FACS original vectors as backup mirror in low memory at +0x280 / +0x288 */
  *(UINT32 *)(low_base + 0x0280) = Info->original_firmware_waking_vector;
  *(UINT64 *)(low_base + 0x0288) = Info->original_xfirmware_waking_vector;

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

  /* 3. Build 64-bit Long Mode S3 Resume Trampoline machine code in low memory (<1MB) */
  UINT32 tramp_bytes = GuAcpiBuildTrampolineCode(
      low_base,
      4096,
      trampoline_phys,
      Info->real_facs_phys,
      shadow_facs_phys,
      root_port_link_reg,
      gpu_pmcsr_reg
  );

  if (tramp_bytes == 0) {
    GuLog("GuAcpiInstallShadowFacs: Failed to build trampoline machine code\n");
    return EFI_DEVICE_ERROR;
  }

  Info->shadow_facs_phys = shadow_facs_phys;
  EFI_PHYSICAL_ADDRESS shadow_fadt_phys = LowTrampPhys + GU_TRAMP_SHADOW_FADT_OFF;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *shadow_fadt =
      (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)(low_base + GU_TRAMP_SHADOW_FADT_OFF);
  Info->shadow_fadt = shadow_fadt;
  Info->shadow_fadt_phys = shadow_fadt_phys;
  Info->trampoline_phys = trampoline_phys;

  /*
   * Plan C: Complete Shadow Chain Architecture (v1.8.40)
   * Real ACPI tables (RealRSDP, RealRSDT, RealXSDT, RealFADT) remain 100% UNTOUCHED byte-for-byte.
   * Complete shadow hierarchy is constructed in EfiACPIMemoryNVS:
   * ShadowRSDP (0x9E600) -> ShadowXSDT (0x9E640) -> ShadowFADT (0x9E400) -> ShadowFACS (0x9E200)
   */
  GuLog("S3 Plan C Pre-Build: RealRSDP=%#llx RealXSDT=%#llx (Chk=%#02x) RealFADT=%#llx (Len=%u Chk=%#02x)\n",
        (unsigned long long)(UINTN)Info->rsdp,
        (unsigned long long)Info->xsdt_phys, (unsigned)Info->xsdt_original_checksum,
        (unsigned long long)Info->fadt_phys, (unsigned)Info->fadt->Header.Length,
        (unsigned)Info->fadt_original_checksum);

  /*
   * 1. Populate ShadowFADT at LowTrampoline + 0x400 (e.g. 0x9E400):
   *    Copy full Header.Length bytes from original RealFADT.
   *    Set FirmwareCtrl = ShadowFACS (0x9E200), XFirmwareCtrl = 0.
   *    Recalculate ShadowFADT->Header.Checksum.
   */
  UINT32 fadt_len = Info->fadt->Header.Length;
  if (fadt_len > (GU_TRAMP_SHADOW_RSDP_OFF - GU_TRAMP_SHADOW_FADT_OFF)) {
    fadt_len = GU_TRAMP_SHADOW_RSDP_OFF - GU_TRAMP_SHADOW_FADT_OFF;
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
   * 2. Populate ShadowXSDT:
   *    If RealXSDT fits in remaining LowTrampoline buffer (Length <= 0x9C0 = 2496 bytes),
   *    place at LowTrampoline + 0x640 (e.g. 0x9E640). Otherwise allocate a page under 4GB.
   *    Copy RealXSDT, update FACP/FADT entry to point to ShadowFADT, recalculate Checksum.
   */
  EFI_ACPI_DESCRIPTION_HEADER *shadow_xsdt = NULL;
  EFI_PHYSICAL_ADDRESS shadow_xsdt_phys = 0;

  if (Info->xsdt != NULL) {
    UINT32 max_inline_xsdt = 4096 - GU_TRAMP_SHADOW_XSDT_OFF;
    if (Info->xsdt->Length <= max_inline_xsdt) {
      shadow_xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(low_base + GU_TRAMP_SHADOW_XSDT_OFF);
      shadow_xsdt_phys = LowTrampPhys + GU_TRAMP_SHADOW_XSDT_OFF;
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
   * 3. Populate ShadowRSDP at LowTrampoline + 0x600 (e.g. 0x9E600):
   *    Copy RealRSDP (36 bytes for Revision >= 2).
   *    Set XsdtAddress = ShadowXSDT.
   *    Leave RsdtAddress = RealRSDP->RsdtAddress untouched.
   *    Recalculate 20-byte legacy Checksum (Offset 8), THEN 36-byte ExtendedChecksum (Offset 32).
   */
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *shadow_rsdp =
      (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)(low_base + GU_TRAMP_SHADOW_RSDP_OFF);
  EFI_PHYSICAL_ADDRESS shadow_rsdp_phys = LowTrampPhys + GU_TRAMP_SHADOW_RSDP_OFF;

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
   * 4. In-Place Patch SystemTable->ConfigurationTable:
   *    Save original VendorTable pointers, point entries to ShadowRSDP.
   */
  mShadowState.installed = TRUE;
  mShadowState.acpi20_idx = Info->config_table_acpi20_idx;
  mShadowState.acpi10_idx = Info->config_table_acpi10_idx;
  mShadowState.orig_acpi20_vendor_table = (Info->config_table_acpi20_idx >= 0) ?
      (VOID *)(UINTN)Info->rsdp : NULL;
  mShadowState.orig_acpi10_vendor_table = (Info->config_table_acpi10_idx >= 0) ?
      (VOID *)(UINTN)Info->rsdp : NULL;
  mShadowState.shadow_rsdp = (VOID *)(UINTN)shadow_rsdp_phys;
  mShadowState.low_trampoline = (VOID *)low_base;
  mShadowState.system_table = SystemTable;
  mShadowState.orig_exit_boot_services = NULL;
  mShadowState.hook_active = FALSE;

  GuAcpiApplyShadowConfigTable();

  /*
   * 5. Hook Real FACS waking vector to point to our Low Trampoline (<1MB)
   *    RealFACS is what AMI Aptio V PEI wakes to!
   *    RealFADT and RealXSDT are 100% UNTOUCHED!
   */
  if (Info->real_facs->Version < 2) Info->real_facs->Version = 2;
  Info->real_facs->Flags |= 0x00000002U;
  *(UINT32 *)((UINT8 *)Info->real_facs + 0x24) |= 0x00000001U; /* OspmFlags: 64-bit wake enabled */
  Info->real_facs->FirmwareWakingVector = (UINT32)trampoline_phys;
  Info->real_facs->XFirmwareWakingVector = (UINT64)trampoline_phys;

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
        "  RealRSDP=%#llx RealXSDT=%#llx RealFADT=%#llx RealFACS=%#llx (Wake=%#08x XWake=%#llx)\n"
        "  ShadowRSDP=%#llx (Chk20=%#02x ExtChk36=%#02x) -> ShadowXSDT=%#llx (Chk=%#02x)\n"
        "  ShadowXSDT [idx=%d] -> ShadowFADT=%#llx (FirmwareCtrl=%#08x XFirmwareCtrl=0 Chk=%#02x)\n"
        "  ShadowFACS=%#llx (XWake=%#llx) LowTrampoline=%#llx\n"
        "  ConfigTable In-Place Patched: Acpi20Idx=%d (RealRSDP=%#llx -> ShadowRSDP=%#llx) Acpi10Idx=%d\n",
        (unsigned long long)(UINTN)Info->rsdp, (unsigned long long)Info->xsdt_phys,
        (unsigned long long)Info->fadt_phys, (unsigned long long)Info->real_facs_phys,
        (unsigned)Info->real_facs->FirmwareWakingVector,
        (unsigned long long)Info->real_facs->XFirmwareWakingVector,
        (unsigned long long)shadow_rsdp_phys, (unsigned)shadow_rsdp->Checksum, (unsigned)shadow_rsdp->ExtendedChecksum,
        (unsigned long long)shadow_xsdt_phys, shadow_xsdt ? (unsigned)shadow_xsdt->Checksum : 0,
        (int)Info->xsdt_fadt_index, (unsigned long long)shadow_fadt_phys,
        (unsigned)shadow_fadt->FirmwareCtrl, (unsigned)shadow_fadt->Header.Checksum,
        (unsigned long long)shadow_facs_phys, (unsigned long long)shadow_facs->XFirmwareWakingVector,
        (unsigned long long)trampoline_phys,
        (int)mShadowState.acpi20_idx, (unsigned long long)(UINTN)mShadowState.orig_acpi20_vendor_table,
        (unsigned long long)shadow_rsdp_phys, (int)mShadowState.acpi10_idx);

  return EFI_SUCCESS;
}
