/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>
/* Shared read-only inventory. In a multi-NVIDIA system, log and REFUSE actuation. */
EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table);
