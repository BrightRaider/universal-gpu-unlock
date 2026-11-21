/* SPDX-License-Identifier: GPL-2.0-only */
/* Repro: the second GPU of a two-GPU system must pass the preflight state check after the first GPU finished cleanly. */

#include "../src/unlock_release.c"

int printf(const char *, ...);
void *malloc(size_t);
void free(void *);

/* Stubs for symbols required by unlock_release.c */
GU_PLATFORM Gu;
GU_ASSET GuAssets[5];
BOOLEAN GuAssetsValid(VOID) { return TRUE; }
const GU_ARCH_ASSETS *GuGetArchAssets(VOID) { return NULL; }
BOOLEAN GuColdEntry(VOID) { return TRUE; }
UINT32 GuRead32(UINT32 off) { (void)off; return 0; }
VOID GuWrite32(UINT32 off, UINT32 v) { (void)off; (void)v; }
VOID GuStall(UINTN us) { (void)us; }
EFI_STATUS GuFlr(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuRestorePci(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuEnableMaster(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuClearMaster(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuS3PrepareAndRegister(EFI_SYSTEM_TABLE *t) { (void)t; return EFI_SUCCESS; }
BOOLEAN GuSafeToReturn(VOID) { return TRUE; }
BOOLEAN GuFlrAllowed(VOID) { return TRUE; }
EFI_STATUS GuBootSyncLog(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuBootClose(VOID) { return EFI_SUCCESS; }
VOID GuLog(const char *f, ...) { (void)f; }
BOOLEAN GuBootLogOpen(VOID) { return TRUE; }
BOOLEAN GuTargetConfig(const UINT32 *Config, UINTN Segment, UINTN Bus, UINTN Device, UINTN Function) {
  (void)Config; (void)Segment; (void)Bus; (void)Device; (void)Function; return TRUE;
}
int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared) {
  (void)pdev; (void)shared; return 0;
}
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared) { (void)shared; }
int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared) {
  (void)pdev; (void)shared; return 0;
}
int gpu_unlock_execute_610(struct gpu_unlock_work_610 *w) { (void)w; return 0; }
VOID GuReleaseStagedVbiosEfiImage(VOID) {}
BOOLEAN GuGetStagedVbiosEfiImage(const UINT8 **p, UINTN *b, UINT16 *m, BOOLEAN *c) { (void)p; (void)b; (void)m; (void)c; return FALSE; }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void FinishedFirstGpu(void) {
  memset(&Plan, 0, sizeof(Plan));
  memset(&Gu, 0, sizeof(Gu));
  Plan.ready = TRUE; Plan.mutated = TRUE; Plan.stage = 3; Plan.claimed = FALSE;
  Gu.quiesced = TRUE; Gu.flrs = 1; Gu.started = TRUE;
  GuHardwareVerified = TRUE;
}

int main(void) {
  FinishedFirstGpu();
#ifdef GU_HAVE_NEXT_GPU_RESET
  CHECK(GuUnlockPrepareNextGpu());
#endif
  memset(&Gu, 0, sizeof(Gu));
  EFI_STATUS s = GuUnlockPreflight(NULL, NULL, NULL);
  printf("second GPU preflight status=%#llx\n", (unsigned long long)s);
  CHECK(s != EFI_ALREADY_STARTED);

#ifdef GU_HAVE_NEXT_GPU_RESET
  FinishedFirstGpu();
  Plan.stage = 2; Plan.claimed = TRUE; Gu.owned = TRUE;
  CHECK(!GuUnlockPrepareNextGpu());
  CHECK(Plan.mutated);

  memset(&Plan, 0, sizeof(Plan));
  memset(&Gu, 0, sizeof(Gu));
  Plan.ready = TRUE;
  CHECK(GuUnlockPrepareNextGpu());
  CHECK(!Plan.ready);
#endif
  if (failures) { printf("test_multi_gpu_second_preflight: %d failure(s)\n", failures); return 1; }
  printf("test_multi_gpu_second_preflight: PASS\n");
  return 0;
}
