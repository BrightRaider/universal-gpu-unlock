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

/* ---- Console preflight & Alive() mocks (refs #24) ---- */
static EFI_HANDLE const MOCK_SELF      = (EFI_HANDLE)(UINTN)0x1000;
static EFI_HANDLE const MOCK_PCI_H     = (EFI_HANDLE)(UINTN)0x2000;
static EFI_HANDLE const MOCK_GOP_H     = (EFI_HANDLE)(UINTN)0x2001;
static EFI_HANDLE const MOCK_NV_AGENT  = (EFI_HANDLE)(UINTN)0x3001;
static EFI_HANDLE const MOCK_GFX_AGENT = (EFI_HANDLE)(UINTN)0x3002;
static EFI_HANDLE const MOCK_OUT_H     = (EFI_HANDLE)(UINTN)0x4001;
static EFI_HANDLE const MOCK_IN_H      = (EFI_HANDLE)(UINTN)0x4002;
static EFI_HANDLE const MOCK_ERR_H     = (EFI_HANDLE)(UINTN)0x4003;

static UINT8 MockCodeRegion[8192];
static const UINT8 MockPciPath[] = { 1, 1, 6, 0, 0, 1, 0x7f, 0xff, 4, 0 };
static const UINT8 MockGopPath[] = { 1, 1, 6, 0, 0, 1, 2, 0x10, 8, 0, 0, 0, 0, 0, 0x7f, 0xff, 4, 0 };

static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION MockGopInfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE MockGopMode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL MockGopProto;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL MockTextOut;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL MockTextErr;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL MockTextIn;
static EFI_PCI_IO_PROTOCOL MockPciIo;
static EFI_DRIVER_BINDING_PROTOCOL MockNvBind;
static EFI_DRIVER_BINDING_PROTOCOL MockGfxBind;
static EFI_LOADED_IMAGE_PROTOCOL MockNvLoaded;
static EFI_LOADED_IMAGE_PROTOCOL MockGfxLoaded;
static EFI_BOOT_SERVICES MockPfBs;
static EFI_SYSTEM_TABLE MockPfSt;
static EFI_EVENT MockWaitEvent = (EFI_EVENT)(UINTN)0x9001;

static EFI_STATUS EFIAPI StubInReset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *T, BOOLEAN Ext) { (void)T; (void)Ext; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI StubInRead(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *T, EFI_INPUT_KEY *K) { (void)T; (void)K; return EFI_NOT_READY; }
static EFI_STATUS EFIAPI StubDisconnect(EFI_HANDLE C, EFI_HANDLE D, EFI_HANDLE Ch) { (void)C; (void)D; (void)Ch; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI StubOpenProto(EFI_HANDLE H, EFI_GUID *P, VOID **I, EFI_HANDLE A, EFI_HANDLE C, UINT32 At) { (void)H; (void)P; (void)I; (void)A; (void)C; (void)At; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI StubCloseProto(EFI_HANDLE H, EFI_GUID *P, EFI_HANDLE A, EFI_HANDLE C) { (void)H; (void)P; (void)A; (void)C; return EFI_SUCCESS; }
static EFI_TPL EFIAPI StubRaiseTpl(EFI_TPL T) { return T; }
static VOID EFIAPI StubRestoreTpl(EFI_TPL T) { (void)T; }
static EFI_STATUS EFIAPI StubFreePool(VOID *B) { free(B); return EFI_SUCCESS; }

static EFI_STATUS EFIAPI StubGetLocation(EFI_PCI_IO_PROTOCOL *T, UINTN *S, UINTN *B, UINTN *D, UINTN *F) {
  (void)T; *S = 0; *B = 1; *D = 0; *F = 0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StubPciRead(EFI_PCI_IO_PROTOCOL *T, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 Off, UINTN Cnt, VOID *Buf) {
  (void)T; (void)W; (void)Off;
  UINT32 *c = (UINT32 *)Buf;
  for (UINTN i = 0; i < Cnt; i++) c[i] = 0;
  if (Cnt >= 2) { c[0] = 0x250410DEU; c[1] = 0x0003U; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StubPciAttrs(EFI_PCI_IO_PROTOCOL *T, EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION Op, UINT64 A, UINT64 *Res) {
  (void)T; (void)A;
  if (Op == EfiPciIoAttributeOperationGet && Res) { *Res = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY; return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI StubBarAttrs(EFI_PCI_IO_PROTOCOL *T, UINT8 Bar, UINT64 *Sup, VOID **Res) {
  (void)T; (void)Bar; if (Sup) *Sup = 0;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)malloc(sizeof(*d));
  memset(d, 0, sizeof(*d));
  d->Desc = 0x8a; d->Len = 43; d->ResType = 0;
  d->AddrRangeMin = Gu.bar_base; d->AddrRangeMax = Gu.bar_base + 0xffffffULL; d->AddrLen = 0x1000000ULL;
  *Res = d;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StubHandleProto(EFI_HANDLE H, EFI_GUID *P, VOID **I) {
  *I = NULL;
  if (Guid(P, &GopGuid) && H == MOCK_GOP_H) { *I = &MockGopProto; return EFI_SUCCESS; }
  if (Guid(P, &PathGuid)) {
    if (H == MOCK_PCI_H) { *I = (VOID *)MockPciPath; return EFI_SUCCESS; }
    if (H == MOCK_GOP_H) { *I = (VOID *)MockGopPath; return EFI_SUCCESS; }
  }
  if (Guid(P, &PciGuid) && H == MOCK_PCI_H) { *I = &MockPciIo; return EFI_SUCCESS; }
  if (Guid(P, &BindingGuid)) {
    if (H == MOCK_NV_AGENT) { *I = &MockNvBind; return EFI_SUCCESS; }
    if (H == MOCK_GFX_AGENT) { *I = &MockGfxBind; return EFI_SUCCESS; }
  }
  if (Guid(P, &ImageGuid)) {
    if (H == MOCK_NV_AGENT) { *I = &MockNvLoaded; return EFI_SUCCESS; }
    if (H == MOCK_GFX_AGENT) { *I = &MockGfxLoaded; return EFI_SUCCESS; }
  }
  if (Guid(P, &InputGuid) && MockPfSt.ConsoleInHandle && H == MockPfSt.ConsoleInHandle && MockPfSt.ConIn) {
    *I = MockPfSt.ConIn; return EFI_SUCCESS;
  }
  if (Guid(P, &TextGuid)) {
    if (MockPfSt.ConsoleOutHandle && H == MockPfSt.ConsoleOutHandle && MockPfSt.ConOut) { *I = MockPfSt.ConOut; return EFI_SUCCESS; }
    if (MockPfSt.StandardErrorHandle && H == MockPfSt.StandardErrorHandle && MockPfSt.StdErr) { *I = MockPfSt.StdErr; return EFI_SUCCESS; }
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI StubLocateHandles(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *P, VOID *Key, UINTN *Count, EFI_HANDLE **Buf) {
  (void)Key;
  if (Type == ByProtocol && P && Guid(P, &GopGuid)) {
    EFI_HANDLE *h = (EFI_HANDLE *)malloc(sizeof(EFI_HANDLE));
    h[0] = MOCK_GOP_H; *Count = 1; *Buf = h; return EFI_SUCCESS;
  }
  if (Type == AllHandles) {
    EFI_HANDLE *h = (EFI_HANDLE *)malloc(2 * sizeof(EFI_HANDLE));
    h[0] = MOCK_PCI_H; h[1] = MOCK_GOP_H; *Count = 2; *Buf = h; return EFI_SUCCESS;
  }
  *Count = 0; *Buf = NULL; return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI StubProtoPerHandle(EFI_HANDLE H, EFI_GUID ***Buf, UINTN *Count) {
  EFI_GUID **g = (EFI_GUID **)malloc(sizeof(EFI_GUID *));
  if (H == MOCK_PCI_H) g[0] = &PciGuid;
  else if (H == MOCK_GOP_H) g[0] = &GopGuid;
  else { free(g); *Count = 0; *Buf = NULL; return EFI_NOT_FOUND; }
  *Count = 1; *Buf = g; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StubOpenInfo(EFI_HANDLE H, EFI_GUID *P, EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **Buf, UINTN *Count) {
  if ((H == MOCK_PCI_H && Guid(P, &PciGuid)) || (H == MOCK_GOP_H && Guid(P, &GopGuid))) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e = (EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *)malloc(sizeof(*e));
    memset(e, 0, sizeof(*e));
    e->AgentHandle = (H == MOCK_PCI_H) ? MOCK_NV_AGENT : MOCK_GFX_AGENT;
    e->ControllerHandle = H;
    e->Attributes = EFI_OPEN_PROTOCOL_BY_DRIVER;
    e->OpenCount = 1;
    *Count = 1; *Buf = e; return EFI_SUCCESS;
  }
  *Count = 0; *Buf = NULL; return EFI_SUCCESS;
}
static void InitConsolePreflightMocks(void) {
  memset(&Plan, 0, sizeof(Plan));
  memset(&Gu, 0, sizeof(Gu));
  memset(&MockPfBs, 0, sizeof(MockPfBs));
  memset(&MockPfSt, 0, sizeof(MockPfSt));
  memset(&MockGopInfo, 0, sizeof(MockGopInfo));
  memset(&MockGopMode, 0, sizeof(MockGopMode));
  memset(&MockGopProto, 0, sizeof(MockGopProto));
  memset(&MockTextOut, 0, sizeof(MockTextOut));
  memset(&MockTextErr, 0, sizeof(MockTextErr));
  memset(&MockTextIn, 0, sizeof(MockTextIn));
  memset(&MockPciIo, 0, sizeof(MockPciIo));
  memset(&MockNvBind, 0, sizeof(MockNvBind));
  memset(&MockGfxBind, 0, sizeof(MockGfxBind));
  memset(&MockNvLoaded, 0, sizeof(MockNvLoaded));
  memset(&MockGfxLoaded, 0, sizeof(MockGfxLoaded));

  MockGopMode.Info = &MockGopInfo;
  MockGopMode.SizeOfInfo = sizeof(MockGopInfo);
  MockGopMode.FrameBufferBase = 0xD0000000ULL;
  MockGopMode.FrameBufferSize = 0x1000000U;
  MockGopProto.Mode = &MockGopMode;

  MockTextIn.Reset = StubInReset;
  MockTextIn.ReadKeyStroke = StubInRead;
  MockTextIn.WaitForKey = MockWaitEvent;

  MockPciIo.GetLocation = StubGetLocation;
  MockPciIo.Pci.Read = StubPciRead;
  MockPciIo.Attributes = StubPciAttrs;
  MockPciIo.GetBarAttributes = StubBarAttrs;

  MockNvLoaded.ImageBase = MockCodeRegion; MockNvLoaded.ImageSize = sizeof(MockCodeRegion); MockNvLoaded.ImageCodeType = EfiBootServicesCode;
  MockGfxLoaded.ImageBase = MockCodeRegion; MockGfxLoaded.ImageSize = sizeof(MockCodeRegion); MockGfxLoaded.ImageCodeType = EfiBootServicesCode;
  MockNvBind.Stop = (EFI_DRIVER_BINDING_STOP)(VOID *)(MockCodeRegion + 64); MockNvBind.DriverBindingHandle = MOCK_NV_AGENT; MockNvBind.ImageHandle = MOCK_NV_AGENT; MockNvBind.Version = 0x60012;
  MockGfxBind.Stop = (EFI_DRIVER_BINDING_STOP)(VOID *)(MockCodeRegion + 128); MockGfxBind.DriverBindingHandle = MOCK_GFX_AGENT; MockGfxBind.ImageHandle = MOCK_GFX_AGENT; MockGfxBind.Version = 0x10;

  MockPfBs.DisconnectController = StubDisconnect;
  MockPfBs.OpenProtocol = StubOpenProto;
  MockPfBs.CloseProtocol = StubCloseProto;
  MockPfBs.RaiseTPL = StubRaiseTpl;
  MockPfBs.RestoreTPL = StubRestoreTpl;
  MockPfBs.ProtocolsPerHandle = StubProtoPerHandle;
  MockPfBs.OpenProtocolInformation = StubOpenInfo;
  MockPfBs.LocateHandleBuffer = StubLocateHandles;
  MockPfBs.HandleProtocol = StubHandleProto;
  MockPfBs.FreePool = StubFreePool;
  MockPfSt.BootServices = &MockPfBs;

  Gu.handle = MOCK_PCI_H; Gu.image = MOCK_SELF; Gu.bs = &MockPfBs; Gu.pci = &MockPciIo;
  Gu.bar_base = 0xA0000000ULL; Gu.profile_verified = TRUE; Gu.prom_checked = TRUE;
}

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

  /* Early BDS DXE without ConIn/StdErr (Fast Boot / Gigabyte B760M D2H, refs #24) */
  InitConsolePreflightMocks();
  MockPfSt.ConsoleOutHandle = MOCK_OUT_H; MockPfSt.ConOut = &MockTextOut;
  CHECK(GuUnlockPreflight(&MockPfSt, MOCK_SELF, MOCK_PCI_H) == EFI_SUCCESS);
  CHECK(Plan.ready == TRUE);
  CHECK(Alive() == EFI_SUCCESS);

  /* Early BDS with completely unpopulated console handles (all NULL) */
  InitConsolePreflightMocks();
  CHECK(GuUnlockPreflight(&MockPfSt, MOCK_SELF, MOCK_PCI_H) == EFI_SUCCESS);
  CHECK(Plan.ready == TRUE);
  CHECK(Alive() == EFI_SUCCESS);

  /* Split ConOut != ConIn != StdErr handles (distinct ConSplitter / Serial handles) */
  InitConsolePreflightMocks();
  MockPfSt.ConsoleOutHandle = MOCK_OUT_H; MockPfSt.ConOut = &MockTextOut;
  MockPfSt.ConsoleInHandle = MOCK_IN_H; MockPfSt.ConIn = &MockTextIn;
  MockPfSt.StandardErrorHandle = MOCK_ERR_H; MockPfSt.StdErr = &MockTextErr;
  CHECK(GuUnlockPreflight(&MockPfSt, MOCK_SELF, MOCK_PCI_H) == EFI_SUCCESS);
  CHECK(Alive() == EFI_SUCCESS);

  /* Invalid ConIn vtable still rejected explicitly */
  InitConsolePreflightMocks();
  MockTextIn.ReadKeyStroke = NULL;
  MockPfSt.ConsoleInHandle = MOCK_IN_H; MockPfSt.ConIn = &MockTextIn;
  CHECK(GuUnlockPreflight(&MockPfSt, MOCK_SELF, MOCK_PCI_H) == EFI_UNSUPPORTED);

  /* ConsoleOutHandle aliasing target physical GOP without ConSplitter defers safely */
  InitConsolePreflightMocks();
  MockPfSt.ConsoleOutHandle = MOCK_GOP_H; MockPfSt.ConOut = &MockTextOut;
  CHECK(GuUnlockPreflight(&MockPfSt, MOCK_SELF, MOCK_PCI_H) == EFI_NOT_READY);
  CHECK(Plan.ready == FALSE);

  if (failures) { printf("test_multi_gpu_second_preflight: %d failure(s)\n", failures); return 1; }
  printf("test_multi_gpu_second_preflight: PASS\n");
  return 0;
}
