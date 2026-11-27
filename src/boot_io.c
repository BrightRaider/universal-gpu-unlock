/* SPDX-License-Identifier: GPL-2.0-only */

/* USB log/assets only. Windows ESP is opened read-only. Boot variables are only
 * read. */

#define GU_UEFI_LANG_IMPLEMENTATION
#include "boot_io.h"
#include "version.h"

#include "compat/compat.h"

#include "format.h"
#include "gop_menu.h"

#include "profile.h"

#include "sha256.h"
#include "s3_facs.h"

#include <Protocol/LoadedImage.h>

#include <Protocol/SimpleFileSystem.h>

#include <Guid/FileInfo.h>

static EFI_SYSTEM_TABLE *Table;

static EFI_HANDLE Self, Windows, OwnVolume, TargetEspVolume;

static EFI_FILE_PROTOCOL *Root, *Log;

static BOOLEAN Silent;
static BOOLEAN GopConNeedsInit;
static GU_GOP_CON GopCon;
static UINT32 MenuLang = GU_LANG_EN;
static BOOLEAN MenuLangExplicit = FALSE;
static UINT32 MenuCsmStatus = GU_CSM_DISABLED;

static UINT32 GuBootActiveLang(VOID) {
  return (GopCon.enabled && GopCon.gop && MenuLang < GU_LANG_COUNT) ? MenuLang : GU_LANG_EN;
}

VOID GuBootSetCsmStatus(UINT32 status) {
  MenuCsmStatus = status;
}

UINT32 GuBootGetLanguage(VOID) {
  return GuBootActiveLang();
}

VOID GuBootSetLanguage(UINT32 lang) {
  MenuLang = (lang < GU_LANG_COUNT) ? lang : GU_LANG_EN;
  MenuLangExplicit = TRUE;
}

const CHAR16 *GuBootUiStr(UINT32 str_id) {
  return GuTr(GuBootActiveLang(), (GU_STR_ID)str_id);
}

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

VOID GuBootEnableLargeConsole(VOID) {
  memset(&GopCon, 0, sizeof(GopCon));
  GopCon.enabled = TRUE;
  GopConNeedsInit = TRUE;
}

VOID GuBootSay(const CHAR16 *text) {
  if (Silent || !text)
    return;
  if (GopCon.enabled && !GopCon.gop && GopConNeedsInit) {
    GopConNeedsInit = FALSE;
    (VOID)GuGopConAttach(&GopCon, Table, TRUE);
  }
  if (GopCon.enabled && GopCon.gop) {
    GuGopConWrite(&GopCon, text);
    return;
  }
  if (Table && Table->ConOut && Table->ConOut->OutputString)
    Table->ConOut->OutputString(Table->ConOut, (CHAR16 *)text);
}

VOID GuBootSilence(VOID) {
  Silent = TRUE;
  GuGopConDetach(&GopCon);
}

VOID GuBootUnsilence(VOID) {
  Silent = FALSE;
  if (GopCon.enabled) {
    GopConNeedsInit = FALSE;
    (VOID)GuGopConAttach(&GopCon, Table, FALSE);
  }
}

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

#define GU_MAX_RUN_LOGS 50U
#define GU_PRUNE_BATCH 64U

static CHAR16 GuUpperAscii(CHAR16 c) {
  return (c >= L'a' && c <= L'z') ? (CHAR16)(c - (L'a' - L'A')) : c;
}

static UINTN ParseRunLogNumber(const CHAR16 *name) {
  UINTN num = 0;
  if (!name)
    return 0;
  if (GuUpperAscii(name[0]) != L'R' || GuUpperAscii(name[1]) != L'U' ||
      GuUpperAscii(name[2]) != L'N')
    return 0;
  for (UINTN i = 3; i < 7; i++) {
    if (name[i] < L'0' || name[i] > L'9')
      return 0;
    num = num * 10U + (UINTN)(name[i] - L'0');
  }
  if (name[7] != L'.' || GuUpperAscii(name[8]) != L'T' ||
      GuUpperAscii(name[9]) != L'X' || GuUpperAscii(name[10]) != L'T' ||
      name[11] != L'\0')
    return 0;
  return num;
}

static VOID FormatRunLogName(UINTN slot, CHAR16 name[12]) {
  static const CHAR16 tmpl[] = L"RUN0000.TXT";
  for (UINTN i = 0; i < 12; i++)
    name[i] = tmpl[i];
  for (UINTN at = 7; at > 3;) {
    at--;
    name[at] = (CHAR16)(L'0' + (slot % 10U));
    slot /= 10U;
  }
}

static UINTN RunLogOrderKey(UINTN num, BOOLEAN wrap) {
  return (wrap && num <= 100U) ? (num + 10000U) : num;
}

static VOID InsertNewestRunLog(UINT16 newest[GU_MAX_RUN_LOGS - 1U],
                               UINTN *keep_cnt, UINTN num, BOOLEAN wrap,
                               UINT16 del_list[GU_PRUNE_BATCH],
                               UINTN *del_cnt) {
  UINTN key = RunLogOrderKey(num, wrap);
  for (UINTN i = 0; i < *keep_cnt; i++) {
    if (newest[i] == (UINT16)num)
      return;
  }
  if (*keep_cnt < GU_MAX_RUN_LOGS - 1U) {
    UINTN pos = *keep_cnt;
    while (pos > 0 && RunLogOrderKey(newest[pos - 1U], wrap) > key) {
      newest[pos] = newest[pos - 1U];
      pos--;
    }
    newest[pos] = (UINT16)num;
    (*keep_cnt)++;
    return;
  }
  UINT16 oldest = newest[0];
  if (key <= RunLogOrderKey(oldest, wrap)) {
    if (*del_cnt < GU_PRUNE_BATCH)
      del_list[(*del_cnt)++] = (UINT16)num;
    return;
  }
  if (*del_cnt < GU_PRUNE_BATCH)
    del_list[(*del_cnt)++] = oldest;
  UINTN pos = 0;
  while (pos + 1U < *keep_cnt &&
         RunLogOrderKey(newest[pos + 1U], wrap) < key) {
    newest[pos] = newest[pos + 1U];
    pos++;
  }
  newest[pos] = (UINT16)num;
}

static EFI_STATUS OpenRotatedRunLog(EFI_FILE_PROTOCOL *dir,
                                    CHAR16 chosen_name[12],
                                    EFI_FILE_PROTOCOL **out_log) {
  UINTN next_num = 1U;
  BOOLEAN dir_read_ok = FALSE;

  if (dir->Read) {
    for (UINTN round = 0; round < 16U; round++) {
      UINTN count = 0, max_raw = 0, max_low = 0;
      BOOLEAN saw_high = FALSE, saw_low = FALSE;
      union {
        EFI_FILE_INFO info;
        UINT8 raw[512];
      } u;

      if (dir->SetPosition)
        (VOID)dir->SetPosition(dir, 0);

      for (;;) {
        UINTN sz = sizeof(u.raw);
        EFI_STATUS rs = dir->Read(dir, &sz, u.raw);
        if (rs != EFI_SUCCESS)
          break;
        if (sz == 0) {
          dir_read_ok = TRUE;
          break;
        }
        dir_read_ok = TRUE;
        if (u.info.Attribute & EFI_FILE_DIRECTORY)
          continue;
        UINTN num = ParseRunLogNumber(u.info.FileName);
        if (!num)
          continue;
        count++;
        if (num >= 9900U)
          saw_high = TRUE;
        if (num <= 100U) {
          saw_low = TRUE;
          if (num > max_low)
            max_low = num;
        }
        if (num > max_raw)
          max_raw = num;
      }

      BOOLEAN wrap = (saw_high && saw_low);
      UINTN latest = wrap ? max_low : max_raw;
      next_num = (latest == 0U) ? 1U : ((latest < 9999U) ? (latest + 1U) : 1U);

      if (count < GU_MAX_RUN_LOGS)
        break;

      {
        UINT16 newest[GU_MAX_RUN_LOGS - 1U];
        UINT16 del_list[GU_PRUNE_BATCH];
        UINTN keep_cnt = 0, del_cnt = 0;

        if (dir->SetPosition)
          (VOID)dir->SetPosition(dir, 0);

        for (;;) {
          UINTN sz = sizeof(u.raw);
          EFI_STATUS rs = dir->Read(dir, &sz, u.raw);
          if (rs != EFI_SUCCESS || sz == 0)
            break;
          if (u.info.Attribute & EFI_FILE_DIRECTORY)
            continue;
          UINTN num = ParseRunLogNumber(u.info.FileName);
          if (num)
            InsertNewestRunLog(newest, &keep_cnt, num, wrap, del_list, &del_cnt);
        }

        if (del_cnt == 0)
          break;

        for (UINTN k = 0; k < del_cnt; k++) {
          CHAR16 del_name[12];
          EFI_FILE_PROTOCOL *oldf = NULL;
          FormatRunLogName(del_list[k], del_name);
          if (dir->Open(dir, &oldf, del_name,
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) == EFI_SUCCESS &&
              oldf) {
            if (oldf->Delete)
              (VOID)oldf->Delete(oldf);
            else
              (VOID)oldf->Close(oldf);
          }
        }
        if (del_cnt < GU_PRUNE_BATCH)
          break;
      }
    }
  }

  /* Verify candidate is free; if dir->Read wasn't supported, fall back to bounded scan. */
  if (dir_read_ok) {
    FormatRunLogName(next_num, chosen_name);
    EFI_FILE_PROTOCOL *existing = NULL;
    EFI_STATUS probe = dir->Open(dir, &existing, chosen_name, EFI_FILE_MODE_READ, 0);
    if (probe == EFI_SUCCESS && existing) {
      (VOID)existing->Close(existing);
      dir_read_ok = FALSE;
    } else if (probe == EFI_NOT_FOUND) {
      return dir->Open(
          dir, out_log, chosen_name,
          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    }
  }

  for (UINTN slot = 1; slot <= GU_MAX_RUN_LOGS; slot++) {
    FormatRunLogName(slot, chosen_name);
    EFI_FILE_PROTOCOL *existing = NULL;
    EFI_STATUS s = dir->Open(dir, &existing, chosen_name, EFI_FILE_MODE_READ, 0);
    if (s == EFI_SUCCESS && existing) {
      s = existing->Close(existing);
      if (s != EFI_SUCCESS)
        return s;
      continue;
    }
    if (s != EFI_NOT_FOUND)
      return s;
    return dir->Open(
        dir, out_log, chosen_name,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  }
  return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS GuBootOpen(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {

  if (Table && Root)
    return EFI_ALREADY_STARTED;

  if (!image || !t || !t->BootServices)
    return EFI_INVALID_PARAMETER;

  Table = t;
  Self = image;
  MenuLang = GU_LANG_EN;
  MenuLangExplicit = FALSE;
  MenuCsmStatus = GU_CSM_DISABLED;

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

  CHAR16 log_name[12];
  s = OpenRotatedRunLog(dir, log_name, &Log);
  if (s == EFI_SUCCESS && Log) {
    UINTN cap = sizeof(ActiveLogName) / sizeof(ActiveLogName[0]);
    UINTN d = GuStrCopy(ActiveLogName, cap,
                        is_ad102_dir ? L"\\AD102-UNLOCK\\" : L"\\GPU-UNLOCK\\");
    (VOID)GuStrCopy(ActiveLogName + d, cap - d, log_name);
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
    if (GuAssets[i].firmware.data) {
      GuFree((VOID *)GuAssets[i].firmware.data);
      GuAssets[i].firmware.data = NULL;
      GuAssets[i].firmware.size = 0;
      GuAssets[i].name = NULL;
    }
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

static UINT16 GuReadLe16(const UINT8 *b, UINTN off) {
  return (UINT16)((UINT32)b[off] | ((UINT32)b[off + 1U] << 8));
}

static UINT32 GuReadLe32(const UINT8 *b, UINTN off) {
  return (UINT32)b[off] | ((UINT32)b[off + 1U] << 8) |
         ((UINT32)b[off + 2U] << 16) | ((UINT32)b[off + 3U] << 24);
}

/* Accept small legitimate EFI chainloaders such as VeraCrypt DcsBoot.efi (~24 KiB)
   while enforcing PE32+ x64 EFI Application header integrity. */
static BOOLEAN ValidateWindowsPeImage(const VOID *data, UINTN size) {
  const UINT8 *b = (const UINT8 *)data;
  UINT32 lfanew;
  UINT16 machine;
  UINT16 opt_sz;
  UINT16 magic;
  UINT16 subsys;

  if (!b || size < 4096U || size > 33554432U)
    return FALSE;
  if (b[0] != 'M' || b[1] != 'Z')
    return FALSE;
  lfanew = GuReadLe32(b, 0x3CU);
  if (lfanew < 0x40U || (UINTN)lfanew > size ||
      size - (UINTN)lfanew < 24U + 72U)
    return FALSE;
  if (b[lfanew] != 'P' || b[lfanew + 1U] != 'E' || b[lfanew + 2U] != 0 ||
      b[lfanew + 3U] != 0)
    return FALSE;
  machine = GuReadLe16(b, (UINTN)lfanew + 4U);
  if (machine != 0x8664U)
    return FALSE;
  opt_sz = GuReadLe16(b, (UINTN)lfanew + 20U);
  if (opt_sz < 72U || size - (UINTN)lfanew - 24U < (UINTN)opt_sz)
    return FALSE;
  magic = GuReadLe16(b, (UINTN)lfanew + 24U);
  if (magic != 0x020BU)
    return FALSE;
  subsys = GuReadLe16(b, (UINTN)lfanew + 24U + 68U);
  return subsys == 10U ? TRUE : FALSE;
}

static EFI_STATUS ReadWindowsBinary(EFI_FILE_PROTOCOL *root, const CHAR16 *name,
                                    UINTN *file_size, VOID **data) {
  VOID *p = NULL;
  UINTN sz = 0;
  EFI_STATUS s = ReadFileData(root, name, 4096U, 33554432U, &sz, &p);
  if (s != EFI_SUCCESS) {
    if (file_size)
      *file_size = 0;
    *data = NULL;
    return s;
  }
  if (!ValidateWindowsPeImage(p, sz)) {
    GuFree(p);
    *data = NULL;
    if (file_size)
      *file_size = 0;
    return EFI_COMPROMISED_DATA;
  }
  if (file_size)
    *file_size = sz;
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

/* Prints prefix, a two-digit number and suffix in a single buffer. */
static VOID GuBootSayCountdown(const CHAR16 *prefix, UINTN value, const CHAR16 *suffix) {
  CHAR16 line[128];
  if (value > 99)
    value = 99;
  UINTN pos = GuStrCopy(line, 128U, prefix);
  if (pos + 2U < 128U) {
    line[pos++] = (CHAR16)(L'0' + value / 10);
    line[pos++] = (CHAR16)(L'0' + value % 10);
    line[pos] = L'\0';
  }
  (VOID)GuStrCopy(line + pos, 128U - pos, suffix);
  GuBootSay(line);
}

static VOID GuPrintCountdownPrompt(BOOLEAN hide_s3, UINTN sec, BOOLEAN overwrite) {
  CHAR16 prefix[64];
  UINTN p = 0U;
  if (overwrite)
    prefix[p++] = L'\r';
  (VOID)GuStrCopy(prefix + p, 64U - p, GuBootUiStr(GU_STR_TIMER_PFX));
  GuBootSayCountdown(prefix, sec,
                     GuBootUiStr(hide_s3 ? GU_STR_TIMER_SFX_NOS3 : GU_STR_TIMER_SFX_ALL));
}

static UINT32 GuReadLangConfig(VOID) {
  if (!Root || !Root->Open)
    return GU_LANG_EN;
  EFI_FILE_PROTOCOL *cfg = NULL;
  EFI_STATUS cs = Root->Open(Root, &cfg, L"EFI\\BOOT\\lang.cfg", EFI_FILE_MODE_READ, 0);
  if (cs != EFI_SUCCESS || !cfg)
    return GU_LANG_EN;
  CHAR8 buf[16];
  UINTN sz = sizeof(buf) - 1U;
  memset(buf, 0, sizeof(buf));
  if (cfg->Read) {
    if (cfg->Read(cfg, &sz, buf) != EFI_SUCCESS)
      sz = 0U;
  } else {
    sz = 0U;
  }
  if (cfg->Close)
    cfg->Close(cfg);
  if (sz > sizeof(buf) - 1U)
    sz = sizeof(buf) - 1U;
  buf[sz] = '\0';
  UINTN start = 0U;
  if (sz >= 3U && (UINT8)buf[0] == 0xEFU && (UINT8)buf[1] == 0xBBU && (UINT8)buf[2] == 0xBFU)
    start = 3U;
  while (start < sz && (buf[start] == ' ' || buf[start] == '\t' || buf[start] == '\r' || buf[start] == '\n'))
    start++;
  if (sz >= start + 2U) {
    CHAR8 c0 = (buf[start] >= 'A' && buf[start] <= 'Z') ? (CHAR8)(buf[start] - 'A' + 'a') : buf[start];
    CHAR8 c1 = (buf[start + 1U] >= 'A' && buf[start + 1U] <= 'Z') ? (CHAR8)(buf[start + 1U] - 'A' + 'a') : buf[start + 1U];
    if (c0 == 'z' && c1 == 'h')
      return GU_LANG_ZH;
    if (c0 == 'd' && c1 == 'e')
      return GU_LANG_DE;
  }
  return GU_LANG_EN;
}

static VOID GuRenderChooseMenu(BOOLEAN hide_s3) {
  BOOLEAN has_gop = (GopCon.enabled && GopCon.gop != NULL);
  if (has_gop) {
    GuGopConClear(&GopCon, Table);
  }
  GuBootSay(GuBootUiStr(GU_STR_SEP_LINE));
  GuBootSay(GuBootUiStr(GU_STR_MENU_TITLE));
  GuBootSay(GuBootUiStr(GU_STR_SEP_LINE));
  GuBootSay(GuBootUiStr(GU_STR_MENU_TIMER_HDR));
  GuBootSay(GuBootUiStr(has_gop ? GU_STR_MENU_HINT_LINE : GU_STR_MENU_HINT_ASCII));
  GuBootSay(GuBootUiStr(GU_STR_SEP_LINE));
  UINT32 csm_str = GU_STR_CSM_OFF;
  if (MenuCsmStatus == GU_CSM_VIDEO_ACTIVE)
    csm_str = GU_STR_CSM_VIDEO_ACTIVE;
  else if (MenuCsmStatus == GU_CSM_LEGACY_BIOS)
    csm_str = GU_STR_CSM_LEGACY_BIOS;
  GuBootSay(GuBootUiStr(csm_str));
  if (GuIsUnlocked()) {
    GuBootSay(GuBootUiStr(GU_STR_ALREADY_UNLOCKED));
  }
  GuBootSay(GuBootUiStr(GU_STR_SEP_LINE));
  if (hide_s3) {
    GuBootSay(GuBootUiStr(GU_STR_OPT_U_NOS3));
    GuBootSay(GuBootUiStr(GU_STR_OPT_I_NOS3));
  } else {
    GuBootSay(GuBootUiStr(GU_STR_OPT_U_STD));
    GuBootSay(GuBootUiStr(GU_STR_OPT_P));
    GuBootSay(GuBootUiStr(GU_STR_OPT_I_S3));
  }
  GuBootSay(GuBootUiStr(GU_STR_OPT_R));
  GuBootSay(GuBootUiStr(GU_STR_OPT_S));
  GuBootSay(GuBootUiStr(GU_STR_SEP_LINE));
  GuBootSay(L"\r\n");
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

  if (GopCon.enabled) {
    if (!GopCon.gop && GopConNeedsInit) {
      GopConNeedsInit = FALSE;
      (VOID)GuGopConAttach(&GopCon, Table, TRUE);
    }
    if (GopCon.gop) {
      if (!MenuLangExplicit)
        MenuLang = GuReadLangConfig();
      GuLog("Boot console: GOP %ux%u scaled %ux lang=%u csm=%u\n",
            (unsigned)GopCon.h_res, (unsigned)GopCon.v_res,
            (unsigned)GopCon.scale, (unsigned)MenuLang, (unsigned)MenuCsmStatus);
    }
  }

  GuRenderChooseMenu(hide_s3);

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
        GuBootSay(GuBootUiStr(hide_s3 ? GU_STR_PAUSED_NOS3 : GU_STR_PAUSED_ALL));
        continue;
      }

      CHAR16 c = key.UnicodeChar;
      if (c >= L'a' && c <= L'z')
        c = (CHAR16)(c - L'a' + L'A');

      if (c == L'L' && GopCon.enabled && GopCon.gop) {
        MenuLang = (MenuLang + 1U) % GU_LANG_COUNT;
        MenuLangExplicit = TRUE;
        GuLog("Boot menu language switched: lang=%u\n", (unsigned)MenuLang);
        GuRenderChooseMenu(hide_s3);
        if (paused) {
          GuBootSay(GuBootUiStr(hide_s3 ? GU_STR_PAUSED_NOS3 : GU_STR_PAUSED_ALL));
        } else {
          tick = 0;
        }
        continue;
      }

      if (key.ScanCode == 0x17 || c == 27)
        c = L'S'; /* Esc */
      if (c == L'U' || c == L'I' || c == L'R' || c == L'S' || (c == L'P' && !hide_s3)) {
        GuBootSay(L"\r\n");
        timed_out = FALSE;
        *choice = c;
        if (GopCon.enabled && GopCon.gop)
          GuGopConClear(&GopCon, Table);
        return EFI_SUCCESS;
      }

    } else if (s != EFI_NOT_READY) {
      if (GopCon.enabled && GopCon.gop)
        GuGopConClear(&GopCon, Table);
      return s;
    }

    s = Table->BootServices->Stall(100000);
    if (s != EFI_SUCCESS) {
      if (GopCon.enabled && GopCon.gop)
        GuGopConClear(&GopCon, Table);
      return s;
    }

    if (!paused)
      tick++;
  }

  if (timed_out) {
    GuPrintCountdownPrompt(hide_s3, 0, TRUE);
    GuBootSay(GuBootUiStr(GU_STR_TIMEOUT_HDR));
    GuBootSay(ActiveLogName);
    GuBootSay(GuBootUiStr(GU_STR_TIMEOUT_FTR));
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
  GuGopConDetach(&GopCon);
  GopCon.enabled = FALSE;

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

static size_t GuFormat(char *Destination, size_t Capacity, const char *Format, ...) {
  va_list args;
  va_start(args, Format);
  size_t n = GuFormatV(Destination, Capacity, Format, args);
  va_end(args);
  return n;
}

static VOID GuBootAppendBuffer(const CHAR8 *Buffer, UINTN Bytes) {
  if (!Buffer || Bytes == 0 || !Table || !Table->BootServices || !OwnVolume)
    return;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_FILE_PROTOCOL *root = NULL;
  EFI_STATUS s = Table->BootServices->HandleProtocol(OwnVolume, &FsGuid, (VOID **)&fs);
  if (EFI_ERROR(s) || !fs)
    return;
  s = fs->OpenVolume(fs, &root);
  if (EFI_ERROR(s) || !root)
    return;
  EFI_FILE_PROTOCOL *log = NULL;
  s = root->Open(root, &log, ActiveLogName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (s == EFI_SUCCESS && log) {
    (VOID)log->SetPosition(log, ~(UINT64)0);
    UINTN bytes = Bytes;
    (VOID)log->Write(log, &bytes, (VOID *)Buffer);
    (VOID)log->Flush(log);
    (VOID)log->Close(log);
  }
  (VOID)root->Close(root);
}

VOID GuBootAppendLog(const char *Format, ...) {
  if (!Format)
    return;
  CHAR8 buffer[512];
  va_list args;
  va_start(args, Format);
  size_t n = GuFormatV(buffer, sizeof(buffer), Format, args);
  va_end(args);
  if (n == (size_t)-1 || n == 0 || n >= sizeof(buffer))
    return;
  if (Log) {
    UINTN bytes = n;
    (VOID)Log->Write(Log, &bytes, buffer);
    (VOID)Log->Flush(Log);
    return;
  }
  GuBootAppendBuffer(buffer, (UINTN)n);
}

static VOID GuBootLogError(EFI_STATUS ErrorStatus) {
  CHAR8 buffer[256];
  size_t n = GuFormat(buffer, sizeof(buffer),
                      "\n[FATAL ERROR] StartImage(Windows) returned failure: %#llx!\n",
                      (unsigned long long)ErrorStatus);
  if (n != (size_t)-1 && n < sizeof(buffer)) {
    GuBootAppendBuffer(buffer, (UINTN)n);
  }
}

EFI_STATUS GuBootWindows(VOID) {

  if (!Windows)
    return EFI_NOT_READY;

  /* Plan C: Install ExitBootServices Sandwich Hook right before starting Windows */
  (VOID)GuAcpiInstallEbsHook(Table);

  /* Apply Shadow ACPI tables immediately before StartImage(Windows) */
  (VOID)GuAcpiApplyShadowConfigTable();

  GuLog("GuBootWindows: Calling StartImage(Windows=%p)...\n", Windows);
  GuGopConClose(&GopCon, Table, TRUE);
  (VOID)GuBootClose();

  EFI_STATUS s = Table->BootServices->StartImage(Windows, NULL, NULL);

  /* If StartImage returns (error or exit), cleanly uninstall hook and restore config table */
  (VOID)GuAcpiUninstallEbsHook(Table);
  (VOID)GuAcpiRestoreConfigTable();

  if (EFI_ERROR(s)) {
    GuBootLogError(s);
  }

  return s;
}

VOID GuBootHalt(VOID) {
  /* Ensure any active ExitBootServices hook is cleanly uninstalled on halt */
  (VOID)GuAcpiUninstallEbsHook(Table);

  if (Table && Table->ConIn && Table->ConIn->ReadKeyStroke) {
    EFI_INPUT_KEY discard;
    while (Table->ConIn->ReadKeyStroke(Table->ConIn, &discard) == EFI_SUCCESS)
      ;
  }
  if (!GopCon.enabled && Table && Table->ConOut && Table->ConOut->Reset) {
    Table->ConOut->Reset(Table->ConOut, FALSE);
  }
  GuBootUnsilence();

  GuBootSay(GuBootUiStr(GU_STR_HALT_HDR));
  GuBootSay(ActiveLogName);
  GuBootSay(GuBootUiStr(GU_STR_HALT_FTR));

  /* Live countdown for the automatic reboot. A key press ends it early. */
#define GU_HALT_REBOOT_SEC 15U
  BOOLEAN key_pressed = FALSE;
  for (UINTN sec = GU_HALT_REBOOT_SEC; sec > 0 && !key_pressed; sec--) {
    GuBootSayCountdown(GuBootUiStr(sec == GU_HALT_REBOOT_SEC ? GU_STR_REBOOT_PFX_INIT
                                                             : GU_STR_REBOOT_PFX_OVER),
                       sec, GuBootUiStr(GU_STR_REBOOT_SFX));
    for (UINTN tick = 0; tick < 10 && !key_pressed; tick++) {
      EFI_INPUT_KEY key;
      if (Table && Table->ConIn && Table->ConIn->ReadKeyStroke &&
          Table->ConIn->ReadKeyStroke(Table->ConIn, &key) == EFI_SUCCESS)
        key_pressed = TRUE;
      else if (Table && Table->BootServices && Table->BootServices->Stall)
        Table->BootServices->Stall(100000);
    }
  }

  GuBootSay(GuBootUiStr(GU_STR_REBOOT_NOW));
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
