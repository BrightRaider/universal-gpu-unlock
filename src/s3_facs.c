/* SPDX-License-Identifier: GPL-2.0-only */
#include "s3_facs.h"
#include "port.h"
#include "compat/compat.h"

typedef char gu_s3_ctx_size_assert[(sizeof(GU_S3_RESUME_CONTEXT) == 100) ? 1 : -1];

static BOOLEAN GuidMatch(const EFI_GUID *a, const EFI_GUID *b) {
  const UINT8 *p1 = (const UINT8 *)a;
  const UINT8 *p2 = (const UINT8 *)b;
  for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
    if (p1[i] != p2[i]) return FALSE;
  }
  return TRUE;
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
  for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
    if (GuidMatch(&SystemTable->ConfigurationTable[i].VendorGuid, &Acpi20Guid)) {
      rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)SystemTable->ConfigurationTable[i].VendorTable;
      break;
    }
  }
  if (!rsdp) {
    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
      if (GuidMatch(&SystemTable->ConfigurationTable[i].VendorGuid, &Acpi10Guid)) {
        rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)SystemTable->ConfigurationTable[i].VendorTable;
        break;
      }
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
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *fadt = NULL;

  /* 2. Traverse XSDT (64-bit table pointers) if ACPI 2.0+ */
  if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
    EFI_ACPI_DESCRIPTION_HEADER *xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)rsdp->XsdtAddress;
    if (xsdt && xsdt->Length >= sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
      UINTN count = (xsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT64);
      UINT8 *entry_ptr = ((UINT8 *)xsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
      for (UINTN i = 0; i < count; i++) {
        UINT64 table_phys = 0;
        memcpy(&table_phys, entry_ptr + i * sizeof(UINT64), sizeof(UINT64));
        EFI_ACPI_DESCRIPTION_HEADER *tbl = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)table_phys;
        if (tbl && tbl->Signature == GU_ACPI_FADT_SIGNATURE) {
          fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)tbl;
          break;
        }
      }
    }
  }

  /* 3. Fallback: Traverse RSDT (32-bit table pointers) */
  if (!fadt && rsdp->RsdtAddress != 0) {
    EFI_ACPI_DESCRIPTION_HEADER *rsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)rsdp->RsdtAddress;
    if (rsdt && rsdt->Length >= sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
      UINTN count = (rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);
      UINT8 *entry_ptr = ((UINT8 *)rsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
      for (UINTN i = 0; i < count; i++) {
        UINT32 table_phys = 0;
        memcpy(&table_phys, entry_ptr + i * sizeof(UINT32), sizeof(UINT32));
        EFI_ACPI_DESCRIPTION_HEADER *tbl = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)table_phys;
        if (tbl && tbl->Signature == GU_ACPI_FADT_SIGNATURE) {
          fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)tbl;
          break;
        }
      }
    }
  }

  if (!fadt) {
    GuLog("GuAcpiLocateFadtAndFacs: FADT ('FACP') table not found in XSDT/RSDT\n");
    return EFI_NOT_FOUND;
  }

  Info->fadt = fadt;
  Info->fadt_original_checksum = fadt->Header.Checksum;

  /* 4. Resolve active FACS address: per ACPI 2.0+ specification, XFirmwareCtrl takes precedence if non-zero */
  EFI_PHYSICAL_ADDRESS facs_phys = 0;
  if (fadt->Header.Length > 132 && fadt->XFirmwareCtrl != 0) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)fadt->XFirmwareCtrl;
  }
  if (facs_phys == 0) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)fadt->FirmwareCtrl;
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
    IN EFI_PHYSICAL_ADDRESS ContextPhys,
    IN EFI_PHYSICAL_ADDRESS ShadowFacsPhys
) {
  if (!Buffer || BufferSize < 1024) return 0;
  (VOID)ContextPhys;
  (VOID)ShadowFacsPhys;

  /*
   * Layout within Low Trampoline buffer (<1MB, 4096 bytes):
   * +0x000: 16-bit Pure Real Mode S3 Resume Trampoline Machine Code
   * +0x030: Far Jump Target: [0x30] = Offset (16-bit), [0x32] = Segment (16-bit)
   * +0x034: Saved BIOS SP
   * +0x036: Saved BIOS SS
   * +0x040: Telemetry Entry Status (0xAA01) or Fault (0xDEAD)
   * +0x042: Telemetry Exit Status (0xAA02)
   * +0x044: Telemetry Captured Vector (Low 16 bits)
   * +0x046: Telemetry Captured Vector (High 16 bits)
   * +0x048: Telemetry Target Offset
   * +0x04A: Telemetry Target Segment
   * +0x04C: Telemetry Magic (0x53335F54 "S3_T")
   * +0x100: Shadow FACS (64 bytes, 256-byte aligned per ACPI spec)
   * +0x180: Real FACS Vector Mirror (32-bit at 0x180, 64-bit at 0x188)
   */

  UINT8 *e = Buffer + 0x000;

  *e++ = 0xFA; /* cli */
  *e++ = 0xFC; /* cld */
  *e++ = 0x8C; *e++ = 0xC8; /* mov ax, cs */
  *e++ = 0x8E; *e++ = 0xD8; /* mov ds, ax */
  *e++ = 0x89; *e++ = 0x26; *e++ = 0x34; *e++ = 0x00; /* mov [0x0034], sp */
  *e++ = 0x8C; *e++ = 0x16; *e++ = 0x36; *e++ = 0x00; /* mov [0x0036], ss */

  /* Set Telemetry Magic: mov dword [0x004C], 0x53335F54 ('S3_T') */
  *e++ = 0x66; *e++ = 0xC7; *e++ = 0x06; *e++ = 0x4C; *e++ = 0x00;
  *e++ = 0x54; *e++ = 0x5F; *e++ = 0x33; *e++ = 0x53;

  /* Telemetry Entry Checkpoint: mov word [0x0040], 0xAA01 */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x40; *e++ = 0x00; *e++ = 0x01; *e++ = 0xAA;

  /* Read 32-bit waking vector from Shadow FACS at 0x0100: [0x010C] */
  *e++ = 0xA1; *e++ = 0x0C; *e++ = 0x01;             /* mov ax, [0x010C] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x0E; *e++ = 0x01; /* mov dx, [0x010E] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax0 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx0 = e++;                   /* jnz .have_vec */

  /* Read 64-bit waking vector from Shadow FACS at 0x0100: [0x0118] */
  *e++ = 0xA1; *e++ = 0x18; *e++ = 0x01;             /* mov ax, [0x0118] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x1A; *e++ = 0x01; /* mov dx, [0x011A] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax1 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx1 = e++;                   /* jnz .have_vec */

  /* Fallback: Read backup 32-bit vector from Real FACS mirror at [0x0180] */
  *e++ = 0xA1; *e++ = 0x80; *e++ = 0x01;             /* mov ax, [0x0180] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x82; *e++ = 0x01; /* mov dx, [0x0182] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax2 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx2 = e++;                   /* jnz .have_vec */

  /* Fallback: Read backup 64-bit vector from Real FACS mirror at [0x0188] */
  *e++ = 0xA1; *e++ = 0x88; *e++ = 0x01;             /* mov ax, [0x0188] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x8A; *e++ = 0x01; /* mov dx, [0x018A] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax3 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx3 = e++;                   /* jnz .have_vec */

  /* No vector found in any FACS table! Flag error 0xDEAD and halt safely */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x40; *e++ = 0x00; *e++ = 0xAD; *e++ = 0xDE;
  *e++ = 0xF4; /* hlt */

  /* .have_vec target */
  UINT8 have_vec_disp = (UINT8)(e - (f_ax0 + 1));
  *f_ax0 = have_vec_disp;
  *f_dx0 = (UINT8)(e - (f_dx0 + 1));
  *f_ax1 = (UINT8)(e - (f_ax1 + 1));
  *f_dx1 = (UINT8)(e - (f_dx1 + 1));
  *f_ax2 = (UINT8)(e - (f_ax2 + 1));
  *f_dx2 = (UINT8)(e - (f_dx2 + 1));
  *f_ax3 = (UINT8)(e - (f_ax3 + 1));
  *f_dx3 = (UINT8)(e - (f_dx3 + 1));

  /* Save captured vector to telemetry [0x0044], [0x0046] */
  *e++ = 0xA3; *e++ = 0x44; *e++ = 0x00;             /* mov [0x0044], ax */
  *e++ = 0x89; *e++ = 0x16; *e++ = 0x46; *e++ = 0x00; /* mov [0x0046], dx */

  /* Convert linear DX:AX into Segment:Offset */
  /* Offset = AX & 0x000F */
  *e++ = 0x89; *e++ = 0xC3;                         /* mov bx, ax */
  *e++ = 0x83; *e++ = 0xE3; *e++ = 0x0F;             /* and bx, 0x0F */

  /* Segment = (AX >> 4) | (DX << 12) */
  *e++ = 0xC1; *e++ = 0xE8; *e++ = 0x04;             /* shr ax, 4 */
  *e++ = 0xC1; *e++ = 0xE2; *e++ = 0x0C;             /* shl dx, 12 */
  *e++ = 0x09; *e++ = 0xD0;                         /* or ax, dx */

  /* Store far pointer into [0x0030]: bx=Offset, ax=Segment */
  *e++ = 0x89; *e++ = 0x1E; *e++ = 0x30; *e++ = 0x00; /* mov [0x0030], bx */
  *e++ = 0xA3; *e++ = 0x32; *e++ = 0x00;             /* mov [0x0032], ax */

  /* Telemetry: save computed Segment:Offset into [0x0048], [0x004A] */
  *e++ = 0x89; *e++ = 0x1E; *e++ = 0x48; *e++ = 0x00; /* mov [0x0048], bx */
  *e++ = 0xA3; *e++ = 0x4A; *e++ = 0x00;             /* mov [0x004A], ax */

  /* Telemetry Checkpoint 2: about to jump! mov word [0x0042], 0xAA02 */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x42; *e++ = 0x00; *e++ = 0x02; *e++ = 0xAA;

  /* Restore BIOS stack */
  *e++ = 0x8E; *e++ = 0x16; *e++ = 0x36; *e++ = 0x00; /* mov ss, [0x0036] */
  *e++ = 0x8B; *e++ = 0x26; *e++ = 0x34; *e++ = 0x00; /* mov sp, [0x0034] */

  /* Zero general purpose registers for clean Windows handoff */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC0;             /* xor eax, eax */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xDB;             /* xor ebx, ebx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC9;             /* xor ecx, ecx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xD2;             /* xor edx, edx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xF6;             /* xor esi, esi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xFF;             /* xor edi, edi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xED;             /* xor ebp, ebp */

  /* Far indirect jump directly to Windows HalpRMStub */
  *e++ = 0x2E; *e++ = 0xFF; *e++ = 0x2E; *e++ = 0x30; *e++ = 0x00; /* jmp far ptr cs:[0x30] */

  return (UINT32)(e - Buffer);
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

  /* Check for prior wake telemetry in low trampoline memory */
  UINT8 *low_base = (UINT8 *)LowTrampBuffer;
  UINT32 prior_magic = *(UINT32 *)(low_base + 0x004C);
  if (prior_magic == GU_TRAMP_TELEMETRY_MAGIC) {
    UINT16 entry_status = *(UINT16 *)(low_base + 0x0040);
    UINT16 exit_status  = *(UINT16 *)(low_base + 0x0042);
    UINT16 vec_lo       = *(UINT16 *)(low_base + 0x0044);
    UINT16 vec_hi       = *(UINT16 *)(low_base + 0x0046);
    UINT16 tgt_off      = *(UINT16 *)(low_base + 0x0048);
    UINT16 tgt_seg      = *(UINT16 *)(low_base + 0x004A);
    UINT32 full_vec     = ((UINT32)vec_hi << 16) | vec_lo;
    GuLog("S3 Prior Wake Telemetry: Magic=S3_T Entry=%#04x Vector=%#08x Target=%04x:%04x Exit=%#04x\n",
          (unsigned)entry_status, (unsigned)full_vec, (unsigned)tgt_seg, (unsigned)tgt_off, (unsigned)exit_status);
  }

  /*
   * Low (<1MB) ACPI NVS Allocation (4096 bytes):
   * +0x00000: Pure 16-bit Real Mode S3 Resume Trampoline Machine Code (<180 bytes)
   * +0x00040: Telemetry Checkpoints & Waking Vector capture
   * +0x00100: Shadow FACS (64 bytes, 256-byte aligned per ACPI spec)
   * +0x00180: Real FACS Vector Mirror (32-bit at 0x180, 64-bit at 0x188)
   */
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = LowTrampPhys + 0x00100;
  EFI_PHYSICAL_ADDRESS trampoline_phys  = LowTrampPhys;

  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(low_base + 0x00100);

  /* Clear the low trampoline memory page */
  memset(low_base, 0, 4096);

  /* 2. Clone Real FACS into Shadow FACS */
  memcpy(shadow_facs, Info->real_facs, sizeof(EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE));
  shadow_facs->FirmwareWakingVector = 0;
  shadow_facs->XFirmwareWakingVector = 0;

  /* Tag Shadow FACS with signature and real FACS address for warm reset recovery */
  UINT32 *shadow_res = (UINT32 *)shadow_facs->Reserved;
  shadow_res[0] = GU_SHADOW_FACS_SIGNATURE;
  memcpy(&shadow_facs->Reserved[4], &Info->real_facs_phys, sizeof(UINT64));

  /* Clear bit 1 (64-bit wake supported) in both tables to force Windows into 32-bit real mode wake */
  shadow_facs->Flags &= ~0x00000002U;
  Info->real_facs->Flags &= ~0x00000002U;
  Info->real_facs->XFirmwareWakingVector = 0;

  /* Store Real FACS original vectors as backup mirror in low memory at +0x180 / +0x188 */
  *(UINT32 *)(low_base + 0x0180) = Info->original_firmware_waking_vector;
  *(UINT64 *)(low_base + 0x0188) = Info->original_xfirmware_waking_vector;

  /* 3. Build Pure 16-bit Real Mode S3 Resume Trampoline machine code in low memory (<1MB) */
  UINT32 tramp_bytes = GuAcpiBuildTrampolineCode(
      low_base,
      4096,
      trampoline_phys,
      NvsPhys,
      shadow_facs_phys
  );

  if (tramp_bytes == 0) {
    GuLog("GuAcpiInstallShadowFacs: Failed to build trampoline machine code\n");
    return EFI_DEVICE_ERROR;
  }

  Info->shadow_facs_phys = shadow_facs_phys;
  Info->trampoline_phys = trampoline_phys;

  /* 4. Update FADT to point to Shadow FACS in low memory */
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *fadt = Info->fadt;
  fadt->FirmwareCtrl = (UINT32)shadow_facs_phys;
  if (fadt->Header.Length > 132 && fadt->XFirmwareCtrl != 0) {
    fadt->XFirmwareCtrl = (UINT64)shadow_facs_phys;
  }

  /* 5. Recalculate FADT table checksum */
  fadt->Header.Checksum = 0;
  UINT8 sum = 0;
  UINT8 *fadt_bytes = (UINT8 *)fadt;
  for (UINT32 i = 0; i < fadt->Header.Length; i++) {
    sum = (UINT8)(sum + fadt_bytes[i]);
  }
  fadt->Header.Checksum = (UINT8)(0 - sum);

  /* 6. Hook Real FACS waking vector to point to our Low Trampoline (<1MB) */
  Info->real_facs->FirmwareWakingVector = (UINT32)trampoline_phys;

  GuLog("S3 FACS Intercept: RealFACS=%#llx ShadowFACS=%#llx LowTrampoline=%#llx (FADT Checksum: %#x -> %#x)\n",
        (unsigned long long)Info->real_facs_phys,
        (unsigned long long)shadow_facs_phys,
        (unsigned long long)trampoline_phys,
        (unsigned)Info->fadt_original_checksum,
        (unsigned)fadt->Header.Checksum);

  return EFI_SUCCESS;
}
