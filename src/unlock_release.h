/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuUnlockPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuUnlockRelease(VOID);
BOOLEAN GuUnlockReleased(VOID);
EFI_STATUS GuUnlockFinish(VOID);
BOOLEAN GuUnlockReadyForWindows(VOID);
BOOLEAN GuUnlockMutated(VOID);
EFI_STATUS GuExecuteUnlock(EFI_SYSTEM_TABLE *t, EFI_HANDLE image, const char **stage);
EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *t);
