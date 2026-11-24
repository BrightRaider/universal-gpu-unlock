/* SPDX-License-Identifier: GPL-2.0-only */
/* multi_gpu.c - Runs the unlock on every discovered GPU, shared by BOOTX64.EFI and the DXE driver. */
#include "multi_gpu.h"
#include "boot_io.h"
#include "profile.h"
#include "unlock_release.h"

static BOOLEAN Failed(EFI_STATUS s) {
  return s != EFI_SUCCESS || Gu.fault;
}

/* The loaded assets depend only on the architecture. */
static BOOLEAN SameAssets(const GU_DISCOVERED_DEVICE *a, const GU_DISCOVERED_DEVICE *b) {
  return a && b && a->is_ampere == b->is_ampere && a->is_turing == b->is_turing;
}

EFI_STATUS GuUnlockAllGpus(EFI_SYSTEM_TABLE *t, EFI_HANDLE image,
                           const GU_DISCOVERED_DEVICE *devices, UINTN count,
                           UINT32 options, const char *log_prefix,
                           const char **stage, UINTN *unlocked, UINTN *failed) {
  const GU_DISCOVERED_DEVICE *assets_for = devices;
  EFI_STATUS s = EFI_SUCCESS;
  EFI_STATUS last_refusal = EFI_NOT_FOUND;
  UINTN g;

  if (!t || !devices || !count || !stage || !unlocked || !failed) return EFI_INVALID_PARAMETER;
  if (!log_prefix) log_prefix = "";
  *unlocked = 0;
  *failed = 0;
  if (options & GU_UNLOCK_REGISTER_S3) {
    Gu.s3_persist_armed = TRUE;
  }

  for (g = 0; g < count; g++) {
    const GU_DISCOVERED_DEVICE *dev = &devices[g];
    const char *skip_reason = NULL;

    if (g > 0) {
      /* Binding resets Gu, so the previous GPU's state is gone from here on. */
      GuBindDevice(t->BootServices, image, dev);
      if (!SameAssets(dev, assets_for)) {
        *stage = "secondary-assets";
        s = GuBootLoadAssets();
        assets_for = Failed(s) ? NULL : dev;
        if (Failed(s)) skip_reason = "asset load";
      }
      if (!skip_reason) {
        *stage = "secondary-cold-profile";
        s = GuPrepare();
        if (Failed(s)) skip_reason = "prepare";
      }
    }

    if (skip_reason) {
      /* Asset load and GuPrepare only read from the GPU, so it is still stock.
       * Binding it again clears their fault flags, so they cannot fail the
       * handover of the GPUs that were unlocked. */
      GuLog("%sGPU [%u/%u] %s failed (%#llx); skipped, it stays stock\n", log_prefix,
            (unsigned)(g + 1), (unsigned)count, skip_reason, (unsigned long long)s);
      GuBindDevice(t->BootServices, image, dev);
      continue;
    }

    GuLog("%s--- Processing GPU [%u/%u]: DevID 0x%04x (%s) ---\n", log_prefix,
          (unsigned)(g + 1), (unsigned)count, (unsigned)dev->device_id, GuArchName(dev));

    if (Gu.already_unlocked) {
      GuLog("%sGPU [%u/%u] is already unlocked (SS0=0x88888888, SS1=0x8); skipping hardware unlock transaction\n",
            log_prefix, (unsigned)(g + 1), (unsigned)count);
      if (options & GU_UNLOCK_REGISTER_S3) {
        EFI_STATUS s3_ret = GuRegisterS3(t);
        if (EFI_ERROR(s3_ret)) {
          GuLog("%s[!] S3 Persistence registration returned %#llx\n", log_prefix, (unsigned long long)s3_ret);
        }
      }
      (*unlocked)++;
      if (g + 1 < count && !GuUnlockPrepareNextGpu()) {
        *stage = "next-gpu-reset";
        *failed = g;
        return EFI_NOT_READY;
      }
      continue;
    }

    s = GuExecuteUnlock(t, image, stage);
    if (Failed(s)) {
      if (!GuUnlockMutated() && !GuUnlockClaimed() && !Gu.started && !Gu.retained &&
          GuUnlockPrepareNextGpu()) {
        GuLog("%sGPU [%u/%u] refused at stage=%s before any change (%#llx); skipped, it stays stock\n",
              log_prefix, (unsigned)(g + 1), (unsigned)count, *stage, (unsigned long long)s);
        GuBindDevice(t->BootServices, image, dev);
        *failed = g;
        last_refusal = s != EFI_SUCCESS ? s : EFI_DEVICE_ERROR;
        continue;
      }
      *failed = g;
      return s != EFI_SUCCESS ? s : EFI_DEVICE_ERROR;
    }

    if (options & GU_UNLOCK_REGISTER_S3) {
      EFI_STATUS s3_ret = GuRegisterS3(t);
      if (EFI_ERROR(s3_ret)) {
        GuLog("%s[!] S3 Persistence registration returned %#llx\n", log_prefix, (unsigned long long)s3_ret);
      }
    }

    if (options & GU_UNLOCK_VERIFY_WINDOWS) {
      *stage = "final-Windows-verification";
      if (!GuUnlockReadyForWindows()) {
        *failed = g;
        return EFI_NOT_READY;
      }
    }
    (*unlocked)++;

    if (g + 1 < count && !GuUnlockPrepareNextGpu()) {
      *stage = "next-gpu-reset";
      *failed = g;
      return EFI_NOT_READY;
    }
  }
  if (*unlocked == 0) return last_refusal;
  return EFI_SUCCESS;
}
