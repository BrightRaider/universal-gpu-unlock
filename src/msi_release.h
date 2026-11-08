/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuMsiRelease(VOID);
BOOLEAN GuMsiReleased(VOID);
EFI_STATUS GuMsiFinish(VOID);
BOOLEAN GuMsiReadyForWindows(VOID);
BOOLEAN GuMsiMutated(VOID);
EFI_STATUS GuExecuteUnlock(EFI_SYSTEM_TABLE *t, EFI_HANDLE image, const char **stage);
EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *t);
