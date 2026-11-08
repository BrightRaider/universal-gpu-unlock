/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#ifdef GU_DISCOVERY_HOST_TEST
#include "../tests/gpu_discovery_mock.h"
#else
#include <Uefi.h>
#include <Protocol/PciIo.h>
#endif

#define GU_MAX_DISCOVERED_DEVICES 8U

typedef struct {
  EFI_HANDLE handle;
  EFI_PCI_IO_PROTOCOL *io;
  UINT16 device_id;
  UINT32 bar_base;
  UINT16 pm_cap, pcie_cap, msi_cap, msix_cap;
  UINT8 pcie_version;
  UINT8 is_ampere;
  UINT8 is_turing;
} GU_DISCOVERED_DEVICE;

/* Multi-GPU discovery: populates up to max_devices verified NVIDIA GPUs. */
EFI_STATUS GuDiscoverAllGpus(EFI_HANDLE image, EFI_SYSTEM_TABLE *table,
                             GU_DISCOVERED_DEVICE *devices, UINTN max_devices,
                             UINTN *out_count);

/* Bind a specific discovered device into the global Gu execution context */
VOID GuBindDevice(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, const GU_DISCOVERED_DEVICE *dev);
