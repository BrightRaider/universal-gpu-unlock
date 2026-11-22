/* SPDX-License-Identifier: GPL-2.0-only */
#include "s3_facs.h"
#include "port.h"
#include "compat/compat.h"

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
  if (!Buffer || BufferSize < 512) return 0;
  memset(Buffer, 0, BufferSize);

  /*
   * Layout within Trampoline buffer:
   * +0x000: 16-bit Real Mode Preamble & Entry
   * +0x040: GDT Descriptor (6 bytes: limit=39, base=TrampolinePhys + 0x050)
   * +0x050: GDT Table (5 entries: Null, 32-Code, 32-Data, 16-Data, 16-Code = 40 bytes)
   * +0x080: 32-bit Flat Protected Mode Unlock Dispatcher
   * +0x140: 16-bit Protected Mode Mode-Switch Stub
   * +0x160: 16-bit Real Mode Return & Far Jump to Windows HalpRMStub
   */

  UINT32 gdt_base = (UINT32)TrampolinePhys + 0x050;
  UINT32 p32 = (UINT32)TrampolinePhys + 0x080;
  UINT32 p16 = (UINT32)TrampolinePhys + 0x140;
  UINT32 pReal = (UINT32)TrampolinePhys + 0x160;

  /* 1. Setup GDT Table at +0x050 */
  UINT64 *gdt = (UINT64 *)(Buffer + 0x050);
  gdt[0] = 0x0000000000000000ULL; /* Null Descriptor */
  gdt[1] = 0x00CF9A000000FFFFULL; /* 0x08: 32-bit Code (Base=0, Limit=4GB, P=1, DPL=0, R/X) */
  gdt[2] = 0x00CF92000000FFFFULL; /* 0x10: 32-bit Data (Base=0, Limit=4GB, P=1, DPL=0, R/W) */
  gdt[3] = 0x000092000000FFFFULL; /* 0x18: 16-bit Data (Base=0, Limit=64KB, P=1, DPL=0, R/W) */
  gdt[4] = 0x00009A000000FFFFULL; /* 0x20: 16-bit Code (Base=0, Limit=64KB, P=1, DPL=0, R/X) */

  /* 2. Setup GDT Descriptor at +0x040 */
  Buffer[0x040] = 39; /* Limit = 40 - 1 */
  Buffer[0x041] = 0;
  Buffer[0x042] = (UINT8)(gdt_base & 0xFF);
  Buffer[0x043] = (UINT8)((gdt_base >> 8) & 0xFF);
  Buffer[0x044] = (UINT8)((gdt_base >> 16) & 0xFF);
  Buffer[0x045] = (UINT8)((gdt_base >> 24) & 0xFF);

  /* 3. 16-bit Real Mode Preamble at +0x000 */
  UINT8 *e = Buffer + 0x000;
  *e++ = 0xFA; /* cli */
  *e++ = 0xFC; /* cld */

  /* Check if CPU is already in protected mode */
  *e++ = 0x0F; *e++ = 0x01; *e++ = 0xF0; /* smsw ax */
  *e++ = 0xA8; *e++ = 0x01;             /* test al, 1 */
  *e++ = 0x75; *e++ = 0x18;             /* jnz -> direct jump to 32-bit (+0x24) */

  /* Load GDT: lgdt [cs:0x0040] */
  *e++ = 0x2E; *e++ = 0x0F; *e++ = 0x01; *e++ = 0x16;
  *e++ = 0x40; *e++ = 0x00;

  /* Enable PE bit: mov eax, cr0; or eax, 1; mov cr0, eax */
  *e++ = 0x0F; *e++ = 0x20; *e++ = 0xC0;
  *e++ = 0x83; *e++ = 0xC8; *e++ = 0x01;
  *e++ = 0x0F; *e++ = 0x22; *e++ = 0xC0;

  /* Far jump to 32-bit flat code: jmp dword 0x0008:p32 */
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

  /* ESI = ContextPhys */
  *c++ = 0xBE;
  *c++ = (UINT8)(ContextPhys & 0xFF);
  *c++ = (UINT8)((ContextPhys >> 8) & 0xFF);
  *c++ = (UINT8)((ContextPhys >> 16) & 0xFF);
  *c++ = (UINT8)((ContextPhys >> 24) & 0xFF);

  /* EDX = [ESI + 8] (bar_base) */
  *c++ = 0x8B; *c++ = 0x56; *c++ = 0x08;

  /* test edx, edx; jz .skip_unlock */
  *c++ = 0x85; *c++ = 0xD2;
  *c++ = 0x74; *c++ = 0x4D; /* jump to .skip_unlock */

  /* bar[0x8403c0 / 4] = 0 (Reset SEC2) */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0xC0; *c++ = 0x03; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* trans = bar[0x840624 / 4] | 0x80 */
  *c++ = 0x8B; *c++ = 0x82; *c++ = 0x24; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x0D; *c++ = 0x80; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x24; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;

  /* bar[0x84010c / 4] = 0 */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0x0C; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* trans = (bar[0x840600 / 4] & ~7) | 5 */
  *c++ = 0x8B; *c++ = 0x82; *c++ = 0x00; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x83; *c++ = 0xE0; *c++ = 0xF8;
  *c++ = 0x83; *c++ = 0xC8; *c++ = 0x05;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x00; *c++ = 0x06; *c++ = 0x84; *c++ = 0x00;

  /* booter DMA: bar[0x840110 / 4] = booter_dma >> 8 */
  *c++ = 0x8B; *c++ = 0x46; *c++ = 0x18; /* booter_dma low dword */
  *c++ = 0xC1; *c++ = 0xE8; *c++ = 0x08;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x10; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0x14; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* booter size: bar[0x840118 / 4] = (booter_size & ~0xff) | 0x10 */
  *c++ = 0x8B; *c++ = 0x46; *c++ = 0x20; /* booter_size */
  *c++ = 0x25; *c++ = 0x00; *c++ = 0xFF; *c++ = 0xFF; *c++ = 0xFF;
  *c++ = 0x83; *c++ = 0xC8; *c++ = 0x10;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x18; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;

  /* CPUCTL start: bar[0x840100 / 4] = 2 */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0x00; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x02; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* Poll Falcon Halt: mov ecx, 50000 */
  *c++ = 0xB9; *c++ = 0x50; *c++ = 0xC3; *c++ = 0x00; *c++ = 0x00;
  /* .poll: test dword [edx + 0x840100], 0x10 */
  *c++ = 0xF7; *c++ = 0x82; *c++ = 0x00; *c++ = 0x01; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x10; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;
  *c++ = 0x75; *c++ = 0x03; /* jnz .halted */
  *c++ = 0x49;             /* dec ecx */
  *c++ = 0x75; *c++ = 0xF0; /* jnz .poll */

  /* .halted: latch selectors */
  /* SS0 = [ESI + 0x48] -> [EDX + 0x82381C] */
  *c++ = 0x8B; *c++ = 0x46; *c++ = 0x48;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x1C; *c++ = 0x38; *c++ = 0x82; *c++ = 0x00;

  /* SS1 = [ESI + 0x4C] -> [EDX + 0x823820] */
  *c++ = 0x8B; *c++ = 0x46; *c++ = 0x4C;
  *c++ = 0x89; *c++ = 0x82; *c++ = 0x20; *c++ = 0x38; *c++ = 0x82; *c++ = 0x00;

  /* Clear SEC2 reset */
  *c++ = 0xC7; *c++ = 0x82; *c++ = 0xC0; *c++ = 0x03; *c++ = 0x84; *c++ = 0x00;
  *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00;

  /* Record status in context */
  *c++ = 0xC7; *c++ = 0x46; *c++ = 0x50; *c++ = 0x01; *c++ = 0x00; *c++ = 0x00; *c++ = 0x00; /* status = 1 */
  *c++ = 0xFF; *c++ = 0x46; *c++ = 0x54; /* resume_count++ */

  /* .skip_unlock: Read Windows Waking Vector from Shadow FACS */
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
  /* Fallback: EBX = [EDI + 24] (XFirmwareWakingVector) */
  *c++ = 0x8B; *c++ = 0x5F; *c++ = 0x18;

  /* .have_vec: Transition back to 16-bit Real Mode */
  /* Load 16-bit data segment (Selector 0x18) */
  *c++ = 0x66; *c++ = 0xB8; *c++ = 0x18; *c++ = 0x00; /* mov ax, 0x18 */
  *c++ = 0x8E; *c++ = 0xD8; /* mov ds, ax */
  *c++ = 0x8E; *c++ = 0xC0; /* mov es, ax */
  *c++ = 0x8E; *c++ = 0xE0; /* mov fs, ax */
  *c++ = 0x8E; *c++ = 0xE8; /* mov gs, ax */
  *c++ = 0x8E; *c++ = 0xD0; /* mov ss, ax */

  /* Far jump to 16-bit protected mode code segment (Selector 0x20): jmp 0x20:p16 */
  *c++ = 0xEA;
  *c++ = (UINT8)(p16 & 0xFF);
  *c++ = (UINT8)((p16 >> 8) & 0xFF);
  *c++ = 0x20; *c++ = 0x00;

  /* 5. 16-bit Protected Mode Stub at +0x140 */
  UINT8 *r16 = Buffer + 0x140;
  /* Disable PE in CR0: mov eax, cr0; and eax, ~1; mov cr0, eax */
  *r16++ = 0x0F; *r16++ = 0x20; *r16++ = 0xC0;
  *r16++ = 0x83; *r16++ = 0xE0; *r16++ = 0xFE;
  *r16++ = 0x0F; *r16++ = 0x22; *r16++ = 0xC0;

  /* Far jump to real mode code segment 0x0000:pReal */
  *r16++ = 0xEA;
  *r16++ = (UINT8)(pReal & 0xFF);
  *r16++ = (UINT8)((pReal >> 8) & 0xFF);
  *r16++ = 0x00; *r16++ = 0x00;

  /* 6. 16-bit Real Mode Return & Far Return to Windows at +0x160 */
  UINT8 *rReal = Buffer + 0x160;
  /* xor ax, ax; mov ds, ax; mov es, ax; mov fs, ax; mov gs, ax; mov ss, ax; xor sp, sp */
  *rReal++ = 0x31; *rReal++ = 0xC0;
  *rReal++ = 0x8E; *rReal++ = 0xD8;
  *rReal++ = 0x8E; *rReal++ = 0xC0;
  *rReal++ = 0x8E; *rReal++ = 0xE0;
  *rReal++ = 0x8E; *rReal++ = 0xE8;
  *rReal++ = 0x8E; *rReal++ = 0xD0;
  *rReal++ = 0x31; *rReal++ = 0xE4;

  /*
   * Convert physical address in EBX into real mode Segment:Offset
   * eax = ebx >> 4 (Segment)
   * ebx = ebx & 0xF (Offset)
   * push ax; push bx; retf
   */
  *rReal++ = 0x66; *rReal++ = 0x89; *rReal++ = 0xD8;             /* mov eax, ebx */
  *rReal++ = 0x66; *rReal++ = 0xC1; *rReal++ = 0xE8; *rReal++ = 0x04; /* shr eax, 4 */
  *rReal++ = 0x66; *rReal++ = 0x83; *rReal++ = 0xE3; *rReal++ = 0x0F; /* and ebx, 0xF */
  *rReal++ = 0x50; /* push ax */
  *rReal++ = 0x53; /* push bx */
  *rReal++ = 0xCB; /* retf -> Jumps directly into Windows HalpRMStub! */

  return 0x180;
}

EFI_STATUS GuAcpiInstallShadowFacs(
    IN EFI_SYSTEM_TABLE *SystemTable,
    IN GU_S3_RESUME_CONTEXT *Context,
    IN VOID *NvsBuffer,
    IN EFI_PHYSICAL_ADDRESS NvsPhys,
    OUT GU_ACPI_FACS_INFO *Info
) {
  if (!SystemTable || !Context || !NvsBuffer || !NvsPhys || !Info) {
    return EFI_INVALID_PARAMETER;
  }

  /* 1. Discover FADT and Real FACS */
  EFI_STATUS s = GuAcpiLocateFadtAndFacs(SystemTable, Info);
  if (EFI_ERROR(s) || !Info->found || !Info->fadt || !Info->real_facs) {
    GuLog("GuAcpiInstallShadowFacs: Failed to locate ACPI tables (%#llx)\n", (unsigned long long)s);
    return s;
  }

  /*
   * Layout within the 256 KB ACPI NVS Allocation:
   * +0x00000: Context (GU_S3_RESUME_CONTEXT)
   * +0x00200: Shadow FACS (64 bytes)
   * +0x00300: S3 Resume Trampoline Machine Code
   * +0x01000: Booter Firmware Asset (~57 KB)
   * +0x11000: V67 Firmware Asset (~64 KB)
   */
  UINT8 *nvs = (UINT8 *)NvsBuffer;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = NvsPhys + 0x00200;
  EFI_PHYSICAL_ADDRESS trampoline_phys = NvsPhys + 0x00300;

  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(nvs + 0x00200);

  /* 2. Clone Real FACS into Shadow FACS */
  memcpy(shadow_facs, Info->real_facs, sizeof(EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE));
  shadow_facs->FirmwareWakingVector = 0;
  shadow_facs->XFirmwareWakingVector = 0;

  /* 3. Build S3 Resume Trampoline machine code */
  UINT32 tramp_bytes = GuAcpiBuildTrampolineCode(
      nvs + 0x00300,
      1024,
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

  /* 6. Hook Real FACS waking vector to point to our Trampoline */
  Info->real_facs->FirmwareWakingVector = (UINT32)trampoline_phys;
  if (Info->real_facs->Length >= 32) {
    Info->real_facs->XFirmwareWakingVector = 0;
  }

  GuLog("S3 FACS Intercept: RealFACS=%#llx ShadowFACS=%#llx Trampoline=%#llx (FADT Checksum: %#x -> %#x)\n",
        (unsigned long long)Info->real_facs_phys,
        (unsigned long long)shadow_facs_phys,
        (unsigned long long)trampoline_phys,
        (unsigned)Info->fadt_original_checksum,
        (unsigned)fadt->Header.Checksum);

  return EFI_SUCCESS;
}
