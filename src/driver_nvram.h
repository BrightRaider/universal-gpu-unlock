/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>

EFI_STATUS GuInstallDriverOption(EFI_HANDLE image, EFI_SYSTEM_TABLE *t);
EFI_STATUS GuRemoveDriverOption(EFI_SYSTEM_TABLE *t);
/* Takes the AdaUnlock entry out of DriverOrder but keeps it, so [I] can restore it. */
EFI_STATUS GuDisableDriverOption(EFI_SYSTEM_TABLE *t);
