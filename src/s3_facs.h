/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GU_S3_FACS_H
#define GU_S3_FACS_H

#include <Uefi.h>
#include <IndustryStandard/Acpi20.h>
#include "s3_save.h"

/* Standard ACPI Table GUIDs in UEFI System Configuration Table */
#define GU_EFI_ACPI_20_TABLE_GUID \
  { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#define GU_EFI_ACPI_10_TABLE_GUID \
  { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* Signatures */
#define GU_ACPI_RSDP_SIGNATURE    0x2052545020445352ULL /* "RSD PTR " */
#define GU_ACPI_XSDT_SIGNATURE    0x54445358U           /* "XSDT" */
#define GU_ACPI_RSDT_SIGNATURE    0x54445352U           /* "RSDT" */
#define GU_ACPI_FADT_SIGNATURE    0x50434146U           /* "FACP" */
#define GU_ACPI_FACS_SIGNATURE    0x53434146U           /* "FACS" */
#define GU_SHADOW_FACS_SIGNATURE  0x53554753U           /* "SGUS" */
#define GU_TRAMP_TELEMETRY_MAGIC  0x53335F54U           /* "S3_T" */
#define GU_TRAMP_ENTRY_STATUS     0xAA01U
#define GU_TRAMP_EXIT_STATUS      0xAA02U
#define GU_TRAMP_FAULT_STATUS     0xDEADU

/* Telemetry and Table Offsets within Low Trampoline Memory (<1MB) */
#define GU_TRAMP_FAR_JUMP_OFF          0x0300U
#define GU_TRAMP_BIOS_SP_OFF           0x0304U
#define GU_TRAMP_BIOS_SS_OFF           0x0306U
#define GU_TRAMP_EBS_CALL_COUNT_OFF    0x0304U
#define GU_TRAMP_EBS_SUCCESS_COUNT_OFF 0x0308U
#define GU_TRAMP_EBS_LAST_STATUS_OFF   0x030CU
#define GU_TRAMP_ENTRY_STATUS_OFF      0x0310U
#define GU_TRAMP_EXIT_STATUS_OFF       0x0312U
#define GU_TRAMP_CAPTURED_VEC_OFF      0x0314U
#define GU_TRAMP_TARGET_PTR_OFF        0x0318U
#define GU_TRAMP_MAGIC_OFF             0x031CU
#define GU_TRAMP_PCI_PROBE_ADDRS_OFF   0x0320U
#define GU_TRAMP_PCI_PROBE_RAW_OFF     0x0340U
#define GU_TRAMP_PCI_PROBE_POST_OFF    0x0360U
#define GU_TRAMP_RM_IDTR_OFF           0x01B4U
#define GU_TRAMP_GDT_OFF               0x01C0U
#define GU_TRAMP_GDTR_OFF              0x01E8U
#define GU_TRAMP_FARPTR32_OFF          0x01F2U
#define GU_TRAMP_SHADOW_FACS_OFF       0x0200U
#define GU_TRAMP_REAL_FACS_MIRROR_OFF  0x0280U
#define GU_TRAMP_SHADOW_FADT_OFF       0x0400U
#define GU_TRAMP_SHADOW_RSDP_OFF       0x0600U
#define GU_TRAMP_SHADOW_XSDT_OFF       0x0640U

#pragma pack(push, 1)
typedef struct {
  BOOLEAN found;
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *rsdp;
  EFI_ACPI_DESCRIPTION_HEADER *rsdt;
  EFI_PHYSICAL_ADDRESS rsdt_phys;
  EFI_ACPI_DESCRIPTION_HEADER *xsdt;
  EFI_PHYSICAL_ADDRESS xsdt_phys;
  INTN xsdt_fadt_index;
  INTN rsdt_fadt_index;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *fadt;
  EFI_PHYSICAL_ADDRESS fadt_phys;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *shadow_fadt;
  EFI_PHYSICAL_ADDRESS shadow_fadt_phys;
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *shadow_rsdp;
  EFI_PHYSICAL_ADDRESS shadow_rsdp_phys;
  EFI_ACPI_DESCRIPTION_HEADER *shadow_xsdt;
  EFI_PHYSICAL_ADDRESS shadow_xsdt_phys;
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *real_facs;
  EFI_PHYSICAL_ADDRESS real_facs_phys;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys;
  EFI_PHYSICAL_ADDRESS trampoline_phys;
  INTN config_table_acpi20_idx;
  INTN config_table_acpi10_idx;
  UINT32 original_firmware_waking_vector;
  UINT64 original_xfirmware_waking_vector;
  UINT8  fadt_original_checksum;
  UINT8  xsdt_original_checksum;
  UINT8  rsdt_original_checksum;
  UINT8  shadow_rsdp_checksum;
  UINT8  shadow_rsdp_extended_checksum;
  UINT8  shadow_xsdt_checksum;
  BOOLEAN ebs_hook_installed;
} GU_ACPI_FACS_INFO;
#pragma pack(pop)

/*
 * Locates the ACPI RSDP, traverses XSDT/RSDT to find FADT ('FACP'),
 * and resolves the active physical FACS ('FACS') table address.
 */
EFI_STATUS GuAcpiLocateFadtAndFacs(
    IN EFI_SYSTEM_TABLE *SystemTable,
    OUT GU_ACPI_FACS_INFO *Info
);

/*
 * Installs the universal ACPI Shadow FACS and S3 Resume Trampoline (Plan C):
 * 1. Clones Real FACS into Shadow FACS in ACPI NVS memory.
 * 2. Builds complete Shadow Chain: ShadowRSDP -> ShadowXSDT -> ShadowFADT -> ShadowFACS.
 * 3. Keeps RealRSDP, RealRSDT, RealXSDT, RealFADT 100% byte-for-byte untouched.
 * 4. Hooks RealFACS->FirmwareWakingVector to point to Low Trampoline (<1MB).
 * 5. In-place patches SystemTable->ConfigurationTable to point to ShadowRSDP.
 */
EFI_STATUS GuAcpiInstallShadowFacs(
    IN EFI_SYSTEM_TABLE *SystemTable,
    IN GU_S3_RESUME_CONTEXT *Context,
    IN VOID *NvsBuffer,
    IN EFI_PHYSICAL_ADDRESS NvsPhys,
    IN VOID *LowTrampBuffer,
    IN EFI_PHYSICAL_ADDRESS LowTrampPhys,
    OUT GU_ACPI_FACS_INFO *Info
);

/*
 * ExitBootServices Sandwich Hook management (Plan C):
 * GuAcpiInstallEbsHook hooks gBS->ExitBootServices right before starting Windows.
 * GuHookedExitBootServices reverts gST->ConfigurationTable to RealRSDP before calling
 * firmware ExitBootServices, and restores ShadowRSDP if ExitBootServices returns.
 */
EFI_STATUS GuAcpiInstallEbsHook(IN EFI_SYSTEM_TABLE *SystemTable);
EFI_STATUS GuAcpiUninstallEbsHook(IN EFI_SYSTEM_TABLE *SystemTable);
VOID GuAcpiApplyShadowConfigTable(VOID);
VOID GuAcpiRestoreConfigTable(VOID);

/*
 * Assembles the S3 Resume Trampoline machine code into the designated buffer.
 * In v1.8.41: 32-bit Flat Protected Mode Trampoline with exact PM16->RM16 mode transition.
 * Returns the size in bytes of the generated trampoline code.
 */
UINT32 GuAcpiBuildTrampolineCode(
    OUT UINT8 *Buffer,
    IN UINT32 BufferSize,
    IN EFI_PHYSICAL_ADDRESS TrampolinePhys,
    IN EFI_PHYSICAL_ADDRESS RealFacsPhys,
    IN EFI_PHYSICAL_ADDRESS ShadowFacsPhys,
    IN UINT32 RootPortLinkReg,
    IN UINT32 GpuPmcsrReg
);

#endif /* GU_S3_FACS_H */
