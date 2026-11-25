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
static const GU_ARCH_ASSETS *MockArchAssets = NULL;
const GU_ARCH_ASSETS *GuGetArchAssets(VOID) { return MockArchAssets; }

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
#define USB_HANDLE      ((EFI_HANDLE)(UINTN)2)
#define ESP_HANDLE      ((EFI_HANDLE)(UINTN)3)
#define UNLOCK_HANDLE   ((EFI_HANDLE)(UINTN)4)
#define SMALLFAT_HANDLE ((EFI_HANDLE)(UINTN)5)
#define DATAFAT_HANDLE  ((EFI_HANDLE)(UINTN)6)
typedef struct VOL VOL;
typedef struct { EFI_FILE_PROTOCOL p; VOL *v; int kind; } MFILE;
enum { MF_ROOT = 0, MF_EFI_DIR, MF_DRIVERS_DIR, MF_ASSETS_DIR, MF_RFILE, MF_WDRIVER, MF_WASSET };
struct VOL {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs; /* must stay first */
  UINT8 path[64];
  const CHAR16 *files[8];
  int has_bootmgfw, has_fs_info, read_only, fail_write, short_write, deleted, fail_after_writes;
  int has_efi_dir, has_drivers_dir, has_assets_dir, driver_present, asset_present;
  UINT64 vol_size, free_space, asset_written_bytes, src_file_size;
  CHAR16 label[32];
  UINTN written, total_writes, write_calls;
  CHAR16 written_name[64];
  MFILE root, dir, sub_efi, sub_assets, rfile, wfile, wasset;
};
static VOL Usb, Esp, UnlockVol, SmallFatVol, DataFatVol;
static int esp_present, unlock_present, small_fat_present, data_fat_present;

static EFI_STATUS EFIAPI FOpen(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr) {
  MFILE *mf = (MFILE *)This;
  VOL *v = mf->v;
  if (Attr & EFI_FILE_DIRECTORY) {
    /* Enforce real UEFI FatDxe rule: Open(..., CREATE, DIRECTORY) cannot create
     * multi-segment paths unless all intermediate parent directories exist. */
    if (mf->kind == MF_ROOT) {
      if (weq(Name, L"\\EFI\\Drivers") || weq(Name, L"EFI\\Drivers")) {
        if (!v->has_efi_dir) return EFI_NOT_FOUND;
        v->has_drivers_dir = 1;
        *New = &v->dir.p;
        return EFI_SUCCESS;
      }
      if (weq(Name, L"EFI") || weq(Name, L"\\EFI")) {
        v->has_efi_dir = 1;
        *New = &v->sub_efi.p;
        return EFI_SUCCESS;
      }
      if (weq(Name, L"AD102-ASSETS") || weq(Name, L"\\AD102-ASSETS")) {
        v->has_assets_dir = 1;
        *New = &v->sub_assets.p;
        return EFI_SUCCESS;
      }
      if (weq(Name, L"Drivers")) return EFI_NOT_FOUND;
    } else if (mf->kind == MF_EFI_DIR) {
      if (weq(Name, L"Drivers")) {
        if (!v->has_efi_dir) return EFI_NOT_FOUND;
        v->has_drivers_dir = 1;
        *New = &v->dir.p;
        return EFI_SUCCESS;
      }
    }
    return EFI_NOT_FOUND;
  }
  if (Mode & EFI_FILE_MODE_CREATE) {
    wcopy(v->written_name, Name, 64);
    if (weq(Name, L"AdaUnlockDxe.efi")) {
      v->driver_present = 1;
      *New = &v->wfile.p;
    } else {
      v->asset_present = 1;
      *New = &v->wasset.p;
    }
    return EFI_SUCCESS;
  }
  if (v->has_bootmgfw && weq(Name, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi")) { *New = &v->rfile.p; return EFI_SUCCESS; }
  for (int i = 0; i < 8; i++)
    if (v->files[i] && weq(Name, v->files[i])) { *New = &v->rfile.p; return EFI_SUCCESS; }
  /* An existing copy, opened by full path ([R]) or relative to \EFI\Drivers (copy). */
  if (v->driver_present && (weq(Name, L"\\EFI\\Drivers\\AdaUnlockDxe.efi") || weq(Name, L"AdaUnlockDxe.efi"))) {
    *New = &v->wfile.p;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI FClose(EFI_FILE_PROTOCOL *This) { (void)This; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI FDelete(EFI_FILE_PROTOCOL *This) {
  MFILE *mf = (MFILE *)This;
  VOL *v = mf->v;
  if (mf->kind == MF_WDRIVER) {
    v->deleted++;
    v->written = 0;
    v->driver_present = 0;
  } else if (mf->kind == MF_WASSET) {
    v->asset_present = 0;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FRead(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer) {
  (void)This;
  memset(Buffer, 0xab, *Size);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FWrite(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buffer) {
  MFILE *mf = (MFILE *)This;
  VOL *v = mf->v;
  (void)Buffer;
  if (v->fail_write) return EFI_DEVICE_ERROR;
  if (v->fail_after_writes && v->total_writes >= (UINTN)v->fail_after_writes) return EFI_VOLUME_FULL;
  if (v->has_fs_info && v->free_space < *Size) return EFI_VOLUME_FULL;
  if (v->short_write) *Size /= 2;
  v->write_calls++;
  if (mf->kind == MF_WDRIVER) {
    v->written = *Size;
  }
  v->total_writes++;
  v->asset_written_bytes += *Size;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FGetInfo(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN *Size, VOID *Buffer) {
  MFILE *mf = (MFILE *)This;
  VOL *v = mf->v;
  if (Type && !memcmp(Type, &FsInfoGuid, sizeof(*Type))) {
    if (!v->has_fs_info) return EFI_UNSUPPORTED;
    UINTN need = sizeof(GU_EFI_FILE_SYSTEM_INFO) + 64;
    if (*Size < need) { *Size = need; return EFI_BUFFER_TOO_SMALL; }
    memset(Buffer, 0, need);
    GU_EFI_FILE_SYSTEM_INFO *fsi = Buffer;
    fsi->Size = need;
    fsi->ReadOnly = v->read_only ? TRUE : FALSE;
    fsi->VolumeSize = v->vol_size;
    fsi->FreeSpace = v->free_space;
    fsi->BlockSize = 4096;
    wcopy(fsi->VolumeLabel, v->label, 32);
    *Size = need;
    return EFI_SUCCESS;
  }
  EFI_FILE_INFO *info = Buffer;
  (void)mf;
  if (*Size < sizeof(*info)) { *Size = sizeof(*info); return EFI_BUFFER_TOO_SMALL; }
  memset(info, 0, sizeof(*info));
  info->Size = sizeof(*info);
  info->FileSize = v->src_file_size ? v->src_file_size : FILE_BYTES;
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
static void mkfile(MFILE *f, VOL *v, int kind) {
  memset(f, 0, sizeof(*f));
  f->v = v;
  f->kind = kind;
  f->p.Revision = EFI_FILE_PROTOCOL_REVISION;
  f->p.Open = FOpen; f->p.Close = FClose; f->p.Delete = FDelete;
  f->p.Read = FRead; f->p.Write = FWrite; f->p.GetInfo = FGetInfo;
}
static void mkvol(VOL *v, int usb, UINT8 sig) {
  memset(v, 0, sizeof(*v));
  v->fs.OpenVolume = OpenVol;
  mkpath(v->path, usb, sig);
  mkfile(&v->root, v, MF_ROOT); mkfile(&v->dir, v, MF_DRIVERS_DIR);
  mkfile(&v->sub_efi, v, MF_EFI_DIR); mkfile(&v->sub_assets, v, MF_ASSETS_DIR);
  mkfile(&v->rfile, v, MF_RFILE); mkfile(&v->wfile, v, MF_WDRIVER);
  mkfile(&v->wasset, v, MF_WASSET);
}

/* ---- Mock boot services ---- */
static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static VOL *VolForHandle(EFI_HANDLE h) {
  if (h == USB_HANDLE) return &Usb;
  if (h == ESP_HANDLE) return &Esp;
  if (h == UNLOCK_HANDLE) return &UnlockVol;
  if (h == SMALLFAT_HANDLE) return &SmallFatVol;
  if (h == DATAFAT_HANDLE) return &DataFatVol;
  return NULL;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h, EFI_GUID *g, VOID **o) {
  VOL *v = VolForHandle(h);
  if (!memcmp(g, &LoadedGuid, sizeof(*g))) { *o = &Loaded; return EFI_SUCCESS; }
  if (!v) return EFI_UNSUPPORTED;
  if (!memcmp(g, &PathGuid, sizeof(*g))) { *o = v->path; return EFI_SUCCESS; }
  if (!memcmp(g, &FsGuid, sizeof(*g))) { *o = &v->fs; return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE t, EFI_GUID *g, VOID *k, UINTN *n, EFI_HANDLE **b) {
  (void)t; (void)g; (void)k;
  *b = malloc(5 * sizeof(EFI_HANDLE));
  (*b)[0] = USB_HANDLE;
  *n = 1;
  if (esp_present) (*b)[(*n)++] = ESP_HANDLE;
  if (unlock_present) (*b)[(*n)++] = UNLOCK_HANDLE;
  if (small_fat_present) (*b)[(*n)++] = SMALLFAT_HANDLE;
  if (data_fat_present) (*b)[(*n)++] = DATAFAT_HANDLE;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Alloc(EFI_MEMORY_TYPE t, UINTN s, VOID **o) { (void)t; *o = malloc(s); return *o ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES; }
static EFI_STATUS EFIAPI Free(VOID *p) { free(p); return EFI_SUCCESS; }

static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rs;
static EFI_SYSTEM_TABLE St;

static void reset(void) {
  memset(Vars, 0, sizeof(Vars));
  writes = 0; order_err = 0;
  esp_present = 0; unlock_present = 0; small_fat_present = 0; data_fat_present = 0;
  TargetEsp = NULL; MockArchAssets = NULL;
  mkvol(&Usb, 1, 0xA0);
  mkvol(&Esp, 0, 0xB0);
  mkvol(&UnlockVol, 0, 0xC0);
  mkvol(&SmallFatVol, 0, 0xD0);
  mkvol(&DataFatVol, 0, 0xE0);
  Usb.files[0] = L"\\EFI\\Drivers\\AdaUnlockDxe.efi";
  Esp.has_bootmgfw = 1;
  wcopy(UnlockVol.label, L"GPU_UNLOCK", 32);
  UnlockVol.has_fs_info = 1;
  UnlockVol.vol_size = 512ULL * 1024 * 1024;
  UnlockVol.free_space = 500ULL * 1024 * 1024;
  wcopy(SmallFatVol.label, L"EXTRA_EFI", 32);
  SmallFatVol.has_fs_info = 1;
  SmallFatVol.vol_size = 512ULL * 1024 * 1024;
  SmallFatVol.free_space = 450ULL * 1024 * 1024;
  wcopy(DataFatVol.label, L"DATA_FAT", 32);
  DataFatVol.has_fs_info = 1;
  DataFatVol.vol_size = 10ULL * 1024 * 1024 * 1024;
  DataFatVol.free_space = 8ULL * 1024 * 1024 * 1024;
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
  CHECK(GuDriverInstalledOnEsp());
  CHECK(Esp.deleted == 0); /* nothing to replace on a fresh ESP */
  CHECK(GuRemoveDriverOption(&St) == EFI_SUCCESS);
  CHECK(Esp.deleted == 1 && find(L"Driver0000") == NULL);

  /* An older, larger copy on the ESP is deleted before the new one is written. */
  reset(); esp_present = 1; Esp.written = 3 * FILE_BYTES; Esp.driver_present = 1;
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
  CHECK(!GuDriverInstalledOnEsp());

  /* Without an ESP copy the entry needs \EFI\Drivers\AdaUnlockDxe.efi on the stick;
   * a copy only under \EFI\BOOT must not produce a dangling entry. */
  reset(); Usb.files[0] = L"\\EFI\\BOOT\\AdaUnlockDxe.efi";
  CHECK(GuInstallDriverOption(image, &St) == EFI_NOT_FOUND);
  CHECK(writes == 0 && find(L"Driver0000") == NULL && !GuDriverInstalledOnEsp());

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

  /* Dedicated 512MB GPU_UNLOCK fallback when 100MB primary ESP lacks free space. */
  static GU_ARCH_ASSETS mock_ada;
  memset(&mock_ada, 0, sizeof(mock_ada));
  mock_ada.arch_name = "Ada";
  mock_ada.entries[0].path = L"\\AD102-ASSETS\\gsp_ga10x.bin";
  mock_ada.entries[0].name = "ad102/gsp_ga10x.bin";
  mock_ada.entries[0].bytes = 84277400U;

  reset();
  esp_present = 1; unlock_present = 1;
  MockArchAssets = &mock_ada;
  Usb.files[1] = L"\\AD102-ASSETS\\gsp_ga10x.bin";
  Esp.has_fs_info = 1; Esp.vol_size = 100ULL * 1024 * 1024; Esp.free_space = 64ULL * 1024 * 1024;
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(GuDriverInstalledOnEsp());
  CHECK(Esp.total_writes == 0); /* skipped upfront without wasting writes on full 100MB ESP */
  CHECK(UnlockVol.total_writes == 2 && UnlockVol.written == FILE_BYTES);
  CHECK(entry_on(L"Driver0000", &UnlockVol));
  CHECK(GuRemoveDriverOption(&St) == EFI_SUCCESS);
  CHECK(UnlockVol.deleted == 1 && find(L"Driver0000") == NULL);

  /* Rollback removes partial AdaUnlockDxe.efi from primary ESP when asset staging hits VOLUME_FULL. */
  reset();
  esp_present = 1; unlock_present = 1;
  MockArchAssets = &mock_ada;
  Usb.files[1] = L"\\AD102-ASSETS\\gsp_ga10x.bin";
  Esp.has_fs_info = 0;
  Esp.fail_after_writes = 1; /* DriverFile write succeeds, asset write hits VOLUME_FULL */
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(GuDriverInstalledOnEsp());
  CHECK(Esp.deleted == 1 && Esp.written == 0);
  CHECK(UnlockVol.total_writes == 2 && entry_on(L"Driver0000", &UnlockVol));
  /* Check manual small FAT32 partition (<2GiB) works even without GPU_UNLOCK label. */
  reset();
  esp_present = 1; small_fat_present = 1;
  MockArchAssets = &mock_ada;
  Usb.files[1] = L"\\AD102-ASSETS\\gsp_ga10x.bin";
  Esp.has_fs_info = 1; Esp.vol_size = 100ULL * 1024 * 1024; Esp.free_space = 60ULL * 1024 * 1024;
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(GuDriverInstalledOnEsp());
  CHECK(SmallFatVol.total_writes == 2 && entry_on(L"Driver0000", &SmallFatVol));

  /* Giant unrelated FAT32 data volume (>2GiB, unlabeled) must NOT be chosen automatically. */
  reset();
  esp_present = 1; data_fat_present = 1;
  MockArchAssets = &mock_ada;
  Usb.files[1] = L"\\AD102-ASSETS\\gsp_ga10x.bin";
  Esp.has_fs_info = 1; Esp.vol_size = 100ULL * 1024 * 1024; Esp.free_space = 60ULL * 1024 * 1024;
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(!GuDriverInstalledOnEsp());
  CHECK(DataFatVol.total_writes == 0 && entry_on(L"Driver0000", &Usb));

  /* Empty GPU_UNLOCK (no pre-existing \EFI folder, trailing spaces in FAT label)
   * + >1 MiB streaming chunked copy across multiple 1 MiB chunks. */
  reset();
  esp_present = 1; unlock_present = 1;
  MockArchAssets = &mock_ada;
  Usb.files[1] = L"\\AD102-ASSETS\\gsp_ga10x.bin";
  Usb.src_file_size = 2500000ULL;
  Esp.has_fs_info = 1; Esp.vol_size = 100ULL * 1024 * 1024; Esp.free_space = 60ULL * 1024 * 1024;
  wcopy(UnlockVol.label, L"GPU_UNLOCK   ", 32);
  CHECK(GuInstallDriverOption(image, &St) == EFI_SUCCESS);
  CHECK(GuDriverInstalledOnEsp());
  CHECK(UnlockVol.has_efi_dir && UnlockVol.has_drivers_dir && UnlockVol.has_assets_dir);
  CHECK(UnlockVol.asset_written_bytes == 5000000ULL);
  CHECK(UnlockVol.write_calls == 6); /* 3 chunks (1M+1M+452K) x 2 files */
  CHECK(entry_on(L"Driver0000", &UnlockVol));

  if (failures) printf("test_driver_nvram: %d FAILED\n", failures);
  else printf("test_driver_nvram: all checks passed\n");
  return failures != 0;
}
