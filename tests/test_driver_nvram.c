/* SPDX-License-Identifier: GPL-2.0-only */
/* MSVC host test for src/driver_nvram.c: the [I] and [R] boot-menu options
 * against a mock NVRAM, a mock USB volume and a mock internal ESP.
 * Run with tests/Run-MsvcHostTests.ps1. */
#include "../src/driver_nvram.c"

int printf(const char *, ...);
void *malloc(size_t);
void free(void *);

VOID GuLog(const char *format, ...) { (void)format; }
static EFI_HANDLE TargetEsp;
EFI_HANDLE GuBootGetTargetEspVolume(VOID) { return TargetEsp; }
const GU_ARCH_ASSETS *GuGetArchAssets(VOID) { return NULL; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

/* ---- Mock NVRAM ---- */
typedef struct { CHAR16 name[32]; UINT8 data[2048]; UINTN size; int used; } VAR;
static VAR Vars[300];
static int writes;
static EFI_STATUS order_err;

static int weq(const CHAR16 *a, const CHAR16 *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void wcopy(CHAR16 *d, const CHAR16 *s, UINTN max) {
  UINTN i = 0;
  while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
  d[i] = 0;
}
static VAR *find(const CHAR16 *n) {
  for (int i = 0; i < 300; i++) if (Vars[i].used && weq(Vars[i].name, n)) return &Vars[i];
  return NULL;
}
static void put(const CHAR16 *n, const void *d, UINTN s) {
  VAR *v = find(n);
  for (int i = 0; i < 300 && !v; i++) if (!Vars[i].used) v = &Vars[i];
  wcopy(v->name, n, 32);
  memcpy(v->data, d, s);
  v->size = s;
  v->used = 1;
}
static EFI_STATUS EFIAPI GetVar(CHAR16 *n, EFI_GUID *g, UINT32 *a, UINTN *s, VOID *d) {
  (void)g; (void)a;
  if (order_err && weq(n, L"DriverOrder")) return order_err;
  VAR *v = find(n);
  if (!v) return EFI_NOT_FOUND;
  if (*s < v->size) { *s = v->size; return EFI_BUFFER_TOO_SMALL; }
  memcpy(d, v->data, v->size);
  *s = v->size;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetVar(CHAR16 *n, EFI_GUID *g, UINT32 a, UINTN s, VOID *d) {
  (void)g; (void)a;
  writes++;
  if (!s) { VAR *v = find(n); if (v) v->used = 0; return EFI_SUCCESS; }
  put(n, d, s);
  return EFI_SUCCESS;
}

/* ---- Mock volumes: USB stick (handle 2) and internal ESP (handle 3) ---- */
#define FILE_BYTES 4096U
#define USB_HANDLE ((EFI_HANDLE)(UINTN)2)
#define ESP_HANDLE ((EFI_HANDLE)(UINTN)3)
typedef struct VOL VOL;
typedef struct { EFI_FILE_PROTOCOL p; VOL *v; } MFILE;
struct VOL {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs; /* must stay first */
  UINT8 path[64];
  const CHAR16 *files[2];
  int has_bootmgfw, fail_write, short_write, deleted;
  UINTN written;
  CHAR16 written_name[64];
  MFILE root, dir, rfile, wfile;
};
static VOL Usb, Esp;
static int esp_present;

static EFI_STATUS EFIAPI FOpen(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr) {
  VOL *v = ((MFILE *)This)->v;
  if (Attr & EFI_FILE_DIRECTORY) { *New = &v->dir.p; return EFI_SUCCESS; }
  if (Mode & EFI_FILE_MODE_CREATE) { wcopy(v->written_name, Name, 64); *New = &v->wfile.p; return EFI_SUCCESS; }
  if (v->has_bootmgfw && weq(Name, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi")) { *New = &v->rfile.p; return EFI_SUCCESS; }
  for (int i = 0; i < 2; i++)
    if (v->files[i] && weq(Name, v->files[i])) { *New = &v->rfile.p; return EFI_SUCCESS; }
  /* An existing copy, opened by full path ([R]) or relative to \EFI\Drivers (copy). */
  if (v->written && (weq(Name, L"\\EFI\\Drivers\\AdaUnlockDxe.efi") || weq(Name, L"AdaUnlockDxe.efi"))) {
    *New = &v->wfile.p;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI FClose(EFI_FILE_PROTOCOL *This) { (void)This; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI FDelete(EFI_FILE_PROTOCOL *This) {
  VOL *v = ((MFILE *)This)->v;
  v->deleted++;
  v->written = 0;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FRead(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer) {
  (void)This;
  memset(Buffer, 0xab, *Size);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FWrite(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer) {
  VOL *v = ((MFILE *)This)->v;
  (void)Buffer;
  if (v->fail_write) return EFI_DEVICE_ERROR;
  if (v->short_write) *Size /= 2;
  v->written = *Size;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FGetInfo(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN *Size, VOID *Buffer) {
  EFI_FILE_INFO *info = Buffer;
  (void)This; (void)Type;
  if (*Size < sizeof(*info)) { *Size = sizeof(*info); return EFI_BUFFER_TOO_SMALL; }
  memset(info, 0, sizeof(*info));
  info->Size = sizeof(*info);
  info->FileSize = FILE_BYTES;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenVol(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs, EFI_FILE_PROTOCOL **Root) {
  *Root = &((VOL *)Fs)->root.p;
  return EFI_SUCCESS;
}
/* [PCI or USB node][HD node with signature][End] */
static void mkpath(UINT8 *p, int usb, UINT8 sig) {
  memset(p, 0, 64);
  p[0] = usb ? 3 : 1; p[1] = usb ? 5 : 1; p[2] = 6;
  p[6] = 4; p[7] = 1; p[8] = 42; p[6 + 24] = sig;
  p[48] = 0x7f; p[49] = 0xff; p[50] = 4;
}
static void mkfile(MFILE *f, VOL *v) {
  memset(f, 0, sizeof(*f));
  f->v = v;
  f->p.Revision = EFI_FILE_PROTOCOL_REVISION;
  f->p.Open = FOpen; f->p.Close = FClose; f->p.Delete = FDelete;
  f->p.Read = FRead; f->p.Write = FWrite; f->p.GetInfo = FGetInfo;
}
static void mkvol(VOL *v, int usb, UINT8 sig) {
  memset(v, 0, sizeof(*v));
  v->fs.OpenVolume = OpenVol;
  mkpath(v->path, usb, sig);
  mkfile(&v->root, v); mkfile(&v->dir, v); mkfile(&v->rfile, v); mkfile(&v->wfile, v);
}

/* ---- Mock boot services ---- */
static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h, EFI_GUID *g, VOID **o) {
  VOL *v = h == USB_HANDLE ? &Usb : h == ESP_HANDLE ? &Esp : NULL;
  if (!memcmp(g, &LoadedGuid, sizeof(*g))) { *o = &Loaded; return EFI_SUCCESS; }
  if (!v) return EFI_UNSUPPORTED;
  if (!memcmp(g, &PathGuid, sizeof(*g))) { *o = v->path; return EFI_SUCCESS; }
  if (!memcmp(g, &FsGuid, sizeof(*g))) { *o = &v->fs; return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE t, EFI_GUID *g, VOID *k, UINTN *n, EFI_HANDLE **b) {
  (void)t; (void)g; (void)k;
  *b = malloc(2 * sizeof(EFI_HANDLE));
  (*b)[0] = USB_HANDLE;
  *n = 1;
  if (esp_present) (*b)[(*n)++] = ESP_HANDLE;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Alloc(EFI_MEMORY_TYPE t, UINTN s, VOID **o) { (void)t; *o = malloc(s); return *o ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES; }
static EFI_STATUS EFIAPI Free(VOID *p) { free(p); return EFI_SUCCESS; }

static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rs;
static EFI_SYSTEM_TABLE St;

static void reset(void) {
  memset(Vars, 0, sizeof(Vars));
  writes = 0; order_err = 0; esp_present = 0; TargetEsp = NULL;
  mkvol(&Usb, 1, 0xA0);
  mkvol(&Esp, 0, 0xB0);
  Usb.files[0] = L"\\EFI\\Drivers\\AdaUnlockDxe.efi";
  Esp.has_bootmgfw = 1;
}
static void foreign(const CHAR16 *n) {
  UINT8 o[64] = {1,0,0,0,4,0,'V',0,0,0,0x7f,0xff,4,0};
  put(n, o, 14);
}
static UINTN order_count(void) { VAR *v = find(L"DriverOrder"); return v ? v->size / 2 : 0; }
static UINT16 order_at(UINTN i) { return ((UINT16 *)find(L"DriverOrder")->data)[i]; }
/* Device path of a Driver#### entry starts after attributes, length and description. */
static int entry_on(const CHAR16 *slot, const VOL *vol) {
  VAR *v = find(slot);
  return v && !memcmp(v->data + 6 + sizeof(DriverDescription), vol->path, 48);
}

int main(void) {
  EFI_HANDLE image = (EFI_HANDLE)(UINTN)1;
  Rs.GetVariable = GetVar; Rs.SetVariable = SetVar;
  Bs.HandleProtocol = Handle; Bs.LocateHandleBuffer = Locate; Bs.AllocatePool = Alloc; Bs.FreePool = Free;
  St.BootServices = &Bs; St.RuntimeServices = &Rs;
  Loaded.DeviceHandle = USB_HANDLE;

  /* A foreign Driver0001 survives install and remove. */
  reset(); foreign(L"Driver0001"); { UINT16 o[1] = {1}; put(L"DriverOrder", o, 2); }
  VAR saved = *find(L"Driver0001");
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(entry_on(L"Driver0000", &Usb));
  CHECK(find(L"Driver0001")->size == saved.size && !memcmp(find(L"Driver0001")->data, saved.data, saved.size));
  CHECK(order_count() == 2 && order_at(0) == 0 && order_at(1) == 1);
  /* Reinstalling reuses the slot and does not duplicate the order entry. */
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(order_count() == 2 && find(L"Driver0002") == NULL);
  CHECK(GuRemoveDriverOption(&St) == EFI_SUCCESS);
  CHECK(find(L"Driver0000") == NULL && find(L"Driver0001") != NULL);
  CHECK(order_count() == 1 && order_at(0) == 1);

  /* Removing with nothing of ours writes nothing. */
  writes = 0;
  CHECK(GuRemoveDriverOption(&St) == EFI_NOT_FOUND);
  CHECK(writes == 0 && find(L"Driver0001") != NULL && order_count() == 1);

  /* An unreadable DriverOrder leaves NVRAM untouched for both options. */
  reset(); foreign(L"Driver0001"); { UINT16 o[1] = {1}; put(L"DriverOrder", o, 2); }
  order_err = EFI_DEVICE_ERROR;
  CHECK(GuInstallDriverOption(image, &St) == EFI_DEVICE_ERROR);
  CHECK(GuRemoveDriverOption(&St) == EFI_DEVICE_ERROR);
  CHECK(writes == 0);

  /* An oversized DriverOrder is not replaced. */
  order_err = EFI_BUFFER_TOO_SMALL; writes = 0;
  CHECK(GuInstallDriverOption(image, &St) == EFI_BUFFER_TOO_SMALL);
  CHECK(writes == 0);

  /* A missing driver file registers nothing. */
  reset(); Usb.files[0] = NULL;
  CHECK(GuInstallDriverOption(image, &St) == EFI_NOT_FOUND);
  CHECK(writes == 0 && find(L"DriverOrder") == NULL);

  /* Fresh system: the order is created, removing the last entry deletes it. */
  reset();
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(order_count() == 1 && order_at(0) == 0);
  CHECK(GuRemoveDriverOption(&St) == EFI_SUCCESS);
  CHECK(find(L"DriverOrder") == NULL && find(L"Driver0000") == NULL);

  /* An entry from the old fixed-slot installer is still recognised. */
  reset(); CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  put(L"Driver0001", find(L"Driver0000")->data, find(L"Driver0000")->size);
  find(L"Driver0000")->used = 0;
  { UINT16 o[1] = {1}; put(L"DriverOrder", o, 2); }
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(find(L"Driver0000") == NULL && find(L"Driver0001") != NULL && order_count() == 1);

  /* All scanned slots belong to other drivers. */
  reset();
  for (UINTN s = 0; s < GU_DRIVER_SCAN_SLOTS; s++) { CHAR16 n[] = L"Driver0000"; SetDriverSlot(n, s); foreign(n); }
  writes = 0;
  CHECK(GuInstallDriverOption(image, &St) == EFI_OUT_OF_RESOURCES);
  CHECK(writes == 0);

  /* With an internal ESP the driver is copied there and the entry points to it. */
  reset(); esp_present = 1;
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(Esp.written == FILE_BYTES && weq(Esp.written_name, L"AdaUnlockDxe.efi"));
  CHECK(entry_on(L"Driver0000", &Esp));
  CHECK(Esp.deleted == 0); /* nothing to replace on a fresh ESP */
  CHECK(GuRemoveDriverOption(&St) == EFI_SUCCESS);
  CHECK(Esp.deleted == 1 && find(L"Driver0000") == NULL);

  /* An older, larger copy on the ESP is deleted before the new one is written. */
  reset(); esp_present = 1; Esp.written = 3 * FILE_BYTES;
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(Esp.deleted == 1 && Esp.written == FILE_BYTES && entry_on(L"Driver0000", &Esp));

  /* A short write counts as a failed copy and falls back to the USB drive. */
  reset(); esp_present = 1; Esp.short_write = 1;
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(entry_on(L"Driver0000", &Usb));

  /* An unreadable DriverOrder stops the install before anything is copied. */
  reset(); esp_present = 1; order_err = EFI_DEVICE_ERROR;
  CHECK(GuInstallDriverOption(image, &St) == EFI_DEVICE_ERROR);
  CHECK(Esp.written == 0 && writes == 0);

  /* A failed ESP copy falls back to an entry on the USB drive. */
  reset(); esp_present = 1; Esp.fail_write = 1;
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(entry_on(L"Driver0000", &Usb));

  /* The driver may also come from \EFI\BOOT on the stick. */
  reset(); esp_present = 1; Usb.files[0] = L"\\EFI\\BOOT\\AdaUnlockDxe.efi";
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(Esp.written == FILE_BYTES && entry_on(L"Driver0000", &Esp));

  /* After a failed DXE unlock the entry leaves DriverOrder but stays registered. */
  reset(); foreign(L"Driver0001"); { UINT16 o[1] = {1}; put(L"DriverOrder", o, 2); }
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(GuDisableDriverOption(&St) == EFI_SUCCESS);
  CHECK(find(L"Driver0000") != NULL && find(L"Driver0001") != NULL);
  CHECK(order_count() == 1 && order_at(0) == 1);
  writes = 0;
  CHECK(GuDisableDriverOption(&St) == EFI_SUCCESS && writes == 0); /* already disabled */
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);       /* [I] enables it again */
  CHECK(order_count() == 2 && order_at(0) == 0 && find(L"Driver0002") == NULL);

  /* Disabling the only entry deletes DriverOrder and keeps the entry for [I]/[R]. */
  reset();
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(GuDisableDriverOption(&St) == EFI_SUCCESS);
  CHECK(find(L"DriverOrder") == NULL && find(L"Driver0000") != NULL);
  CHECK(GuRemoveDriverOption(&St) == EFI_SUCCESS && find(L"Driver0000") == NULL);

  /* Nothing of ours, or an unreadable DriverOrder: nothing is written. */
  reset(); foreign(L"Driver0001"); { UINT16 o[1] = {1}; put(L"DriverOrder", o, 2); }
  writes = 0;
  CHECK(GuDisableDriverOption(&St) == EFI_NOT_FOUND && writes == 0);
  order_err = EFI_DEVICE_ERROR;
  CHECK(GuDisableDriverOption(&St) == EFI_DEVICE_ERROR && writes == 0);

  if (failures) printf("test_driver_nvram: %d FAILED\n", failures);
  else printf("test_driver_nvram: all checks passed\n");
  return failures != 0;
}
