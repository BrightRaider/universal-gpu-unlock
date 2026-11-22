/* SPDX-License-Identifier: GPL-2.0-only */

/* USB log/assets only. Windows ESP is opened read-only. Boot variables are only
 * read. */

#include "boot_io.h"
#include "version.h"

#include "compat/compat.h"

#include "format.h"

#include "profile.h"

#include "sha256.h"

#include <Protocol/LoadedImage.h>

#include <Protocol/SimpleFileSystem.h>

#include <Guid/FileInfo.h>

static EFI_SYSTEM_TABLE *Table;

static EFI_HANDLE Self, Windows, OwnVolume, TargetEspVolume;

static EFI_FILE_PROTOCOL *Root, *Log;

static BOOLEAN Silent;

static CHAR16 ActiveLogName[32] = L"\\GPU-UNLOCK\\RUN0001.TXT";

static EFI_GUID LoadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID,
                FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static EFI_GUID PathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID,
                InfoGuid = EFI_FILE_INFO_ID;

static EFI_GUID GlobalGuid = {
    0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};

static const CHAR16 *const WindowsCandidates[] = {
    L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
    L"EFI\\Microsoft\\Boot\\bootmgfw.efi", L"\\EFI\\Boot\\bootx64.efi",
    L"EFI\\Boot\\bootx64.efi"};

#define GU_WINDOWS_NATIVE_CANDIDATES                                           \
  2U /* Leading WindowsCandidates entries that name bootmgfw.efi. */

#define GU_MAX_WINDOWS_CANDIDATES 16U

typedef struct {
  EFI_HANDLE volume;
  EFI_DEVICE_PATH_PROTOCOL *path;
  UINTN path_bytes, rank;
  const CHAR16 *name;
} GU_WINDOWS_CANDIDATE;

static UINTN GuStrLen(const CHAR16 *s) {
  UINTN len = 0;
  while (s && s[len])
    len++;
  return len;
}

/* Copies src into dst (capacity in characters, always NUL-terminated).
 * Returns the number of characters copied. */
static UINTN GuStrCopy(CHAR16 *dst, UINTN cap, const CHAR16 *src) {
  UINTN n = 0;
  if (!dst || !cap)
    return 0;
  while (src && src[n] && n + 1 < cap) {
    dst[n] = src[n];
    n++;
  }
  dst[n] = L'\0';
  return n;
}

static BOOLEAN Inspect(const EFI_DEVICE_PATH_PROTOCOL *p, UINTN *size,
                       BOOLEAN *usb, BOOLEAN *windows) {
  UINTN at = 0;
  *usb = FALSE;
  *windows = FALSE;
  if (!p)
    return FALSE;
  for (UINTN nodes = 0; nodes < 64 && at + 4 <= 4096; nodes++) {
    UINTN n = p->Length[0] + ((UINTN)p->Length[1] << 8);
    if (n < 4 || n > 4096 - at)
      return FALSE;
    if (p->Type == END_DEVICE_PATH_TYPE) {
      if (n != 4 || p->SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE)
        return FALSE;
      *size = at + 4;
      return TRUE;
    }
    if (p->Type == MESSAGING_DEVICE_PATH && p->SubType == MSG_USB_DP &&
        n >= sizeof(USB_DEVICE_PATH))
      *usb = TRUE;
    if (p->Type == MEDIA_DEVICE_PATH) {
      *windows = TRUE;
    }
    at += n;
    p = (const VOID *)((const UINT8 *)p + n);
  }
  return FALSE;
}

VOID GuBootSay(const CHAR16 *text) {

  if (!Silent && Table && Table->ConOut && Table->ConOut->OutputString)

    Table->ConOut->OutputString(Table->ConOut, (CHAR16 *)text);
}

VOID GuBootSilence(VOID) { Silent = TRUE; }

VOID GuBootUnsilence(VOID) { Silent = FALSE; }

VOID GuLog(const char *format, ...) {

  if (!Log)
    return;

  CHAR8 buffer[2048];
  va_list args;
  va_start(args, format);

  size_t n = GuFormatV(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (n == (size_t)-1 || n >= sizeof(buffer))
    return;

  UINTN bytes = n;
  EFI_STATUS s = Log->Write(Log, &bytes, buffer);

  if (s == EFI_SUCCESS && bytes == n)
    Log->Flush(Log);
}

BOOLEAN GuBootLogOpen(VOID) { return Log != NULL; }
EFI_HANDLE GuBootGetTargetEspVolume(VOID) { return TargetEspVolume; }
const CHAR16 *GuBootGetLogPath(VOID) { return ActiveLogName; }

EFI_STATUS GuBootOpen(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {

  if (Table)
    return EFI_ALREADY_STARTED;

  if (!image || !t || !t->BootServices)
    return EFI_INVALID_PARAMETER;

  Table = t;
  Self = image;

  EFI_BOOT_SERVICES *b = t->BootServices;
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_DEVICE_PATH_PROTOCOL *path = NULL;

  EFI_STATUS s = b->HandleProtocol(image, &LoadedGuid, (VOID **)&loaded);

  if (s != EFI_SUCCESS || !loaded || !loaded->DeviceHandle)
    return EFI_UNSUPPORTED;

  OwnVolume = loaded->DeviceHandle;
  s = b->HandleProtocol(OwnVolume, &PathGuid, (VOID **)&path);

  UINTN length = 0;
  BOOLEAN usb = FALSE, windows = FALSE;

  if (s != EFI_SUCCESS || !Inspect(path, &length, &usb, &windows) || (!usb && !windows))
    return EFI_ACCESS_DENIED;

  s = b->HandleProtocol(OwnVolume, &FsGuid, (VOID **)&fs);
  if (s != EFI_SUCCESS || !fs)
    return EFI_UNSUPPORTED;

  s = fs->OpenVolume(fs, &Root);
  if (s != EFI_SUCCESS || !Root)
    return EFI_DEVICE_ERROR;

  EFI_FILE_PROTOCOL *dir = NULL;
  BOOLEAN is_ad102_dir = FALSE;

  s = Root->Open(Root, &dir, L"\\GPU-UNLOCK",
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                     EFI_FILE_MODE_CREATE,
                 EFI_FILE_DIRECTORY);
  if (s != EFI_SUCCESS || !dir) {
    s = Root->Open(Root, &dir, L"\\AD102-UNLOCK",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                       EFI_FILE_MODE_CREATE,
                   EFI_FILE_DIRECTORY);
    if (s == EFI_SUCCESS && dir)
      is_ad102_dir = TRUE;
  }

  if (s != EFI_SUCCESS || !dir) {
    if (usb)
      return EFI_DEVICE_ERROR;
    Log = NULL;
    if (!b->SetWatchdogTimer)
      return EFI_UNSUPPORTED;
    return b->SetWatchdogTimer(0, 0, 0, NULL);
  }

  for (UINTN slot = 1; slot <= 9999; slot++) {

    CHAR16 name[] = L"RUN0000.TXT";
    UINTN number = slot;

    for (UINTN at = 7; at > 3;) {
      at--;
      name[at] = (CHAR16)('0' + number % 10);
      number /= 10;
    }

    EFI_FILE_PROTOCOL *existing = NULL;
    s = dir->Open(dir, &existing, name, EFI_FILE_MODE_READ, 0);

    if (s == EFI_SUCCESS && existing) {
      s = existing->Close(existing);
      if (s != EFI_SUCCESS)
        break;
      continue;
    }

    if (s != EFI_NOT_FOUND)
      break;

    s = dir->Open(
        dir, &Log, name,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (s == EFI_SUCCESS && Log) {
      UINTN cap = sizeof(ActiveLogName) / sizeof(ActiveLogName[0]);
      UINTN d = GuStrCopy(ActiveLogName, cap, is_ad102_dir ? L"\\AD102-UNLOCK\\" : L"\\GPU-UNLOCK\\");
      (VOID)GuStrCopy(ActiveLogName + d, cap - d, name);
    }
    break;
  }

  EFI_STATUS closed = dir->Close(dir);
  if (s != EFI_SUCCESS || !Log) {
    if (usb)
      return s == EFI_SUCCESS ? EFI_OUT_OF_RESOURCES : s;
    Log = NULL;
  }

  if (closed != EFI_SUCCESS)
    return closed;

  if (!b->SetWatchdogTimer)
    return EFI_UNSUPPORTED;

  return b->SetWatchdogTimer(0, 0, 0, NULL);
}

/* MindAllik v1.3.3 / v1.3.4 Boot-Loop Safeguard: ATTEMPT.FLG
 * Staged before hardware mutation, cleared on clean handoff.
 * Prevents system boot loops if register mutation ever hangs. */
BOOLEAN GuCheckSafeguardFlag(VOID) {
  if (!Root)
    return FALSE;

  EFI_FILE_PROTOCOL *f = NULL;
  EFI_STATUS s = Root->Open(Root, &f, L"\\GPU-UNLOCK\\ATTEMPT.FLG", EFI_FILE_MODE_READ, 0);
  if (s != EFI_SUCCESS || !f) {
    s = Root->Open(Root, &f, L"\\AD102-UNLOCK\\ATTEMPT.FLG", EFI_FILE_MODE_READ, 0);
  }

  if (s == EFI_SUCCESS && f) {
    (VOID)f->Close(f);
    return TRUE;
  }
  return FALSE;
}

EFI_STATUS GuSetSafeguardFlag(VOID) {
  if (!Root)
    return EFI_DEVICE_ERROR;

  EFI_FILE_PROTOCOL *f = NULL;
  EFI_STATUS s = Root->Open(Root, &f, L"\\GPU-UNLOCK\\ATTEMPT.FLG",
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (s != EFI_SUCCESS || !f) {
    s = Root->Open(Root, &f, L"\\AD102-UNLOCK\\ATTEMPT.FLG",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  }

  if (s == EFI_SUCCESS && f) {
    static const CHAR8 marker[] = "GPU_UNLOCK_ATTEMPT_ACTIVE\r\n";
    UINTN len = sizeof(marker) - 1;
    (VOID)f->Write(f, &len, (VOID *)marker);
    (VOID)f->Flush(f);
    (VOID)f->Close(f);
    GuLog("Safeguard: Staged ATTEMPT.FLG before hardware mutation\n");
    return EFI_SUCCESS;
  }
  GuLog("Safeguard: Warning: unable to stage ATTEMPT.FLG (s=%#llx)\n", (unsigned long long)s);
  return s;
}

EFI_STATUS GuClearSafeguardFlag(VOID) {
  if (!Root)
    return EFI_DEVICE_ERROR;

  EFI_STATUS s = EFI_NOT_FOUND;
  EFI_FILE_PROTOCOL *f = NULL;

  s = Root->Open(Root, &f, L"\\GPU-UNLOCK\\ATTEMPT.FLG",
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (s == EFI_SUCCESS && f) {
    s = f->Delete(f);
    GuLog("Safeguard: Cleaned \\GPU-UNLOCK\\ATTEMPT.FLG after verified handoff (status=%#llx)\n",
          (unsigned long long)s);
    return s;
  }

  s = Root->Open(Root, &f, L"\\AD102-UNLOCK\\ATTEMPT.FLG",
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (s == EFI_SUCCESS && f) {
    s = f->Delete(f);
    GuLog("Safeguard: Cleaned \\AD102-UNLOCK\\ATTEMPT.FLG after verified handoff (status=%#llx)\n",
          (unsigned long long)s);
    return s;
  }

  return EFI_SUCCESS;
}

static EFI_STATUS ReadFileData(EFI_FILE_PROTOCOL *root, const CHAR16 *name,
                               UINTN min_size, UINTN max_size, UINTN *file_size,
                               VOID **data) {
  *data = NULL;
  if (file_size)
    *file_size = 0;
  EFI_FILE_PROTOCOL *f = NULL;
  UINT64 info_words[128];
  UINTN info_bytes = sizeof(info_words);

  EFI_STATUS s = root->Open(root, &f, (CHAR16 *)name, EFI_FILE_MODE_READ, 0);
  if (s != EFI_SUCCESS || !f)
    return s == EFI_SUCCESS ? EFI_DEVICE_ERROR : s;

  s = f->GetInfo(f, &InfoGuid, &info_bytes, info_words);
  EFI_FILE_INFO *info = (VOID *)info_words;
  if (s == EFI_SUCCESS &&
      (info_bytes < sizeof(EFI_FILE_INFO) || info_bytes > sizeof(info_words) ||
       info->FileSize < min_size || info->FileSize > max_size ||
       (info->Attribute & EFI_FILE_DIRECTORY))) {
    s = EFI_COMPROMISED_DATA;
  }

  UINTN sz = (UINTN)info->FileSize;
  VOID *p = NULL;
  if (s == EFI_SUCCESS) {
    p = GuAlloc(sz);
    if (!p)
      s = EFI_OUT_OF_RESOURCES;
  }

  for (UINTN at = 0; s == EFI_SUCCESS && at < sz;) {
    UINTN n = sz - at;
    if (n > 1048576)
      n = 1048576;
    UINTN wanted = n;
    s = f->Read(f, &n, (UINT8 *)p + at);
    if (s == EFI_SUCCESS && n != wanted)
      s = EFI_DEVICE_ERROR;
    at += n;
  }

  if (s == EFI_SUCCESS && min_size == max_size) {
    UINT8 tail;
    UINTN n = 1;
    s = f->Read(f, &n, &tail);
    if (s == EFI_SUCCESS && n)
      s = EFI_COMPROMISED_DATA;
  }

  EFI_STATUS closed = f->Close(f);
  if (s == EFI_SUCCESS)
    s = closed;
  if (s != EFI_SUCCESS) {
    GuFree(p);
    return s;
  }

  *data = p;
  if (file_size)
    *file_size = sz;
  return EFI_SUCCESS;
}

static EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *root, const CHAR16 *name,
                           UINTN expected, VOID **data) {
  return ReadFileData(root, name, expected, expected, NULL, data);
}

EFI_STATUS GuBootLoadAssets(VOID) {
  if (!Root || Gu.started)
    return EFI_NOT_READY;
  const GU_ARCH_ASSETS *arch = GuGetArchAssets();
  for (UINTN i = 0; i < GU_ASSET_COUNT; i++) {
    VOID *p = NULL;
    EFI_STATUS s =
        ReadFile(Root, arch->entries[i].path, arch->entries[i].bytes, &p);
    if (s != EFI_SUCCESS && i == 2 && arch->alt_gsp_path) {
      /* Shared gsp_ga10x.bin fallback between directories */
      s = ReadFile(Root, arch->alt_gsp_path, arch->entries[i].bytes, &p);
    }
    if (s != EFI_SUCCESS && Table && Table->BootServices) {
      /* Multi-volume fallback: search across all available filesystem volumes */
      EFI_HANDLE *vh = NULL;
      UINTN vcount = 0;
      if (Table->BootServices->LocateHandleBuffer(ByProtocol, &FsGuid, NULL, &vcount, &vh) == EFI_SUCCESS && vh) {
        for (UINTN v = 0; v < vcount; v++) {
          if (vh[v] == OwnVolume) continue;
          EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *vfs = NULL;
          EFI_FILE_PROTOCOL *vroot = NULL;
          if (Table->BootServices->HandleProtocol(vh[v], &FsGuid, (VOID **)&vfs) == EFI_SUCCESS && vfs &&
              vfs->OpenVolume(vfs, &vroot) == EFI_SUCCESS && vroot) {
            s = ReadFile(vroot, arch->entries[i].path, arch->entries[i].bytes, &p);
            if (s != EFI_SUCCESS && i == 2 && arch->alt_gsp_path) {
              s = ReadFile(vroot, arch->alt_gsp_path, arch->entries[i].bytes, &p);
            }
            vroot->Close(vroot);
            if (s == EFI_SUCCESS) break;
          }
        }
        Table->BootServices->FreePool(vh);
      }
    }
    if (s != EFI_SUCCESS) {
      GuLog("Asset load failed: %s (%s)\n", arch->entries[i].name,
            arch->arch_name);
      return s;
    }
    GuAssets[i].name = arch->entries[i].name;
    GuAssets[i].firmware = (struct firmware){arch->entries[i].bytes, p};
  }
  return GuAssetsValid() ? EFI_SUCCESS : EFI_CRC_ERROR;
}

static EFI_STATUS ReadWindowsBinary(EFI_FILE_PROTOCOL *root, const CHAR16 *name,
                                    UINTN *file_size, VOID **data) {
  VOID *p = NULL;
  EFI_STATUS s = ReadFileData(root, name, 65536, 33554432, file_size, &p);
  if (s != EFI_SUCCESS)
    return s;
  if (((UINT8 *)p)[0] != 'M' || ((UINT8 *)p)[1] != 'Z') {
    GuFree(p);
    *data = NULL;
    if (file_size)
      *file_size = 0;
    return EFI_COMPROMISED_DATA;
  }
  *data = p;
  return EFI_SUCCESS;
}

/* Find the partition node of a device path, bounded by the caller's byte count.
 */
static const HARDDRIVE_DEVICE_PATH *FindHardDrive(const VOID *path,
                                                  UINTN limit) {
  const UINT8 *p = path;
  UINTN at = 0;
  for (UINTN nodes = 0; nodes < 64 && limit - at >= 4; nodes++) {
    UINTN n = p[at + 2] + ((UINTN)p[at + 3] << 8);
    if (n < 4 || n > limit - at || p[at] == END_DEVICE_PATH_TYPE)
      return NULL;
    if (p[at] == MEDIA_DEVICE_PATH && p[at + 1] == MEDIA_HARDDRIVE_DP &&
        n >= sizeof(HARDDRIVE_DEVICE_PATH))
      return (const VOID *)(p + at);
    at += n;
  }
  return NULL;
}

/* Among equally ranked volumes, follow the firmware's own BootOrder so the OS
 * the machine would normally boot wins. Falls back to the first candidate. */
static UINTN PreferredWindowsCandidate(const GU_WINDOWS_CANDIDATE *c,
                                       UINTN count, UINTN rank) {
  static const CHAR16 hex[] = L"0123456789ABCDEF";
  EFI_RUNTIME_SERVICES *rs = Table->RuntimeServices;
  UINT16 order[128];
  UINTN order_bytes = sizeof(order), first = count;
  UINT32 attrs = 0;
  for (UINTN i = 0; i < count; i++)
    if (c[i].rank == rank) {
      first = i;
      break;
    }
  if (!rs || !rs->GetVariable ||
      rs->GetVariable(L"BootOrder", &GlobalGuid, &attrs, &order_bytes, order) !=
          EFI_SUCCESS)
    return first;
  for (UINTN i = 0; i < order_bytes / sizeof(UINT16); i++) {
    CHAR16 name[] = L"Boot0000";
    UINT8 option[2048];
    UINTN option_bytes = sizeof(option);
    for (UINTN d = 0; d < 4; d++)
      name[7 - d] = hex[(order[i] >> (4 * d)) & 0xf];
    if (rs->GetVariable(name, &GlobalGuid, &attrs, &option_bytes, option) !=
            EFI_SUCCESS ||
        option_bytes < 8 || !(option[0] & 1))
      continue;
    UINTN list_bytes = option[4] + ((UINTN)option[5] << 8), at = 6;
    while (at + 2 <= option_bytes && (option[at] || option[at + 1]))
      at += 2;
    at += 2;
    if (at > option_bytes || list_bytes > option_bytes - at)
      continue;
    const HARDDRIVE_DEVICE_PATH *wanted =
        FindHardDrive(option + at, list_bytes);
    if (!wanted)
      continue;
    for (UINTN j = 0; j < count; j++) {
      if (c[j].rank != rank)
        continue;
      const HARDDRIVE_DEVICE_PATH *have =
          FindHardDrive(c[j].path, c[j].path_bytes);
      if (have &&
          !memcmp(have->Signature, wanted->Signature, sizeof(have->Signature)))
        return j;
    }
  }
  return first;
}

EFI_STATUS GuBootLoadWindows(VOID) {
  if (Windows || !Table || Gu.started)
    return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b = Table->BootServices;
  EFI_HANDLE *h = NULL, volume = NULL;
  UINTN count = 0, matches = 0, stored = 0, path_bytes = 0;
  EFI_DEVICE_PATH_PROTOCOL *base = NULL;
  const CHAR16 *selected_name = NULL;
  GU_WINDOWS_CANDIDATE candidates[GU_MAX_WINDOWS_CANDIDATES];
  EFI_STATUS s = b->LocateHandleBuffer(ByProtocol, &FsGuid, NULL, &count, &h);
  if (s == EFI_SUCCESS && (!h || !count || count > 128))
    s = EFI_UNSUPPORTED;
  for (UINTN i = 0; s == EFI_SUCCESS && i < count; i++) {
    if (h[i] == OwnVolume)
      continue;
    EFI_DEVICE_PATH_PROTOCOL *p = NULL;
    UINTN bytes = 0;
    BOOLEAN usb = FALSE, windows = FALSE;
    if (b->HandleProtocol(h[i], &PathGuid, (VOID **)&p) == EFI_SUCCESS &&
        Inspect(p, &bytes, &usb, &windows) && !usb) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *tfs = NULL;
      EFI_FILE_PROTOCOL *troot = NULL;
      if (b->HandleProtocol(h[i], &FsGuid, (VOID **)&tfs) == EFI_SUCCESS &&
          tfs && tfs->OpenVolume(tfs, &troot) == EFI_SUCCESS && troot) {
        const CHAR16 *found_name = NULL;
        UINTN found_rank = 0;
        for (UINTN c_idx = 0;
             c_idx < sizeof(WindowsCandidates) / sizeof(WindowsCandidates[0]);
             c_idx++) {
          EFI_FILE_PROTOCOL *tf = NULL;
          if (troot->Open(troot, &tf, (CHAR16 *)WindowsCandidates[c_idx],
                          EFI_FILE_MODE_READ, 0) == EFI_SUCCESS &&
              tf) {
            tf->Close(tf);
            found_name = WindowsCandidates[c_idx];
            found_rank = c_idx < GU_WINDOWS_NATIVE_CANDIDATES ? 0 : 1;
            break;
          }
        }
        if (found_name) {
          if (stored < GU_MAX_WINDOWS_CANDIDATES) {
            candidates[stored].volume = h[i];
            candidates[stored].path = p;
            candidates[stored].path_bytes = bytes;
            candidates[stored].rank = found_rank;
            candidates[stored].name = found_name;
            stored++;
          }
          matches++;
          GuLog("GuBootLoadWindows: candidate %zu volume=%p found '%ls' (match "
                "#%zu)\n",
                i, (void *)h[i], found_name, matches);
        }
        troot->Close(troot);
      }
    }
  }
  if (h)
    b->FreePool(h);
  /* A real bootmgfw.efi always beats the generic removable-media loader. */
  UINTN best_rank = 1;
  for (UINTN i = 0; i < stored; i++)
    if (!candidates[i].rank)
      best_rank = 0;
  EFI_FILE_PROTOCOL *root = NULL;
  if (s == EFI_SUCCESS && stored) {
    const GU_WINDOWS_CANDIDATE *pick =
        &candidates[PreferredWindowsCandidate(candidates, stored, best_rank)];
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *selected_fs = NULL;
    volume = pick->volume;
    TargetEspVolume = volume;
    base = pick->path;
    path_bytes = pick->path_bytes;
    selected_name = pick->name;
    if (b->HandleProtocol(volume, &FsGuid, (VOID **)&selected_fs) !=
            EFI_SUCCESS ||
        !selected_fs ||
        selected_fs->OpenVolume(selected_fs, &root) != EFI_SUCCESS)
      root = NULL;
  }
  if (s != EFI_SUCCESS || !root || !selected_name) {
    GuLog("GuBootLoadWindows failed: matches=%zu volume=%p s=%#llx\n",
          (size_t)matches, (void *)volume, (unsigned long long)s);
    return EFI_NOT_FOUND;
  }
  if (matches > 1) {
    GuLog("GuBootLoadWindows: multiple targets found (%zu). Selected volume=%p "
          "path='%ls'\n",
          matches, (void *)volume, selected_name);
  }
  VOID *bytes = NULL;
  UINTN actual_size = 0;
  s = ReadWindowsBinary(root, selected_name, &actual_size, &bytes);
  EFI_STATUS closed = root->Close(root);
  if (s == EFI_SUCCESS)
    s = closed;
  UINTN name_bytes = (GuStrLen(selected_name) + 1) * sizeof(CHAR16);
  UINTN node_bytes = 4 + name_bytes, total = path_bytes - 4 + node_bytes + 4;
  UINT8 *path = NULL;
  if (s == EFI_SUCCESS) {
    path = GuAlloc(total);
    if (!path)
      s = EFI_OUT_OF_RESOURCES;
  }
  if (s == EFI_SUCCESS) {
    memcpy(path, base, path_bytes - 4);
    UINT8 *node = path + path_bytes - 4;
    node[0] = MEDIA_DEVICE_PATH;
    node[1] = MEDIA_FILEPATH_DP;
    node[2] = (UINT8)node_bytes;
    node[3] = (UINT8)(node_bytes >> 8);
    memcpy(node + 4, selected_name, name_bytes);
    node += node_bytes;
    node[0] = END_DEVICE_PATH_TYPE;
    node[1] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    node[2] = 4;
    node[3] = 0;
    s = b->LoadImage(FALSE, Self, (VOID *)path, bytes, actual_size, &Windows);
  }
  GuFree(bytes);
  GuFree(path);
  if (s == EFI_SUCCESS) {
    EFI_LOADED_IMAGE_PROTOCOL *im = NULL;
    if (b->HandleProtocol(Windows, &LoadedGuid, (VOID **)&im) == EFI_SUCCESS &&
        im && im->DeviceHandle != volume) {
      im->DeviceHandle = volume;
    }
  }
  GuLog("Windows image staged: status=%#llx matches=%zu path='%ls'\n",
        (unsigned long long)s, (size_t)matches, selected_name);
  return s;
}

/* Prints prefix, a two-digit number and suffix. */
static VOID GuBootSayCountdown(const CHAR16 *prefix, UINTN value, const CHAR16 *suffix) {
  CHAR16 digits[3];
  if (value > 99)
    value = 99;
  digits[0] = (CHAR16)(L'0' + value / 10);
  digits[1] = (CHAR16)(L'0' + value % 10);
  digits[2] = L'\0';
  GuBootSay(prefix);
  GuBootSay(digits);
  GuBootSay(suffix);
}

static VOID GuPrintCountdownPrompt(BOOLEAN hide_s3, UINTN sec, BOOLEAN overwrite) {
  GuBootSayCountdown(overwrite ? L"\r--> TIMER: " : L"--> TIMER: ", sec,
                     hide_s3 ? L"s left (default: Stock Windows) - Select [U/I/R/S]: "
                             : L"s left (default: Stock Windows) - Select [U/P/I/R/S]: ");
}

EFI_STATUS GuBootChoose(CHAR16 *choice) {

  if (!choice || !Table || !Table->ConIn || !Table->ConIn->ReadKeyStroke)
    return EFI_UNSUPPORTED;

  *choice = L'S';

  BOOLEAN hide_s3 = FALSE;
  if (Root && Root->Open) {
    EFI_FILE_PROTOCOL *cfg = NULL;
    EFI_STATUS cs = Root->Open(Root, &cfg, L"EFI\\BOOT\\hide_s3.cfg",
                               EFI_FILE_MODE_READ, 0);
    if (cs == EFI_SUCCESS && cfg) {
      hide_s3 = TRUE;
      if (cfg->Close)
        cfg->Close(cfg);
    }
  }

  GuBootSay(
      L"\r\n=======================================================\r\n"
      L" UNIVERSAL GPU UNLOCK " GU_VERSION_TAG_WIDE L" (Multi-Gen Edition)\r\n"
      L"=======================================================\r\n"
      L" >>> 30-SECOND TIMER ACTIVE - PRESS [U] TO UNLOCK <<<\r\n"
      L" (Press [Space] to pause countdown and read in peace)\r\n"
      L"=======================================================\r\n");
  if (GuIsUnlocked()) {
    GuBootSay(
        L" [OK] GPU Silicon Status: ALREADY UNLOCKED (by DXE Driver in POST)\r\n"
        L"      Pressing [P] or [U] will register S3 and boot Windows directly.\r\n"
        L"=======================================================\r\n");
  }
  if (hide_s3) {
    GuBootSay(L"  [U] Full GPU Unlock (RAM-only)\r\n"
              L"  [I] Install DXE Driver to internal ESP and UEFI NVRAM (Auto-boot)\r\n");
  } else {
    GuBootSay(L"  [U] Standard Unlock (RAM-only)\r\n"
              L"  [P] Experimental S3 Persistence Unlock (Boot-Script)\r\n"
              L"  [I] Install DXE Driver to internal ESP and UEFI NVRAM (Auto-boot / S3)\r\n");
  }
  GuBootSay(L"  [R] Remove DXE Driver from UEFI NVRAM\r\n"
            L"  [S] Stock Windows (Diagnostic hardware scan, logs to USB, normal boot)\r\n"
            L"=======================================================\r\n\r\n");

#define GU_BOOT_TIMEOUT_SEC 30U
  BOOLEAN timed_out = TRUE;
  BOOLEAN paused = FALSE;
  UINTN tick = 0;

  while (paused || tick < GU_BOOT_TIMEOUT_SEC * 10) {
    if (!paused && tick % 10 == 0) {
      UINTN remaining = GU_BOOT_TIMEOUT_SEC - (tick / 10);
      GuPrintCountdownPrompt(hide_s3, remaining, tick > 0);
    }

    EFI_INPUT_KEY key;
    EFI_STATUS s = Table->ConIn->ReadKeyStroke(Table->ConIn, &key);

    if (s == EFI_SUCCESS) {
      if (key.UnicodeChar == ' ' && !paused) {
        paused = TRUE;
        GuBootSay(hide_s3 ? L"\r\n[PAUSED] Countdown stopped. Take your time to select [U/I/R/S]: "
                          : L"\r\n[PAUSED] Countdown stopped. Take your time to select [U/P/I/R/S]: ");
        continue;
      }

      GuBootSay(L"\r\n");
      timed_out = FALSE;

      CHAR16 c = key.UnicodeChar;
      if (c >= L'a' && c <= L'z')
        c = (CHAR16)(c - L'a' + L'A');
      if (key.ScanCode == 0x17 || c == 27)
        c = L'S'; /* Esc */
      if (c == L'U' || c == L'I' || c == L'R' || c == L'S' || (c == L'P' && !hide_s3)) {
        *choice = c;
        return EFI_SUCCESS;
      }

    } else if (s != EFI_NOT_READY)
      return s;

    s = Table->BootServices->Stall(100000);
    if (s != EFI_SUCCESS)
      return s;

    if (!paused)
      tick++;
  }

  if (timed_out) {
    GuPrintCountdownPrompt(hide_s3, 0, TRUE);
    GuBootSay(L"\r\n\r\n[TIMEOUT] 30s elapsed - booting Stock Windows.\r\n"
              L"  Diagnostic log: ");
    GuBootSay(ActiveLogName);
    GuBootSay(L"\r\n  Need help or want to report your hardware? Open an issue:\r\n"
              L"  https://github.com/BrightRaider/universal-gpu-unlock/issues\r\n\r\n");
    if (Table->BootServices && Table->BootServices->Stall)
      Table->BootServices->Stall(2000000);
  }

  return EFI_SUCCESS;
}

EFI_STATUS GuBootSyncLog(VOID) {
  if (!Log || !Root)
    return EFI_SUCCESS;

  EFI_STATUS s = Log->Flush(Log);
  EFI_STATUS closed = Log->Close(Log);
  Log = NULL;
  if (s == EFI_SUCCESS)
    s = closed;

  EFI_FILE_PROTOCOL *reopened = NULL;
  EFI_STATUS rs = Root->Open(
      Root, &reopened, ActiveLogName,
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (rs == EFI_SUCCESS && reopened) {
    (VOID)reopened->SetPosition(reopened, ~(UINT64)0);
    Log = reopened;
  } else if (s == EFI_SUCCESS) {
    s = rs;
  }

  return s;
}

EFI_STATUS GuBootClose(VOID) {

  EFI_STATUS s = EFI_SUCCESS;

  if (Log) {
    s = Log->Flush(Log);
    EFI_STATUS closed = Log->Close(Log);
    Log = NULL;
    if (s == EFI_SUCCESS)
      s = closed;
  }

  if (Root) {
    EFI_STATUS closed = Root->Close(Root);
    Root = NULL;
    if (s == EFI_SUCCESS)
      s = closed;
  }

  return s;
}

EFI_STATUS GuBootWindows(VOID) {

  if (!Windows)
    return EFI_NOT_READY;

  return Table->BootServices->StartImage(Windows, NULL, NULL);
}

VOID GuBootHalt(VOID) {
  GuBootUnsilence();

  if (Table && Table->ConIn && Table->ConIn->ReadKeyStroke) {
    EFI_INPUT_KEY discard;
    while (Table->ConIn->ReadKeyStroke(Table->ConIn, &discard) == EFI_SUCCESS)
      ;
  }
  if (Table && Table->ConOut && Table->ConOut->Reset) {
    Table->ConOut->Reset(Table->ConOut, FALSE);
  }

  GuBootSay(
      L"\r\n=======================================================\r\n"
      L" UNIVERSAL GPU UNLOCK - TRANSACTION HALTED\r\n"
      L"=======================================================\r\n"
      L" [!] Hardware unlock could not be completed safely.\r\n"
      L" [!] The GPU may be partially changed. The reboot below resets it.\r\n"
      L" [+] Diagnostic log: ");
  GuBootSay(ActiveLogName);
  GuBootSay(
      L"\r\n=======================================================\r\n"
      L" Please report this issue with your log file on GitHub:\r\n"
      L" https://github.com/BrightRaider/universal-gpu-unlock/issues\r\n\r\n");

  /* Live countdown for the automatic reboot. A key press ends it early. */
#define GU_HALT_REBOOT_SEC 15U
  BOOLEAN key_pressed = FALSE;
  for (UINTN sec = GU_HALT_REBOOT_SEC; sec > 0 && !key_pressed; sec--) {
    GuBootSayCountdown(sec == GU_HALT_REBOOT_SEC ? L"Rebooting in " : L"\rRebooting in ",
                       sec, L"s (press any key to reboot now)... ");
    for (UINTN tick = 0; tick < 10 && !key_pressed; tick++) {
      EFI_INPUT_KEY key;
      if (Table && Table->ConIn && Table->ConIn->ReadKeyStroke &&
          Table->ConIn->ReadKeyStroke(Table->ConIn, &key) == EFI_SUCCESS)
        key_pressed = TRUE;
      else if (Table && Table->BootServices && Table->BootServices->Stall)
        Table->BootServices->Stall(100000);
    }
  }

  GuBootSay(L"\r\nRebooting now...\r\n");
  if (Table && Table->BootServices && Table->BootServices->Stall)
    Table->BootServices->Stall(500000);

  if (Table && Table->RuntimeServices && Table->RuntimeServices->ResetSystem) {
    Table->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
  }

#if defined(_MSC_VER)

  for (;;)
    ;

#else

  for (;;)
    __asm__ volatile("pause");

#endif
}
