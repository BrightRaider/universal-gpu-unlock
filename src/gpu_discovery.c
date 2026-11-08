/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_discovery.h"
#ifndef GU_DISCOVERY_HOST_TEST
#include "port.h"
#include "checks.h"
#endif
#include "gpu_inventory.h"

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

typedef struct {
  EFI_BOOT_SERVICES *services;
  EFI_HANDLE *handles;
} GU_PCI_READER_CONTEXT;

static int ReadPciIdentity(void *opaque, size_t index, GU_GPU_RECORD *record) {
  GU_PCI_READER_CONTEXT *ctx = (GU_PCI_READER_CONTEXT *)opaque;
  EFI_PCI_IO_PROTOCOL *io = NULL;
  EFI_STATUS s;
  UINT32 id = 0, pci_class = 0, subsystem = 0;
  UINTN segment, bus, device, function;
  if (!ctx || !record || !ctx->handles || !ctx->services) return -1;
  s = ctx->services->HandleProtocol(ctx->handles[index], &PciGuid, (VOID **)&io);
  if (EFI_ERROR(s) || !io || !io->Pci.Read) return -1;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 0, 1, &id);
  if (EFI_ERROR(s)) return -1;
  record->handle = ctx->handles[index];
  record->pci = io;
  record->pci_id = id;
  if ((id & 0xffffU) != 0x10deU) return 0;
  if (!io->GetLocation) return -1;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 8, 1, &pci_class);
  if (EFI_ERROR(s)) return -1;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 0x2c, 1, &subsystem);
  if (EFI_ERROR(s)) return -1;
  s = io->GetLocation(io, &segment, &bus, &device, &function);
  if (EFI_ERROR(s) || segment > 0xffffU || bus > 255U ||
      device > 31U || function > 7U) return -1;
  record->class_code = (UINT16)(pci_class >> 16);
  record->subsystem_id = subsystem;
  record->segment = (UINT32)segment;
  record->bus = (UINT32)bus;
  record->device = (UINT32)device;
  record->function = (UINT32)function;
  return 0;
}

EFI_STATUS GuDiscoverAllGpus(EFI_HANDLE image, EFI_SYSTEM_TABLE *table,
                             GU_DISCOVERED_DEVICE *devices, UINTN max_devices,
                             UINTN *out_count) {
  EFI_HANDLE *handles = NULL;
  UINTN count = 0, seg, bus, dev, fun;
  GU_GPU_INVENTORY inventory;
  GU_PCI_READER_CONTEXT context;
  size_t i;
  int result;
  EFI_STATUS s, released;
  UINTN discovered = 0;

  if (!image || !table || !table->BootServices ||
      !table->BootServices->LocateHandleBuffer ||
      !table->BootServices->HandleProtocol ||
      !table->BootServices->FreePool ||
      !devices || max_devices == 0 || !out_count) return EFI_INVALID_PARAMETER;

  *out_count = 0;
  s = table->BootServices->LocateHandleBuffer(ByProtocol, &PciGuid, NULL,
                                               &count, &handles);
  if (EFI_ERROR(s)) return EFI_UNSUPPORTED;
  if (!handles || !count || count > GU_INVENTORY_MAX_HANDLES) {
    if (handles) (VOID)table->BootServices->FreePool(handles);
    return EFI_UNSUPPORTED;
  }
  context.services = table->BootServices;
  context.handles = handles;
  result = GuCollectGpuInventory((size_t)count, ReadPciIdentity, &context,
                                 &inventory);
  released = table->BootServices->FreePool(handles);
  if (result != GU_INVENTORY_OK || EFI_ERROR(released)) {
    GuLog("PCI inventory refused: scan=%d free=%#llx\n", result,
          (unsigned long long)released);
    return EFI_UNSUPPORTED;
  }

  for (i = 0; i < inventory.count && discovered < max_devices; i++) {
    const GU_GPU_RECORD *p = &inventory.nvidia[i];
    EFI_PCI_IO_PROTOCOL *io = NULL;
    UINT32 config[64];
    GU_CAPS caps;

    if (!GuIsSupportedPciId(p->pci_id)) continue;

    s = table->BootServices->HandleProtocol((EFI_HANDLE)p->handle,
                                            &PciGuid, (VOID **)&io);
    if (EFI_ERROR(s) || io != p->pci || !io ||
        !io->GetLocation || !io->Pci.Read) continue;

    s = io->GetLocation(io, &seg, &bus, &dev, &fun);
    if (EFI_ERROR(s) || seg != p->segment || bus != p->bus ||
        dev != p->device || fun != p->function) continue;

    s = io->Pci.Read(io, EfiPciIoWidthUint32, 0, 64, config);
    if (EFI_ERROR(s) || config[0] != p->pci_id ||
        config[0x2c / 4] != p->subsystem_id ||
        (config[2] >> 16) != p->class_code ||
        !GuCheckConfig(config, seg, bus, dev, fun, &caps)) continue;

    GU_DISCOVERED_DEVICE *d = &devices[discovered];
    d->handle = (EFI_HANDLE)p->handle;
    d->io = io;
    d->device_id = (UINT16)(p->pci_id >> 16);
    d->bar_base = (config[4] & ~0xfU);
    d->pm_cap = caps.pm;
    d->pcie_cap = caps.pcie;
    d->pcie_version = caps.pcie_version;
    d->msi_cap = caps.msi;
    d->msix_cap = caps.msix;
    d->is_ampere = (UINT8)GuIsAmpereDeviceId(d->device_id);
    d->is_turing = (UINT8)GuIsTuringDeviceId(d->device_id);
    discovered++;
  }

  *out_count = discovered;
  if (discovered == 0) return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}

VOID GuBindDevice(EFI_BOOT_SERVICES *bs, EFI_HANDLE image, const GU_DISCOVERED_DEVICE *dev) {
  if (!bs || !dev) return;
  GuInit(bs, dev->io);
  Gu.image = image;
  Gu.handle = dev->handle;
  Gu.device_id = dev->device_id;
  Gu.is_ampere = (UINT8)dev->is_ampere;
  Gu.is_turing = (UINT8)dev->is_turing;
  Gu.bar_base = dev->bar_base;
  Gu.bar_size = 0x1000000;
  Gu.pm_cap = dev->pm_cap;
  Gu.pcie_cap = dev->pcie_cap;
  Gu.pcie_version = dev->pcie_version;
  Gu.msi_cap = dev->msi_cap;
  Gu.msix_cap = dev->msix_cap;
}