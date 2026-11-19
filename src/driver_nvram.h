/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>

EFI_STATUS GuInstallDriverOption(EFI_HANDLE image, EFI_SYSTEM_TABLE *t);
/* After a successful GuInstallDriverOption: TRUE if the driver went to the internal ESP,
 * FALSE if the entry points to the USB drive. */
BOOLEAN GuDriverInstalledOnEsp(VOID);
EFI_STATUS GuRemoveDriverOption(EFI_SYSTEM_TABLE *t);
/* Takes the AdaUnlock entry out of DriverOrder but keeps it, so [I] can restore it. */
EFI_STATUS GuDisableDriverOption(EFI_SYSTEM_TABLE *t);
