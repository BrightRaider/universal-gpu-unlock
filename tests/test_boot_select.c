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
const GU_ARCH_ASSETS *GuGetArchAssets(VOID) { return NULL; }
VOID *GuAlloc(size_t n) { (void)n; return NULL; }
VOID GuFree(VOID *p) { (void)p; }

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

  if (failures) printf("test_boot_select: %d FAILED\n", failures);
  else printf("test_boot_select: all checks passed\n");
  return failures != 0;
}
