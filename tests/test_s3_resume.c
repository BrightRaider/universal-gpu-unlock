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

GU_PLATFORM Gu;
GU_ASSET GuAssets[5];
VOID GuLog(const char *Format, ...) {
  (VOID)Format;
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
  CHECK(sizeof(GU_S3_RESUME_CONTEXT) == 100, "GU_S3_RESUME_CONTEXT size must be 100 bytes");
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

  /* Mock XSDT containing pointer to FADT */
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

  /* Mock RSDP */
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER mock_rsdp;
  memset(&mock_rsdp, 0, sizeof(mock_rsdp));
  mock_rsdp.Signature = GU_ACPI_RSDP_SIGNATURE;
  mock_rsdp.Revision = 2;
  mock_rsdp.XsdtAddress = (UINT64)(uintptr_t)&mock_xsdt;

  /* Mock EFI System Table */
  EFI_CONFIGURATION_TABLE config_tables[1];
  EFI_GUID acpi20_guid = GU_EFI_ACPI_20_TABLE_GUID;
  config_tables[0].VendorGuid = acpi20_guid;
  config_tables[0].VendorTable = &mock_rsdp;

  EFI_SYSTEM_TABLE mock_st;
  memset(&mock_st, 0, sizeof(mock_st));
  mock_st.NumberOfTableEntries = 1;
  mock_st.ConfigurationTable = config_tables;

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

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.bar_base = 0xfb000000ULL;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;
  ctx.pci_cmd_port_addr = 0x80000004U;

  /* Install Shadow FACS */
  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, mock_nvs, mock_nvs_phys, mock_low_tramp, mock_low_tramp_phys, &info);
  CHECK(s == EFI_SUCCESS, "GuAcpiInstallShadowFacs must return EFI_SUCCESS");
  CHECK(info.shadow_facs_phys == mock_low_tramp_phys + 0x00100, "Shadow FACS physical address must be low_tramp + 0x100");
  CHECK(info.trampoline_phys == mock_low_tramp_phys, "Trampoline physical address must be mock_low_tramp_phys (<1MB)");

  /* Verify FADT was redirected to Shadow FACS */
  CHECK(mock_fadt.FirmwareCtrl == (UINT32)(mock_low_tramp_phys + 0x00100), "FADT FirmwareCtrl must point to Shadow FACS");
  CHECK(mock_fadt.XFirmwareCtrl == (UINT64)(mock_low_tramp_phys + 0x00100), "FADT XFirmwareCtrl must point to Shadow FACS");

  /* Verify FADT checksum was recalculated and is valid */
  sum = 0;
  p = (UINT8 *)&mock_fadt;
  for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Updated FADT checksum must be 0 mod 256");

  /* Verify Real FACS was hooked to point to Low Trampoline */
  CHECK(mock_facs.FirmwareWakingVector == (UINT32)mock_low_tramp_phys, "Real FACS FirmwareWakingVector must point to Low Trampoline");
  CHECK(mock_facs.XFirmwareWakingVector == 0, "Real FACS XFirmwareWakingVector must be cleared");

  /* Verify bit 1 (64-bit wake supported) is cleared in both tables */
  CHECK((mock_facs.Flags & 2U) == 0, "Real FACS 64-bit wake flag must be cleared");

  /* Verify Shadow FACS retains identical signature and hardware signature */
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(mock_low_tramp + 0x00100);
  CHECK(shadow_facs->Signature == GU_ACPI_FACS_SIGNATURE, "Shadow FACS signature must be 'FACS'");
  CHECK(shadow_facs->HardwareSignature == 0x12345678U, "Shadow FACS hardware signature must match original");
  CHECK(shadow_facs->FirmwareWakingVector == 0, "Shadow FACS FirmwareWakingVector initialized to 0");
  CHECK((shadow_facs->Flags & 2U) == 0, "Shadow FACS 64-bit wake flag must be cleared");

  /* Verify warm reset recovery: re-running discovery when FADT points to Shadow FACS recovers Real FACS */
  GU_ACPI_FACS_INFO info_warm;
  s = GuAcpiLocateFadtAndFacs(&mock_st, &info_warm);
  CHECK(s == EFI_SUCCESS, "Warm reset discovery must succeed");
  CHECK(info_warm.real_facs == &mock_facs, "Warm reset must recover original Real FACS table");
  CHECK(info_warm.real_facs_phys == (EFI_PHYSICAL_ADDRESS)(uintptr_t)&mock_facs, "Warm reset must recover original Real FACS address");
  printf("[6] Discovery & Shadow FACS Patching (including Warm Reset): OK!\n");
  fflush(stdout);
}

static void TestGuAcpiBuildTrampolineCode(void) {
  printf("[7] Testing S3 Resume Trampoline Machine Code Generation...\n");
  fflush(stdout);

  UINT8 buffer[4096];
  EFI_PHYSICAL_ADDRESS tramp_phys = 0x0009E000ULL;
  EFI_PHYSICAL_ADDRESS ctx_phys = 0xdc5d8000ULL;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = 0x0009E100ULL;

  UINT32 bytes = GuAcpiBuildTrampolineCode(buffer, sizeof(buffer), tramp_phys, ctx_phys, shadow_facs_phys);
  CHECK(bytes > 0, "Trampoline builder must generate non-zero bytes");
  CHECK(bytes <= 256, "Pure 16-bit real mode trampoline must fit within 256 bytes");

  /* 1. Check Preamble: cli, cld */
  CHECK(buffer[0] == 0xFA, "Preamble must start with CLI (0xFA)");
  CHECK(buffer[1] == 0xFC, "Preamble must have CLD (0xFC)");

  /* 2. Check Segment Setup & Stack Save */
  CHECK(buffer[2] == 0x8C && buffer[3] == 0xC8, "mov ax, cs");
  CHECK(buffer[4] == 0x8E && buffer[5] == 0xD8, "mov ds, ax");
  CHECK(buffer[6] == 0x89 && buffer[7] == 0x26 && buffer[8] == 0x34 && buffer[9] == 0x00, "mov [0x0034], sp");
  CHECK(buffer[10] == 0x8C && buffer[11] == 0x16 && buffer[12] == 0x36 && buffer[13] == 0x00, "mov [0x0036], ss");

  /* 3. Check Telemetry Magic: mov dword [0x004C], 0x53335F54 */
  CHECK(buffer[14] == 0x66 && buffer[15] == 0xC7 && buffer[16] == 0x06 && buffer[17] == 0x4C && buffer[18] == 0x00, "telemetry magic mov");
  CHECK(buffer[19] == 0x54 && buffer[20] == 0x5F && buffer[21] == 0x33 && buffer[22] == 0x53, "telemetry magic S3_T");

  /* 4. Check Entry Checkpoint: mov word [0x0040], 0xAA01 */
  CHECK(buffer[23] == 0xC7 && buffer[24] == 0x06 && buffer[25] == 0x40 && buffer[26] == 0x00, "entry checkpoint mov");
  CHECK(buffer[27] == 0x01 && buffer[28] == 0xAA, "entry checkpoint 0xAA01");

  /* 5. Check Vector Reads: Shadow FACS 32-bit vector at [0x010C], 64-bit at [0x0118] */
  CHECK(buffer[29] == 0xA1 && buffer[30] == 0x0C && buffer[31] == 0x01, "mov ax, [0x010C]");

  /* 6. Check ljmp cs:[0x30] (0x2E, 0xFF, 0x2E, 0x30, 0x00) */
  BOOLEAN found_ljmp_indirect = FALSE;
  for (UINT32 i = 0; i < bytes - 4; i++) {
    if (buffer[i] == 0x2E && buffer[i+1] == 0xFF && buffer[i+2] == 0x2E && buffer[i+3] == 0x30 && buffer[i+4] == 0x00) {
      found_ljmp_indirect = TRUE;
      break;
    }
  }
  CHECK(found_ljmp_indirect, "Real mode return stub must end with ljmp cs:[0x30]");
  printf("[7] Trampoline Generation: OK!\n");
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
