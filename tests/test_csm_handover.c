/* SPDX-License-Identifier: GPL-2.0-only */
/* MSVC host test for CSM (CsmVideo) detection, Option ROM extraction,
 * Layer 1 UEFI GOP bind, and Layer 2 safe headless fallback GOP.
 * Run with tests/Run-MsvcHostTests.ps1. */

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

static const UINT8 *MockStagedPayload = NULL;
static UINTN MockStagedBytes = 0;
static UINT16 MockStagedMachine = 0;
static BOOLEAN MockStagedCompressed = FALSE;

VOID GuReleaseStagedVbiosEfiImage(VOID) {
  MockStagedPayload = NULL;
  MockStagedBytes = 0;
  MockStagedMachine = 0;
  MockStagedCompressed = FALSE;
}

BOOLEAN GuGetStagedVbiosEfiImage(const UINT8 **payload, UINTN *bytes,
                                 UINT16 *machine_type, BOOLEAN *compressed) {
  if (!MockStagedPayload || !MockStagedBytes) return FALSE;
  if (payload) *payload = MockStagedPayload;
  if (bytes) *bytes = MockStagedBytes;
  if (machine_type) *machine_type = MockStagedMachine;
  if (compressed) *compressed = MockStagedCompressed;
  return TRUE;
}

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

/* Mock tracking state */
static UINTN MockConnectControllerCalls = 0;
static EFI_HANDLE MockLastConnectedHandle = NULL;
static BOOLEAN MockPciHandleConnected = FALSE;
static EFI_HANDLE MockPciHandle = (EFI_HANDLE)(UINTN)0x1000;
static EFI_HANDLE MockChildGopHandle = (EFI_HANDLE)(UINTN)0x2000;
static EFI_HANDLE MockOtherGopHandle = (EFI_HANDLE)(UINTN)0x3000;

static EFI_STATUS EFIAPI MockConnectController(
    EFI_HANDLE ControllerHandle,
    EFI_HANDLE *DriverImageHandle,
    EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath,
    BOOLEAN Recursive) {
  (void)DriverImageHandle; (void)RemainingDevicePath; (void)Recursive;
  MockConnectControllerCalls++;
  MockLastConnectedHandle = ControllerHandle;
  if (ControllerHandle == MockPciHandle) {
    MockPciHandleConnected = TRUE;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockStall(UINTN Microseconds) {
  (void)Microseconds;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockAllocatePool(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer) {
  (void)PoolType;
  if (!Buffer) return EFI_INVALID_PARAMETER;
  *Buffer = malloc(Size);
  return *Buffer ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI MockFreePool(VOID *Buffer) {
  if (Buffer) free(Buffer);
  return EFI_SUCCESS;
}

static VOID *MockReservedPages = NULL;
static EFI_STATUS EFIAPI MockAllocatePages(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory) {
  (void)Type; (void)MemoryType;
  if (!Memory) return EFI_INVALID_PARAMETER;
  MockReservedPages = malloc(Pages * 4096);
  if (!MockReservedPages) return EFI_OUT_OF_RESOURCES;
  *Memory = (EFI_PHYSICAL_ADDRESS)(UINTN)MockReservedPages;
  return EFI_SUCCESS;
}

static UINTN MockInstalledProtocols = 0;
static EFI_STATUS EFIAPI MockInstallMultipleProtocolInterfaces(EFI_HANDLE *Handle, ...) {
  if (!Handle) return EFI_INVALID_PARAMETER;
  if (!*Handle) *Handle = (EFI_HANDLE)(UINTN)0x9000;
  MockInstalledProtocols++;
  return EFI_SUCCESS;
}

/* Mock DriverBinding */
static BOOLEAN MockSupportedCalled = FALSE;
static BOOLEAN MockStartCalled = FALSE;
static EFI_STATUS EFIAPI MockDriverSupported(EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle, EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath) {
  (void)This; (void)RemainingDevicePath;
  if (ControllerHandle == MockPciHandle) {
    MockSupportedCalled = TRUE;
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI MockDriverStart(EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle, EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath) {
  (void)This; (void)RemainingDevicePath;
  if (ControllerHandle == MockPciHandle) {
    MockStartCalled = TRUE;
    return EFI_SUCCESS;
  }
  return EFI_DEVICE_ERROR;
}

static EFI_DRIVER_BINDING_PROTOCOL MockBinding = {
  MockDriverSupported,
  MockDriverStart,
  NULL,
  0x10,
  (EFI_HANDLE)(UINTN)0x5000,
  (EFI_HANDLE)(UINTN)0x5000
};

static EFI_STATUS EFIAPI MockLoadImage(
    BOOLEAN BootPolicy,
    EFI_HANDLE ParentImageHandle,
    EFI_DEVICE_PATH_PROTOCOL *DevicePath,
    VOID *SourceBuffer,
    UINTN SourceSize,
    EFI_HANDLE *ImageHandle) {
  (void)BootPolicy; (void)ParentImageHandle; (void)DevicePath; (void)SourceBuffer; (void)SourceSize;
  if (!ImageHandle) return EFI_INVALID_PARAMETER;
  *ImageHandle = (EFI_HANDLE)(UINTN)0x5000;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockStartImage(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData) {
  (void)ExitDataSize; (void)ExitData;
  if (ImageHandle == (EFI_HANDLE)(UINTN)0x5000) return EFI_SUCCESS;
  return EFI_DEVICE_ERROR;
}

/* Device Paths for GopOnTarget matching:
 * MockPciHandle: [PCI_DP][End]
 * MockChildGopHandle: [PCI_DP][CONTROLLER_DP][End] (extends MockPciHandle!)
 * MockOtherGopHandle: [ACPI_DP][End] (does not extend!)
 */
static UINT8 PciDpBytes[] = {
  HARDWARE_DEVICE_PATH, HW_PCI_DP, 0x06, 0x00, 0x00, 0x00,
  0x7f, 0xff, 0x04, 0x00
};
static UINT8 ChildGopDpBytes[] = {
  HARDWARE_DEVICE_PATH, HW_PCI_DP, 0x06, 0x00, 0x00, 0x00,
  HARDWARE_DEVICE_PATH, HW_CONTROLLER_DP, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x7f, 0xff, 0x04, 0x00
};
static UINT8 OtherGopDpBytes[] = {
  0x02, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xff, 0x04, 0x00
};

static EFI_GRAPHICS_OUTPUT_PROTOCOL MockChildGop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE MockChildGopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION MockChildGopInfo;

static EFI_STATUS EFIAPI MockHandleProtocol(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **ProtocolInterface) {
  if (!ProtocolInterface) return EFI_INVALID_PARAMETER;
  *ProtocolInterface = NULL;

  if (Guid(Protocol, &PathGuid)) {
    if (Handle == MockPciHandle) { *ProtocolInterface = PciDpBytes; return EFI_SUCCESS; }
    if (Handle == MockChildGopHandle) { *ProtocolInterface = ChildGopDpBytes; return EFI_SUCCESS; }
    if (Handle == MockOtherGopHandle) { *ProtocolInterface = OtherGopDpBytes; return EFI_SUCCESS; }
    return EFI_UNSUPPORTED;
  }

  if (Guid(Protocol, &PciGuid)) {
    if (Handle == MockPciHandle) { *ProtocolInterface = (VOID *)0x1234; return EFI_SUCCESS; }
    return EFI_UNSUPPORTED;
  }

  if (Guid(Protocol, &GopGuid)) {
    if (Handle == MockChildGopHandle) { *ProtocolInterface = &MockChildGop; return EFI_SUCCESS; }
    return EFI_UNSUPPORTED;
  }

  if (Guid(Protocol, &BindingGuid)) {
    if (Handle == (EFI_HANDLE)(UINTN)0x5000) { *ProtocolInterface = &MockBinding; return EFI_SUCCESS; }
    return EFI_UNSUPPORTED;
  }

  return EFI_UNSUPPORTED;
}

static EFI_HANDLE MockGopHandlesBuffer[2];
static UINTN MockGopCount = 0;

static EFI_STATUS EFIAPI MockLocateHandleBuffer(
    EFI_LOCATE_SEARCH_TYPE SearchType,
    EFI_GUID *Protocol,
    VOID *SearchKey,
    UINTN *NoHandles,
    EFI_HANDLE **Buffer) {
  (void)SearchType; (void)SearchKey;
  if (!NoHandles || !Buffer) return EFI_INVALID_PARAMETER;
  if (Guid(Protocol, &GopGuid)) {
    *NoHandles = MockGopCount;
    if (MockGopCount == 0) {
      *Buffer = NULL;
      return EFI_NOT_FOUND;
    }
    EFI_HANDLE *b = (EFI_HANDLE *)malloc(sizeof(EFI_HANDLE) * MockGopCount);
    for (UINTN i = 0; i < MockGopCount; i++) b[i] = MockGopHandlesBuffer[i];
    *Buffer = b;
    return EFI_SUCCESS;
  }
  if (Guid(Protocol, &BindingGuid)) {
    *NoHandles = 1;
    EFI_HANDLE *b = (EFI_HANDLE *)malloc(sizeof(EFI_HANDLE));
    b[0] = (EFI_HANDLE)(UINTN)0x5000;
    *Buffer = b;
    return EFI_SUCCESS;
  }
  *NoHandles = 0;
  *Buffer = NULL;
  return EFI_NOT_FOUND;
}

static EFI_BOOT_SERVICES MockBootServices;
static EFI_SYSTEM_TABLE MockSystemTable;

static void SetupMocks(void) {
  memset(&MockBootServices, 0, sizeof(MockBootServices));
  MockBootServices.ConnectController = MockConnectController;
  MockBootServices.Stall = MockStall;
  MockBootServices.AllocatePool = MockAllocatePool;
  MockBootServices.FreePool = MockFreePool;
  MockBootServices.AllocatePages = MockAllocatePages;
  MockBootServices.InstallMultipleProtocolInterfaces = MockInstallMultipleProtocolInterfaces;
  MockBootServices.LoadImage = MockLoadImage;
  MockBootServices.StartImage = MockStartImage;
  MockBootServices.HandleProtocol = MockHandleProtocol;
  MockBootServices.LocateHandleBuffer = MockLocateHandleBuffer;

  memset(&MockSystemTable, 0, sizeof(MockSystemTable));
  MockSystemTable.BootServices = &MockBootServices;
  MockSystemTable.ConsoleOutHandle = (EFI_HANDLE)(UINTN)0x8000;

  MockConnectControllerCalls = 0;
  MockLastConnectedHandle = NULL;
  MockPciHandleConnected = FALSE;
  MockSupportedCalled = FALSE;
  MockStartCalled = FALSE;
  MockInstalledProtocols = 0;
  FallbackHandle = NULL;
  if (MockReservedPages) { free(MockReservedPages); MockReservedPages = NULL; }

  memset(&MockChildGopInfo, 0, sizeof(MockChildGopInfo));
  MockChildGopInfo.HorizontalResolution = 1920;
  MockChildGopInfo.VerticalResolution = 1080;
  MockChildGopInfo.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  MockChildGopInfo.PixelsPerScanLine = 1920;

  memset(&MockChildGopMode, 0, sizeof(MockChildGopMode));
  MockChildGopMode.Info = &MockChildGopInfo;
  MockChildGopMode.SizeOfInfo = sizeof(MockChildGopInfo);
  MockChildGopMode.FrameBufferBase = 0xE0000000ULL;
  MockChildGopMode.FrameBufferSize = 1920 * 1080 * 4;

  memset(&MockChildGop, 0, sizeof(MockChildGop));
  MockChildGop.Mode = &MockChildGopMode;

  MockGopHandlesBuffer[0] = MockChildGopHandle;
  MockGopCount = 1;

  memset(&Plan, 0, sizeof(Plan));
  Plan.table = &MockSystemTable;
  Plan.pci = MockPciHandle;
}

/* Helper to construct a synthetic Option ROM */
static void ConstructMockRom(UINT8 *buf, UINTN total_len,
                             BOOLEAN include_legacy,
                             BOOLEAN include_efi,
                             BOOLEAN efi_compressed,
                             BOOLEAN use_npde) {
  memset(buf, 0, total_len);
  UINTN pos = 0;

  if (include_legacy) {
    /* Legacy x86 Option ROM (512 bytes) */
    buf[pos] = 0x55; buf[pos + 1] = 0xaa;
    buf[pos + 2] = 1; /* 1 unit of 512 bytes */
    buf[pos + 0x18] = 0x20; buf[pos + 0x19] = 0x00; /* PCIR at +0x20 */
    UINTN pcir = pos + 0x20;
    buf[pcir] = 'P'; buf[pcir + 1] = 'C'; buf[pcir + 2] = 'I'; buf[pcir + 3] = 'R';
    buf[pcir + 0x10] = 1; buf[pcir + 0x11] = 0; /* 1 unit = 512 bytes */
    buf[pcir + 0x14] = 0x00; /* CodeType 0x00 = PC-AT compatible */
    buf[pcir + 0x15] = include_efi ? 0x00 : 0x80; /* Not last if EFI follows */
    pos += 512;
  }

  if (include_efi) {
    /* EFI Option ROM (1024 bytes) */
    UINTN efi_pos = pos;
    buf[efi_pos] = 0x55; buf[efi_pos + 1] = 0xaa;
    buf[efi_pos + 2] = 2; /* 2 units = 1024 bytes */
    buf[efi_pos + 4] = 0xf1; buf[efi_pos + 5] = 0x0e; /* EfiSignature 0x0EF1 */
    buf[efi_pos + 8] = 0x0b; buf[efi_pos + 9] = 0x00; /* Subsystem */
    buf[efi_pos + 10] = 0x64; buf[efi_pos + 11] = 0x86; /* Machine 0x8664 */
    buf[efi_pos + 12] = efi_compressed ? 1 : 0; buf[efi_pos + 13] = 0; /* Compression */
    buf[efi_pos + 0x16] = 0x40; buf[efi_pos + 0x17] = 0x00; /* ImageHeaderOffset = +0x40 */
    buf[efi_pos + 0x18] = 0x20; buf[efi_pos + 0x19] = 0x00; /* PCIR at +0x20 */

    UINTN pcir = efi_pos + 0x20;
    buf[pcir] = 'P'; buf[pcir + 1] = 'C'; buf[pcir + 2] = 'I'; buf[pcir + 3] = 'R';
    buf[pcir + 0x10] = 2; buf[pcir + 0x11] = 0; /* ImageLength = 2 units (1024 bytes) */
    buf[pcir + 0x14] = 0x03; /* CodeType 0x03 = EFI */
    buf[pcir + 0x15] = 0x80; /* Last image */

    if (use_npde) {
      /* Place NPDE after PCIR */
      buf[pcir + 0x0a] = 0x18; buf[pcir + 0x0b] = 0; /* npde_off = +0x18 */
      UINTN npde = (pcir + 0x18 + 15) & ~(UINTN)15;
      buf[npde] = 'N'; buf[npde + 1] = 'P'; buf[npde + 2] = 'D'; buf[npde + 3] = 'E';
      buf[npde + 4] = 0x00; buf[npde + 5] = 0x01; /* rev 0x100 */
      buf[npde + 6] = 0x0c; buf[npde + 7] = 0x00; /* len 12 */
      buf[npde + 8] = 2; buf[npde + 9] = 0;       /* sub length = 2 units (1024 bytes) */
      buf[npde + 10] = 0x80; /* last */
    }

    /* Build synthetic PE image at efi_pos + 0x40 */
    UINTN pe_pos = efi_pos + 0x40;
    buf[pe_pos] = 'M'; buf[pe_pos + 1] = 'Z';
    buf[pe_pos + 0x3c] = 0x80; buf[pe_pos + 0x3d] = 0; buf[pe_pos + 0x3e] = 0; buf[pe_pos + 0x3f] = 0;
    UINTN sig_pos = pe_pos + 0x80;
    buf[sig_pos] = 'P'; buf[sig_pos + 1] = 'E'; buf[sig_pos + 2] = 0; buf[sig_pos + 3] = 0;
    buf[sig_pos + 4] = 0x64; buf[sig_pos + 5] = 0x86; /* machine 0x8664 */
  }
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

static void TestDetectCsmVideoDriver(void) {
  printf("Running TestDetectCsmVideoDriver...\n");

  /* Case 1: VGA memory attribute (0x08) -> CSM */
  CHECK(GuDetectCsmVideoDriver(EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY, FALSE, 0x6000a, 179776) == TRUE);

  /* Case 2: VGA I/O attribute (0x10) -> CSM */
  CHECK(GuDetectCsmVideoDriver(EFI_PCI_IO_ATTRIBUTE_VGA_IO, FALSE, 0x6000a, 179776) == TRUE);

  /* Case 3: Both VGA memory and IO (0x18) -> CSM */
  CHECK(GuDetectCsmVideoDriver(0x18, FALSE, 0x6000a, 179776) == TRUE);

  /* Case 4: ASRock A520M-HVS captured attributes 0x718 (IO | MEM | BM | VGA_MEM | VGA_IO) -> CSM */
  CHECK(GuDetectCsmVideoDriver(0x718, TRUE, 0x24, 16384) == TRUE);

  /* Case 5: Native UEFI with 0x300 (IO | MEM) -> Native UEFI (NOT CSM) */
  CHECK(GuDetectCsmVideoDriver(0x300, TRUE, 0x6000a, 179776) == FALSE);

  /* Case 6: Native UEFI with 0 attributes, modern driver ver 0x70009, size 161696 -> NOT CSM */
  CHECK(GuDetectCsmVideoDriver(0x0, TRUE, 0x70009, 161696) == FALSE);

  /* Case 7: Compact legacy video thunk (< 64KB) -> CSM */
  CHECK(GuDetectCsmVideoDriver(0x0, TRUE, 0x50000, 32768) == TRUE);

  /* Case 8: Compact legacy video thunk (version < 0x1000) -> CSM */
  CHECK(GuDetectCsmVideoDriver(0x0, TRUE, 0x800, 131072) == TRUE);

  /* Case 9: No NV agent with 0 attributes -> NOT CSM */
  CHECK(GuDetectCsmVideoDriver(0x0, FALSE, 0x24, 16384) == FALSE);
}

static void TestFindVbiosEfiOptionRom(void) {
  printf("Running TestFindVbiosEfiOptionRom...\n");
  UINT8 rom[4096];
  GU_VBIOS_EFI_IMAGE out;

  /* Case 1: Uncompressed EFI image following legacy image */
  ConstructMockRom(rom, sizeof(rom), TRUE, TRUE, FALSE, FALSE);
  CHECK(GuFindVbiosEfiOptionRom(rom, sizeof(rom), &out) == TRUE);
  CHECK(out.rom_offset == 512);
  CHECK(out.rom_bytes == 1024);
  CHECK(out.payload_offset == 512 + 0x40);
  CHECK(out.payload_bytes == 1024 - 0x40);
  CHECK(out.machine_type == 0x8664);
  CHECK(out.compressed == FALSE);

  /* Case 2: Compressed EFI image */
  ConstructMockRom(rom, sizeof(rom), FALSE, TRUE, TRUE, FALSE);
  CHECK(GuFindVbiosEfiOptionRom(rom, sizeof(rom), &out) == TRUE);
  CHECK(out.rom_offset == 0);
  CHECK(out.compressed == TRUE);

  /* Case 3: ROM with NPDE extension */
  ConstructMockRom(rom, sizeof(rom), FALSE, TRUE, FALSE, TRUE);
  CHECK(GuFindVbiosEfiOptionRom(rom, sizeof(rom), &out) == TRUE);
  CHECK(out.rom_offset == 0);
  CHECK(out.rom_bytes == 1024);

  /* Case 4: Legacy only (no EFI) */
  ConstructMockRom(rom, sizeof(rom), TRUE, FALSE, FALSE, FALSE);
  CHECK(GuFindVbiosEfiOptionRom(rom, sizeof(rom), &out) == FALSE);

  /* Case 5: Null / invalid length */
  CHECK(GuFindVbiosEfiOptionRom(NULL, sizeof(rom), &out) == FALSE);
  CHECK(GuFindVbiosEfiOptionRom(rom, 256, &out) == FALSE);

  /* Case 6: Truncated header bounds check */
  ConstructMockRom(rom, sizeof(rom), FALSE, TRUE, FALSE, FALSE);
  CHECK(GuFindVbiosEfiOptionRom(rom, 512, &out) == FALSE);
}

static void TestValidatePeImage(void) {
  printf("Running TestValidatePeImage...\n");
  UINT8 pe[512];
  memset(pe, 0, sizeof(pe));

  /* Invalid: all zeroes */
  CHECK(ValidatePeImage(pe, sizeof(pe)) == FALSE);

  /* Invalid: MZ but no PE */
  pe[0] = 'M'; pe[1] = 'Z';
  pe[0x3c] = 0x80;
  CHECK(ValidatePeImage(pe, sizeof(pe)) == FALSE);

  /* Valid: MZ, PE\0\0, x86_64 */
  pe[0x80] = 'P'; pe[0x81] = 'E'; pe[0x82] = 0; pe[0x83] = 0;
  pe[0x84] = 0x64; pe[0x85] = 0x86;
  CHECK(ValidatePeImage(pe, sizeof(pe)) == TRUE);

  /* Valid: EFI Byte Code (0x0EBC) */
  pe[0x84] = 0xbc; pe[0x85] = 0x0e;
  CHECK(ValidatePeImage(pe, sizeof(pe)) == TRUE);

  /* Invalid: unrecognized machine (e.g. ARM32 0x01c0) */
  pe[0x84] = 0xc0; pe[0x85] = 0x01;
  CHECK(ValidatePeImage(pe, sizeof(pe)) == FALSE);

  /* Invalid: size too small */
  CHECK(ValidatePeImage(pe, 128) == FALSE);
}

static void TestNativeUefiReconnect(void) {
  printf("Running TestNativeUefiReconnect...\n");
  SetupMocks();
  Plan.csm_video = FALSE;

  EFI_STATUS s = GuReconnectDisplayAfterUnlock(&MockSystemTable, (EFI_HANDLE)(UINTN)0x4000, MockPciHandle, TRUE);
  CHECK(s == EFI_SUCCESS);

  /* On Native UEFI: ConnectController MUST be called on MockPciHandle */
  CHECK(MockPciHandleConnected == TRUE);
  CHECK(MockConnectControllerCalls >= 1);
  CHECK(MockSupportedCalled == FALSE);
  CHECK(FallbackHandle == NULL);
}

static void TestCsmLayer1OptionRomBind(void) {
  printf("Running TestCsmLayer1OptionRomBind...\n");
  SetupMocks();
  Plan.csm_video = TRUE;

  /* Synthetic uncompressed PE image staged */
  UINT8 pe[512];
  memset(pe, 0, sizeof(pe));
  pe[0] = 'M'; pe[1] = 'Z';
  pe[0x3c] = 0x80;
  pe[0x80] = 'P'; pe[0x81] = 'E'; pe[0x82] = 0; pe[0x83] = 0;
  pe[0x84] = 0x64; pe[0x85] = 0x86;

  MockStagedPayload = pe;
  MockStagedBytes = sizeof(pe);
  MockStagedMachine = 0x8664;
  MockStagedCompressed = FALSE;

  EFI_STATUS s = GuReconnectDisplayAfterUnlock(&MockSystemTable, (EFI_HANDLE)(UINTN)0x4000, MockPciHandle, TRUE);
  CHECK(s == EFI_SUCCESS);

  /* CRITICAL: ConnectController MUST NEVER be called on MockPciHandle on CSM! */
  CHECK(MockPciHandleConnected == FALSE);

  /* Layer 1 DriverBinding Supported and Start MUST be called on MockPciHandle */
  CHECK(MockSupportedCalled == TRUE);
  CHECK(MockStartCalled == TRUE);

  /* Child GOP handle (MockChildGopHandle) connected, NOT pci_handle */
  CHECK(MockConnectControllerCalls >= 1);
  CHECK(MockLastConnectedHandle != MockPciHandle);

  /* Staged image should be released at exit */
  CHECK(MockStagedPayload == NULL);

  /* Layer 2 Fallback GOP should NOT be installed since Layer 1 gave a linear GOP */
  CHECK(FallbackHandle == NULL);
}

static void TestCsmLayer2FallbackGop(void) {
  printf("Running TestCsmLayer2FallbackGop...\n");
  SetupMocks();
  Plan.csm_video = TRUE;

  /* No staged VBIOS GOP available, and no existing GOPs */
  GuReleaseStagedVbiosEfiImage();
  MockGopCount = 0;

  EFI_STATUS s = GuReconnectDisplayAfterUnlock(&MockSystemTable, (EFI_HANDLE)(UINTN)0x4000, MockPciHandle, TRUE);
  CHECK(s == EFI_SUCCESS);

  /* CRITICAL: ConnectController MUST NEVER be called on MockPciHandle on CSM! */
  CHECK(MockPciHandleConnected == FALSE);

  /* Layer 2 Fallback GOP MUST be installed */
  CHECK(FallbackHandle != NULL);
  CHECK(MockInstalledProtocols >= 1);
  CHECK(FallbackMode.FrameBufferBase != 0);
  CHECK(FallbackMode.FrameBufferSize == 1024 * 768 * 4);
  CHECK(FallbackInfo.HorizontalResolution == 1024);
  CHECK(FallbackInfo.VerticalResolution == 768);

  /* Verify Fallback GOP QueryMode */
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *qinfo = NULL;
  UINTN qsize = 0;
  EFI_STATUS qs = FallbackGop.QueryMode(&FallbackGop, 0, &qsize, &qinfo);
  CHECK(qs == EFI_SUCCESS);
  CHECK(qinfo != NULL);
  CHECK(qinfo->HorizontalResolution == 1024);
  CHECK(qinfo->VerticalResolution == 768);
  if (qinfo) free(qinfo);

  /* Verify Fallback GOP SetMode */
  EFI_STATUS sms = FallbackGop.SetMode(&FallbackGop, 0);
  CHECK(sms == EFI_SUCCESS);
  CHECK(FallbackGop.SetMode(&FallbackGop, 1) == EFI_UNSUPPORTED);

  /* Verify Fallback GOP Blt VideoFill */
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL fill_pixel = { 0x11, 0x22, 0x33, 0x00 };
  EFI_STATUS bs = FallbackGop.Blt(&FallbackGop, &fill_pixel, EfiBltVideoFill, 0, 0, 10, 20, 100, 50, 0);
  CHECK(bs == EFI_SUCCESS);
  UINT32 *fb = (UINT32 *)(UINTN)FallbackMode.FrameBufferBase;
  UINT32 expected_raw = *(UINT32 *)&fill_pixel;
  CHECK(fb[20 * 1024 + 10] == expected_raw);
  CHECK(fb[(20 + 49) * 1024 + (10 + 99)] == expected_raw);

  /* Verify Fallback GOP Blt VideoToVideo */
  bs = FallbackGop.Blt(&FallbackGop, NULL, EfiBltVideoToVideo, 10, 20, 200, 300, 100, 50, 0);
  CHECK(bs == EFI_SUCCESS);
  CHECK(fb[300 * 1024 + 200] == expected_raw);
}

static void TestCsmHaltNoFallback(void) {
  printf("Running TestCsmHaltNoFallback...\n");
  SetupMocks();
  Plan.csm_video = TRUE;

  /* Failed transaction path: allow_fallback_gop == FALSE */
  GuReleaseStagedVbiosEfiImage();
  MockGopCount = 0;

  EFI_STATUS s = GuReconnectDisplayAfterUnlock(&MockSystemTable, (EFI_HANDLE)(UINTN)0x4000, MockPciHandle, FALSE);
  CHECK(s == EFI_SUCCESS);

  /* CRITICAL: ConnectController MUST NEVER be called on MockPciHandle on CSM! */
  CHECK(MockPciHandleConnected == FALSE);

  /* Layer 2 Fallback GOP must NOT be installed */
  CHECK(FallbackHandle == NULL);
}

int main(void) {
  TestDetectCsmVideoDriver();
  TestFindVbiosEfiOptionRom();
  TestValidatePeImage();
  TestNativeUefiReconnect();
  TestCsmLayer1OptionRomBind();
  TestCsmLayer2FallbackGop();
  TestCsmHaltNoFallback();

  if (failures != 0) {
    printf("test_csm_handover: %d failures\n", failures);
    return 1;
  }

  printf("test_csm_handover: all checks passed\n");
  return 0;
}
