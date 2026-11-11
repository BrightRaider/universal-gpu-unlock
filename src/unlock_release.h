/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuUnlockPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuUnlockRelease(VOID);
BOOLEAN GuUnlockReleased(VOID);
EFI_STATUS GuUnlockFinish(VOID);
BOOLEAN GuUnlockReadyForWindows(VOID);
BOOLEAN GuUnlockMutated(VOID);
BOOLEAN GuUnlockIsCsmVideo(VOID);
BOOLEAN GuDetectCsmVideoDriver(UINT64 pci_attributes, BOOLEAN has_nv_agent,
                               UINT32 driver_ver, UINTN driver_size);
EFI_STATUS GuReconnectDisplayAfterUnlock(EFI_SYSTEM_TABLE *t, EFI_HANDLE image,
                                         EFI_HANDLE pci_handle, BOOLEAN allow_fallback_gop);
EFI_STATUS GuExecuteUnlock(EFI_SYSTEM_TABLE *t, EFI_HANDLE image, const char **stage);
EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *t);
