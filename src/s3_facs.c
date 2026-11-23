/* SPDX-License-Identifier: GPL-2.0-only */
#include "s3_facs.h"
#include "port.h"
#include "compat/compat.h"

typedef char gu_s3_ctx_size_assert[(sizeof(GU_S3_RESUME_CONTEXT) == 108) ? 1 : -1];

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
    EFI_ACPI_DESCRIPTION_HEADER *rsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)rsdp->RsdtAddress;
    if (rsdt && rsdt->Length >= sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
      Info->rsdt = rsdt;
      Info->rsdt_phys = (EFI_PHYSICAL_ADDRESS)rsdp->RsdtAddress;
      Info->rsdt_original_checksum = rsdt->Checksum;
      UINTN count = (rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);
      UINT8 *entry_ptr = ((UINT8 *)rsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
      for (UINTN i = 0; i < count; i++) {
        UINT32 table_phys = 0;
        memcpy(&table_phys, entry_ptr + i * sizeof(UINT32), sizeof(UINT32));
        EFI_ACPI_DESCRIPTION_HEADER *tbl = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)table_phys;
        if (tbl && tbl->Signature == GU_ACPI_FADT_SIGNATURE) {
          rsdt_fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)tbl;
          rsdt_fadt_phys = (EFI_PHYSICAL_ADDRESS)table_phys;
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
    facs_phys = (EFI_PHYSICAL_ADDRESS)fadt->FirmwareCtrl;
  } else if (fadt->Header.Length >= 140 && fadt->XFirmwareCtrl >= 0x00100000ULL) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)fadt->XFirmwareCtrl;
  } else if (fadt->FirmwareCtrl != 0) {
    facs_phys = (EFI_PHYSICAL_ADDRESS)fadt->FirmwareCtrl;
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
    IN EFI_PHYSICAL_ADDRESS ShadowFacsPhys,
    IN UINT32 RootPortLinkReg,
    IN UINT32 GpuPmcsrReg
) {
  if (!Buffer || BufferSize < 1024) return 0;
  (VOID)ContextPhys;
  (VOID)ShadowFacsPhys;

  /*
   * Layout within Low Trampoline buffer (<1MB, 4096 bytes):
   * +0x000: 16-bit Pure Real Mode S3 Resume Trampoline Machine Code (<380 bytes)
   * +0x200: Shadow FACS (64 bytes, 512-byte aligned per ACPI spec)
   * +0x280: Real FACS Vector Mirror (32-bit at 0x280, 64-bit at 0x288)
   * +0x300: Far Jump Target: [0x300] = Offset (16-bit), [0x302] = Segment (16-bit)
   * +0x304: Saved BIOS SP
   * +0x306: Saved BIOS SS
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

  *e++ = 0xFA; /* cli */
  *e++ = 0xFC; /* cld */
  *e++ = 0x8C; *e++ = 0xC8; /* mov ax, cs */
  *e++ = 0x8E; *e++ = 0xD8; /* mov ds, ax */
  *e++ = 0x89; *e++ = 0x26; *e++ = 0x04; *e++ = 0x03; /* mov [0x0304], sp */
  *e++ = 0x8C; *e++ = 0x16; *e++ = 0x06; *e++ = 0x03; /* mov [0x0306], ss */

  /* Set Telemetry Magic: mov dword [0x031C], 0x53335F54 ('S3_T') */
  *e++ = 0x66; *e++ = 0xC7; *e++ = 0x06; *e++ = 0x1C; *e++ = 0x03;
  *e++ = 0x54; *e++ = 0x5F; *e++ = 0x33; *e++ = 0x53;

  /* Telemetry Entry Checkpoint: mov word [0x0310], 0xAA01 */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x10; *e++ = 0x03; *e++ = 0x01; *e++ = 0xAA;

  /* Probe loop: si=0x0320, di=0x0340, cx=8 */
  *e++ = 0xBE; *e++ = 0x20; *e++ = 0x03; /* mov si, 0x0320 */
  *e++ = 0xBF; *e++ = 0x40; *e++ = 0x03; /* mov di, 0x0340 */
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

  /* Check if Bridge DevID is AMD (Vendor ID 0x1022 at [0x0340]) */
  *e++ = 0xA1; *e++ = 0x40; *e++ = 0x03; /* mov ax, [0x0340] */
  *e++ = 0x3D; *e++ = 0x22; *e++ = 0x10; /* cmp ax, 0x1022 */
  *e++ = 0x0F; *e++ = 0x85;              /* jne near .skip_bridge (4 bytes) */
  UINT8 *skip_bridge_disp = e;
  *e++ = 0x00; *e++ = 0x00;              /* placeholder rel16 */

  /*
   * 1. Load original Bus Numbers from [0x0348], preserve Latency Timer (bits 31:24),
   * set Pri=0, Sec=0x2D, Sub=0x2D:
   */
  *e++ = 0x66; *e++ = 0xA1; *e++ = 0x48; *e++ = 0x03;             /* mov eax, [0x0348] */
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

  /* 2. Probe GPU 45:0:0 now that bridge is routing: */
  /* Read GPU DevID at 0x802D0000 -> [0x0360] */
  *e++ = 0x66; *e++ = 0xB8; *e++ = 0x00; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x80; /* mov eax, 0x802D0000 */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xED;             /* in eax, dx */
  *e++ = 0x66; *e++ = 0xA3; *e++ = 0x60; *e++ = 0x03;                         /* mov [0x0360], eax */

  /* Read GPU Cmd/Stat at 0x802D0004 -> [0x0364] */
  *e++ = 0x66; *e++ = 0xB8; *e++ = 0x04; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x80; /* mov eax, 0x802D0004 */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xED;             /* in eax, dx */
  *e++ = 0x66; *e++ = 0xA3; *e++ = 0x64; *e++ = 0x03;                         /* mov [0x0364], eax */

  /* Read GPU BAR0 at 0x802D0010 -> [0x0368] */
  *e++ = 0x66; *e++ = 0xB8; *e++ = 0x10; *e++ = 0x00; *e++ = 0x2D; *e++ = 0x80; /* mov eax, 0x802D0010 */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xED;             /* in eax, dx */
  *e++ = 0x66; *e++ = 0xA3; *e++ = 0x68; *e++ = 0x03;                         /* mov [0x0368], eax */

  /* Read GPU PMCSR using address from probe_addrs[7] at [0x033C] -> [0x036C] */
  *e++ = 0x66; *e++ = 0xA1; *e++ = 0x3C; *e++ = 0x03;                         /* mov eax, [0x033C] */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xED;             /* in eax, dx */
  *e++ = 0x66; *e++ = 0xA3; *e++ = 0x6C; *e++ = 0x03;                         /* mov [0x036C], eax */

  /*
   * 3. MANDATORY BRIDGE RESTORE: Restore Root Port Offset 0x18 to exact original value!
   * Windows pci.sys must find the bridge in its exact pre-trampoline state.
   */
  *e++ = 0x66; *e++ = 0xB8; *e++ = 0x18; *e++ = 0x19; *e++ = 0x00; *e++ = 0x80; /* mov eax, 0x80001918 */
  *e++ = 0xBA; *e++ = 0xF8; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */
  *e++ = 0x66; *e++ = 0xA1; *e++ = 0x48; *e++ = 0x03;                         /* mov eax, [0x0348] */
  *e++ = 0xBA; *e++ = 0xFC; *e++ = 0x0C; *e++ = 0x66; *e++ = 0xEF;             /* out dx, eax */

  /* Patch .skip_bridge near jne displacement (relative to end of 4-byte instruction) */
  INT32 skip_disp = (INT32)(e - (skip_bridge_disp + 2));
  skip_bridge_disp[0] = (UINT8)(skip_disp & 0xFF);
  skip_bridge_disp[1] = (UINT8)((skip_disp >> 8) & 0xFF);

  /* Read 32-bit waking vector from Shadow FACS at 0x0200: [0x020C] */
  *e++ = 0xA1; *e++ = 0x0C; *e++ = 0x02;             /* mov ax, [0x020C] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x0E; *e++ = 0x02; /* mov dx, [0x020E] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax0 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx0 = e++;                   /* jnz .have_vec */

  /* Read 64-bit waking vector from Shadow FACS at 0x0200: [0x0218] */
  *e++ = 0xA1; *e++ = 0x18; *e++ = 0x02;             /* mov ax, [0x0218] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x1A; *e++ = 0x02; /* mov dx, [0x021A] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax1 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx1 = e++;                   /* jnz .have_vec */

  /* Fallback: Read backup 32-bit vector from Real FACS mirror at [0x0280] */
  *e++ = 0xA1; *e++ = 0x80; *e++ = 0x02;             /* mov ax, [0x0280] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x82; *e++ = 0x02; /* mov dx, [0x0282] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax2 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx2 = e++;                   /* jnz .have_vec */

  /* Fallback: Read backup 64-bit vector from Real FACS mirror at [0x0288] */
  *e++ = 0xA1; *e++ = 0x88; *e++ = 0x02;             /* mov ax, [0x0288] */
  *e++ = 0x8B; *e++ = 0x16; *e++ = 0x8A; *e++ = 0x02; /* mov dx, [0x028A] */
  *e++ = 0x85; *e++ = 0xC0;                         /* test ax, ax */
  *e++ = 0x75; UINT8 *f_ax3 = e++;                   /* jnz .have_vec */
  *e++ = 0x85; *e++ = 0xD2;                         /* test dx, dx */
  *e++ = 0x75; UINT8 *f_dx3 = e++;                   /* jnz .have_vec */

  /* No vector found in any FACS table! Flag error 0xDEAD and halt safely */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x10; *e++ = 0x03; *e++ = 0xAD; *e++ = 0xDE;
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

  /* Save captured vector to telemetry [0x0314], [0x0316] */
  *e++ = 0xA3; *e++ = 0x14; *e++ = 0x03;             /* mov [0x0314], ax */
  *e++ = 0x89; *e++ = 0x16; *e++ = 0x16; *e++ = 0x03; /* mov [0x0316], dx */

  /* Convert linear DX:AX into Segment:Offset */
  /* Offset = AX & 0x000F */
  *e++ = 0x89; *e++ = 0xC3;                         /* mov bx, ax */
  *e++ = 0x83; *e++ = 0xE3; *e++ = 0x0F;             /* and bx, 0x0F */

  /* Segment = (AX >> 4) | (DX << 12) */
  *e++ = 0xC1; *e++ = 0xE8; *e++ = 0x04;             /* shr ax, 4 */
  *e++ = 0xC1; *e++ = 0xE2; *e++ = 0x0C;             /* shl dx, 12 */
  *e++ = 0x09; *e++ = 0xD0;                         /* or ax, dx */

  /* Store far pointer into [0x0300]: bx=Offset, ax=Segment */
  *e++ = 0x89; *e++ = 0x1E; *e++ = 0x00; *e++ = 0x03; /* mov [0x0300], bx */
  *e++ = 0xA3; *e++ = 0x02; *e++ = 0x03;             /* mov [0x0302], ax */

  /* Telemetry: save computed Segment:Offset into [0x0318], [0x031A] */
  *e++ = 0x89; *e++ = 0x1E; *e++ = 0x18; *e++ = 0x03; /* mov [0x0318], bx */
  *e++ = 0xA3; *e++ = 0x1A; *e++ = 0x03;             /* mov [0x031A], ax */

  /* Telemetry Checkpoint 2: about to jump! mov word [0x0312], 0xAA02 */
  *e++ = 0xC7; *e++ = 0x06; *e++ = 0x12; *e++ = 0x03; *e++ = 0x02; *e++ = 0xAA;

  /* Restore BIOS stack */
  *e++ = 0x8E; *e++ = 0x16; *e++ = 0x06; *e++ = 0x03; /* mov ss, [0x0306] */
  *e++ = 0x8B; *e++ = 0x26; *e++ = 0x04; *e++ = 0x03; /* mov sp, [0x0304] */

  /* Zero general purpose registers for clean Windows handoff */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC0;             /* xor eax, eax */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xDB;             /* xor ebx, ebx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xC9;             /* xor ecx, ecx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xD2;             /* xor edx, edx */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xF6;             /* xor esi, esi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xFF;             /* xor edi, edi */
  *e++ = 0x66; *e++ = 0x31; *e++ = 0xED;             /* xor ebp, ebp */

  /* Far indirect jump directly to Windows HalpRMStub */
  *e++ = 0x2E; *e++ = 0xFF; *e++ = 0x2E; *e++ = 0x00; *e++ = 0x03; /* jmp far ptr cs:[0x0300] */

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
    if (entry_chk == 0xAA01 || entry_chk == 0xDEAD) {
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
        if (entry_chk == 0xAA01 || entry_chk == 0xDEAD) {
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

  /* 3. Build Pure 16-bit Real Mode S3 Resume Trampoline machine code in low memory (<1MB) */
  UINT32 tramp_bytes = GuAcpiBuildTrampolineCode(
      low_base,
      4096,
      trampoline_phys,
      NvsPhys,
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
   * Plan B: RSDT / XSDT Split Architecture
   * 1. Log pre-patch state: RSDT, XSDT, original FADT address, and their original checksums.
   */
  GuLog("S3 Plan B Pre-Patch: RSDT=%#llx (Chk=%#02x) XSDT=%#llx (Chk=%#02x) OrigFADT=%#llx (Len=%u Chk=%#02x)\n",
        (unsigned long long)Info->rsdt_phys, (unsigned)Info->rsdt_original_checksum,
        (unsigned long long)Info->xsdt_phys, (unsigned)Info->xsdt_original_checksum,
        (unsigned long long)Info->fadt_phys, (unsigned)Info->fadt->Header.Length,
        (unsigned)Info->fadt_original_checksum);

  /*
   * 2. Populate ShadowFADT at LowTrampoline + 0x400 (e.g. 0x9E400):
   *    Copy full Header.Length bytes from original FADT.
   */
  UINT32 fadt_len = Info->fadt->Header.Length;
  if (fadt_len > (4096 - GU_TRAMP_SHADOW_FADT_OFF)) {
    fadt_len = 4096 - GU_TRAMP_SHADOW_FADT_OFF;
  }
  memcpy(shadow_fadt, Info->fadt, fadt_len);

  /*
   * 3. In ShadowFADT (for XSDT / Windows):
   *    - FirmwareCtrl = (UINT32)shadow_facs_phys (0x9E200)
   *    - XFirmwareCtrl = 0 (exact original FADT layout so Windows maps 0x9E000 into virtual memory)
   *    - Recalculate ShadowFADT->Header.Checksum
   */
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
   * 4. In Original FADT (for RSDT / 32-bit BIOS):
   *    - FirmwareCtrl = RealFACS (0xDC8BB000)
   *    - XFirmwareCtrl = 0 (clean original layout)
   *    - Recalculate FADT->Header.Checksum
   */
  Info->fadt->FirmwareCtrl = (UINT32)Info->real_facs_phys;
  if (Info->fadt->Header.Length >= 140) {
    Info->fadt->XFirmwareCtrl = 0;
  }
  Info->fadt->Header.Checksum = 0;
  UINT8 f_sum = 0;
  UINT8 *f_bytes = (UINT8 *)Info->fadt;
  for (UINT32 i = 0; i < Info->fadt->Header.Length; i++) {
    f_sum = (UINT8)(f_sum + f_bytes[i]);
  }
  Info->fadt->Header.Checksum = (UINT8)(0 - f_sum);

  /*
   * 5. In XSDT:
   *    Divert the 64-bit table pointer for FACP/FADT to shadow_fadt_phys.
   *    Recalculate XSDT->Header.Checksum.
   */
  if (Info->xsdt && Info->xsdt_fadt_index >= 0) {
    UINT8 *entry_ptr = ((UINT8 *)Info->xsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
    UINT64 sf_phys64 = (UINT64)shadow_fadt_phys;
    memcpy(entry_ptr + Info->xsdt_fadt_index * sizeof(UINT64), &sf_phys64, sizeof(UINT64));

    Info->xsdt->Checksum = 0;
    UINT8 xsdt_sum = 0;
    UINT8 *xsdt_bytes = (UINT8 *)Info->xsdt;
    for (UINT32 i = 0; i < Info->xsdt->Length; i++) {
      xsdt_sum = (UINT8)(xsdt_sum + xsdt_bytes[i]);
    }
    Info->xsdt->Checksum = (UINT8)(0 - xsdt_sum);
  } else if (!Info->xsdt && Info->rsdt && Info->rsdt_fadt_index >= 0) {
    /* ACPI 1.0 Fallback (no XSDT): update RSDT 32-bit entry */
    UINT8 *entry_ptr = ((UINT8 *)Info->rsdt) + sizeof(EFI_ACPI_DESCRIPTION_HEADER);
    UINT32 sf_phys32 = (UINT32)shadow_fadt_phys;
    memcpy(entry_ptr + Info->rsdt_fadt_index * sizeof(UINT32), &sf_phys32, sizeof(UINT32));

    Info->rsdt->Checksum = 0;
    UINT8 rsdt_sum = 0;
    UINT8 *rsdt_bytes = (UINT8 *)Info->rsdt;
    for (UINT32 i = 0; i < Info->rsdt->Length; i++) {
      rsdt_sum = (UINT8)(rsdt_sum + rsdt_bytes[i]);
    }
    Info->rsdt->Checksum = (UINT8)(0 - rsdt_sum);
  }

  /*
   * 6. Hook Real FACS waking vector to point to our Low Trampoline (<1MB)
   */
  Info->real_facs->FirmwareWakingVector = (UINT32)trampoline_phys;

  GuLog("S3 Plan B RSDT/XSDT Split Armed:\n"
        "  RSDT [idx=%d] -> RealFADT=%#llx (FirmwareCtrl=%#08x XFirmwareCtrl=%#llx Chk=%#02x)\n"
        "  XSDT [idx=%d] -> ShadowFADT=%#llx (FirmwareCtrl=%#08x XFirmwareCtrl=%#llx Chk=%#02x, XSDT Chk=%#02x)\n"
        "  RealFACS=%#llx (WakingVector=%#08x) ShadowFACS=%#llx LowTrampoline=%#llx\n",
        (int)Info->rsdt_fadt_index, (unsigned long long)Info->fadt_phys,
        (unsigned)Info->fadt->FirmwareCtrl, (unsigned long long)Info->fadt->XFirmwareCtrl,
        (unsigned)Info->fadt->Header.Checksum,
        (int)Info->xsdt_fadt_index, (unsigned long long)shadow_fadt_phys,
        (unsigned)shadow_fadt->FirmwareCtrl, (unsigned long long)shadow_fadt->XFirmwareCtrl,
        (unsigned)shadow_fadt->Header.Checksum,
        Info->xsdt ? (unsigned)Info->xsdt->Checksum : 0,
        (unsigned long long)Info->real_facs_phys,
        (unsigned)Info->real_facs->FirmwareWakingVector,
        (unsigned long long)shadow_facs_phys,
        (unsigned long long)trampoline_phys);

  return EFI_SUCCESS;
}
