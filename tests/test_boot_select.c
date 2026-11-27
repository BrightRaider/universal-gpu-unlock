/* SPDX-License-Identifier: GPL-2.0-only */
/* MSVC host test for the Windows volume selection in src/boot_io.c:
 * bootmgfw.efi ranking, BootOrder preference and device-path bounds.
 * Run with tests/Run-MsvcHostTests.ps1. */
#include "../src/boot_io.c"

int printf(const char *, ...);

GU_PLATFORM Gu;
GU_ASSET GuAssets[5];
size_t GuFormatV(char *d, size_t c, const char *f, va_list a) { (void)d; (void)c; (void)f; (void)a; return 0; }
BOOLEAN GuAssetsValid(VOID) { return TRUE; }
BOOLEAN GuIsUnlocked(VOID) { return FALSE; }
const GU_ARCH_ASSETS *GuGetArchAssets(VOID) { return NULL; }
VOID *GuAlloc(size_t n) { (void)n; return NULL; }
VOID GuFree(VOID *p) { (void)p; }
EFI_STATUS GuAcpiInstallEbsHook(EFI_SYSTEM_TABLE *SystemTable) { (void)SystemTable; return EFI_SUCCESS; }
EFI_STATUS GuAcpiUninstallEbsHook(EFI_SYSTEM_TABLE *SystemTable) { (void)SystemTable; return EFI_SUCCESS; }
UINT32 GuAcpiApplyShadowConfigTable(VOID) { return 0; }
UINT32 GuAcpiRestoreConfigTable(VOID) { return 0; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

typedef struct { const CHAR16 *name; UINT8 data[256]; UINTN size; } VAR;
static VAR Vars[8];
static int nvars;
static int weq(const CHAR16 *a, const CHAR16 *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static EFI_STATUS EFIAPI GetVar(CHAR16 *n, EFI_GUID *g, UINT32 *a, UINTN *s, VOID *d) {
  (void)g; (void)a;
  for (int i = 0; i < nvars; i++)
    if (weq(Vars[i].name, n)) {
      if (*s < Vars[i].size) return EFI_BUFFER_TOO_SMALL;
      memcpy(d, Vars[i].data, Vars[i].size);
      *s = Vars[i].size;
      return EFI_SUCCESS;
    }
  return EFI_NOT_FOUND;
}
/* [HD node with signature][End] */
static void hd(UINT8 *o, UINT8 sig) {
  HARDDRIVE_DEVICE_PATH *h = (HARDDRIVE_DEVICE_PATH *)o;
  memset(h, 0, sizeof(*h));
  o[0] = MEDIA_DEVICE_PATH; o[1] = MEDIA_HARDDRIVE_DP; o[2] = (UINT8)sizeof(*h);
  h->Signature[0] = sig; h->SignatureType = 2;
  o[sizeof(*h)] = 0x7f; o[sizeof(*h) + 1] = 0xff; o[sizeof(*h) + 2] = 4; o[sizeof(*h) + 3] = 0;
}
static void boot_order(const UINT16 *o, UINTN n) {
  VAR *v = &Vars[nvars++];
  v->name = L"BootOrder"; memcpy(v->data, o, n * 2); v->size = n * 2;
}
static void option(const CHAR16 *name, UINT8 active, UINT8 sig, UINT16 bogus_len) {
  VAR *v = &Vars[nvars++];
  UINTN list = sizeof(HARDDRIVE_DEVICE_PATH) + 4;
  UINT16 len = bogus_len ? bogus_len : (UINT16)list;
  v->name = name; memset(v->data, 0, sizeof(v->data));
  v->data[0] = active;
  v->data[4] = (UINT8)len; v->data[5] = (UINT8)(len >> 8);
  v->data[6] = 'W'; /* description "W" + NUL */
  hd(v->data + 10, sig);
  v->size = 10 + list;
}

static EFI_HANDLE mock_drv_vol = (EFI_HANDLE)(UINTN)0x7001;
static EFI_HANDLE mock_drv_img = (EFI_HANDLE)(UINTN)0x7002;
static UINT8 mock_hd_path[] = {
  MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP, sizeof(HARDDRIVE_DEVICE_PATH), 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0x7f, 0xff, 4, 0
};
typedef struct { CHAR16 name[16]; BOOLEAN is_dir; BOOLEAN alive; } MOCK_ENTRY;
static MOCK_ENTRY MockEntries[256];
static UINTN MockReadCursor;
static EFI_FILE_PROTOCOL MockRootDir;
static EFI_FILE_PROTOCOL MockUnlockDir;
static EFI_FILE_PROTOCOL MockOpenFiles[8];

static int MockNameEqNoCase(const CHAR16 *a, const CHAR16 *b) {
  while (*a && GuUpperAscii(*a) == GuUpperAscii(*b)) { a++; b++; }
  return GuUpperAscii(*a) == GuUpperAscii(*b);
}
static const CHAR16 *SkipLeadingSlash(const CHAR16 *p) {
  while (*p == L'\\') p++;
  return p;
}
static EFI_STATUS EFIAPI MockFileClose(EFI_FILE_PROTOCOL *This) { (void)This; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MockFileWrite(EFI_FILE_PROTOCOL *This, UINTN *Sz, VOID *Buf) { (void)This; (void)Sz; (void)Buf; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MockFileFlush(EFI_FILE_PROTOCOL *This) { (void)This; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MockFileSetPos(EFI_FILE_PROTOCOL *This, UINT64 Pos) { (void)This; if (Pos == 0) MockReadCursor = 0; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MockFileDelete(EFI_FILE_PROTOCOL *This) {
  UINTN idx = (UINTN)(This - MockOpenFiles);
  for (UINTN i = 0; i < 256; i++) {
    if (MockEntries[i].alive && (UINTN)MockEntries[i].name[15] == idx) {
      MockEntries[i].alive = FALSE;
      MockEntries[i].name[15] = 0;
      break;
    }
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockDirRead(EFI_FILE_PROTOCOL *This, UINTN *BufSize, VOID *Buf) {
  (void)This;
  while (MockReadCursor < 256 && !MockEntries[MockReadCursor].alive) MockReadCursor++;
  if (MockReadCursor >= 256) { *BufSize = 0; return EFI_SUCCESS; }
  MOCK_ENTRY *e = &MockEntries[MockReadCursor++];
  UINTN nchars = GuStrLen(e->name) + 1U;
  UINTN needed = OFFSET_OF(EFI_FILE_INFO, FileName) + nchars * sizeof(CHAR16);
  if (*BufSize < needed) { *BufSize = needed; return EFI_BUFFER_TOO_SMALL; }
  EFI_FILE_INFO *fi = (EFI_FILE_INFO *)Buf;
  memset(fi, 0, needed);
  fi->Size = needed;
  fi->Attribute = e->is_dir ? EFI_FILE_DIRECTORY : 0;
  GuStrCopy(fi->FileName, nchars, e->name);
  *BufSize = needed;
  return EFI_SUCCESS;
}
static const char *MockLangCfgData = NULL;
static UINTN MockLangCfgCursor = 0U;
static EFI_FILE_PROTOCOL MockLangCfgFile;

static EFI_STATUS EFIAPI MockLangCfgRead(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf) {
  (void)This;
  if (!Size || !Buf || !MockLangCfgData) return EFI_DEVICE_ERROR;
  UINTN avail = strlen(MockLangCfgData);
  UINTN rem = (MockLangCfgCursor < avail) ? (avail - MockLangCfgCursor) : 0U;
  UINTN copy = (*Size < rem) ? *Size : rem;
  if (copy > 0U) memcpy(Buf, MockLangCfgData + MockLangCfgCursor, copy);
  MockLangCfgCursor += copy;
  *Size = copy;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockDirOpen(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewH,
                                     CHAR16 *FileName, UINT64 Mode, UINT64 Attr) {
  (void)Attr;
  const CHAR16 *clean = SkipLeadingSlash(FileName);
  if (This == &MockRootDir) {
    if (MockNameEqNoCase(clean, L"GPU-UNLOCK")) { *NewH = &MockUnlockDir; MockReadCursor = 0; return EFI_SUCCESS; }
    if (MockLangCfgData && MockNameEqNoCase(clean, L"EFI\\BOOT\\lang.cfg")) {
      memset(&MockLangCfgFile, 0, sizeof(MockLangCfgFile));
      MockLangCfgFile.Read = MockLangCfgRead;
      MockLangCfgFile.Close = MockFileClose;
      MockLangCfgCursor = 0U;
      *NewH = &MockLangCfgFile;
      return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
  }
  for (UINTN i = 0; i < 256; i++) {
    if (MockEntries[i].alive && MockNameEqNoCase(MockEntries[i].name, clean)) {
      MockEntries[i].name[15] = (CHAR16)(i & 7U);
      *NewH = &MockOpenFiles[i & 7U];
      return EFI_SUCCESS;
    }
  }
  if (Mode & EFI_FILE_MODE_CREATE) {
    for (UINTN i = 0; i < 256; i++) {
      if (!MockEntries[i].alive) {
        memset(&MockEntries[i], 0, sizeof(MockEntries[i]));
        MockEntries[i].alive = TRUE;
        GuStrCopy(MockEntries[i].name, 15, clean);
        MockEntries[i].name[15] = (CHAR16)(i & 7U);
        *NewH = &MockOpenFiles[i & 7U];
        return EFI_SUCCESS;
      }
    }
    return EFI_OUT_OF_RESOURCES;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI MockOpenVol(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **R) {
  (void)This; *R = &MockRootDir; return EFI_SUCCESS;
}
static EFI_LOADED_IMAGE_PROTOCOL MockLogImg;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL MockLogFs;
static EFI_BOOT_SERVICES MockLogBs;
static EFI_SYSTEM_TABLE MockLogSt;
static EFI_STATUS EFIAPI MockLogHandleProto(EFI_HANDLE H, EFI_GUID *P, VOID **I) {
  if (H == mock_drv_img && memcmp(P, &LoadedGuid, sizeof(EFI_GUID)) == 0) { *I = &MockLogImg; return EFI_SUCCESS; }
  if (H == mock_drv_vol && memcmp(P, &PathGuid, sizeof(EFI_GUID)) == 0) { *I = mock_hd_path; return EFI_SUCCESS; }
  if (H == mock_drv_vol && memcmp(P, &FsGuid, sizeof(EFI_GUID)) == 0) { *I = &MockLogFs; return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI MockSetWatchdog(UINTN T, UINT64 C, UINTN D, CHAR16 *Data) {
  (void)T; (void)C; (void)D; (void)Data; return EFI_SUCCESS;
}
static VOID InitMockLogFs(VOID) {
  memset(MockEntries, 0, sizeof(MockEntries));
  MockReadCursor = 0;
  memset(&MockRootDir, 0, sizeof(MockRootDir));
  memset(&MockUnlockDir, 0, sizeof(MockUnlockDir));
  memset(MockOpenFiles, 0, sizeof(MockOpenFiles));
  MockRootDir.Open = MockDirOpen; MockRootDir.Close = MockFileClose;
  MockUnlockDir.Open = MockDirOpen; MockUnlockDir.Close = MockFileClose;
  MockUnlockDir.Read = MockDirRead; MockUnlockDir.SetPosition = MockFileSetPos;
  for (UINTN i = 0; i < 8; i++) {
    MockOpenFiles[i].Close = MockFileClose; MockOpenFiles[i].Delete = MockFileDelete;
    MockOpenFiles[i].Write = MockFileWrite; MockOpenFiles[i].Flush = MockFileFlush;
  }
  memset(&MockLogImg, 0, sizeof(MockLogImg)); MockLogImg.DeviceHandle = mock_drv_vol;
  memset(&MockLogFs, 0, sizeof(MockLogFs)); MockLogFs.OpenVolume = MockOpenVol;
  memset(&MockLogBs, 0, sizeof(MockLogBs));
  MockLogBs.HandleProtocol = MockLogHandleProto; MockLogBs.SetWatchdogTimer = MockSetWatchdog;
  memset(&MockLogSt, 0, sizeof(MockLogSt)); MockLogSt.BootServices = &MockLogBs;
  Table = NULL; Self = NULL; OwnVolume = NULL; Root = NULL; Log = NULL;
}
static VOID SeedRunEntry(const CHAR16 *name) {
  for (UINTN i = 0; i < 256; i++) {
    if (!MockEntries[i].alive) {
      memset(&MockEntries[i], 0, sizeof(MockEntries[i]));
      MockEntries[i].alive = TRUE;
      GuStrCopy(MockEntries[i].name, 15, name);
      return;
    }
  }
}
static UINTN CountAliveRunLogs(VOID) {
  UINTN c = 0;
  for (UINTN i = 0; i < 256; i++)
    if (MockEntries[i].alive && ParseRunLogNumber(MockEntries[i].name) > 0) c++;
  return c;
}
static BOOLEAN HasAliveEntry(const CHAR16 *name) {
  for (UINTN i = 0; i < 256; i++)
    if (MockEntries[i].alive && MockNameEqNoCase(MockEntries[i].name, name)) return TRUE;
  return FALSE;
}

static UINTN MockGopFillCalls, MockGopBufCalls, MockGopOobCalls;
static UINTN MockConOutClears, MockConOutSayCalls, MockGopKeyPos;
static CHAR16 MockGopKeys[8];
static EFI_HANDLE MockGopConHandle = (EFI_HANDLE)(UINTN)0x7009;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION MockGopInfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE MockGopMode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL MockGopProto;

static EFI_STATUS EFIAPI MockGopBlt(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation, UINTN SourceX, UINTN SourceY,
    UINTN DestinationX, UINTN DestinationY, UINTN Width, UINTN Height, UINTN Delta) {
  (void)SourceX; (void)SourceY;
  if (!This || !This->Mode || !This->Mode->Info || !BltBuffer) return EFI_INVALID_PARAMETER;
  UINT32 h = This->Mode->Info->HorizontalResolution, v = This->Mode->Info->VerticalResolution;
  if (DestinationX + Width > h || DestinationY + Height > v) MockGopOobCalls++;
  if (BltOperation == EfiBltVideoFill) MockGopFillCalls++;
  else if (BltOperation == EfiBltBufferToVideo) {
    if (Delta != Width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) MockGopOobCalls++;
    MockGopBufCalls++;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockGopHandleProto(EFI_HANDLE H, EFI_GUID *G, VOID **Out) {
  EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  if (H == MockGopConHandle && G && memcmp(G, &gop_guid, sizeof(gop_guid)) == 0) {
    *Out = &MockGopProto; return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI MockGopStall(UINTN us) { (void)us; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI MockGopReadKey(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key) {
  (void)This;
  if (MockGopKeyPos >= (sizeof(MockGopKeys) / sizeof(MockGopKeys[0])) || MockGopKeys[MockGopKeyPos] == 0) return EFI_NOT_READY;
  Key->ScanCode = 0; Key->UnicodeChar = MockGopKeys[MockGopKeyPos++];
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockGopConOutStr(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *S) {
  (void)This; (void)S; MockConOutSayCalls++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockGopConOutClear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This) {
  (void)This; MockConOutClears++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockGopConOutCursorPos(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN C, UINTN R) {
  (void)This; (void)C; (void)R; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockGopConOutEnableCursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN V) {
  (void)This; (void)V; return EFI_SUCCESS;
}

int main(void) {
  static EFI_RUNTIME_SERVICES rs;
  static EFI_SYSTEM_TABLE st;
  static UINT8 p0[64], p1[64], p2[64];
  GU_WINDOWS_CANDIDATE c[3];
  UINTN pl = sizeof(HARDDRIVE_DEVICE_PATH) + 4;
  rs.GetVariable = GetVar; st.RuntimeServices = &rs; Table = &st;
  hd(p0, 0xA0); hd(p1, 0xB0); hd(p2, 0xC0);
  c[0] = (GU_WINDOWS_CANDIDATE){(EFI_HANDLE)(UINTN)1, (VOID *)p0, pl, 1, L"generic"}; /* bootx64.efi only */
  c[1] = (GU_WINDOWS_CANDIDATE){(EFI_HANDLE)(UINTN)2, (VOID *)p1, pl, 0, L"windows-1"};
  c[2] = (GU_WINDOWS_CANDIDATE){(EFI_HANDLE)(UINTN)3, (VOID *)p2, pl, 0, L"windows-2"};

  nvars = 0;
  CHECK(PreferredWindowsCandidate(c, 3, 0) == 1); /* no BootOrder: first bootmgfw volume */
  CHECK(PreferredWindowsCandidate(c, 3, 1) == 0); /* only the generic rank */

  { UINT16 o[2] = {5, 6}; nvars = 0; boot_order(o, 2); option(L"Boot0005", 1, 0xC0, 0); option(L"Boot0006", 1, 0xB0, 0); }
  CHECK(PreferredWindowsCandidate(c, 3, 0) == 2); /* BootOrder beats enumeration order */

  { UINT16 o[2] = {5, 6}; nvars = 0; boot_order(o, 2); option(L"Boot0005", 0, 0xC0, 0); option(L"Boot0006", 1, 0xB0, 0); }
  CHECK(PreferredWindowsCandidate(c, 3, 0) == 1); /* inactive option is skipped */

  { UINT16 o[2] = {0xA, 6}; nvars = 0; boot_order(o, 2); option(L"Boot000A", 1, 0xC0, 0); option(L"Boot0006", 1, 0xB0, 0); }
  CHECK(PreferredWindowsCandidate(c, 3, 0) == 2); /* upper-case hex variable name */

  { UINT16 o[1] = {5}; nvars = 0; boot_order(o, 1); option(L"Boot0005", 1, 0xA0, 0); }
  CHECK(PreferredWindowsCandidate(c, 3, 0) == 1); /* a lower-ranked volume is never chosen */

  { UINT16 o[1] = {5}; nvars = 0; boot_order(o, 1); option(L"Boot0005", 1, 0xC0, 9000); }
  CHECK(PreferredWindowsCandidate(c, 3, 0) == 1); /* malformed list length is ignored */

  { UINT8 bad[8] = {4, 1, 2, 0, 0, 0, 0, 0}; CHECK(FindHardDrive(bad, 8) == NULL); } /* node length < 4 */
  { UINT8 cut[8] = {4, 1, 42, 0, 0, 0, 0, 0}; CHECK(FindHardDrive(cut, 8) == NULL); } /* node past limit */

  /* PE32+ EFI application validation (including ~24 KiB VeraCrypt DcsBoot.efi, refs #23) */
  {
    static UINT8 vc[24088];
    UINTN i;
    for (i = 0; i < sizeof(vc); i++) vc[i] = 0;
    vc[0] = 'M'; vc[1] = 'Z';
    vc[0x3C] = 0x80;
    vc[0x80] = 'P'; vc[0x81] = 'E';
    vc[0x84] = 0x64; vc[0x85] = 0x86; /* IMAGE_FILE_MACHINE_AMD64 (0x8664) */
    vc[0x94] = 0xF0; vc[0x95] = 0x00; /* SizeOfOptionalHeader = 240 */
    vc[0x98] = 0x0B; vc[0x99] = 0x02; /* PE32+ Magic (0x020B) */
    vc[0xDC] = 10;                    /* IMAGE_SUBSYSTEM_EFI_APPLICATION (10) */

    CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == TRUE);
    CHECK(ValidateWindowsPeImage(vc, 4096U) == TRUE);
    CHECK(ValidateWindowsPeImage(vc, 4095U) == FALSE);
    CHECK(ValidateWindowsPeImage(NULL, sizeof(vc)) == FALSE);
    CHECK(ValidateWindowsPeImage(vc, 33554433U) == FALSE);

    vc[0] = 'X'; CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE); vc[0] = 'M';
    vc[0x80] = 'X'; CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE); vc[0x80] = 'P';
    vc[0x84] = 0x4C; vc[0x85] = 0x01; CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE);
    vc[0x84] = 0x64; vc[0x85] = 0x86;
    vc[0x98] = 0x0B; vc[0x99] = 0x01; CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE);
    vc[0x98] = 0x0B; vc[0x99] = 0x02;
    vc[0xDC] = 2; CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE);
    vc[0xDC] = 10;
    vc[0x94] = 64; CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE);
    vc[0x94] = 0xF0;
    vc[0x3C] = 0x20; CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE);
    vc[0x3C] = 0xFF; vc[0x3D] = 0xFF; vc[0x3E] = 0xFF; vc[0x3F] = 0x7F;
    CHECK(ValidateWindowsPeImage(vc, sizeof(vc)) == FALSE);
  }

  /* UEFI Boot Log rotation (max 50 RUNxxxx.TXT files + wraparound 9999 -> 1) */
  {
    CHAR16 tmp[12];
    CHECK(ParseRunLogNumber(L"RUN0001.TXT") == 1U);
    CHECK(ParseRunLogNumber(L"run9999.txt") == 9999U);
    CHECK(ParseRunLogNumber(L"RUN0000.TXT") == 0U);
    CHECK(ParseRunLogNumber(L"ATTEMPT.FLG") == 0U);
    CHECK(ParseRunLogNumber(L"RUN0001.TXT2") == 0U);
    FormatRunLogName(42U, tmp);
    CHECK(MockNameEqNoCase(tmp, L"RUN0042.TXT"));

    /* 1. Empty directory -> opens RUN0001.TXT */
    InitMockLogFs();
    CHECK(GuBootOpen(mock_drv_img, &MockLogSt) == EFI_SUCCESS);
    CHECK(GuBootLogOpen() == TRUE);
    CHECK(MockNameEqNoCase(GuBootGetLogPath(), L"\\GPU-UNLOCK\\RUN0001.TXT"));
    CHECK(CountAliveRunLogs() == 1U);
    CHECK(GuBootClose() == EFI_SUCCESS);

    /* 2. 55 existing logs + ATTEMPT.FLG + README.TXT -> prunes 1..6, keeps 7..55 + creates RUN0056.TXT (total 50) */
    InitMockLogFs();
    SeedRunEntry(L"ATTEMPT.FLG");
    SeedRunEntry(L"README.TXT");
    for (UINTN n = 1; n <= 55; n++) { FormatRunLogName(n, tmp); SeedRunEntry(tmp); }
    CHECK(GuBootOpen(mock_drv_img, &MockLogSt) == EFI_SUCCESS);
    CHECK(MockNameEqNoCase(GuBootGetLogPath(), L"\\GPU-UNLOCK\\RUN0056.TXT"));
    CHECK(CountAliveRunLogs() == 50U);
    CHECK(!HasAliveEntry(L"RUN0001.TXT"));
    CHECK(!HasAliveEntry(L"RUN0006.TXT"));
    CHECK(HasAliveEntry(L"RUN0007.TXT"));
    CHECK(HasAliveEntry(L"RUN0056.TXT"));
    CHECK(HasAliveEntry(L"ATTEMPT.FLG"));
    CHECK(HasAliveEntry(L"README.TXT"));
    CHECK(GuBootClose() == EFI_SUCCESS);

    /* 3. Multi-round prune: 150 old logs -> trims down cleanly to 50 (102..151) */
    InitMockLogFs();
    for (UINTN n = 1; n <= 150; n++) { FormatRunLogName(n, tmp); SeedRunEntry(tmp); }
    CHECK(GuBootOpen(mock_drv_img, &MockLogSt) == EFI_SUCCESS);
    CHECK(MockNameEqNoCase(GuBootGetLogPath(), L"\\GPU-UNLOCK\\RUN0151.TXT"));
    CHECK(CountAliveRunLogs() == 50U);
    CHECK(!HasAliveEntry(L"RUN0101.TXT"));
    CHECK(HasAliveEntry(L"RUN0102.TXT"));
    CHECK(HasAliveEntry(L"RUN0151.TXT"));
    CHECK(GuBootClose() == EFI_SUCCESS);

    /* 4. Counter wraparound 9995..9999 + 1..48 (53 logs) -> prunes 9995..9998, creates RUN0049.TXT */
    InitMockLogFs();
    for (UINTN n = 9995; n <= 9999; n++) { FormatRunLogName(n, tmp); SeedRunEntry(tmp); }
    for (UINTN n = 1; n <= 48; n++) { FormatRunLogName(n, tmp); SeedRunEntry(tmp); }
    CHECK(GuBootOpen(mock_drv_img, &MockLogSt) == EFI_SUCCESS);
    CHECK(MockNameEqNoCase(GuBootGetLogPath(), L"\\GPU-UNLOCK\\RUN0049.TXT"));
    CHECK(CountAliveRunLogs() == 50U);
    CHECK(!HasAliveEntry(L"RUN9995.TXT"));
    CHECK(!HasAliveEntry(L"RUN9998.TXT"));
    CHECK(HasAliveEntry(L"RUN9999.TXT"));
    CHECK(HasAliveEntry(L"RUN0049.TXT"));
    CHECK(GuBootClose() == EFI_SUCCESS);
  }

  /* Scaled 2x-3x scrolling GOP console tests (menu + follow-up status + silence/unsilence) */
  {
    CHECK(GuGopConPickScale(3840U, 2160U) == 3U);
    CHECK(GuGopConPickScale(2560U, 1440U) == 2U);
    CHECK(GuGopConPickScale(1920U, 1080U) == 2U);
    CHECK(GuGopConPickScale(1280U, 720U) == 2U);
    CHECK(GuGopConPickScale(1024U, 768U) == 0U);

    static EFI_SYSTEM_TABLE gm_st;
    static EFI_BOOT_SERVICES gm_bs;
    static EFI_SIMPLE_TEXT_INPUT_PROTOCOL gm_in;
    static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL gm_out;
    memset(&gm_st, 0, sizeof(gm_st)); memset(&gm_bs, 0, sizeof(gm_bs));
    memset(&gm_in, 0, sizeof(gm_in)); memset(&gm_out, 0, sizeof(gm_out));
    memset(&MockGopInfo, 0, sizeof(MockGopInfo)); memset(&MockGopMode, 0, sizeof(MockGopMode));
    memset(&MockGopProto, 0, sizeof(MockGopProto));
    gm_bs.HandleProtocol = MockGopHandleProto; gm_bs.Stall = MockGopStall;
    gm_in.ReadKeyStroke = MockGopReadKey;
    gm_out.OutputString = MockGopConOutStr; gm_out.ClearScreen = MockGopConOutClear;
    gm_out.SetCursorPosition = MockGopConOutCursorPos; gm_out.EnableCursor = MockGopConOutEnableCursor;
    gm_st.BootServices = &gm_bs; gm_st.ConsoleOutHandle = MockGopConHandle;
    gm_st.ConOut = &gm_out; gm_st.ConIn = &gm_in;
    MockGopInfo.HorizontalResolution = 2560U; MockGopInfo.VerticalResolution = 1440U;
    MockGopMode.Info = &MockGopInfo; MockGopProto.Mode = &MockGopMode; MockGopProto.Blt = MockGopBlt;

    Table = &gm_st; Root = NULL; Silent = FALSE;
    MockGopFillCalls = 0; MockGopBufCalls = 0; MockGopOobCalls = 0;
    MockConOutClears = 0; MockConOutSayCalls = 0; MockGopKeyPos = 0;
    MockGopKeys[0] = L' '; MockGopKeys[1] = L'p'; MockGopKeys[2] = 0;
    GuBootEnableLargeConsole();
    CHAR16 picked = L'S';
    CHECK(GuBootChoose(&picked) == EFI_SUCCESS);
    CHECK(picked == L'P');
    CHECK(GopCon.scale == 2U);
    CHECK(MockGopFillCalls >= 4U);
    CHECK(MockGopBufCalls > 0U);
    CHECK(MockGopOobCalls == 0U);
    CHECK(MockConOutSayCalls == 0U);

    /* Follow-up status messages before/after Silence->Unsilence stay in GOP console */
    GuBootSay(L" [1/4] Discovery [ OK ]\r\n");
    UINTN before_buf = MockGopBufCalls;
    CHECK(before_buf > 0U);
    GuBootSilence();
    CHECK(GopCon.gop == NULL);
    GuBootUnsilence();
    CHECK(GopCon.gop == &MockGopProto);
    CHECK(MockGopBufCalls > before_buf);
    GuBootSay(L" [3/4] Handover [ OK ]\r\n");
    GuGopConClose(&GopCon, Table, TRUE);
    CHECK(MockConOutClears >= 1U);

    /* Fallback to standard ConOut when resolution is too small for 2x scaling */
    MockGopInfo.HorizontalResolution = 1024U; MockGopInfo.VerticalResolution = 768U;
    MockGopFillCalls = 0; MockGopBufCalls = 0; MockConOutSayCalls = 0; MockGopKeyPos = 0;
    MockGopKeys[0] = L'u'; MockGopKeys[1] = 0;
    GuBootEnableLargeConsole();
    CHECK(GuBootChoose(&picked) == EFI_SUCCESS);
    CHECK(picked == L'U');
    CHECK(MockGopFillCalls == 0U);
    CHECK(MockGopBufCalls == 0U);
    CHECK(MockConOutSayCalls > 0U);
    (VOID)GuBootClose();
    Table = NULL;
  }

  /* Trilingual GOP boot menu: lang.cfg preset, [L] cycle EN->ZH->DE->EN, wide CJK wrap, and CSM colors */
  {
    EFI_BOOT_SERVICES gm_bs;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL gm_in;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL gm_out;
    EFI_SYSTEM_TABLE gm_st;
    memset(&gm_bs, 0, sizeof(gm_bs)); memset(&gm_in, 0, sizeof(gm_in));
    memset(&gm_out, 0, sizeof(gm_out)); memset(&gm_st, 0, sizeof(gm_st));
    memset(&MockGopInfo, 0, sizeof(MockGopInfo)); memset(&MockGopMode, 0, sizeof(MockGopMode));
    memset(&MockGopProto, 0, sizeof(MockGopProto));
    gm_bs.HandleProtocol = MockGopHandleProto; gm_bs.Stall = MockGopStall;
    gm_in.ReadKeyStroke = MockGopReadKey;
    gm_out.OutputString = MockGopConOutStr; gm_out.ClearScreen = MockGopConOutClear;
    gm_out.SetCursorPosition = MockGopConOutCursorPos; gm_out.EnableCursor = MockGopConOutEnableCursor;
    gm_st.BootServices = &gm_bs; gm_st.ConsoleOutHandle = MockGopConHandle;
    gm_st.ConIn = &gm_in; gm_st.ConOut = &gm_out;
    MockGopInfo.HorizontalResolution = 3840U; MockGopInfo.VerticalResolution = 2160U;
    MockGopMode.Info = &MockGopInfo; MockGopProto.Mode = &MockGopMode; MockGopProto.Blt = MockGopBlt;

    InitMockLogFs();
    Root = &MockRootDir;
    Table = &gm_st;
    MenuLang = GU_LANG_EN;
    MenuLangExplicit = FALSE;
    MockLangCfgData = "zh\n";
    MockGopFillCalls = 0; MockGopBufCalls = 0; MockGopOobCalls = 0;
    MockConOutSayCalls = 0; MockGopKeyPos = 0;
    MockGopKeys[0] = L'l'; /* ZH -> DE */
    MockGopKeys[1] = L'L'; /* DE -> EN */
    MockGopKeys[2] = L'l'; /* EN -> ZH */
    MockGopKeys[3] = L' '; /* pause in ZH */
    MockGopKeys[4] = L'u'; /* select U */
    MockGopKeys[5] = 0;

    GuBootSetCsmStatus(GU_CSM_LEGACY_BIOS);
    GuBootEnableLargeConsole();
    CHAR16 picked = 0;
    CHECK(GuBootChoose(&picked) == EFI_SUCCESS);
    CHECK(picked == L'U');
    CHECK(GopCon.scale == 3U);
    CHECK(GuBootGetLanguage() == GU_LANG_ZH);
    CHECK(MockGopOobCalls == 0U);
    CHECK(MockGopBufCalls > 0U);

    /* Wrap guard: writing 75 ASCII columns + 1 wide CJK glyph wraps cleanly without overflow */
    GopCon.enabled = TRUE;
    GopCon.gop = &MockGopProto;
    GopCon.scale = 3U;
    GuGopConClear(&GopCon, Table);
    CHAR16 edge_buf[77];
    for (UINTN k = 0; k < 75U; k++) edge_buf[k] = L'A';
    edge_buf[75] = 0x89E3U; /* '解' */
    edge_buf[76] = L'\0';
    GuGopConWrite(&GopCon, edge_buf);
    CHECK(GopCon.cur_row == 1U);
    CHECK(GopCon.cur_col == 2U);
    CHECK(GopCon.rows[0][75] == L'\0');
    CHECK(GopCon.rows[1][0] == 0x89E3U);
    CHECK(GopCon.rows[1][1] == GU_GOP_WIDE_TRAIL);
    CHECK(MockGopOobCalls == 0U);

    /* Semantic CSM colors across all 3 languages */
    for (UINT32 l = 0; l < GU_LANG_COUNT; l++) {
      CHECK(GuGopPickLineColor(GuTr(l, GU_STR_CSM_OFF)) == 0x3FB950U);
      CHECK(GuGopPickLineColor(GuTr(l, GU_STR_CSM_LEGACY_BIOS)) == 0xD29922U);
      CHECK(GuGopPickLineColor(GuTr(l, GU_STR_CSM_VIDEO_ACTIVE)) == 0xF85149U);
    }

    /* Small GOP (<1280x680) must force ASCII English fallback and ignore [L] */
    (VOID)GuBootClose();
    InitMockLogFs();
    Root = &MockRootDir;
    Table = &gm_st;
    MenuLang = GU_LANG_EN;
    MenuLangExplicit = FALSE;
    MockLangCfgData = "zh\n";
    MockGopInfo.HorizontalResolution = 1024U; MockGopInfo.VerticalResolution = 768U;
    MockGopKeyPos = 0; MockGopKeys[0] = L'l'; MockGopKeys[1] = L'u'; MockGopKeys[2] = 0;
    GuBootEnableLargeConsole();
    picked = 0;
    CHECK(GuBootChoose(&picked) == EFI_SUCCESS);
    CHECK(picked == L'U');
    CHECK(GopCon.gop == NULL);
    CHECK(GuBootGetLanguage() == GU_LANG_EN);

    MockLangCfgData = NULL;
    GuBootSetCsmStatus(GU_CSM_DISABLED);
    GuBootSetLanguage(GU_LANG_EN);
    MenuLangExplicit = FALSE;
    (VOID)GuBootClose();
    Root = NULL;
    Table = NULL;
  }

  if (failures) printf("test_boot_select: %d FAILED\n", failures);
  else printf("test_boot_select: all checks passed\n");
  return failures != 0;
}
