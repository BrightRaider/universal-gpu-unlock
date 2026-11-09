/* SPDX-License-Identifier: GPL-2.0-only */
#include "port.h"
#include "compat/compat.h"
#include "boot_io.h"
#include "profile.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

static EFI_GUID GlobalGuid = {0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};
static EFI_GUID LoadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID PathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID InfoGuid = EFI_FILE_INFO_ID;

#ifndef LOAD_OPTION_ACTIVE
#define LOAD_OPTION_ACTIVE 0x00000001U
#endif

#define GU_DRIVER_SCAN_SLOTS 256U
#define GU_DRIVER_ORDER_MAX 128U
#define GU_DRIVER_VAR_ATTRS (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

static const CHAR16 DriverDescription[] = L"AdaUnlock";
static const CHAR16 DriverFile[] = L"\\EFI\\Drivers\\AdaUnlockDxe.efi";

static UINTN GetDevicePathSize(const EFI_DEVICE_PATH_PROTOCOL *dp) {
  if (!dp) return 0;
  const EFI_DEVICE_PATH_PROTOCOL *node = dp;
  while (node->Type != 0x7f || node->SubType != 0xff) {
    UINTN len = (UINTN)node->Length[0] | ((UINTN)node->Length[1] << 8);
    if (len < 4) return 0;
    node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + len);
  }
  return ((const UINT8 *)node - (const UINT8 *)dp) + 4;
}

static VOID SetDriverSlot(CHAR16 name[11], UINTN slot) {
  static const CHAR16 hex[] = L"0123456789ABCDEF";
  for (UINTN digit = 0; digit < 4; digit++) name[9 - digit] = hex[(slot >> (4 * digit)) & 0xf];
}

/* An entry is ours only if both its description and its file path are ours. */
static BOOLEAN IsOwnOption(const UINT8 *option, UINTN size) {
  UINTN header = sizeof(UINT32) + sizeof(UINT16);
  if (size < header + sizeof(DriverDescription) + sizeof(DriverFile)) return FALSE;
  if (memcmp(option + header, DriverDescription, sizeof(DriverDescription))) return FALSE;
  UINTN list_bytes = option[4] + ((UINTN)option[5] << 8);
  UINTN list_at = header + sizeof(DriverDescription);
  if (list_bytes > size - list_at || list_bytes < sizeof(DriverFile)) return FALSE;
  for (UINTN at = 0; at + sizeof(DriverFile) <= list_bytes; at++)
    if (!memcmp(option + list_at + at, DriverFile, sizeof(DriverFile))) return TRUE;
  return FALSE;
}

/* A read error aborts the scan: an unreadable slot must never count as free. */
static EFI_STATUS ScanDriverOptions(EFI_RUNTIME_SERVICES *rs, BOOLEAN own[GU_DRIVER_SCAN_SLOTS],
                                    UINTN *first_own, UINTN *first_free) {
  *first_own = *first_free = GU_DRIVER_SCAN_SLOTS;
  for (UINTN slot = 0; slot < GU_DRIVER_SCAN_SLOTS; slot++) {
    CHAR16 name[] = L"Driver0000";
    UINT8 option[1024];
    UINTN size = sizeof(option);
    UINT32 attrs = 0;
    SetDriverSlot(name, slot);
    own[slot] = FALSE;
    EFI_STATUS s = rs->GetVariable(name, &GlobalGuid, &attrs, &size, option);
    if (s == EFI_NOT_FOUND) {
      if (*first_free == GU_DRIVER_SCAN_SLOTS) *first_free = slot;
      continue;
    }
    if (s == EFI_BUFFER_TOO_SMALL) continue; /* Larger than any entry we write. */
    if (s != EFI_SUCCESS) return s;
    if (IsOwnOption(option, size)) {
      own[slot] = TRUE;
      if (*first_own == GU_DRIVER_SCAN_SLOTS) *first_own = slot;
    }
  }
  return EFI_SUCCESS;
}

/* Only a missing variable means "empty"; any other failure must not rewrite it. */
static EFI_STATUS ReadDriverOrder(EFI_RUNTIME_SERVICES *rs, UINT16 order[GU_DRIVER_ORDER_MAX], UINTN *count) {
  UINTN size = GU_DRIVER_ORDER_MAX * sizeof(UINT16);
  UINT32 attrs = 0;
  EFI_STATUS s = rs->GetVariable(L"DriverOrder", &GlobalGuid, &attrs, &size, order);
  *count = 0;
  if (s == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (s != EFI_SUCCESS) return s;
  if (size % sizeof(UINT16)) return EFI_COMPROMISED_DATA;
  *count = size / sizeof(UINT16);
  return EFI_SUCCESS;
}

static EFI_STATUS DriverFilePresentOnVolume(EFI_BOOT_SERVICES *bs, EFI_HANDLE volume, const CHAR16 *path) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
  EFI_STATUS s = bs->HandleProtocol(volume, &FsGuid, (VOID **)&fs);
  if (s != EFI_SUCCESS || !fs) return EFI_UNSUPPORTED;
  s = fs->OpenVolume(fs, &root);
  if (s != EFI_SUCCESS || !root) return EFI_DEVICE_ERROR;
  s = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
  if (s == EFI_SUCCESS && file) file->Close(file);
  root->Close(root);
  return s;
}

static EFI_STATUS CopyFileBetweenVolumes(EFI_BOOT_SERVICES *bs,
                                         EFI_HANDLE src_vol, const CHAR16 *src_path,
                                         EFI_HANDLE dst_vol, const CHAR16 *dst_dir, const CHAR16 *dst_file) {
  if (!bs || !src_vol || !dst_vol || !src_path || !dst_file) return EFI_INVALID_PARAMETER;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *src_fs = NULL, *dst_fs = NULL;
  EFI_FILE_PROTOCOL *src_root = NULL, *dst_root = NULL, *src_file = NULL, *dst_dir_proto = NULL, *dst_file_proto = NULL;
  EFI_STATUS s;

  s = bs->HandleProtocol(src_vol, &FsGuid, (VOID **)&src_fs);
  if (s != EFI_SUCCESS || !src_fs) return s;
  s = src_fs->OpenVolume(src_fs, &src_root);
  if (s != EFI_SUCCESS || !src_root) return s;

  s = src_root->Open(src_root, &src_file, (CHAR16 *)src_path, EFI_FILE_MODE_READ, 0);
  if (s != EFI_SUCCESS || !src_file) {
    src_root->Close(src_root);
    return s;
  }

  UINT64 info_buf[128];
  UINTN info_sz = sizeof(info_buf);
  s = src_file->GetInfo(src_file, &InfoGuid, &info_sz, info_buf);
  if (s != EFI_SUCCESS) {
    src_file->Close(src_file);
    src_root->Close(src_root);
    return s;
  }

  EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
  UINTN file_sz = (UINTN)info->FileSize;
  VOID *buf = NULL;
  s = bs->AllocatePool(EfiBootServicesData, file_sz, (VOID **)&buf);
  if (s != EFI_SUCCESS || !buf) {
    src_file->Close(src_file);
    src_root->Close(src_root);
    return EFI_OUT_OF_RESOURCES;
  }

  UINTN read_bytes = file_sz;
  s = src_file->Read(src_file, &read_bytes, buf);
  src_file->Close(src_file);
  src_root->Close(src_root);
  if (s != EFI_SUCCESS || read_bytes != file_sz) {
    bs->FreePool(buf);
    return EFI_DEVICE_ERROR;
  }

  s = bs->HandleProtocol(dst_vol, &FsGuid, (VOID **)&dst_fs);
  if (s != EFI_SUCCESS || !dst_fs) {
    bs->FreePool(buf);
    return s;
  }
  s = dst_fs->OpenVolume(dst_fs, &dst_root);
  if (s != EFI_SUCCESS || !dst_root) {
    bs->FreePool(buf);
    return s;
  }

  if (dst_dir && *dst_dir) {
    s = dst_root->Open(dst_root, &dst_dir_proto, (CHAR16 *)dst_dir,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                       EFI_FILE_DIRECTORY);
    if (s != EFI_SUCCESS || !dst_dir_proto) {
      dst_root->Close(dst_root);
      bs->FreePool(buf);
      return s;
    }
  } else {
    dst_dir_proto = dst_root;
  }

  s = dst_dir_proto->Open(dst_dir_proto, &dst_file_proto, (CHAR16 *)dst_file,
                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (s == EFI_SUCCESS && dst_file_proto) {
    UINTN write_bytes = file_sz;
    s = dst_file_proto->Write(dst_file_proto, &write_bytes, buf);
    dst_file_proto->Close(dst_file_proto);
  }

  if (dst_dir_proto != dst_root) dst_dir_proto->Close(dst_dir_proto);
  dst_root->Close(dst_root);
  bs->FreePool(buf);
  return s;
}

static VOID StageArchAssetsToVolume(EFI_BOOT_SERVICES *bs, EFI_HANDLE src_vol, EFI_HANDLE dst_vol) {
  const GU_ARCH_ASSETS *arch = GuGetArchAssets();
  if (!arch || !src_vol || !dst_vol || src_vol == dst_vol) return;

  for (UINTN i = 0; i < GU_ASSET_COUNT; i++) {
    const CHAR16 *path = arch->entries[i].path;
    if (!path) continue;
    const CHAR16 *fn = path;
    while (*fn == L'\\') fn++;
    const CHAR16 *slash = fn;
    while (*slash && *slash != L'\\') slash++;
    if (*slash == L'\\') {
      CHAR16 dir[64];
      UINTN dir_len = slash - path;
      if (dir_len < 63) {
        for (UINTN k = 0; k < dir_len; k++) dir[k] = path[k];
        dir[dir_len] = 0;
        const CHAR16 *name = slash + 1;
        (VOID)CopyFileBetweenVolumes(bs, src_vol, path, dst_vol, dir, name);
      }
    }
  }
}

static EFI_HANDLE LocateInternalEspVolume(EFI_BOOT_SERVICES *bs) {
  EFI_HANDLE target = GuBootGetTargetEspVolume();
  if (target) return target;

  EFI_HANDLE *h = NULL;
  UINTN count = 0;
  EFI_STATUS s = bs->LocateHandleBuffer(ByProtocol, &FsGuid, NULL, &count, &h);
  if (s != EFI_SUCCESS || !h) return NULL;

  for (UINTN i = 0; i < count; i++) {
    EFI_DEVICE_PATH_PROTOCOL *p = NULL;
    if (bs->HandleProtocol(h[i], &PathGuid, (VOID **)&p) != EFI_SUCCESS || !p) continue;

    /* Check for media/disk path that is not USB */
    BOOLEAN is_usb = FALSE, is_media = FALSE;
    const EFI_DEVICE_PATH_PROTOCOL *node = p;
    while (node->Type != 0x7f || node->SubType != 0xff) {
      UINTN len = (UINTN)node->Length[0] | ((UINTN)node->Length[1] << 8);
      if (len < 4) break;
      if (node->Type == 0x03 && node->SubType == 0x05) is_usb = TRUE; /* MESSAGING / MSG_USB_DP */
      if (node->Type == 0x04) is_media = TRUE; /* MEDIA_DEVICE_PATH */
      node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + len);
    }
    if (is_media && !is_usb) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
      EFI_FILE_PROTOCOL *root = NULL, *mgfw = NULL;
      if (bs->HandleProtocol(h[i], &FsGuid, (VOID **)&fs) == EFI_SUCCESS && fs &&
          fs->OpenVolume(fs, &root) == EFI_SUCCESS && root) {
        if (root->Open(root, &mgfw, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", EFI_FILE_MODE_READ, 0) == EFI_SUCCESS && mgfw) {
          mgfw->Close(mgfw);
          root->Close(root);
          target = h[i];
          break;
        }
        root->Close(root);
      }
    }
  }
  bs->FreePool(h);
  return target;
}

EFI_STATUS GuInstallDriverOption(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
  if (!image || !t || !t->BootServices || !t->RuntimeServices) return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *bs = t->BootServices;
  EFI_RUNTIME_SERVICES *rs = t->RuntimeServices;

  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
  EFI_STATUS s = bs->HandleProtocol(image, &LoadedGuid, (VOID **)&loaded);
  if (s != EFI_SUCCESS || !loaded || !loaded->DeviceHandle) return EFI_UNSUPPORTED;

  /* Verify source driver file exists on the boot medium */
  const CHAR16 *src_driver_path = DriverFile;
  s = DriverFilePresentOnVolume(bs, loaded->DeviceHandle, src_driver_path);
  if (s != EFI_SUCCESS) {
    src_driver_path = L"\\EFI\\BOOT\\AdaUnlockDxe.efi";
    s = DriverFilePresentOnVolume(bs, loaded->DeviceHandle, src_driver_path);
    if (s != EFI_SUCCESS) return s;
  }

  EFI_HANDLE install_vol = loaded->DeviceHandle;
  EFI_HANDLE esp_vol = LocateInternalEspVolume(bs);
  BOOLEAN is_internal = FALSE;

  if (esp_vol && esp_vol != loaded->DeviceHandle) {
    /* Copy AdaUnlockDxe.efi to the Internal NVMe-ESP */
    s = CopyFileBetweenVolumes(bs, loaded->DeviceHandle, src_driver_path,
                               esp_vol, L"\\EFI\\Drivers", L"AdaUnlockDxe.efi");
    if (s == EFI_SUCCESS) {
      install_vol = esp_vol;
      is_internal = TRUE;
      GuLog("GuInstallDriverOption: Successfully deployed AdaUnlockDxe.efi to Internal ESP\\EFI\\Drivers\n");
      /* Also stage active architecture assets to the Internal ESP */
      StageArchAssetsToVolume(bs, loaded->DeviceHandle, esp_vol);
    } else {
      GuLog("GuInstallDriverOption: Warning: ESP copy failed (s=%#llx), falling back to USB medium\n",
            (unsigned long long)s);
    }
  }

  EFI_DEVICE_PATH_PROTOCOL *vol_path = NULL;
  s = bs->HandleProtocol(install_vol, &PathGuid, (VOID **)&vol_path);
  if (s != EFI_SUCCESS || !vol_path) return EFI_UNSUPPORTED;

  UINTN vol_size = GetDevicePathSize(vol_path);
  if (vol_size < 4) return EFI_UNSUPPORTED;
  UINTN vol_nodes_size = vol_size - 4; // exclude end node

  // Read everything first so a failure leaves NVRAM untouched.
  UINT16 order[GU_DRIVER_ORDER_MAX];
  UINTN count = 0, first_own = 0, first_free = 0;
  BOOLEAN own[GU_DRIVER_SCAN_SLOTS];
  s = ReadDriverOrder(rs, order, &count);
  if (s != EFI_SUCCESS) return s;
  s = ScanDriverOptions(rs, own, &first_own, &first_free);
  if (s != EFI_SUCCESS) return s;
  BOOLEAN reinstall = first_own != GU_DRIVER_SCAN_SLOTS;
  UINTN slot = reinstall ? first_own : first_free;
  if (slot == GU_DRIVER_SCAN_SLOTS) return EFI_OUT_OF_RESOURCES;

  UINTN path_bytes = sizeof(DriverFile);
  UINTN file_node_len = 4 + path_bytes;
  UINTN total_dp_len = vol_nodes_size + file_node_len + 4; // plus end node
  UINTN desc_bytes = sizeof(DriverDescription);

  UINTN load_opt_size = sizeof(UINT32) + sizeof(UINT16) + desc_bytes + total_dp_len;
  UINT8 *buf = NULL;
  s = bs->AllocatePool(EfiBootServicesData, load_opt_size, (VOID **)&buf);
  if (s != EFI_SUCCESS || !buf) return EFI_OUT_OF_RESOURCES;

  UINT8 *p = buf;
  // 1. Attributes
  *(UINT32 *)p = LOAD_OPTION_ACTIVE; p += sizeof(UINT32);
  // 2. FilePathListLength
  *(UINT16 *)p = (UINT16)total_dp_len; p += sizeof(UINT16);
  // 3. Description (null-terminated)
  for (UINTN i = 0; i < desc_bytes; i++) p[i] = ((const UINT8 *)DriverDescription)[i];
  p += desc_bytes;

  // 4. FilePathList: Volume Device Path (without end node)
  for (UINTN i = 0; i < vol_nodes_size; i++) p[i] = ((const UINT8 *)vol_path)[i];
  p += vol_nodes_size;

  // File Path Node: Type=4 (Media), SubType=4 (FilePath)
  p[0] = 0x04; p[1] = 0x04;
  p[2] = (UINT8)(file_node_len & 0xff);
  p[3] = (UINT8)((file_node_len >> 8) & 0xff);
  p += 4;
  for (UINTN i = 0; i < path_bytes; i++) p[i] = ((const UINT8 *)DriverFile)[i];
  p += path_bytes;

  // End Node: Type=0x7f, SubType=0xff, Len=4
  p[0] = 0x7f; p[1] = 0xff; p[2] = 0x04; p[3] = 0x00;

  CHAR16 name[] = L"Driver0000";
  SetDriverSlot(name, slot);
  s = rs->SetVariable(name, &GlobalGuid, GU_DRIVER_VAR_ATTRS, load_opt_size, buf);
  bs->FreePool(buf);
  if (s != EFI_SUCCESS) return s;

  // Prepend our slot to DriverOrder unless it is already listed.
  BOOLEAN listed = FALSE;
  for (UINTN i = 0; i < count; i++) {
    if (order[i] == slot) { listed = TRUE; break; }
  }
  if (!listed) {
    UINT16 new_order[GU_DRIVER_ORDER_MAX + 1];
    new_order[0] = (UINT16)slot;
    for (UINTN i = 0; i < count; i++) new_order[i + 1] = order[i];
    s = rs->SetVariable(L"DriverOrder", &GlobalGuid, GU_DRIVER_VAR_ATTRS, (count + 1) * sizeof(UINT16), new_order);
    if (s != EFI_SUCCESS && !reinstall) (VOID)rs->SetVariable(name, &GlobalGuid, GU_DRIVER_VAR_ATTRS, 0, NULL);
  }

  GuLog("GuInstallDriverOption: Success! Registered Driver%04x in DriverOrder (internal_esp=%u)\n",
        (unsigned)slot, (unsigned)is_internal);
  return s;
}

EFI_STATUS GuRemoveDriverOption(EFI_SYSTEM_TABLE *t) {
  if (!t || !t->RuntimeServices) return EFI_INVALID_PARAMETER;
  EFI_RUNTIME_SERVICES *rs = t->RuntimeServices;

  UINT16 order[GU_DRIVER_ORDER_MAX], kept[GU_DRIVER_ORDER_MAX];
  UINTN count = 0, kept_count = 0, first_own = 0, first_free = 0;
  BOOLEAN own[GU_DRIVER_SCAN_SLOTS];
  EFI_STATUS s = ReadDriverOrder(rs, order, &count);
  if (s != EFI_SUCCESS) return s;
  s = ScanDriverOptions(rs, own, &first_own, &first_free);
  if (s != EFI_SUCCESS) return s;
  if (first_own == GU_DRIVER_SCAN_SLOTS) return EFI_NOT_FOUND;

  for (UINTN i = 0; i < count; i++) {
    if (order[i] >= GU_DRIVER_SCAN_SLOTS || !own[order[i]]) kept[kept_count++] = order[i];
  }
  if (kept_count != count) {
    s = rs->SetVariable(L"DriverOrder", &GlobalGuid, GU_DRIVER_VAR_ATTRS,
                        kept_count * sizeof(UINT16), kept_count ? kept : NULL);
    if (s != EFI_SUCCESS) return s;
  }

  EFI_STATUS result = EFI_SUCCESS;
  for (UINTN slot = first_own; slot < GU_DRIVER_SCAN_SLOTS; slot++) {
    if (!own[slot]) continue;
    CHAR16 name[] = L"Driver0000";
    SetDriverSlot(name, slot);
    s = rs->SetVariable(name, &GlobalGuid, GU_DRIVER_VAR_ATTRS, 0, NULL);
    if (s != EFI_SUCCESS && result == EFI_SUCCESS) result = s;
  }

  /* Also clean up AdaUnlockDxe.efi from the Internal ESP if accessible */
  if (t->BootServices) {
    EFI_HANDLE esp_vol = LocateInternalEspVolume(t->BootServices);
    if (esp_vol) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
      EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
      if (t->BootServices->HandleProtocol(esp_vol, &FsGuid, (VOID **)&fs) == EFI_SUCCESS && fs &&
          fs->OpenVolume(fs, &root) == EFI_SUCCESS && root) {
        if (root->Open(root, &file, (CHAR16 *)DriverFile, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) == EFI_SUCCESS && file) {
          (VOID)file->Delete(file);
        }
        root->Close(root);
      }
    }
  }

  return result;
}
