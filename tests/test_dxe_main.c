/* SPDX-License-Identifier: GPL-2.0-only */
/* MSVC host test for src/dxe_main.c: verifies early-BDS Plan C deferral to
 * ReadyToBoot, fallback ReadyToBoot activation, and no-S3 guard.
 * Run with tests/Run-MsvcHostTests.ps1. */
#include "../src/dxe_main.c"

int printf(const char *, ...);

GU_PLATFORM Gu;
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

/* ---- Test state & stubs ---- */
static EFI_STATUS mock_discover_status = EFI_SUCCESS;
static UINTN mock_discover_count = 1;
static EFI_STATUS mock_unlock_status = EFI_SUCCESS;
static BOOLEAN mock_s3_armed = TRUE;
static UINTN ebs_hook_calls = 0;
static UINTN shadow_apply_calls = 0;
static UINT32 mock_shadow_apply_ret = 2;
static UINTN close_event_calls = 0;
static UINTN create_ex_calls = 0;
static UINTN create_legacy_calls = 0;
static EFI_EVENT_NOTIFY last_callback = NULL;
static VOID *last_context = NULL;
static EFI_EVENT next_event_handle = (EFI_EVENT)(UINTN)0x1001;

VOID GuLog(const char *format, ...) { (void)format; }
VOID GuBootSay(const CHAR16 *text) { (void)text; }
EFI_STATUS GuBootOpen(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) { (void)image; (void)t; return EFI_SUCCESS; }
EFI_STATUS GuBootSyncLog(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuBootClose(VOID) { return EFI_SUCCESS; }
VOID GuBootHalt(VOID) {}
BOOLEAN GuCheckSafeguardFlag(VOID) { return FALSE; }
EFI_STATUS GuSetSafeguardFlag(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuClearSafeguardFlag(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuBootLoadAssets(VOID) { return EFI_SUCCESS; }
EFI_STATUS GuPrepare(VOID) { return EFI_SUCCESS; }
BOOLEAN GuUnlockMutated(VOID) { return FALSE; }
EFI_STATUS GuClearMaster(VOID) { return EFI_SUCCESS; }
VOID GuReleaseStagedVbiosEfiImage(VOID) {}
EFI_STATUS GuDisableDriverOption(EFI_SYSTEM_TABLE *t) { (void)t; return EFI_SUCCESS; }

EFI_STATUS GuDiscoverAllGpus(EFI_HANDLE image, EFI_SYSTEM_TABLE *table,
                             GU_DISCOVERED_DEVICE *devices, UINTN max_devices,
                             UINTN *out_count) {
  (void)image; (void)table;
  if (mock_discover_status != EFI_SUCCESS) { *out_count = 0; return mock_discover_status; }
  UINTN n = mock_discover_count <= max_devices ? mock_discover_count : max_devices;
  for (UINTN i = 0; i < n; i++) {
    memset(&devices[i], 0, sizeof(devices[i]));
    devices[i].handle = (EFI_HANDLE)(UINTN)(0x200 + i);
    devices[i].device_id = 0x2684;
  }
  *out_count = n;
  return EFI_SUCCESS;
}

VOID GuBindDevice(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, const GU_DISCOVERED_DEVICE *dev) {
  (void)bs; (void)image; (void)dev;
}

VOID GuLogInventory(const char *prefix, const GU_DISCOVERED_DEVICE *devices, UINTN count) {
  (void)prefix; (void)devices; (void)count;
}

EFI_STATUS GuUnlockAllGpus(EFI_SYSTEM_TABLE *t, EFI_HANDLE image,
                           const GU_DISCOVERED_DEVICE *devices, UINTN count,
                           UINT32 options, const char *log_prefix,
                           const char **stage, UINTN *unlocked, UINTN *failed) {
  (void)t; (void)image; (void)devices; (void)log_prefix;
  if (mock_unlock_status != EFI_SUCCESS) {
    *stage = "mock-fail"; *unlocked = 0; *failed = 0;
    return mock_unlock_status;
  }
  Gu.s3_registered = ((options & GU_UNLOCK_REGISTER_S3) != 0) && mock_s3_armed;
  *unlocked = count;
  *failed = 0;
  return EFI_SUCCESS;
}

EFI_STATUS GuReconnectDisplayAfterUnlock(EFI_SYSTEM_TABLE *t, EFI_HANDLE image,
                                         EFI_HANDLE pci_handle, BOOLEAN allow_fallback_gop) {
  (void)t; (void)image; (void)pci_handle; (void)allow_fallback_gop;
  return EFI_SUCCESS;
}

EFI_STATUS GuAcpiInstallEbsHook(IN EFI_SYSTEM_TABLE *SystemTable) {
  (void)SystemTable;
  ebs_hook_calls++;
  return EFI_SUCCESS;
}

UINT32 GuAcpiApplyShadowConfigTable(VOID) {
  shadow_apply_calls++;
  return mock_shadow_apply_ret;
}

static EFI_STATUS EFIAPI MockCreateEventEx(UINT32 Type, EFI_TPL NotifyTpl,
                                           EFI_EVENT_NOTIFY NotifyFunction,
                                           CONST VOID *NotifyContext,
                                           CONST EFI_GUID *EventGroup,
                                           EFI_EVENT *Event) {
  (void)Type; (void)NotifyTpl; (void)EventGroup;
  create_ex_calls++;
  last_callback = NotifyFunction;
  last_context = (VOID *)NotifyContext;
  *Event = next_event_handle;
  next_event_handle = (EFI_EVENT)((UINTN)next_event_handle + 1);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockCreateEvent(UINT32 Type, EFI_TPL NotifyTpl,
                                         EFI_EVENT_NOTIFY NotifyFunction,
                                         VOID *NotifyContext,
                                         EFI_EVENT *Event) {
  (void)Type; (void)NotifyTpl;
  create_legacy_calls++;
  last_callback = NotifyFunction;
  last_context = NotifyContext;
  *Event = next_event_handle;
  next_event_handle = (EFI_EVENT)((UINTN)next_event_handle + 1);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockCloseEvent(EFI_EVENT Event) {
  (void)Event;
  close_event_calls++;
  return EFI_SUCCESS;
}

static EFI_BOOT_SERVICES MockBs;
static EFI_SYSTEM_TABLE MockSt;

static VOID ResetTestState(BOOLEAN use_create_ex) {
  memset(&Gu, 0, sizeof(Gu));
  memset(&MockBs, 0, sizeof(MockBs));
  memset(&MockSt, 0, sizeof(MockSt));
  MockBs.CreateEventEx = use_create_ex ? MockCreateEventEx : NULL;
  MockBs.CreateEvent = MockCreateEvent;
  MockBs.CloseEvent = MockCloseEvent;
  MockSt.BootServices = &MockBs;

  mDxeImage = NULL;
  mDxeTable = NULL;
  mReadyToBootEvent = NULL;
  mPlanCReadyToBootEvent = NULL;
  mDriverExecuted = FALSE;
  mPlanCActivated = FALSE;

  mock_discover_status = EFI_SUCCESS;
  mock_discover_count = 1;
  mock_unlock_status = EFI_SUCCESS;
  mock_s3_armed = TRUE;
  ebs_hook_calls = 0;
  shadow_apply_calls = 0;
  mock_shadow_apply_ret = 2;
  close_event_calls = 0;
  create_ex_calls = 0;
  create_legacy_calls = 0;
  last_callback = NULL;
  last_context = NULL;
}

int main(void) {
  EFI_HANDLE img = (EFI_HANDLE)(UINTN)0x1234;

  /* 1. Early BDS succeeds with Plan C S3 armed -> deferred until ReadyToBoot */
  ResetTestState(TRUE);
  CHECK(AdaUnlockDriverEntryPoint(img, &MockSt) == EFI_SUCCESS);
  CHECK(mDriverExecuted == TRUE);
  CHECK(Gu.s3_registered == TRUE);
  CHECK(ebs_hook_calls == 0);
  CHECK(shadow_apply_calls == 0);
  CHECK(create_ex_calls == 1);
  CHECK(mPlanCReadyToBootEvent != NULL);
  CHECK(last_callback == AdaUnlockOnPlanCReadyToBoot);

  /* Signal ReadyToBoot -> activates EBS hook and Shadow ConfigTable once */
  last_callback(mPlanCReadyToBootEvent, last_context);
  CHECK(mPlanCReadyToBootEvent == NULL);
  CHECK(close_event_calls == 1);
  CHECK(mPlanCActivated == TRUE);
  CHECK(ebs_hook_calls == 1);
  CHECK(shadow_apply_calls == 1);

  /* Second ReadyToBoot invocation must be a safe no-op */
  AdaUnlockOnPlanCReadyToBoot(NULL, NULL);
  CHECK(ebs_hook_calls == 1);
  CHECK(shadow_apply_calls == 1);

  /* 2. Early BDS succeeds WITHOUT S3 registered -> no Plan C event registered */
  ResetTestState(TRUE);
  mock_s3_armed = FALSE;
  CHECK(AdaUnlockDriverEntryPoint(img, &MockSt) == EFI_SUCCESS);
  CHECK(mDriverExecuted == TRUE);
  CHECK(create_ex_calls == 0);
  CHECK(mPlanCReadyToBootEvent == NULL);
  CHECK(ebs_hook_calls == 0);
  CHECK(shadow_apply_calls == 0);

  /* 3. Early BDS sees 0 GPUs, ReadyToBoot fallback discovers GPU & activates Plan C immediately */
  ResetTestState(TRUE);
  mock_discover_status = EFI_NOT_FOUND;
  mock_discover_count = 0;
  CHECK(AdaUnlockDriverEntryPoint(img, &MockSt) == EFI_SUCCESS);
  CHECK(mDriverExecuted == FALSE);
  CHECK(create_ex_calls == 1);
  CHECK(mReadyToBootEvent != NULL);
  CHECK(last_callback == AdaUnlockOnReadyToBoot);
  CHECK(ebs_hook_calls == 0);

  /* Now PCI devices are ready when ReadyToBoot fires */
  mock_discover_status = EFI_SUCCESS;
  mock_discover_count = 1;
  last_callback(mReadyToBootEvent, last_context);
  CHECK(mDriverExecuted == TRUE);
  CHECK(mPlanCActivated == TRUE);
  CHECK(ebs_hook_calls == 1);
  CHECK(shadow_apply_calls == 1);

  /* 4. Legacy UEFI 1.x fallback (CreateEventEx == NULL uses CreateEvent) */
  ResetTestState(FALSE);
  CHECK(AdaUnlockDriverEntryPoint(img, &MockSt) == EFI_SUCCESS);
  CHECK(create_ex_calls == 0);
  CHECK(create_legacy_calls == 1);
  CHECK(last_callback == AdaUnlockOnPlanCReadyToBoot);
  last_callback(mPlanCReadyToBootEvent, last_context);
  CHECK(ebs_hook_calls == 1);
  CHECK(shadow_apply_calls == 1);

  if (failures) {
    printf("test_dxe_main: FAILED (%d failures)\n", failures);
    return 1;
  }
  printf("test_dxe_main: all checks passed\n");
  return 0;
}
