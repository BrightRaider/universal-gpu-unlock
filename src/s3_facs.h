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

/* Telemetry and Table Offsets within Low Trampoline Memory (<1MB) (v1.8.59: 0x0800..0x08FF) */
#define GU_TRAMP_FAR_JUMP_OFF          0x0800U
#define GU_TRAMP_RESTORED_COUNT_OFF    0x0800U
#define GU_TRAMP_S3_SAVE_COUNT_OFF     0x0802U
#define GU_TRAMP_RESTORED_RSDP_OFF     0x0804U
#define GU_TRAMP_EBS_CALL_COUNT_OFF    0x0808U
#define GU_TRAMP_EBS_SUCCESS_COUNT_OFF 0x080AU
#define GU_TRAMP_EBS_LAST_STATUS_OFF   0x080CU
#define GU_TRAMP_ENTRY_STATUS_OFF      0x0810U
#define GU_TRAMP_EXIT_STATUS_OFF       0x0812U
#define GU_TRAMP_CAPTURED_VEC_OFF      0x0814U
#define GU_TRAMP_TARGET_PTR_OFF        0x0818U
#define GU_TRAMP_MAGIC_OFF             0x081CU
#define GU_TRAMP_PCI_PROBE_ADDRS_OFF   0x0820U
#define GU_TRAMP_PCI_PROBE_RAW_OFF     0x0840U
#define GU_TRAMP_PCI_PROBE_POST_OFF    0x0860U
#define GU_TRAMP_BIOS_SP_OFF           0x0870U
#define GU_TRAMP_BIOS_SS_OFF           0x0872U
#define GU_TRAMP_REAL_FACS_MIRROR_OFF  0x0880U

/* GDT & IDTR Offsets within Low Trampoline Memory (<1MB) (v1.8.59: 0x0900..0x093F) */
#define GU_TRAMP_RM_IDTR_OFF           0x0900U
#define GU_TRAMP_PM_GDTR_OFF           0x0908U
#define GU_TRAMP_GDT_BASE_OFF          0x0910U

/* S3 Wake Hardware Unlock Telemetry Offsets within Non-NVS Telemetry Page (v1.8.47 / v1.8.59) */
#define GU_TELEM_UNLOCK_MAGIC          0x554E4C4BU /* "UNLK" */
#define GU_TELEM_UNLOCK_MAGIC_OFF      0x0080U /* "UNLK" (0x554E4C4B) */
#define GU_TELEM_UNLOCK_STATUS_OFF     0x0084U /* 1=OK, 2=Mismatch, 3=Fault */
#define GU_TELEM_CHIP_ID_OFF           0x0088U /* e.g. 0x192000a1 (AD102) */
#define GU_TELEM_BAR0_ASSIGNED_OFF     0x008CU /* e.g. 0xFB000000 */
#define GU_TELEM_SS0_BEFORE_OFF        0x0090U /* SS0 before unlock */
#define GU_TELEM_SS1_BEFORE_OFF        0x0094U /* SS1 before unlock */
#define GU_TELEM_PLM_BEFORE_OFF        0x0098U /* PLM before unlock */
#define GU_TELEM_READOUT_BEFORE_OFF    0x009CU /* Readout1 before unlock */
#define GU_TELEM_SS0_AFTER_OFF         0x00A0U /* SS0 after unlock (0x88888888) */
#define GU_TELEM_SS1_AFTER_OFF         0x00A4U /* SS1 after unlock (0x00000008) */
#define GU_TELEM_READOUT_AFTER_OFF     0x00A8U /* Readout1 after unlock (0x00000000) */
#define GU_TELEM_PLM_AFTER_OFF         0x00ACU /* PLM after unlock */
#define GU_TELEM_RP_CMD_BEFORE_OFF     0x00B0U /* Root Port Command before */
#define GU_TELEM_RP_MEM_BEFORE_OFF     0x00B4U /* Root Port MemBase/Limit before */
#define GU_TELEM_GPU_CMD_BEFORE_OFF    0x00B8U /* GPU Command before */
#define GU_TELEM_GPU_BAR0_BEFORE_OFF   0x00BCU /* GPU BAR0 before */
#define GU_TELEM_PMC_ENABLE_OFF        0x00C0U /* PMC_ENABLE (BAR0 + 0x00000200) (v1.8.51) */
#define GU_TELEM_SEC2_RESET_OFF        0x00C4U /* SEC2_RESET (BAR0 + 0x008403C0) (v1.8.51) */
#define GU_TELEM_SEC2_HWCFG2_OFF       0x00C8U /* SEC2_HWCFG2 (BAR0 + 0x008400F4) (v1.8.51) */
#define GU_TELEM_FALCON2_BCR_OFF       0x00CCU /* FALCON2_BCR (BAR0 + 0x00841668) (v1.8.51) */
#define GU_TELEM_SEC2_CPUCTL_POST_OFF  0x00D0U /* SEC2_CPUCTL after reset cycle (BAR0 + 0x00840100) (v1.8.51) */
#define GU_TELEM_FALCON2_BCR_POST_OFF  0x00D4U /* FALCON2_BCR after switch out of Boot-ROM (BAR0 + 0x00841668) (v1.8.52) */
#define GU_TELEM_BOOTER_CPUCTL_POST_OFF 0x00D8U /* SEC2_CPUCTL after booter execution (BAR0 + 0x00840100) (v1.8.53) */
#define GU_TELEM_BOOTER_MARKER_POST_OFF 0x00DCU /* Mailbox 0 / Marker after booter execution (BAR0 + 0x00840040) (v1.8.54) */
#define GU_TELEM_POST_FLR_WPR2_OFF     0x00E0U /* Post-FLR WPR2_HI (BAR0 + 0x001fa828) (v1.8.59) */
#define GU_TELEM_POST_FLR_SS0_OFF      0x00E4U /* Post-FLR SS0 (BAR0 + 0x0082381C) (v1.8.59) */
#define GU_TELEM_POST_FLR_SS1_OFF      0x00E8U /* Post-FLR SS1 (BAR0 + 0x00823820) (v1.8.59) */
#define GU_TELEM_POST_FLR_PLM_OFF      0x00ECU /* Post-FLR PLM (BAR0 + 0x00823804) (v1.8.59) */
#define GU_TELEM_PRE_BOOTER_WPR2_LO_OFF  0x00F0U /* Pre-Booter WPR2_LO (BAR0 + 0x001fa824) (v1.8.63) */
#define GU_TELEM_PRE_BOOTER_WPR2_HI_OFF  0x00F4U /* Pre-Booter WPR2_HI (BAR0 + 0x001fa828) (v1.8.63) */
#define GU_TELEM_MMU_LOCAL_MEM_RANGE_OFF 0x00F8U /* NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE (BAR0 + 0x00100ce0) (v1.8.63) */
#define GU_TELEM_GFW_BOOT_STATUS_OFF     0x00FCU /* GFW-BOOT-Completion (BAR0 + 0x00118234) (v1.8.63) */
#define GU_TELEM_GSP_HWCFG2_OFF          0x0100U /* GSP_HWCFG2 (BAR0 + 0x001100f4) (v1.8.63) */
#define GU_TELEM_GSP_CPUCTL_OFF          0x0104U /* GSP_CPUCTL (BAR0 + 0x00110100) (v1.8.63) */
#define GU_TELEM_POST_FWSEC_WPR2_LO_OFF  0x0108U /* Post-FWSEC WPR2_LO (BAR0 + 0x001fa824) (v1.8.64) */
#define GU_TELEM_POST_FWSEC_WPR2_HI_OFF  0x010CU /* Post-FWSEC WPR2_HI (BAR0 + 0x001fa828) (v1.8.64) */
#define GU_TELEM_POST_FWSEC_GSP_CPUCTL_OFF 0x0110U /* Post-FWSEC GSP_CPUCTL (BAR0 + 0x00110100) (v1.8.64) */
#define GU_TELEM_POST_FWSEC_GSP_MAILBOX0_OFF 0x0114U /* Post-FWSEC GSP_MAILBOX0 (BAR0 + 0x00110040) (v1.8.64) */
#define GU_TELEM_POST_FWSEC_SCRATCH_OFF  0x0118U /* Post-FWSEC Scratch (BAR0 + 0x00001438) (v1.8.64) */

/* Dedicated High (>1MB, <4GB) ACPI NVS Page Offsets for Shadow Tables */
#define GU_HIGH_SHADOW_RSDP_OFF        0x0000U
#define GU_HIGH_SHADOW_XSDT_OFF        0x0080U
#define GU_HIGH_SHADOW_FADT_OFF        0x0200U
#define GU_HIGH_SHADOW_FACS_OFF        0x0400U

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
  EFI_PHYSICAL_ADDRESS high_acpi_phys;
  EFI_PHYSICAL_ADDRESS trampoline_phys;
  EFI_PHYSICAL_ADDRESS telemetry_page_phys;
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
  BOOLEAN s3_save_hook_installed;
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
UINT32 GuAcpiApplyShadowConfigTable(VOID);
UINT32 GuAcpiRestoreConfigTable(VOID);

/*
 * Assembles the S3 Resume Trampoline machine code into the designated buffer.
 * Returns the size in bytes of the generated trampoline code.
 */
UINT32 GuAcpiBuildTrampolineCode(
    OUT UINT8 *Buffer,
    IN UINT32 BufferSize,
    IN EFI_PHYSICAL_ADDRESS TrampolinePhys,
    IN EFI_PHYSICAL_ADDRESS ContextPhys,
    IN EFI_PHYSICAL_ADDRESS ShadowFacsPhys,
    IN EFI_PHYSICAL_ADDRESS TelemetryPhys,
    IN EFI_PHYSICAL_ADDRESS GpuBar0Phys,
    IN UINT32 RootPortLinkReg,
    IN UINT32 GpuPmcsrReg,
    IN EFI_PHYSICAL_ADDRESS FwsecDmaPhys
);

#endif /* GU_S3_FACS_H */
