/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#undef NULL
#undef EPROTO
#undef EBADMSG
#undef EOPNOTSUPP
#undef ETIMEDOUT
#undef EOVERFLOW
#undef EALREADY

#include "../src/s3_save.h"
#include "../src/s3_facs.h"
#include "../src/s3_resume.h"
#include "../src/port.h"
#include "../src/compat/compat.h"
#include <stddef.h>

#include <stdarg.h>

GU_PLATFORM Gu;
GU_ASSET GuAssets[5];
static char g_log_history[8192] = {0};

VOID GuLog(const char *Format, ...) {
  char temp[1024];
  va_list args;
  va_start(args, Format);
  vsnprintf(temp, sizeof(temp), Format, args);
  va_end(args);
  size_t cur = strlen(g_log_history);
  if (cur + strlen(temp) + 1 < sizeof(g_log_history)) {
    strcat(g_log_history, temp);
  }
}

static unsigned int g_checks = 0;
#define CHECK(cond, msg) do { \
  g_checks++; \
  if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    fflush(stderr); \
    exit(1); \
  } \
} while(0)

static void TestGuS3ResumeContextOffsets(void) {
  printf("[0] Testing GU_S3_RESUME_CONTEXT struct offsets & alignment...\n");
  CHECK(sizeof(GU_S3_RESUME_CONTEXT) == 108, "GU_S3_RESUME_CONTEXT size must be 108 bytes");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, root_port_link_reg) == GU_S3_CTX_OFF_ROOT_PORT_LNK, "root_port_link_reg offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, gpu_pmcsr_reg) == GU_S3_CTX_OFF_GPU_PMCSR, "gpu_pmcsr_reg offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, magic) == GU_S3_CTX_OFF_MAGIC, "magic offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, bar_base) == GU_S3_CTX_OFF_BAR_BASE, "bar_base offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, is_ampere) == GU_S3_CTX_OFF_IS_AMPERE, "is_ampere offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, is_turing) == GU_S3_CTX_OFF_IS_TURING, "is_turing offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, pci_cmd_port_addr) == GU_S3_CTX_OFF_PCI_CMD_ADDR, "pci_cmd_port_addr offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, booter_dma) == GU_S3_CTX_OFF_BOOTER_DMA, "booter_dma offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, booter_size) == GU_S3_CTX_OFF_BOOTER_SZ, "booter_size offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, val_ss0) == GU_S3_CTX_OFF_VAL_SS0, "val_ss0 offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, val_ss1) == GU_S3_CTX_OFF_VAL_SS1, "val_ss1 offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, resume_status) == GU_S3_CTX_OFF_STATUS, "resume_status offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, resume_count) == GU_S3_CTX_OFF_COUNT, "resume_count offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, last_sec2_marker) == GU_S3_CTX_OFF_SEC2_MARKER, "last_sec2_marker offset");
}

/* Mock 16MB MMIO window for BAR0 */
#define MOCK_BAR_DWORDS (0x1000000 / 4)
static UINT32 g_mock_bar[MOCK_BAR_DWORDS];

static void TestGuS3ExecuteResumeUnlock_Validation(void) {
  printf("[1] Testing S3 Resume validation (NULL & bad magic)...\n");
  INTN ret = GuS3ExecuteResumeUnlock(NULL);
  CHECK(ret == -1, "NULL context must return -1");

  GU_S3_RESUME_CONTEXT bad_ctx;
  memset(&bad_ctx, 0, sizeof(bad_ctx));
  bad_ctx.magic = 0x12345678U;
  ret = GuS3ExecuteResumeUnlock(&bad_ctx);
  CHECK(ret == -1, "Bad magic must return -1");

  bad_ctx.magic = GU_S3_CONTEXT_MAGIC;
  bad_ctx.bar_base = 0;
  ret = GuS3ExecuteResumeUnlock(&bad_ctx);
  CHECK(ret == -1, "Zero bar_base must return -1");
}

static void TestGuS3ExecuteResumeUnlock_AdaAmpere(void) {
  printf("[2] Testing S3 Resume hardware unlock for Ada/Ampere...\n");
  memset(g_mock_bar, 0, sizeof(g_mock_bar));

  /* Simulate cold locked state: SS0=0, SS1=0, CPUCTL halt bit initially 0 */
  g_mock_bar[0x0082381c / 4] = 0;
  g_mock_bar[0x00823820 / 4] = 0;
  g_mock_bar[0x00840100 / 4] = 0x10; /* Set halt bit so loop terminates immediately */
  g_mock_bar[0x00840040 / 4] = 0x67b00001U; /* Simulated SEC2 marker */

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 1;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.pci_id = 0x268410deU; /* RTX 4090 */
  ctx.is_ampere = 0;
  ctx.is_turing = 0;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;
  ctx.booter_dma = 0xdc5d9000ULL;
  ctx.booter_size = 57672;

  INTN ret = GuS3ExecuteResumeUnlock(&ctx);
  CHECK(ret == 0, "Ada/Ampere unlock must succeed (return 0)");
  CHECK(ctx.resume_status == 1, "Resume status must be 1 (success)");
  CHECK(ctx.resume_count == 1, "Resume count must be incremented to 1");
  CHECK(ctx.last_sec2_marker == 0x67b00001U, "SEC2 marker must be captured");

  /* Verify register writes occurred correctly on the mock BAR */
  CHECK(g_mock_bar[0x0082381c / 4] == 0x88888888U, "BAR0 SS0 must be 0x88888888");
  CHECK(g_mock_bar[0x00823820 / 4] == 0x00000008U, "BAR0 SS1 must be 0x00000008");
  CHECK(g_mock_bar[0x00840110 / 4] == (UINT32)(ctx.booter_dma >> 8), "Booter DMA address must be programmed");
  CHECK(g_mock_bar[0x00840100 / 4] == 2, "CPUCTL must have been started with 2");
}

static void TestGuS3ExecuteResumeUnlock_Turing(void) {
  printf("[3] Testing S3 Resume hardware unlock for Turing...\n");
  memset(g_mock_bar, 0, sizeof(g_mock_bar));
  g_mock_bar[0x00840100 / 4] = 0x10;

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 1;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.pci_id = 0x1e0410deU; /* TU102 (RTX 2080 Ti) */
  ctx.is_ampere = 0;
  ctx.is_turing = 1;
  ctx.booter_dma = 0xdc5d9000ULL;

  INTN ret = GuS3ExecuteResumeUnlock(&ctx);
  CHECK(ret == 0, "Turing unlock must succeed (return 0)");
  CHECK(ctx.resume_status == 1, "Resume status must be 1 (success)");
  CHECK(ctx.resume_count == 1, "Resume count must be incremented to 1");
  CHECK(g_mock_bar[0x00840110 / 4] == (UINT32)(ctx.booter_dma >> 8), "Turing booter DMA programmed");
}

static void TestGuS3ResumeDispatch_Wrapper(void) {
  printf("[4] Testing S3 Resume Dispatch wrapper entrypoint...\n");
  memset(g_mock_bar, 0, sizeof(g_mock_bar));
  g_mock_bar[0x00840100 / 4] = 0x10;

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;

  GuS3ResumeDispatch(NULL, &ctx);
  CHECK(ctx.resume_status == 1, "Dispatch must set resume_status to 1");
  CHECK(ctx.resume_count == 1, "Dispatch must increment resume_count to 1");

  /* Call a second time to simulate subsequent resume cycle */
  GuS3ResumeDispatch(NULL, &ctx);
  CHECK(ctx.resume_count == 2, "Second resume cycle must increment resume_count to 2");

  /* Call with context passed in This (RCX parameter fallback) */
  GuS3ResumeDispatch(&ctx, NULL);
  CHECK(ctx.resume_count == 3, "RCX parameter fallback must increment resume_count to 3");
}

static void TestGuS3GuidsAndOpcodes(void) {
  printf("[5] Testing UEFI PI Specification S3 Protocol GUIDs & Opcodes...\n");
  EFI_GUID s3_guid = EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  CHECK(s3_guid.Data1 == 0xe857caf6, "S3 Save State GUID Data1 must be 0xe857caf6 (UEFI PI standard)");
  CHECK(s3_guid.Data2 == 0xc046, "S3 Save State GUID Data2 must be 0xc046");
  CHECK(s3_guid.Data3 == 0x45dc, "S3 Save State GUID Data3 must be 0x45dc");

  EFI_GUID framework_guid = EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID;
  CHECK(framework_guid.Data1 == 0x470e1529, "Boot Script Save GUID Data1 must be 0x470e1529 (Framework standard)");

  CHECK(EFI_BOOT_SCRIPT_IO_WRITE_OPCODE == 0x00, "IO_WRITE must be 0x00");
  CHECK(EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE == 0x02, "MEM_WRITE must be 0x02");
  CHECK(EFI_BOOT_SCRIPT_DISPATCH_OPCODE == 0x08, "DISPATCH must be 0x08");
  CHECK(EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE == 0x09, "DISPATCH_2 must be 0x09");
}

static VOID *g_mock_ebs_observed_vendor_table = NULL;
static EFI_SYSTEM_TABLE mock_st;
static EFI_STATUS g_mock_ebs_return_status = EFI_SUCCESS;

static EFI_STATUS EFIAPI MockOriginalExitBootServices(EFI_HANDLE ImageHandle, UINTN MapKey) {
  (VOID)ImageHandle; (VOID)MapKey;
  if (mock_st.ConfigurationTable) {
    g_mock_ebs_observed_vendor_table = mock_st.ConfigurationTable[0].VendorTable;
  }
  return g_mock_ebs_return_status;
}

static EFI_STATUS EFIAPI MockCalculateCrc32(VOID *Data, UINTN DataSize, UINT32 *Crc32) {
  (VOID)Data; (VOID)DataSize;
  if (Crc32) *Crc32 = 0x12345678;
  return EFI_SUCCESS;
}

static void TestGuAcpiFacsDiscoveryAndPatching(void) {
  printf("[6] Testing Universal ACPI FACS & FADT Discovery and Shadow Patching...\n");

  /* Setup Mock ACPI hierarchy */
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE mock_facs;
  memset(&mock_facs, 0, sizeof(mock_facs));
  mock_facs.Signature = GU_ACPI_FACS_SIGNATURE; /* 'FACS' */
  mock_facs.Length = sizeof(mock_facs);
  mock_facs.HardwareSignature = 0x12345678U;
  mock_facs.FirmwareWakingVector = 0x00007C00U; /* Initial waking vector */

  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE mock_fadt;
  memset(&mock_fadt, 0, sizeof(mock_fadt));
  mock_fadt.Header.Signature = GU_ACPI_FADT_SIGNATURE; /* 'FACP' */
  mock_fadt.Header.Length = sizeof(mock_fadt);
  mock_fadt.Header.Revision = 3;
  mock_fadt.FirmwareCtrl = (UINT32)(uintptr_t)&mock_facs;
  mock_fadt.XFirmwareCtrl = (UINT64)(uintptr_t)&mock_facs;

  /* Calculate initial FADT checksum */
  mock_fadt.Header.Checksum = 0;
  UINT8 sum = 0;
  UINT8 *p = (UINT8 *)&mock_fadt;
  for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + p[i]);
  mock_fadt.Header.Checksum = (UINT8)(0 - sum);

  /* Verify initial FADT checksum is 0 mod 256 */
  sum = 0;
  for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Initial mock FADT checksum must be 0 mod 256");

  /* Mock RSDT containing 32-bit pointer to FADT */
#pragma pack(push, 1)
  struct {
    EFI_ACPI_DESCRIPTION_HEADER Header;
    UINT32 Entry[1];
  } mock_rsdt;
#pragma pack(pop)
  memset(&mock_rsdt, 0, sizeof(mock_rsdt));
  mock_rsdt.Header.Signature = 0x54445352U; /* 'RSDT' */
  mock_rsdt.Header.Length = sizeof(mock_rsdt);
  mock_rsdt.Entry[0] = (UINT32)(uintptr_t)&mock_fadt;
  mock_rsdt.Header.Checksum = 0;
  UINT8 r_sum = 0;
  UINT8 *r_p = (UINT8 *)&mock_rsdt;
  for (UINT32 i = 0; i < mock_rsdt.Header.Length; i++) r_sum = (UINT8)(r_sum + r_p[i]);
  mock_rsdt.Header.Checksum = (UINT8)(0 - r_sum);

  /* Mock XSDT containing 64-bit pointer to FADT */
#pragma pack(push, 1)
  struct {
    EFI_ACPI_DESCRIPTION_HEADER Header;
    UINT64 Entry[1];
  } mock_xsdt;
#pragma pack(pop)
  memset(&mock_xsdt, 0, sizeof(mock_xsdt));
  mock_xsdt.Header.Signature = 0x54445358U; /* 'XSDT' */
  mock_xsdt.Header.Length = sizeof(mock_xsdt);
  mock_xsdt.Entry[0] = (UINT64)(uintptr_t)&mock_fadt;
  mock_xsdt.Header.Checksum = 0;
  UINT8 x_sum = 0;
  UINT8 *x_p = (UINT8 *)&mock_xsdt;
  for (UINT32 i = 0; i < mock_xsdt.Header.Length; i++) x_sum = (UINT8)(x_sum + x_p[i]);
  mock_xsdt.Header.Checksum = (UINT8)(0 - x_sum);

  /* Mock RSDP */
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER mock_rsdp;
  memset(&mock_rsdp, 0, sizeof(mock_rsdp));
  mock_rsdp.Signature = GU_ACPI_RSDP_SIGNATURE;
  mock_rsdp.Length = sizeof(mock_rsdp);
  mock_rsdp.Revision = 2;
  mock_rsdp.RsdtAddress = (UINT32)(uintptr_t)&mock_rsdt;
  mock_rsdp.XsdtAddress = (UINT64)(uintptr_t)&mock_xsdt;

  /* Calculate initial mock RSDP checksums */
  mock_rsdp.Checksum = 0;
  UINT8 rsdp_init_sum20 = 0;
  UINT8 *p_mrsdp = (UINT8 *)&mock_rsdp;
  for (UINT32 i = 0; i < 20; i++) rsdp_init_sum20 = (UINT8)(rsdp_init_sum20 + p_mrsdp[i]);
  mock_rsdp.Checksum = (UINT8)(0 - rsdp_init_sum20);

  mock_rsdp.ExtendedChecksum = 0;
  UINT8 rsdp_init_sum36 = 0;
  for (UINT32 i = 0; i < mock_rsdp.Length; i++) rsdp_init_sum36 = (UINT8)(rsdp_init_sum36 + p_mrsdp[i]);
  mock_rsdp.ExtendedChecksum = (UINT8)(0 - rsdp_init_sum36);

  /* Save original checksums of untouched real tables */
  UINT8 orig_fadt_checksum = mock_fadt.Header.Checksum;
  UINT8 orig_xsdt_checksum = mock_xsdt.Header.Checksum;
  UINT8 orig_rsdt_checksum = mock_rsdt.Header.Checksum;
  UINT8 orig_rsdp_checksum = mock_rsdp.Checksum;
  UINT8 orig_rsdp_ext_checksum = mock_rsdp.ExtendedChecksum;

  /* Mock EFI System Table & Boot Services */
  EFI_BOOT_SERVICES mock_bs;
  memset(&mock_bs, 0, sizeof(mock_bs));
  mock_bs.Hdr.HeaderSize = sizeof(mock_bs);
  mock_bs.ExitBootServices = MockOriginalExitBootServices;
  mock_bs.CalculateCrc32 = MockCalculateCrc32;

  EFI_CONFIGURATION_TABLE config_tables[1];
  EFI_GUID acpi20_guid = GU_EFI_ACPI_20_TABLE_GUID;
  config_tables[0].VendorGuid = acpi20_guid;
  config_tables[0].VendorTable = &mock_rsdp;

  memset(&mock_st, 0, sizeof(mock_st));
  mock_st.NumberOfTableEntries = 1;
  mock_st.ConfigurationTable = config_tables;
  mock_st.BootServices = &mock_bs;

  /* Run Discovery */
  GU_ACPI_FACS_INFO info;
  EFI_STATUS s = GuAcpiLocateFadtAndFacs(&mock_st, &info);
  CHECK(s == EFI_SUCCESS, "GuAcpiLocateFadtAndFacs must return EFI_SUCCESS");
  CHECK(info.found == TRUE, "ACPI tables must be found");
  CHECK(info.fadt == &mock_fadt, "Discovered FADT must match mock_fadt");
  CHECK(info.real_facs == &mock_facs, "Discovered FACS must match mock_facs");
  CHECK(info.original_firmware_waking_vector == 0x00007C00U, "Original waking vector must be preserved");

  /* Setup mock 256 KB NVS buffer for Shadow FACS and Trampoline */
  UINT8 mock_nvs[4096];
  memset(mock_nvs, 0, sizeof(mock_nvs));
  EFI_PHYSICAL_ADDRESS mock_nvs_phys = (EFI_PHYSICAL_ADDRESS)(uintptr_t)mock_nvs;

  /* Setup mock 4 KB Low Trampoline Buffer (<1MB) */
  UINT8 mock_low_tramp[4096];
  memset(mock_low_tramp, 0, sizeof(mock_low_tramp));
  EFI_PHYSICAL_ADDRESS mock_low_tramp_phys = (EFI_PHYSICAL_ADDRESS)(uintptr_t)mock_low_tramp;

  /* Host-Test: Pre-populate mock telemetry in mock_low_tramp to verify it is NOT destroyed before reading */
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_MAGIC_OFF) = GU_TRAMP_TELEMETRY_MAGIC;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_ENTRY_STATUS_OFF) = 0xAA01;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_EXIT_STATUS_OFF) = 0xAA02;
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_CAPTURED_VEC_OFF) = 0x0009E000U;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_TARGET_PTR_OFF) = 0x0000;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_TARGET_PTR_OFF + 2) = 0x9E00;
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x00) = 0x10221483U; /* AMD Root Port */
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x08) = 0x002D2D00U;
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x0C) = 0x20430040U; /* Gen4 x16 DLLLA=1 */
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x10) = 0x10DE2684U; /* RTX 4090 */

  g_log_history[0] = '\0';

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.bar_base = 0xfb000000ULL;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;
  ctx.pci_cmd_port_addr = 0x80000004U;

  /* Install Shadow FACS - must read pre-populated telemetry before low_base is zeroed */
  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, mock_nvs, mock_nvs_phys, mock_low_tramp, mock_low_tramp_phys, &info);
  CHECK(s == EFI_SUCCESS, "GuAcpiInstallShadowFacs must return EFI_SUCCESS");
  CHECK(strstr(g_log_history, "S3 Prior Wake Telemetry") != NULL, "Prior wake telemetry must be read and logged");
  CHECK(strstr(g_log_history, "DLLLA=1") != NULL, "Link status telemetry must report DLLLA=1");

  /* Verify fresh re-invocation logs 'none found' */
  g_log_history[0] = '\0';
  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, mock_nvs, mock_nvs_phys, mock_low_tramp, mock_low_tramp_phys, &info);
  CHECK(s == EFI_SUCCESS, "Second GuAcpiInstallShadowFacs must return EFI_SUCCESS");
  CHECK(strstr(g_log_history, "S3 Prior Wake Telemetry: none found") != NULL, "Re-run must log 'none found'");
  CHECK(s == EFI_SUCCESS, "GuAcpiInstallShadowFacs must return EFI_SUCCESS");
  CHECK(info.shadow_facs_phys == mock_low_tramp_phys + 0x00200, "Shadow FACS physical address must be low_tramp + 0x200");
  CHECK(info.shadow_fadt_phys == mock_low_tramp_phys + 0x00400, "Shadow FADT physical address must be low_tramp + 0x400");
  CHECK(info.shadow_rsdp_phys == mock_low_tramp_phys + 0x00600, "Shadow RSDP physical address must be low_tramp + 0x600");
  CHECK(info.shadow_xsdt_phys == mock_low_tramp_phys + 0x00640, "Shadow XSDT physical address must be low_tramp + 0x640");
  CHECK(info.trampoline_phys == mock_low_tramp_phys, "Trampoline physical address must be mock_low_tramp_phys (<1MB)");

  /* 1. Verify Plan C: Real ACPI tables remain 100% UNTOUCHED byte-for-byte */
  CHECK(mock_fadt.Header.Checksum == orig_fadt_checksum, "Real FADT checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_xsdt.Header.Checksum == orig_xsdt_checksum, "Real XSDT checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_rsdt.Header.Checksum == orig_rsdt_checksum, "Real RSDT checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_rsdp.Checksum == orig_rsdp_checksum, "Real RSDP 20-byte checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_rsdp.ExtendedChecksum == orig_rsdp_ext_checksum, "Real RSDP 36-byte extended checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_xsdt.Entry[0] == (UINT64)(uintptr_t)&mock_fadt, "Real XSDT Entry[0] must still point to Real FADT");
  CHECK(mock_rsdt.Entry[0] == (UINT32)(uintptr_t)&mock_fadt, "Real RSDT Entry[0] must still point to Real FADT");

  /* Verify Real FACS was hooked to point to Low Trampoline (<1MB) for AMI Aptio V */
  CHECK(mock_facs.FirmwareWakingVector == (UINT32)mock_low_tramp_phys, "Real FACS FirmwareWakingVector must point to Low Trampoline");
  CHECK(mock_facs.XFirmwareWakingVector == (UINT64)mock_low_tramp_phys, "Real FACS XFirmwareWakingVector must point to Low Trampoline (32-bit PM Wake)");
  CHECK(*(UINT32 *)((UINT8 *)&mock_facs + 0x24) == 0, "Real FACS OspmFlags must be 0 (no 64-bit wake)");
  CHECK((mock_facs.Flags & 2U) == 0, "Real FACS 64-bit wake flag must be cleared");

  /* 2. Verify Plan C: Shadow FACS */
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(mock_low_tramp + 0x00200);
  CHECK(shadow_facs->Signature == GU_ACPI_FACS_SIGNATURE, "Shadow FACS signature must be 'FACS'");
  CHECK(shadow_facs->HardwareSignature == 0x12345678U, "Shadow FACS hardware signature must match original");
  CHECK(shadow_facs->FirmwareWakingVector == 0, "Shadow FACS FirmwareWakingVector initialized to 0");
  CHECK(shadow_facs->XFirmwareWakingVector == (UINT64)mock_low_tramp_phys, "Shadow FACS XFirmwareWakingVector must point to Low Trampoline");
  CHECK(*(UINT32 *)((UINT8 *)shadow_facs + 0x24) == 0, "Shadow FACS OspmFlags must be 0");
  CHECK((shadow_facs->Flags & 2U) == 0, "Shadow FACS 64-bit wake flag must be cleared");

  /* 3. Verify Plan C: Shadow FADT at low_tramp + 0x400 */
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *shadow_fadt =
      (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)(mock_low_tramp + 0x00400);
  CHECK(shadow_fadt->Header.Signature == GU_ACPI_FADT_SIGNATURE, "Shadow FADT signature must be 'FACP'");
  CHECK(shadow_fadt->FirmwareCtrl == (UINT32)(mock_low_tramp_phys + 0x00200), "Shadow FADT FirmwareCtrl must point to Shadow FACS (0x9E200) for Windows");
  CHECK(shadow_fadt->XFirmwareCtrl == 0, "Shadow FADT XFirmwareCtrl must be 0");

  sum = 0;
  p = (UINT8 *)shadow_fadt;
  for (UINT32 i = 0; i < shadow_fadt->Header.Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow FADT checksum must be 0 mod 256");

  /* 4. Verify Plan C: Shadow XSDT at low_tramp + 0x640 */
  EFI_ACPI_DESCRIPTION_HEADER *shadow_xsdt =
      (EFI_ACPI_DESCRIPTION_HEADER *)(mock_low_tramp + 0x00640);
  CHECK(shadow_xsdt->Signature == GU_ACPI_XSDT_SIGNATURE, "Shadow XSDT signature must be 'XSDT'");
  UINT64 *sxsdt_entries = (UINT64 *)((UINT8 *)shadow_xsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
  CHECK(sxsdt_entries[0] == (UINT64)(mock_low_tramp_phys + 0x00400), "Shadow XSDT Entry[0] must point to Shadow FADT");

  sum = 0;
  p = (UINT8 *)shadow_xsdt;
  for (UINT32 i = 0; i < shadow_xsdt->Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow XSDT checksum must be 0 mod 256");

  /* 5. Verify Plan C: Shadow RSDP at low_tramp + 0x600 */
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *shadow_rsdp =
      (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)(mock_low_tramp + 0x00600);
  CHECK(shadow_rsdp->Signature == GU_ACPI_RSDP_SIGNATURE, "Shadow RSDP signature must be 'RSD PTR '");
  CHECK(shadow_rsdp->XsdtAddress == (UINT64)(mock_low_tramp_phys + 0x00640), "Shadow RSDP XsdtAddress must point to Shadow XSDT");
  CHECK(shadow_rsdp->RsdtAddress == mock_rsdp.RsdtAddress, "Shadow RSDP RsdtAddress must remain pointing to Real RSDT");

  sum = 0;
  p = (UINT8 *)shadow_rsdp;
  for (UINT32 i = 0; i < 20; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow RSDP 20-byte legacy checksum must be 0 mod 256");

  sum = 0;
  for (UINT32 i = 0; i < shadow_rsdp->Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow RSDP 36-byte extended checksum must be 0 mod 256");

  /* 6. Verify ConfigurationTable In-Place Patching */
  CHECK(mock_st.ConfigurationTable[0].VendorTable == (VOID *)(UINTN)(mock_low_tramp_phys + 0x00600),
        "SystemTable ConfigurationTable must point to Shadow RSDP");

  /* 7. Verify ExitBootServices Sandwich Hook */
  g_mock_ebs_observed_vendor_table = NULL;
  s = GuAcpiInstallEbsHook(&mock_st);
  CHECK(s == EFI_SUCCESS, "GuAcpiInstallEbsHook must return EFI_SUCCESS");
  CHECK(mock_st.BootServices->ExitBootServices != MockOriginalExitBootServices, "EBS pointer must be hooked");

  /* Invoke hooked EBS with EFI_SUCCESS */
  s = mock_st.BootServices->ExitBootServices((EFI_HANDLE)0x1, 0x1234);
  CHECK(s == EFI_SUCCESS, "Hooked EBS invocation must return EFI_SUCCESS");
  CHECK(g_mock_ebs_observed_vendor_table == &mock_rsdp,
        "Inside EBS sandwich: firmware/SMM must observe Real RSDP in ConfigurationTable");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == &mock_rsdp,
        "After successful EBS: ConfigurationTable must remain Real RSDP for SMM/firmware");

  /* Check diagnostic counters at LowTrampoline + 0x304 */
  UINT32 *ebs_diag = (UINT32 *)(mock_low_tramp + 0x304);
  CHECK(ebs_diag[0] == 1, "EbsCallCount at +0x304 must be 1");
  CHECK(ebs_diag[1] == 1, "EbsSuccessCount at +0x308 must be 1");
  CHECK(ebs_diag[2] == 0, "EbsLastStatus at +0x30C must be 0 (EFI_SUCCESS)");

  /* Test retry behavior when EBS fails (e.g. EFI_INVALID_PARAMETER) */
  g_mock_ebs_return_status = EFI_INVALID_PARAMETER;
  s = mock_st.BootServices->ExitBootServices((EFI_HANDLE)0x1, 0x5678);
  CHECK(s == EFI_INVALID_PARAMETER, "Hooked EBS must pass through error status");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == (VOID *)(UINTN)(mock_low_tramp_phys + 0x00600),
        "After failed EBS: ConfigurationTable must be restored to Shadow RSDP for winload retry");
  CHECK(ebs_diag[0] == 2, "EbsCallCount at +0x304 must be 2 after retry");
  CHECK(ebs_diag[1] == 1, "EbsSuccessCount at +0x308 must still be 1 after failed call");
  CHECK(ebs_diag[2] == (UINT32)EFI_INVALID_PARAMETER, "EbsLastStatus at +0x30C must record EFI_INVALID_PARAMETER");
  g_mock_ebs_return_status = EFI_SUCCESS;

  /* Uninstall EBS Hook */
  GuAcpiUninstallEbsHook(&mock_st);
  CHECK(mock_st.BootServices->ExitBootServices == MockOriginalExitBootServices,
        "Uninstalling EBS hook must restore original BootServices pointer");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == &mock_rsdp,
        "Uninstalling EBS hook must restore ConfigurationTable to Real RSDP");

  /* 8. Verify warm reset recovery: re-running discovery recovers Real FADT & Real FACS */
  GU_ACPI_FACS_INFO info_warm;
  s = GuAcpiLocateFadtAndFacs(&mock_st, &info_warm);
  CHECK(s == EFI_SUCCESS, "Warm reset discovery must succeed");
  CHECK(info_warm.fadt == &mock_fadt, "Warm reset must recover original Real FADT from RSDT");
  CHECK(info_warm.real_facs == &mock_facs, "Warm reset must recover original Real FACS table");
  CHECK(info_warm.real_facs_phys == (EFI_PHYSICAL_ADDRESS)(uintptr_t)&mock_facs, "Warm reset must recover original Real FACS address");
  printf("[6] Discovery, Shadow Chain & EBS Sandwich (Plan C): OK!\n");
  fflush(stdout);
}

static void TestGuAcpiBuildTrampolineCode(void) {
  printf("[7] Testing S3 Resume 32-Bit Flat Protected Mode Trampoline Generation...\n");
  fflush(stdout);

  UINT8 buffer[4096];
  EFI_PHYSICAL_ADDRESS tramp_phys = 0x0009E000ULL;
  EFI_PHYSICAL_ADDRESS real_facs_phys = 0xDC8BB000ULL;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = 0x0009E200ULL;

  UINT32 bytes = GuAcpiBuildTrampolineCode(buffer, sizeof(buffer), tramp_phys, real_facs_phys, shadow_facs_phys, 0x80001960U, 0x802D0064U);
  CHECK(bytes > 0, "Trampoline builder must generate non-zero bytes");
  CHECK(bytes <= GU_TRAMP_GDT_OFF, "Trampoline machine code must fit before GDT at +0x0180");

  /* 1. Check Preamble: cli, cld */
  CHECK(buffer[0] == 0xFA, "Preamble must start with CLI (0xFA)");
  CHECK(buffer[1] == 0xFC, "Preamble must have CLD (0xFC)");

  /* 2. Check 32-bit Checkpoint 1: mov dword ptr [0x0009E310], 0x0000AA01 (C7 05 10 E3 09 00 01 AA 00 00) */
  CHECK(buffer[2] == 0xC7 && buffer[3] == 0x05, "mov dword ptr [addr], imm32 opcode");
  CHECK(*(UINT32 *)(buffer + 4) == (UINT32)(tramp_phys + 0x0310), "Entry Checkpoint destination address [0x9E310]");
  CHECK(*(UINT32 *)(buffer + 8) == 0x0000AA01U, "Entry Checkpoint value 0xAA01");

  /* 3. Check 32-bit Telemetry Magic: mov dword ptr [0x0009E31C], 0x53335F54 (C7 05 1C E3 09 00 54 5F 33 53) */
  CHECK(buffer[12] == 0xC7 && buffer[13] == 0x05, "mov dword ptr [addr], imm32 opcode");
  CHECK(*(UINT32 *)(buffer + 14) == (UINT32)(tramp_phys + 0x031C), "Telemetry Magic destination address [0x9E31C]");
  CHECK(*(UINT32 *)(buffer + 18) == 0x53335F54U, "Telemetry Magic 'S3_T'");

  /* 4. Check 32-bit Native I/O Opcodes (EF, ED without 0x66 prefix) */
  UINT32 num_outs = 0, num_ins = 0;
  for (UINT32 i = 0; i < bytes; i++) {
    if (buffer[i] == 0xEF) { /* out dx, eax */
      CHECK(i == 0 || buffer[i - 1] != 0x66, "In 32-bit PM, out dx, eax must NOT have 0x66 prefix");
      num_outs++;
    } else if (buffer[i] == 0xED) { /* in eax, dx */
      CHECK(i == 0 || buffer[i - 1] != 0x66, "In 32-bit PM, in eax, dx must NOT have 0x66 prefix");
      num_ins++;
    }
  }
  CHECK(num_outs >= 8, "Trampoline must contain at least 8 native 32-bit OUT instructions (EF)");
  CHECK(num_ins >= 5, "Trampoline must contain at least 5 native 32-bit IN instructions (ED)");

  /* 5. Check stack-free execution (no push, no pop) */
  BOOLEAN found_stack_op = FALSE;
  for (UINT32 i = 0; i < bytes; i++) {
    /* Skip immediate operand of telemetry magic mov at indices 12..21 */
    if (i >= 12 && i <= 21) continue;
    if (buffer[i] >= 0x50 && buffer[i] <= 0x5F) {
      printf("  -> Stack op byte 0x%02X found at offset 0x%04X (%u)\n", buffer[i], i, i);
      found_stack_op = TRUE;
      break;
    }
  }
  CHECK(!found_stack_op, "Trampoline must be 100% stack-free (no push, no pop)");

  /* 6. Check GDT at +0x0180 with dynamic Base = 0x0009E000 */
  UINT8 *gdt = buffer + GU_TRAMP_GDT_OFF;
  UINT8 expected_null[8] = { 0 };
  CHECK(memcmp(gdt, expected_null, 8) == 0, "GDT Entry 0 must be Null Descriptor");

  UINT8 expected_code_9e[8] = { 0xFF, 0xFF, 0x00, 0xE0, 0x09, 0x9A, 0x00, 0x00 };
  CHECK(memcmp(gdt + 8, expected_code_9e, 8) == 0, "GDT Entry 1 must be 16-bit Code Descriptor with Base=0x9E000");

  UINT8 expected_data_9e[8] = { 0xFF, 0xFF, 0x00, 0xE0, 0x09, 0x92, 0x00, 0x00 };
  CHECK(memcmp(gdt + 16, expected_data_9e, 8) == 0, "GDT Entry 2 must be 16-bit Data Descriptor with Base=0x9E000");

  /* Dynamic Base verification with alternate address 0x0008C000 */
  UINT8 buffer2[4096];
  UINT32 bytes2 = GuAcpiBuildTrampolineCode(buffer2, sizeof(buffer2), 0x0008C000ULL, real_facs_phys, shadow_facs_phys, 0x80001960U, 0x802D0064U);
  CHECK(bytes2 > 0, "Second trampoline build with 0x8C000 must succeed");
  UINT8 expected_code_8c[8] = { 0xFF, 0xFF, 0x00, 0xC0, 0x08, 0x9A, 0x00, 0x00 };
  CHECK(memcmp(buffer2 + GU_TRAMP_GDT_OFF + 8, expected_code_8c, 8) == 0, "GDT Code Descriptor must dynamically reflect Base=0x8C000");
  UINT8 expected_data_8c[8] = { 0xFF, 0xFF, 0x00, 0xC0, 0x08, 0x92, 0x00, 0x00 };
  CHECK(memcmp(buffer2 + GU_TRAMP_GDT_OFF + 16, expected_data_8c, 8) == 0, "GDT Data Descriptor must dynamically reflect Base=0x8C000");

  /* 7. Check GDTR at +0x0198 and Real-Mode IDTR at +0x01A0 */
  CHECK(*(UINT16 *)(buffer + GU_TRAMP_GDTR_OFF) == 0x0017, "GDTR Limit must be 0x0017 (23 bytes, 3 descriptors)");
  CHECK(*(UINT32 *)(buffer + GU_TRAMP_GDTR_OFF + 2) == (UINT32)(tramp_phys + GU_TRAMP_GDT_OFF), "GDTR Base must point to LowTrampoline + 0x180");

  CHECK(*(UINT16 *)(buffer + GU_TRAMP_RM_IDTR_OFF) == 0x03FF, "Real-Mode IDTR Limit must be 0x03FF (1023 bytes)");
  CHECK(*(UINT32 *)(buffer + GU_TRAMP_RM_IDTR_OFF + 2) == 0x00000000U, "Real-Mode IDTR Base must be 0x00000000");

  /* 8. Check Transition Sequence opcodes */
  /* lgdt [TrampolinePhys + 0x0198] */
  BOOLEAN found_lgdt = FALSE;
  for (UINT32 i = 0; i < bytes - 6; i++) {
    if (buffer[i] == 0x0F && buffer[i+1] == 0x01 && buffer[i+2] == 0x15 &&
        *(UINT32 *)(buffer + i + 3) == (UINT32)(tramp_phys + GU_TRAMP_GDTR_OFF)) {
      found_lgdt = TRUE;
      break;
    }
  }
  CHECK(found_lgdt, "Must contain lgdt [TrampolinePhys + 0x0198]");

  /* jmp far 0x0008:pm16_rel_off */
  BOOLEAN found_jmp_pm16 = FALSE;
  for (UINT32 i = 0; i < bytes - 6; i++) {
    if (buffer[i] == 0xEA && buffer[i+5] == 0x08 && buffer[i+6] == 0x00) {
      found_jmp_pm16 = TRUE;
      break;
    }
  }
  CHECK(found_jmp_pm16, "Must contain jmp far 0x0008:pm16_rel_off");

  /* lidt [0x01A0] */
  BOOLEAN found_lidt = FALSE;
  for (UINT32 i = 0; i < bytes - 5; i++) {
    if (buffer[i] == 0x0F && buffer[i+1] == 0x01 && buffer[i+2] == 0x1E &&
        buffer[i+3] == 0xA0 && buffer[i+4] == 0x01) {
      found_lidt = TRUE;
      break;
    }
  }
  CHECK(found_lidt, "Must contain lidt [0x01A0]");

  /* CR0.PE clear: mov eax, cr0; and al, 0xFE; mov cr0, eax */
  BOOLEAN found_cr0_clear = FALSE;
  for (UINT32 i = 0; i < bytes - 7; i++) {
    if (buffer[i] == 0x0F && buffer[i+1] == 0x20 && buffer[i+2] == 0xC0 &&
        buffer[i+3] == 0x24 && buffer[i+4] == 0xFE &&
        buffer[i+5] == 0x0F && buffer[i+6] == 0x22 && buffer[i+7] == 0xC0) {
      found_cr0_clear = TRUE;
      break;
    }
  }
  CHECK(found_cr0_clear, "Must contain CR0.PE clear sequence (mov eax, cr0; and al, 0xFE; mov cr0, eax)");

  /* Checkpoint 2: mov word [0x0312], 0xAA02 (C7 06 12 03 02 AA) */
  BOOLEAN found_chk2 = FALSE;
  for (UINT32 i = 0; i < bytes - 5; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x06 && buffer[i+2] == 0x12 &&
        buffer[i+3] == 0x03 && buffer[i+4] == 0x02 && buffer[i+5] == 0xAA) {
      found_chk2 = TRUE;
      break;
    }
  }
  CHECK(found_chk2, "Must contain Checkpoint 2 (mov word [0x0312], 0xAA02)");

  /* Final directly patched jmp far (EA ip_lo ip_hi cs_lo cs_hi) */
  CHECK(buffer[bytes - 5] == 0xEA, "Last instruction must be jmp far (0xEA)");
  CHECK(buffer[bytes - 2] == 0x00 && buffer[bytes - 1] == 0x01, "Initial far jump target CS placeholder must be 0x0100");

  /* Explicit Sanity Check: Dynamic PMCSR discovery & Link Status table verification */
  UINT32 *tbl = (UINT32 *)(buffer + 0x0320);
  CHECK(tbl[0] == 0x80001900U, "Bridge DevID address at tbl[0]");
  CHECK(tbl[1] == 0x80001904U, "Bridge Cmd/Stat address at tbl[1]");
  CHECK(tbl[2] == 0x80001918U, "Bridge BusNum address at tbl[2]");
  CHECK(tbl[3] == 0x80001960U, "Bridge PCIe Link Status address at tbl[3] (Cap 0x10 + 0x10)");
  CHECK(tbl[4] == 0x802D0000U, "GPU DevID address at tbl[4]");
  CHECK(tbl[5] == 0x802D0004U, "GPU Cmd/Stat address at tbl[5]");
  CHECK(tbl[6] == 0x802D0010U, "GPU BAR0 address at tbl[6]");
  CHECK(tbl[7] == 0x802D0064U, "GPU PMCSR address at tbl[7] (Cap 0x01 + 0x04)");

  /* Explicit Sanity Check: Verify linear-to-real-mode segment:offset conversion for Windows HalpRMStub (0x00001000) */
  UINT32 win_stub_vec = 0x00001000U;
  UINT16 win_stub_ip = (UINT16)(win_stub_vec & 0x0FU);
  UINT16 win_stub_cs = (UINT16)(((win_stub_vec >> 4) | ((win_stub_vec >> 16) << 12)) & 0xFFFFU);
  CHECK(win_stub_ip == 0x0000, "HalpRMStub (0x00001000) offset must be 0x0000");
  CHECK(win_stub_cs == 0x0100, "HalpRMStub (0x00001000) segment must be 0x0100 (CS = vec >> 4)");
  CHECK(((UINT32)win_stub_cs << 4) + win_stub_ip == win_stub_vec, "Real mode CS:IP (0100:0000) must resolve to linear 0x00001000");

  printf("[7] 32-bit Flat PM Trampoline, Dynamic GDT Base & Mode Transitions: OK!\n");
  fflush(stdout);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== Universal S3 Persistence Resume Engine Test Suite ===\n");
  fflush(stdout);
  TestGuS3ResumeContextOffsets();
  TestGuS3ExecuteResumeUnlock_Validation();
  TestGuS3ExecuteResumeUnlock_AdaAmpere();
  TestGuS3ExecuteResumeUnlock_Turing();
  TestGuS3ResumeDispatch_Wrapper();
  TestGuS3GuidsAndOpcodes();
  TestGuAcpiFacsDiscoveryAndPatching();
  TestGuAcpiBuildTrampolineCode();

  printf("=== ALL S3 RESUME TESTS PASSED (%u checks) [OK] ===\n", g_checks);
  fflush(stdout);
  return 0;
}
