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
  memset(Buffer, 0, BufferSize);

  /*
   * Layout within low Trampoline buffer (<1MB):
   * +0x000: 16-bit Real Mode Preamble & Entry
   * +0x030: Storage: windows_waking_vector_far (0x30), saved_sp (0x34), saved_ss (0x36)
   * +0x040: GDT Descriptor (6 bytes: limit=39, base=TrampolinePhys + 0x050)
   * +0x050: GDT Table (5 entries: Null, 32-Code, 32-Data, 16-Data, 16-Code = 40 bytes)
   * +0x080: 32-bit Flat Protected Mode Unlock Dispatcher
   * +0x220: 16-bit Protected Mode Mode-Switch Stub
   * +0x240: 16-bit Real Mode Return & Far Jump to Windows HalpRMStub
   */

  UINT32 gdt_base = (UINT32)TrampolinePhys + 0x050;
  UINT32 p32 = (UINT32)TrampolinePhys + 0x080;
  UINT32 p16 = 0x0220; /* Offset within 16-bit code segment (Selector 0x20) */
  UINT32 pReal = 0x0240; /* Offset within real-mode code segment */
  UINT16 tramp_seg = (UINT16)((TrampolinePhys >> 4) & 0xFFFF);

  /* 1. Setup GDT Table at +0x050 */
  UINT64 *gdt = (UINT64 *)(Buffer + 0x050);
  gdt[0] = 0x0000000000000000ULL; /* Null Descriptor */
  gdt[1] = 0x00CF9A000000FFFFULL; /* 0x08: 32-bit Code (Base=0, Limit=4GB, P=1, DPL=0, R/X) */
  gdt[2] = 0x00CF92000000FFFFULL; /* 0x10: 32-bit Data (Base=0, Limit=4GB, P=1, DPL=0, R/W) */
  gdt[3] = 0x000092000000FFFFULL; /* 0x18: 16-bit Data (Base=0, Limit=64KB, P=1, DPL=0, R/W) */

  /* 0x20: 16-bit Code (Base=TrampolinePhys, Limit=64KB, P=1, DPL=0, R/X) */
  UINT64 base4 = (UINT64)TrampolinePhys;
  UINT64 limit4 = 0xFFFFULL;
  UINT64 desc4 = (limit4 & 0xFFFFULL) |
                 ((base4 & 0xFFFFULL) << 16) |
                 (((base4 >> 16) & 0xFFULL) << 32) |
                 (0x9AULL << 40) |
                 (0x00ULL << 48) |
                 (((base4 >> 24) & 0xFFULL) << 56);
  gdt[4] = desc4;

  /* 2. Setup GDT Descriptor at +0x040 */
  *(UINT16 *)(Buffer + 0x040) = 39; /* Limit = 40 - 1 */
  *(UINT32 *)(Buffer + 0x042) = gdt_base;

  /* 3. 16-bit Real Mode Preamble at +0x000 */
  UINT8 *e = Buffer + 0x000;
  *e++ = 0xFA; /* cli */
  *e++ = 0xFC; /* cld */
  *e++ = 0x8C; *e++ = 0xC8; /* mov ax, cs */
  *e++ = 0x8E; *e++ = 0xD8; /* mov ds, ax */
  *e++ = 0x89; *e++ = 0x26; *e++ = 0x34; *e++ = 0x00; /* mov [0x0034], sp */
  *e++ = 0x8C; *e++ = 0x16; *e++ = 0x36; *e++ = 0x00; /* mov [0x0036], ss */

  /* Check if CPU is already in protected mode: smsw ax; test al, 1; jnz -> ljmp 8:p32 */
  *e++ = 0x0F; *e++ = 0x01; *e++ = 0xE0; /* smsw ax */
  *e++ = 0xA8; *e++ = 0x01;             /* test al, 1 */
  *e++ = 0x75; *e++ = 0x0F;             /* jne -> 0x24 */

  /* Load GDT: lgdt [cs:0x0040] */
  *e++ = 0x2E; *e++ = 0x0F; *e++ = 0x01; *e++ = 0x16; *e++ = 0x40; *e++ = 0x00;

  /* Enable PE bit: mov eax, cr0; or ax, 1; mov cr0, eax */
  *e++ = 0x0F; *e++ = 0x20; *e++ = 0xC0;
  *e++ = 0x83; *e++ = 0xC8; *e++ = 0x01;
  *e++ = 0x0F; *e++ = 0x22; *e++ = 0xC0;

  /* 0x24: Far jump to 32-bit flat code: jmp dword 0x0008:p32 */
  *e++ = 0x66; *e++ = 0xEA;
  *e++ = (UINT8)(p32 & 0xFF);
  *e++ = (UINT8)((p32 >> 8) & 0xFF);
  *e++ = (UINT8)((p32 >> 16) & 0xFF);
  *e++ = (UINT8)((p32 >> 24) & 0xFF);
  *e++ = 0x08; *e++ = 0x00;

  /* 4. 32-bit Flat Protected Mode Unlock Routine at +0x080 */
  UINT8 *c = Buffer + 0x080;

  /* Set data segments to Selector 0x10 */
  *c++ = 0x66; *c++ = 0xB8; *c++ = 0x10; *c++ = 0x00; /* mov ax, 0x10 */
  *c++ = 0x8E; *c++ = 0xD8; /* mov ds, ax */
  *c++ = 0x8E; *c++ = 0xC0; /* mov es, ax */
  *c++ = 0x8E; *c++ = 0xE0; /* mov fs, ax */
  *c++ = 0x8E; *c++ = 0xE8; /* mov gs, ax */
  *c++ = 0x8E; *c++ = 0xD0; /* mov ss, ax */

  /* Set ESP to private stack at TrampolinePhys + 0x800 */
  UINT32 esp_val = (UINT32)TrampolinePhys + 0x800;
  *c++ = 0xBC;
  *c++ = (UINT8)(esp_val & 0xFF);
  *c++ = (UINT8)((esp_val >> 8) & 0xFF);
  *c++ = (UINT8)((esp_val >> 16) & 0xFF);
  *c++ = (UINT8)((esp_val >> 24) & 0xFF);

  /* ESI = ContextPhys */
  *c++ = 0xBE;
  *c++ = (UINT8)(ContextPhys & 0xFF);
  *c++ = (UINT8)((ContextPhys >> 8) & 0xFF);
  *c++ = (UINT8)((ContextPhys >> 16) & 0xFF);
  *c++ = (UINT8)((ContextPhys >> 24) & 0xFF);

  /* Check context magic: cmp dword [esi + GU_S3_CTX_OFF_MAGIC], GU_S3_CONTEXT_MAGIC */
  *c++ = 0x81; *c++ = 0x3E; *c++ = 0x50; *c++ = 0x5F; *c++ = 0x33; *c++ = 0x53;
  /* jne .skip_unlock (rel32 fixup) */
  *c++ = 0x0F; *c++ = 0x85;
  UINT8 *fixup_magic = c;
  c += 4;

  /* Enable PCI Command register (MSE + BME) via CF8 / CFC if pci_cmd_port_addr != 0 */
  *c++ = 0x8B; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_PCI_CMD_ADDR; /* mov eax, [esi + 0x1B] */
  *c++ = 0x85; *c++ = 0xC0;                                    /* test eax, eax */
  *c++ = 0x74; *c++ = 0x15;                                    /* jz .skip_pci_cmd (+21 bytes) */
  *c++ = 0xBA; *c++ = 0xF8; *c++ = 0x0C; *c++ = 0x00; *c++ = 0x00; /* mov edx, 0xCF8 */
  *c++ = 0xEF;                                                 /* out dx, eax */
  *c++ = 0xBA; *c++ = 0xFC; *c++ = 0x0C; *c++ = 0x00; *c++ = 0x00; /* mov edx, 0xCFC */
  *c++ = 0xED;                                                 /* in eax, dx */
  *c++ = 0x83; *c++ = 0xC8; *c++ = 0x06;                         /* or eax, 6 (MSE | BME) */
  *c++ = 0xEF;                                                 /* out dx, eax */
  /* .skip_pci_cmd */

  /* EDX = [esi + GU_S3_CTX_OFF_BAR_BASE] (bar_base) */
  *c++ = 0x8B; *c++ = 0x56; *c++ = GU_S3_CTX_OFF_BAR_BASE;
  *c++ = 0x85; *c++ = 0xD2; /* test edx, edx */
  /* jz .skip_unlock (rel32 fixup) */
  *c++ = 0x0F; *c++ = 0x84;
  UINT8 *fixup_bar = c;
  c += 4;

  /* Check is_turing: cmp byte [esi + GU_S3_CTX_OFF_IS_TURING], 0 */
  *c++ = 0x80; *c++ = 0x7E; *c++ = GU_S3_CTX_OFF_IS_TURING; *c++ = 0x00;
  /* jne .turing_path (rel32 fixup) */
  *c++ = 0x0F; *c++ = 0x85;
  UINT8 *fixup_turing = c;
  c += 4;

  /* Ada / Ampere path: */
  /* bar[0x8403C0 / 4] = 0 (Reset SEC2) */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0xC0; *c++ = 0x03; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* trans = bar[0x840624 / 4] | 0x80; bar[0x840624 / 4] = trans */
  *c++ = 0x8B; *c++ = 0x82; *c++ = 0x24; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x0D; *c++ = 0x80; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x24; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;

  /* bar[0x84010C / 4] = 0 */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0x0C; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* trans = (bar[0x840600 / 4] & ~7) | 5; bar[0x840600 / 4] = trans */
  *c++ = 0x8B; *c++ = 0x82; *c++ = 0x00; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x83; *c++ = 0xE0; *c++ = 0xF8;
  *c++ = 0x83; *c++ = 0xC8; *c++ = 0x05;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x00; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;

  /* booter DMA: */
  /* mov eax, [esi + GU_S3_CTX_OFF_BOOTER_DMA]; test eax, eax; jz .skip_booter_ada */
  *c++ = 0x8B; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_BOOTER_DMA;
  *c++ = 0x85; *c++ = 0xC0;
  *c++ = 0x74;
  UINT8 *fixup_booter_dma0 = c++;
  /* mov ecx, [esi + GU_S3_CTX_OFF_BOOTER_SZ]; test ecx, ecx; jz .skip_booter_ada */
  *c++ = 0x8B; *c++ = 0x4E; *c++ = GU_S3_CTX_OFF_BOOTER_SZ;
  *c++ = 0x85; *c++ = 0xC9;
  *c++ = 0x74;
  UINT8 *fixup_booter_sz0 = c++;

  /* bar[0x840110 / 4] = booter_dma >> 8 */
  *c++ = 0xC1; *c++ = 0xE8; *c++ = 0x08;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x10; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  /* bar[0x840114 / 4] = 0 */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0x14; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  /* bar[0x840118 / 4] = (booter_size & ~0xFF) | 0x10 */
  *c++ = 0x81; *c++ = 0xE1; *c++ = 0x00; *c++ = 0xFF; *c++ = 0xFF; *c++ = 0xFF;
  *c++ = 0x83; *c++ = 0xC9; *c++ = 0x10;
  *c++ = 0x89; *c++ = 0x8A; *c++ = 0x18; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;

  /* .skip_booter_ada target */
  *fixup_booter_dma0 = (UINT8)(c - (fixup_booter_dma0 + 1));
  *fixup_booter_sz0 = (UINT8)(c - (fixup_booter_sz0 + 1));

  /* CPUCTL start: bar[0x840100 / 4] = 2 */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0x00; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x02; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* Poll Falcon Halt: mov ecx, 50000; .poll: test [edx + 0x840100], 0x10; jnz .halted; dec ecx; jnz .poll */
  *c++ = 0xB9; *c++ = 0x50; *c++ = 0xC3; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0xF7; *c++ = 0x82; *c++ = 0x00; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x10; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0x75; *c++ = 0x03; /* jnz .halted */
  *c++ = 0x49;             /* dec ecx */
  *c++ = 0x75; *c++ = 0xF1; /* jnz .poll (-15 bytes) */

  /* .halted: Latch rate-limiter selectors */
  *c++ = 0x8B; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_VAL_SS0;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x1C; *c++ = 0x38; *c++ = 0x82; *c++ = 0x00;
  *c++ = 0x8B; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_VAL_SS1;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x20; *c++ = 0x38; *c++ = 0x82; *c++ = 0x00;

  /* Clear SEC2 reset */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0xC0; *c++ = 0x03; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* Read SEC2 marker: mov eax, [edx + 0x840040]; mov [esi + GU_S3_CTX_OFF_SEC2_MARKER], eax */
  *c++ = 0x8B; *c++ = 0x82; *c++ = 0x40; *c++ = 0x00; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x89; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_SEC2_MARKER;

  /* jmp .unlock_success */
  *c++ = 0xEB;
  UINT8 *fixup_ada_done = c++;

  /* .turing_path target */
  INT32 turing_disp = (INT32)(c - (fixup_turing + 4));
  memcpy(fixup_turing, &turing_disp, 4);

  /* bar[0x8403C0 / 4] = 0 */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0xC0; *c++ = 0x03; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* booter DMA: */
  *c++ = 0x8B; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_BOOTER_DMA;
  *c++ = 0x85; *c++ = 0xC0;
  *c++ = 0x74;
  UINT8 *fixup_turing_dma0 = c++;

  *c++ = 0xC1; *c++ = 0xE8; *c++ = 0x08;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x10; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0x00; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x02; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0xB9; *c++ = 0x50; *c++ = 0xC3; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0xF7; *c++ = 0x82; *c++ = 0x00; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x10; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0x75; *c++ = 0x03;
  *c++ = 0x49;
  *c++ = 0x75; *c++ = 0xF1;

  *fixup_turing_dma0 = (UINT8)(c - (fixup_turing_dma0 + 1));

  /* .unlock_success target */
  *fixup_ada_done = (UINT8)(c - (fixup_ada_done + 1));

  /* Set resume_status = 1, resume_count++ */
  *c++ = 0xC7; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_STATUS;
  *c++ = 0x01; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0xFF; *c++ = 0x46; *c++ = GU_S3_CTX_OFF_COUNT;

  /* .skip_unlock target */
  INT32 magic_disp = (INT32)(c - (fixup_magic + 4));
  INT32 bar_disp = (INT32)(c - (fixup_bar + 4));
  memcpy(fixup_magic, &magic_disp, 4);
  memcpy(fixup_bar, &bar_disp, 4);

  /* Read Windows Waking Vector from Shadow FACS: */
  /* EDI = ShadowFacsPhys */
  *c++ = 0xBF;
  *c++ = (UINT8)(ShadowFacsPhys & 0xFF);
  *c++ = (UINT8)((ShadowFacsPhys >> 8) & 0xFF);
  *c++ = (UINT8)((ShadowFacsPhys >> 16) & 0xFF);
  *c++ = (UINT8)((ShadowFacsPhys >> 24) & 0xFF);

  /* EBX = [EDI + 12] (FirmwareWakingVector) */
  *c++ = 0x8B; *c++ = 0x5F; *c++ = 0x0C;
  *c++ = 0x85; *c++ = 0xDB; /* test ebx, ebx */
  *c++ = 0x75; *c++ = 0x03; /* jnz .have_vec */
  /* EBX = [EDI + 24] (XFirmwareWakingVector) */
  *c++ = 0x8B; *c++ = 0x5F; *c++ = 0x18;

  /* .have_vec: Convert EBX to real-mode Segment:Offset */
  *c++ = 0x89; *c++ = 0xD8;             /* mov eax, ebx */
  *c++ = 0xC1; *c++ = 0xE8; *c++ = 0x04; /* shr eax, 4 (Segment) */
  *c++ = 0x83; *c++ = 0xE3; *c++ = 0x0F; /* and ebx, 0x0F (Offset) */

  /* Store far pointer into [TrampolinePhys + 0x0030] */
  *c++ = 0xBA;
  *c++ = (UINT8)(TrampolinePhys & 0xFF);
  *c++ = (UINT8)((TrampolinePhys >> 8) & 0xFF);
  *c++ = (UINT8)((TrampolinePhys >> 16) & 0xFF);
  *c++ = (UINT8)((TrampolinePhys >> 24) & 0xFF);
  *c++ = 0x66; *c++ = 0x89; *c++ = 0x5A; *c++ = 0x30; /* mov word [edx + 0x30], bx */
  *c++ = 0x66; *c++ = 0x89; *c++ = 0x42; *c++ = 0x32; /* mov word [edx + 0x32], ax */

  /* Reload 16-bit data segment (Selector 0x18) */
  *c++ = 0x66; *c++ = 0xB8; *c++ = 0x18; *c++ = 0x00; /* mov ax, 0x18 */
  *c++ = 0x8E; *c++ = 0xD8; /* mov ds, ax */
  *c++ = 0x8E; *c++ = 0xC0; /* mov es, ax */
  *c++ = 0x8E; *c++ = 0xE0; /* mov fs, ax */
  *c++ = 0x8E; *c++ = 0xE8; /* mov gs, ax */
  *c++ = 0x8E; *c++ = 0xD0; /* mov ss, ax */

  /* Far jump to 16-bit code segment (Selector 0x20:0x0220): jmp 0x0020:p16 */
  *c++ = 0xEA;
  *c++ = (UINT8)(p16 & 0xFF);
  *c++ = (UINT8)((p16 >> 8) & 0xFF);
  *c++ = (UINT8)((p16 >> 16) & 0xFF);
  *c++ = (UINT8)((p16 >> 24) & 0xFF);
  *c++ = 0x20; *c++ = 0x00;

  /* 5. 16-bit Protected Mode Stub at +0x220 */
  UINT8 *r16 = Buffer + 0x220;
  /* Disable PE in CR0: mov eax, cr0; and al, 0xFE; mov cr0, eax */
  *r16++ = 0x0F; *r16++ = 0x20; *r16++ = 0xC0;
  *r16++ = 0x24; *r16++ = 0xFE;
  *r16++ = 0x0F; *r16++ = 0x22; *r16++ = 0xC0;

  /* Far jump to real mode code segment: jmp far tramp_seg:pReal */
  *r16++ = 0xEA;
  *r16++ = (UINT8)(pReal & 0xFF);
  *r16++ = (UINT8)((pReal >> 8) & 0xFF);
  *r16++ = (UINT8)(tramp_seg & 0xFF);
  *r16++ = (UINT8)((tramp_seg >> 8) & 0xFF);

  /* 6. 16-bit Real Mode Return Stub at +0x240 */
  UINT8 *rReal = Buffer + 0x240;
  *rReal++ = 0x8C; *rReal++ = 0xC8; /* mov ax, cs */
  *rReal++ = 0x8E; *rReal++ = 0xD8; /* mov ds, ax */
  *rReal++ = 0x8E; *rReal++ = 0xC0; /* mov es, ax */
  *rReal++ = 0x8E; *rReal++ = 0xE0; /* mov fs, ax */
  *rReal++ = 0x8E; *rReal++ = 0xE8; /* mov gs, ax */

  /* Restore saved BIOS stack */
  *rReal++ = 0x8E; *rReal++ = 0x16; *rReal++ = 0x36; *rReal++ = 0x00; /* mov ss, [0x0036] */
  *rReal++ = 0x8B; *rReal++ = 0x26; *rReal++ = 0x34; *rReal++ = 0x00; /* mov sp, [0x0034] */

  /* Zero general purpose registers for clean Windows handoff */
  *rReal++ = 0x66; *rReal++ = 0x31; *rReal++ = 0xC0; /* xor eax, eax */
  *rReal++ = 0x66; *rReal++ = 0x31; *rReal++ = 0xDB; /* xor ebx, ebx */
  *rReal++ = 0x66; *rReal++ = 0x31; *rReal++ = 0xC9; /* xor ecx, ecx */
  *rReal++ = 0x66; *rReal++ = 0x31; *rReal++ = 0xD2; /* xor edx, edx */
  *rReal++ = 0x66; *rReal++ = 0x31; *rReal++ = 0xF6; /* xor esi, esi */
  *rReal++ = 0x66; *rReal++ = 0x31; *rReal++ = 0xFF; /* xor edi, edi */
  *rReal++ = 0x66; *rReal++ = 0x31; *rReal++ = 0xED; /* xor ebp, ebp */

  /* Far indirect jump directly to Windows HalpRMStub */
  *rReal++ = 0x2E; *rReal++ = 0xFF; *rReal++ = 0x2E; *rReal++ = 0x30; *rReal++ = 0x00; /* jmp far ptr cs:[0x30] */

  return (UINT32)(rReal - Buffer);
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

  /*
   * Layout within the 256 KB High ACPI NVS Allocation:
   * +0x00000: Context (GU_S3_RESUME_CONTEXT)
   * +0x00200: Shadow FACS (64 bytes)
   * +0x01000: Booter Firmware Asset (~57 KB)
   * +0x11000: V67 Firmware Asset (~64 KB)
   *
   * Low (<1MB) ACPI NVS Allocation:
   * LowTrampPhys: S3 Resume Trampoline Machine Code (<1024 bytes)
   */
  UINT8 *nvs = (UINT8 *)NvsBuffer;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = NvsPhys + 0x00200;
  EFI_PHYSICAL_ADDRESS trampoline_phys = LowTrampPhys;

  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(nvs + 0x00200);

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

  /* 3. Build S3 Resume Trampoline machine code in low memory (<1MB) */
  UINT32 tramp_bytes = GuAcpiBuildTrampolineCode(
      (UINT8 *)LowTrampBuffer,
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

  /* 4. Update FADT to point to Shadow FACS */
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
