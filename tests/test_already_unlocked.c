/* SPDX-License-Identifier: GPL-2.0-only */
/* Test already_unlocked detection and bypass in multi_gpu.c */
#include "../src/port.h"
#include "../src/compat/compat.h"
#include "../src/multi_gpu.h"

int printf(const char *, ...);
void *memset(void *, int, size_t);

GU_PLATFORM Gu;
GU_ASSET GuAssets[5];
static UINT32 mock_pmc = 0x192000a1;
static UINT32 mock_ss0 = 0x24144046;
static UINT32 mock_ss1 = 0x7;
static UINT32 mock_wpr2_hi = 0;
static int mock_execute_unlock_called = 0;
static int mock_s3_register_called = 0;

UINT32 GuRead32(UINT32 off) {
  if (off == 0) return mock_pmc;
  if (off == 0x1fa828) return mock_wpr2_hi;
  if (off == 0x82381c) return mock_ss0;
  if (off == 0x823820) return mock_ss1;
  return 0;
}

VOID GuWrite32(UINT32 off, UINT32 v) {
  if (off == 0x82381c) mock_ss0 = v;
  if (off == 0x823820) mock_ss1 = v;
}

VOID GuLog(const char *f, ...) { (void)f; }
VOID GuBindDevice(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, const GU_DISCOVERED_DEVICE *dev) {
  (void)bs; (void)image;
  memset(&Gu, 0, sizeof(Gu));
  if (dev) {
    Gu.bar_base = dev->bar_base;
    Gu.device_id = dev->device_id;
    Gu.is_ampere = (UINT8)dev->is_ampere;
    Gu.is_turing = (UINT8)dev->is_turing;
  }
}
const char *GuArchName(const GU_DISCOVERED_DEVICE *dev) {
  (void)dev;
  return "Ada Lovelace";
}
EFI_STATUS GuBootLoadAssets(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuPrepare(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuExecuteUnlock(EFI_SYSTEM_TABLE *t, EFI_HANDLE image, const char **stage) {
  (void)t; (void)image; (void)stage;
  mock_execute_unlock_called++;
  return EFI_SUCCESS;
}
EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *t) {
  (void)t;
  mock_s3_register_called++;
  return EFI_SUCCESS;
}
BOOLEAN GuUnlockReadyForWindows(VOID) { return TRUE; }
BOOLEAN GuUnlockMutated(VOID) { return FALSE; }
BOOLEAN GuUnlockClaimed(VOID) { return FALSE; }
BOOLEAN GuUnlockPrepareNextGpu(VOID) { return TRUE; }

BOOLEAN GuIsUnlocked(VOID) {
  if (!Gu.bar_base) return FALSE;
  if (Gu.is_turing) return FALSE;
  UINT32 pmc = GuRead32(0);
  if (Gu.fault || pmc == 0xffffffff || (pmc & 0xffff0000) == 0xbadf0000) return FALSE;
  UINT32 ss0 = GuRead32(0x82381c);
  UINT32 ss1 = GuRead32(0x823820);
  return (!Gu.fault && ss0 == 0x88888888U && (ss1 & 0xfU) == 0x8U);
}

#include "../src/multi_gpu.c"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void) {
  /* 1. Cold state: bar_base = 0 */
  memset(&Gu, 0, sizeof(Gu));
  mock_pmc = 0x192000a1;
  mock_ss0 = 0x88888888;
  mock_ss1 = 0x8;
  CHECK(!GuIsUnlocked());

  /* 2. Cold state: bar_base set, stock selectors */
  Gu.bar_base = 0xd0000000;
  mock_ss0 = 0x24144046;
  mock_ss1 = 0x7;
  CHECK(!GuIsUnlocked());

  /* 3. Turing state: returns FALSE */
  Gu.is_turing = TRUE;
  mock_ss0 = 0x88888888;
  mock_ss1 = 0x8;
  CHECK(!GuIsUnlocked());

  /* 4. Fault state */
  Gu.is_turing = FALSE;
  Gu.fault = TRUE;
  CHECK(!GuIsUnlocked());
  Gu.fault = FALSE;

  /* 5. Unlocked state on Ada/Ampere */
  mock_ss0 = 0x88888888;
  mock_ss1 = 0x8;
  CHECK(GuIsUnlocked());

  /* 6. Verify GuUnlockAllGpus bypasses GuExecuteUnlock when already_unlocked is TRUE */
  EFI_SYSTEM_TABLE mock_st;
  EFI_BOOT_SERVICES mock_bs;
  mock_st.BootServices = &mock_bs;

  GU_DISCOVERED_DEVICE dev;
  memset(&dev, 0, sizeof(dev));
  dev.device_id = 0x2684;
  dev.bar_base = 0xd0000000;

  const char *stage = "";
  UINTN unlocked = 0, failed = 0;

  Gu.already_unlocked = TRUE;
  mock_execute_unlock_called = 0;
  mock_s3_register_called = 0;

  EFI_STATUS s = GuUnlockAllGpus(&mock_st, (EFI_HANDLE)0x1, &dev, 1,
                                 GU_UNLOCK_REGISTER_S3 | GU_UNLOCK_VERIFY_WINDOWS,
                                 "TEST: ", &stage, &unlocked, &failed);

  CHECK(s == EFI_SUCCESS);
  CHECK(unlocked == 1);
  CHECK(mock_execute_unlock_called == 0); /* Must NOT call execute unlock! */
  CHECK(mock_s3_register_called == 1);   /* Must register S3! */

  if (failures == 0) {
    printf("test_already_unlocked: all checks passed\n");
    return 0;
  }
  printf("test_already_unlocked: %d check(s) failed\n", failures);
  return 1;
}
