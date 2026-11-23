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
  CHECK(mock_facs.XFirmwareWakingVector == (UINT64)mock_low_tramp_phys, "Real FACS XFirmwareWakingVector must point to Low Trampoline (Universal Wake)");
  CHECK((*(UINT32 *)((UINT8 *)&mock_facs + 0x24) & 1U) != 0, "Real FACS OspmFlags must have bit 0 set (64-bit wake enabled)");
  CHECK((mock_facs.Flags & 2U) != 0, "Real FACS 64-bit wake flag must be set");
  CHECK(mock_facs.Version >= 2, "Real FACS Version must be >= 2");

  /* 2. Verify Plan C: Shadow FACS */
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(mock_low_tramp + 0x00200);
  CHECK(shadow_facs->Signature == GU_ACPI_FACS_SIGNATURE, "Shadow FACS signature must be 'FACS'");
  CHECK(shadow_facs->HardwareSignature == 0x12345678U, "Shadow FACS hardware signature must match original");
  CHECK(shadow_facs->FirmwareWakingVector == (UINT32)mock_low_tramp_phys, "Shadow FACS FirmwareWakingVector must point to Low Trampoline");
  CHECK(shadow_facs->XFirmwareWakingVector == (UINT64)mock_low_tramp_phys, "Shadow FACS XFirmwareWakingVector must point to Low Trampoline");
  CHECK((*(UINT32 *)((UINT8 *)shadow_facs + 0x24) & 1U) != 0, "Shadow FACS OspmFlags must have bit 0 set (64-bit wake enabled)");
  CHECK((shadow_facs->Flags & 2U) != 0, "Shadow FACS 64-bit wake flag must be set");
  CHECK(shadow_facs->Version >= 2, "Shadow FACS Version must be >= 2");

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
  printf("[7] Testing S3 Resume 64-Bit Long Mode Trampoline Generation...\n");
  fflush(stdout);

  UINT8 buffer[4096];
  EFI_PHYSICAL_ADDRESS tramp_phys = 0x0009E000ULL;
  EFI_PHYSICAL_ADDRESS real_facs_phys = 0xDC8BB000ULL;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = 0x0009E200ULL;

  UINT32 bytes = GuAcpiBuildTrampolineCode(buffer, sizeof(buffer), tramp_phys, real_facs_phys, shadow_facs_phys, 0x80001960U, 0x802D0064U);
  CHECK(bytes > 0, "Trampoline builder must generate non-zero bytes");
  CHECK(bytes <= GU_TRAMP_RM_IDTR_OFF, "Trampoline machine code must fit before RM_IDTR at +0x01B4");
  CHECK(bytes <= GU_TRAMP_GDT_OFF, "Trampoline machine code must fit before GDT at +0x01C0");

  /* 1. Check Polyglot Preamble: cli, cld, xor eax, eax, inc eax (32) / REX (64), jz rel32 */
  CHECK(buffer[0] == 0xFA, "Preamble must start with CLI (0xFA)");
  CHECK(buffer[1] == 0xFC, "Preamble must have CLD (0xFC)");
  CHECK(buffer[2] == 0x31 && buffer[3] == 0xC0, "Preamble must have xor eax, eax (31 C0)");
  CHECK(buffer[4] == 0x40, "Preamble must have inc eax (32-bit) / REX (64-bit)");
  CHECK(buffer[5] == 0x0F && buffer[6] == 0x84, "Preamble must have jz rel32 to 64-bit path (0F 84)");

  /* 2. Check 32-bit Checkpoint 1: mov dword ptr [imm32], 0x00003201 (C7 05 <addr32> 01 32 00 00) */
  CHECK(buffer[11] == 0xC7 && buffer[12] == 0x05, "mov dword ptr [imm32], imm32 opcode");
  CHECK(*(UINT32 *)(buffer + 13) == (UINT32)(tramp_phys + 0x0310), "Entry Checkpoint address [0x9E310]");
  CHECK(*(UINT32 *)(buffer + 17) == 0x00003201U, "Entry Checkpoint 32-bit value 0x3201");

  /* 3. Check 32-bit Telemetry Magic: mov dword ptr [imm32], 0x53335F54 (C7 05 <addr32> 54 5F 33 53) */
  CHECK(buffer[21] == 0xC7 && buffer[22] == 0x05, "mov dword ptr [imm32], imm32 opcode");
  CHECK(*(UINT32 *)(buffer + 23) == (UINT32)(tramp_phys + 0x031C), "Telemetry Magic address [0x9E31C]");
  CHECK(*(UINT32 *)(buffer + 27) == 0x53335F54U, "Telemetry Magic 'S3_T'");

  /* 4. Check native 32-bit I/O Opcodes (EF, ED without 0x66 prefix) */
  UINT32 num_outs = 0, num_ins = 0;
  for (UINT32 i = 0; i < bytes; i++) {
    if (buffer[i] == 0xEF) { /* out dx, eax */
      CHECK(i == 0 || buffer[i - 1] != 0x66, "out dx, eax must NOT have 0x66 prefix");
      num_outs++;
    } else if (buffer[i] == 0xED) { /* in eax, dx */
      CHECK(i == 0 || buffer[i - 1] != 0x66, "in eax, dx must NOT have 0x66 prefix");
      num_ins++;
    }
  }
  CHECK(num_outs >= 6, "Trampoline must contain at least 6 native 32-bit OUT instructions (EF)");
  CHECK(num_ins >= 2, "Trampoline must contain at least 2 native 32-bit IN instructions (ED)");

  /* 5. Check stack-free execution: decode opcodes and ensure no push (0x50-0x57) or pop (0x58-0x5F) */
  BOOLEAN found_stack_op = FALSE;
  UINT32 idx = 0;
  while (idx < bytes) {
    UINT8 op = buffer[idx];
    if (op >= 0x50 && op <= 0x5F) {
      printf("  -> Stack op byte 0x%02X found at opcode offset 0x%04X (%u)\n", op, idx, idx);
      found_stack_op = TRUE;
      break;
    }
    if (op == 0xFA || op == 0xFC || op == 0xEF || op == 0xED || op == 0xF4 || op == 0x40) {
      idx += 1;
    } else if (op == 0xA1 || op == 0xA3) {
      idx += 5; /* mov eax, [imm32] / mov [imm32], eax */
    } else if (op == 0x8B || op == 0x89) {
      if (buffer[idx + 1] == 0x05) idx += 6; /* mov reg, [rip+disp32] */
      else idx += 2; /* mov reg, [rsi] / mov [rdi], eax / mov ecx, eax */
    } else if (op == 0xC7) {
      if (buffer[idx + 1] == 0x05) idx += 10; /* mov [rip+disp32], imm32 OR mov [imm32], imm32 */
      else if (buffer[idx + 1] == 0x06) idx += 6; /* mov word [disp16], imm16 */
      else idx += 2;
    } else if (op == 0x66) {
      UINT8 next = buffer[idx + 1];
      if (next == 0xBA) idx += 4; /* mov dx, imm16 */
      else if (next == 0xB8) idx += 4; /* mov ax, imm16 */
      else if (next == 0x8B && buffer[idx + 2] == 0x05) idx += 7; /* mov ax, [rip+disp32] */
      else if (next == 0x3D) idx += 4; /* cmp ax, imm16 */
      else if (next == 0xC7 && buffer[idx + 2] == 0x05) idx += 9; /* mov word [rip+disp32], imm16 */
      else if (next == 0x31) idx += 3; /* xor reg32, reg32 */
      else if (next == 0xA1) idx += 6; /* mov ax, [imm32] */
      else idx += 2;
    } else if (op == 0xBE || op == 0xBF || op == 0xB9 || op == 0xB8) {
      idx += 5; /* mov r32, imm32 */
    } else if (op == 0x80) {
      idx += 3; /* add/sub dl, 4 */
    } else if (op == 0x83) {
      idx += 3; /* add esi/edi, 4 / and ebx, 0x0F */
    } else if (op == 0xE2 || op == 0x75 || op == 0x74) {
      idx += 2; /* loop / jne short / jz short */
    } else if (op == 0x25 || op == 0x0D) {
      idx += 5; /* and/or eax, imm32 */
    } else if (op == 0x85 || op == 0x31 || op == 0x8E || op == 0x8C || op == 0x09) {
      idx += 2; /* test/xor/mov seg/or */
    } else if (op == 0xC1) {
      idx += 3; /* shr/shl reg, imm8 */
    } else if (op == 0x0F) {
      UINT8 next = buffer[idx + 1];
      if (next == 0x84) {
        idx += 6; /* jz rel32 (0F 84 disp32) */
      } else if (next == 0x01) {
        if (buffer[idx + 2] == 0x15) idx += 7; /* lgdt [rip+disp32] / lgdt [addr32] */
        else if (buffer[idx + 2] == 0x1E) idx += 5; /* lidt [disp16] */
        else idx += 3;
      } else if (next == 0x20 || next == 0x22) {
        idx += 3; /* mov eax, crX / mov crX, eax */
      } else if (next == 0x30 || next == 0x32) {
        idx += 2; /* wrmsr / rdmsr */
      } else {
        idx += 2;
      }
    } else if (op == 0xFF) {
      if (buffer[idx + 1] == 0x2D || buffer[idx + 1] == 0x25) idx += 6; /* jmp far/near [rip+disp32] */
      else idx += 2;
    } else if (op == 0xEA) {
      /* Far jump: either 7 bytes (EA off32 sel16) or 5 bytes (EA off16 sel16) */
      if (idx + 7 <= bytes && buffer[idx + 5] == 0x18 && buffer[idx + 6] == 0x00) idx += 7;
      else idx += 5;
    } else if (op == 0x24) {
      idx += 2; /* and al, imm8 */
    } else if (op == 0xE9) {
      idx += 5; /* jmp rel32 */
    } else {
      idx += 1;
    }
  }
  CHECK(!found_stack_op, "Trampoline must be 100% stack-free (no push, no pop)");
  CHECK(idx == bytes, "Instruction stepper must decode exact trampoline size");

  /* 6. Check 5-entry GDT at +0x01C0 with dynamic Base = 0x0009E000 */
  UINT8 *gdt = buffer + GU_TRAMP_GDT_OFF;
  UINT8 expected_null[8] = { 0 };
  CHECK(memcmp(gdt, expected_null, 8) == 0, "GDT Entry 0 must be Null Descriptor");

  UINT8 expected_c32[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00 };
  CHECK(memcmp(gdt + 8, expected_c32, 8) == 0, "GDT Entry 1 must be 32-bit Compat Code Descriptor");

  UINT8 expected_d32[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00 };
  CHECK(memcmp(gdt + 16, expected_d32, 8) == 0, "GDT Entry 2 must be 32-bit Flat Data Descriptor");

  UINT8 expected_code_9e[8] = { 0xFF, 0xFF, 0x00, 0xE0, 0x09, 0x9A, 0x00, 0x00 };
  CHECK(memcmp(gdt + 24, expected_code_9e, 8) == 0, "GDT Entry 3 must be 16-bit Code Descriptor with Base=0x9E000");

  UINT8 expected_data_9e[8] = { 0xFF, 0xFF, 0x00, 0xE0, 0x09, 0x92, 0x00, 0x00 };
  CHECK(memcmp(gdt + 32, expected_data_9e, 8) == 0, "GDT Entry 4 must be 16-bit Data Descriptor with Base=0x9E000");

  /* Dynamic Base verification with alternate address 0x0008C000 */
  UINT8 buffer2[4096];
  UINT32 bytes2 = GuAcpiBuildTrampolineCode(buffer2, sizeof(buffer2), 0x0008C000ULL, real_facs_phys, shadow_facs_phys, 0x80001960U, 0x802D0064U);
  CHECK(bytes2 > 0, "Second trampoline build with 0x8C000 must succeed");
  UINT8 expected_code_8c[8] = { 0xFF, 0xFF, 0x00, 0xC0, 0x08, 0x9A, 0x00, 0x00 };
  CHECK(memcmp(buffer2 + GU_TRAMP_GDT_OFF + 24, expected_code_8c, 8) == 0, "GDT Code Descriptor must dynamically reflect Base=0x8C000");
  UINT8 expected_data_8c[8] = { 0xFF, 0xFF, 0x00, 0xC0, 0x08, 0x92, 0x00, 0x00 };
  CHECK(memcmp(buffer2 + GU_TRAMP_GDT_OFF + 32, expected_data_8c, 8) == 0, "GDT Data Descriptor must dynamically reflect Base=0x8C000");

  /* 7. Check GDTR at +0x01E8, Real-Mode IDTR at +0x01B4, and FARPTR32 at +0x01F2 */
  CHECK(*(UINT16 *)(buffer + GU_TRAMP_GDTR_OFF) == 0x0027, "GDTR Limit must be 0x0027 (39 bytes, 5 descriptors)");
  CHECK(*(UINT64 *)(buffer + GU_TRAMP_GDTR_OFF + 2) == (UINT64)(tramp_phys + GU_TRAMP_GDT_OFF), "GDTR Base must point to LowTrampoline + 0x1C0");

  CHECK(*(UINT16 *)(buffer + GU_TRAMP_RM_IDTR_OFF) == 0x03FF, "Real-Mode IDTR Limit must be 0x03FF (1023 bytes)");
  CHECK(*(UINT32 *)(buffer + GU_TRAMP_RM_IDTR_OFF + 2) == 0x00000000U, "Real-Mode IDTR Base must be 0x00000000");

  /* Locate .pm32_compat start in Section 2 (starts with mov ax, 0x0010: 66 B8 10 00 8E D8) */
  UINT32 pm32_compat_off = 0;
  for (UINT32 i = 0; i < bytes - 6; i++) {
    if (buffer[i] == 0x66 && buffer[i+1] == 0xB8 && buffer[i+2] == 0x10 && buffer[i+3] == 0x00 &&
        buffer[i+4] == 0x8E && buffer[i+5] == 0xD8) {
      pm32_compat_off = i;
      break;
    }
  }
  CHECK(pm32_compat_off > 0, "Must locate .pm32_compat start in Section 2");
  CHECK(*(UINT32 *)(buffer + GU_TRAMP_FARPTR32_OFF) == (UINT32)(tramp_phys + pm32_compat_off),
        "FARPTR32 Offset must dynamically point to .pm32_compat");
  CHECK(*(UINT16 *)(buffer + GU_TRAMP_FARPTR32_OFF + 4) == 0x0008, "FARPTR32 Selector must be 0x0008 (Compat Code)");

  /* 8. Check Section 2 RIP-Relative Displacements */
  /* Calculate Section 2 start offset from Section 0's jz near .is_64bit (0F 84 disp32 at offset 5..10) */
  UINT32 sec2_start = 11 + (UINT32)(*(INT32 *)(buffer + 7));
  CHECK(sec2_start < bytes, "Section 2 start must be within generated machine code");

  /* a) Checkpoint 1: C7 05 <disp32> 01 64 00 00 (target 0x0310) */
  BOOLEAN found_chk64 = FALSE;
  for (UINT32 i = sec2_start; i < bytes - 10; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x05 &&
        buffer[i+6] == 0x01 && buffer[i+7] == 0x64 && buffer[i+8] == 0x00 && buffer[i+9] == 0x00) {
      found_chk64 = TRUE;
      INT32 disp = *(INT32 *)(buffer + i + 2);
      UINT32 target = (i + 10) + (UINT32)disp;
      CHECK(target == 0x0310, "chk64 RIP displacement must target linear offset 0x0310 exactly");
      break;
    }
  }
  CHECK(found_chk64, "Must contain 64-bit Checkpoint 1 (0x6401)");

  /* b) Magic: C7 05 <disp32> 54 5F 33 53 (target 0x031C) */
  BOOLEAN found_magic64 = FALSE;
  for (UINT32 i = sec2_start; i < bytes - 10; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x05 &&
        buffer[i+6] == 0x54 && buffer[i+7] == 0x5F && buffer[i+8] == 0x33 && buffer[i+9] == 0x53) {
      found_magic64 = TRUE;
      INT32 disp = *(INT32 *)(buffer + i + 2);
      UINT32 target = (i + 10) + (UINT32)disp;
      CHECK(target == 0x031C, "magic64 RIP displacement must target linear offset 0x031C exactly");
      break;
    }
  }
  CHECK(found_magic64, "Must contain 64-bit Telemetry Magic 'S3_T'");

  /* c) lgdt [rip + disp32]: 0F 01 15 <disp32> (target GU_TRAMP_GDTR_OFF 0x01E8) */
  BOOLEAN found_lgdt = FALSE;
  for (UINT32 i = sec2_start; i < bytes - 7; i++) {
    if (buffer[i] == 0x0F && buffer[i+1] == 0x01 && buffer[i+2] == 0x15) {
      found_lgdt = TRUE;
      INT32 disp = *(INT32 *)(buffer + i + 3);
      UINT32 target = (i + 7) + (UINT32)disp;
      CHECK(target == GU_TRAMP_GDTR_OFF, "lgdt64 RIP displacement must target GU_TRAMP_GDTR_OFF (0x01E8) exactly");
      break;
    }
  }
  CHECK(found_lgdt, "Must contain lgdt [rip + disp32] (0F 01 15)");

  /* d) Indirect far jump: FF 2D <disp32> (target GU_TRAMP_FARPTR32_OFF 0x01F2) */
  BOOLEAN found_jmp_pm32 = FALSE;
  for (UINT32 i = sec2_start; i < bytes - 6; i++) {
    if (buffer[i] == 0xFF && buffer[i+1] == 0x2D) {
      found_jmp_pm32 = TRUE;
      INT32 disp = *(INT32 *)(buffer + i + 2);
      UINT32 target = (i + 6) + (UINT32)disp;
      CHECK(target == GU_TRAMP_FARPTR32_OFF, "far_jmp32 RIP displacement must target GU_TRAMP_FARPTR32_OFF (0x01F2) exactly");
      break;
    }
  }
  CHECK(found_jmp_pm32, "Must contain indirect far jump to 32-bit Compat Mode (FF 2D)");

  /* 9. Check Section 2 Long Mode Teardown Sequence */
  /* CR0 Paging clear: mov eax, cr0; and eax, 0x7FFFFFFF; mov cr0, eax */
  BOOLEAN found_cr0_pg_clear = FALSE;
  for (UINT32 i = 0; i < bytes - 10; i++) {
    if (buffer[i] == 0x0F && buffer[i+1] == 0x20 && buffer[i+2] == 0xC0 &&
        buffer[i+3] == 0x25 && buffer[i+4] == 0xFF && buffer[i+5] == 0xFF && buffer[i+6] == 0xFF && buffer[i+7] == 0x7F &&
        buffer[i+8] == 0x0F && buffer[i+9] == 0x22 && buffer[i+10] == 0xC0) {
      found_cr0_pg_clear = TRUE;
      break;
    }
  }
  CHECK(found_cr0_pg_clear, "Must contain CR0.PG clear sequence (and eax, 0x7FFFFFFF)");

  /* IA32_EFER MSR (0xC0000080) clear: mov ecx, 0xC0000080; rdmsr; and eax, 0xFFFFF6FF; wrmsr */
  BOOLEAN found_efer = FALSE;
  for (UINT32 i = 0; i < bytes - 14; i++) {
    if (buffer[i] == 0xB9 && buffer[i+1] == 0x80 && buffer[i+2] == 0x00 && buffer[i+3] == 0x00 && buffer[i+4] == 0xC0 &&
        buffer[i+5] == 0x0F && buffer[i+6] == 0x32 &&
        buffer[i+7] == 0x25 && buffer[i+8] == 0xFF && buffer[i+9] == 0xF6 && buffer[i+10] == 0xFF && buffer[i+11] == 0xFF &&
        buffer[i+12] == 0x0F && buffer[i+13] == 0x30) {
      found_efer = TRUE;
      break;
    }
  }
  CHECK(found_efer, "Must contain IA32_EFER clear sequence (rdmsr; and eax, 0xFFFFF6FF; wrmsr)");

  /* CR3 clear: xor eax, eax; mov cr3, eax */
  BOOLEAN found_cr3 = FALSE;
  for (UINT32 i = 0; i < bytes - 5; i++) {
    if (buffer[i] == 0x31 && buffer[i+1] == 0xC0 &&
        buffer[i+2] == 0x0F && buffer[i+3] == 0x22 && buffer[i+4] == 0xD8) {
      found_cr3 = TRUE;
      break;
    }
  }
  CHECK(found_cr3, "Must contain CR3 clear sequence (xor eax, eax; mov cr3, eax)");

  /* CR4 PAE clear: mov eax, cr4; and eax, 0xFFFFFFDF; mov cr4, eax */
  BOOLEAN found_cr4 = FALSE;
  for (UINT32 i = 0; i < bytes - 11; i++) {
    if (buffer[i] == 0x0F && buffer[i+1] == 0x20 && buffer[i+2] == 0xE0 &&
        buffer[i+3] == 0x25 && buffer[i+4] == 0xDF && buffer[i+5] == 0xFF && buffer[i+6] == 0xFF && buffer[i+7] == 0xFF &&
        buffer[i+8] == 0x0F && buffer[i+9] == 0x22 && buffer[i+10] == 0xE0) {
      found_cr4 = TRUE;
      break;
    }
  }
  CHECK(found_cr4, "Must contain CR4 PAE clear sequence (and eax, 0xFFFFFFDF)");

  /* Branch to common 32-bit path: jmp rel32 .sec1_common */
  BOOLEAN found_jmp_sec1 = FALSE;
  for (UINT32 i = 0; i < bytes - 5; i++) {
    if (buffer[i] == 0xE9) {
      INT32 disp = *(INT32 *)(buffer + i + 1);
      UINT32 target = (i + 5) + (UINT32)disp;
      if (target < bytes && buffer[target] == 0xBE && *(UINT32 *)(buffer + target + 1) == (UINT32)(tramp_phys + 0x0320)) {
        found_jmp_sec1 = TRUE;
        break;
      }
    }
  }
  CHECK(found_jmp_sec1, "Must contain jmp rel32 .sec1_common targeting common 32-bit reconnaissance path");

  /* 10. Check Section 3 & Section 4 Transitions */
  /* jmp far 0x0018:pm16_rel_off */
  BOOLEAN found_jmp_pm16 = FALSE;
  for (UINT32 i = 0; i < bytes - 6; i++) {
    if (buffer[i] == 0xEA && buffer[i+5] == 0x18 && buffer[i+6] == 0x00) {
      found_jmp_pm16 = TRUE;
      UINT32 pm16_target = *(UINT32 *)(buffer + i + 1);
      CHECK(pm16_target < bytes, "pm16 far jump target must be within machine code");
      break;
    }
  }
  CHECK(found_jmp_pm16, "Must contain jmp far 0x0018:pm16_rel_off");

  /* lidt [0x01B4]: 0F 01 1E B4 01 */
  BOOLEAN found_lidt = FALSE;
  for (UINT32 i = 0; i < bytes - 5; i++) {
    if (buffer[i] == 0x0F && buffer[i+1] == 0x01 && buffer[i+2] == 0x1E &&
        buffer[i+3] == 0xB4 && buffer[i+4] == 0x01) {
      found_lidt = TRUE;
      break;
    }
  }
  CHECK(found_lidt, "Must contain lidt [0x01B4] (0F 01 1E B4 01)");

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

  /* Far jump from Section 3 into Section 4 Real Mode: jmp far (tramp_phys >> 4):rm16_rel */
  BOOLEAN found_rm_jmp = FALSE;
  for (UINT32 i = 0; i < bytes - 5; i++) {
    if (buffer[i] == 0xEA) {
      UINT16 cs = *(UINT16 *)(buffer + i + 3);
      if (cs == (UINT16)(tramp_phys >> 4)) {
        found_rm_jmp = TRUE;
        UINT16 rm16_rel = *(UINT16 *)(buffer + i + 1);
        CHECK(rm16_rel < 0x1000, "rm16_rel offset must be < 0x1000 (relative to page base, NOT 0x9E1xx)");
        CHECK((UINT32)rm16_rel < bytes, "rm16_rel must point into real-mode code section");
        break;
      }
    }
  }
  CHECK(found_rm_jmp, "Must contain far jump to Real Mode (CS = TrampolinePhys >> 4)");

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

  printf("  -> Trampoline total machine code: %u bytes (0x%04X), fits before RM_IDTR (0x%04X)\n",
         bytes, bytes, GU_TRAMP_RM_IDTR_OFF);
  printf("  -> Real Mode IDTR at +0x%04X (6 bytes: 0x%04X..0x%04X)\n",
         GU_TRAMP_RM_IDTR_OFF, GU_TRAMP_RM_IDTR_OFF, GU_TRAMP_RM_IDTR_OFF + 5);
  printf("  -> 5-Entry GDT at +0x%04X (40 bytes: 0x%04X..0x%04X, 8-byte aligned)\n",
         GU_TRAMP_GDT_OFF, GU_TRAMP_GDT_OFF, GU_TRAMP_GDT_OFF + 39);
  printf("  -> 10-Byte GDTR64 at +0x%04X (10 bytes: 0x%04X..0x%04X)\n",
         GU_TRAMP_GDTR_OFF, GU_TRAMP_GDTR_OFF, GU_TRAMP_GDTR_OFF + 9);
  printf("  -> 6-Byte FARPTR32 at +0x%04X (6 bytes: 0x%04X..0x%04X)\n",
         GU_TRAMP_FARPTR32_OFF, GU_TRAMP_FARPTR32_OFF, GU_TRAMP_FARPTR32_OFF + 5);
  printf("  -> Shadow FACS at +0x0200 (64 bytes, 512-byte aligned)\n");

  printf("[7] 64-bit Long Mode Trampoline, 5-Entry GDT & Mode Transitions: OK!\n");
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
