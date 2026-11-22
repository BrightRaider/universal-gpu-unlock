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
#include "../src/s3_resume.h"
#include "../src/port.h"
#include "../src/compat/compat.h"

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
    exit(1); \
  } \
} while(0)

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

  /* Preset Falcon CPUCTL to report halt (bit 4 = 0x10) when polled */
  g_mock_bar[0x00840100 / 4] = 0x10;

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 1;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.is_ampere = 0;
  ctx.is_turing = 0;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;
  ctx.booter_dma = 0x10000000ULL;
  ctx.booter_size = 57672;

  INTN ret = GuS3ExecuteResumeUnlock(&ctx);
  CHECK(ret == 0, "Ada/Ampere S3 unlock must return 0");
  CHECK(ctx.resume_status == 1, "ctx.resume_status must be 1 (SUCCESS)");
  CHECK(ctx.resume_count == 1, "ctx.resume_count must be 1");

  /* Verify rate-limiter selectors were written to BAR0 */
  CHECK(g_mock_bar[0x0082381c / 4] == 0x88888888U, "SS0 must be 0x88888888");
  CHECK((g_mock_bar[0x00823820 / 4] & 0x0fU) == 0x08U, "SS1 low nibble must be 0x8");

  /* Verify FBIF & Falcon DMA initiation */
  CHECK((g_mock_bar[0x00840624 / 4] & (1U << 7)) != 0, "FBIF trans enable bit must be set");
  CHECK(g_mock_bar[0x00840110 / 4] == (UINT32)(ctx.booter_dma >> 8), "Falcon DMA base must match booter >> 8");
}

static void TestGuS3ExecuteResumeUnlock_Turing(void) {
  printf("[3] Testing S3 Resume hardware unlock for Turing...\n");
  memset(g_mock_bar, 0, sizeof(g_mock_bar));

  /* Preset Falcon CPUCTL halt */
  g_mock_bar[0x00840100 / 4] = 0x10;

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 1;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.is_ampere = 0;
  ctx.is_turing = 1;
  ctx.booter_dma = 0x20000000ULL;
  ctx.booter_size = 59976;

  INTN ret = GuS3ExecuteResumeUnlock(&ctx);
  CHECK(ret == 0, "Turing S3 unlock must return 0");
  CHECK(ctx.resume_status == 1, "ctx.resume_status must be 1");
  CHECK(ctx.resume_count == 1, "ctx.resume_count must be 1");
  CHECK(g_mock_bar[0x00840110 / 4] == (UINT32)(ctx.booter_dma >> 8), "Turing Falcon DMA base must match booter >> 8");
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

int main(void) {
  printf("=== Universal S3 Persistence Resume Engine Test Suite ===\n");
  TestGuS3ExecuteResumeUnlock_Validation();
  TestGuS3ExecuteResumeUnlock_AdaAmpere();
  TestGuS3ExecuteResumeUnlock_Turing();
  TestGuS3ResumeDispatch_Wrapper();
  TestGuS3GuidsAndOpcodes();

  printf("=== ALL S3 RESUME TESTS PASSED (%u checks) [OK] ===\n", g_checks);
  return 0;
}
