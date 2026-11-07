/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef GU_DISCOVERY_HOST_TEST
#include "../tests/gpu_discovery_mock.h"
#else
#include "gpu_discovery.h"
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

EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  EFI_HANDLE *handles = NULL;
  UINTN count = 0, seg, bus, dev, fun;
  GU_GPU_INVENTORY inventory;
  GU_PCI_READER_CONTEXT context;
  const GU_GPU_RECORD *selected;
  EFI_PCI_IO_PROTOCOL *io;
  UINT32 config[64];
  GU_CAPS caps;
  size_t i, ad102 = 0, ga102 = 0;
  int result;
  EFI_STATUS s, released;
  if (!image || !table || !table->BootServices ||
      !table->BootServices->LocateHandleBuffer ||
      !table->BootServices->HandleProtocol ||
      !table->BootServices->FreePool) return EFI_INVALID_PARAMETER;
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
  for (i = 0; i < inventory.count; i++) {
    const GU_GPU_RECORD *p = &inventory.nvidia[i];
    GuLog("PCI inventory: %04x:%02x:%02x.%u id=%04x:%04x subsystem=%04x:%04x class=%04x\n",
          (unsigned)p->segment, (unsigned)p->bus, (unsigned)p->device,
          (unsigned)p->function, (unsigned)(p->pci_id & 0xffffU),
          (unsigned)(p->pci_id >> 16), (unsigned)(p->subsystem_id & 0xffffU),
          (unsigned)(p->subsystem_id >> 16), (unsigned)p->class_code);
  }
  if (inventory.count > 1) {
    result = GuFindPinnedGpuPair(&inventory, &ad102, &ga102);
    GuLog("PCI inventory: NVIDIA display count=%zu exact B850 pair=%u (AD102 index=%zu GA102 index=%zu); active dual-GPU path BLOCKED\n",
          inventory.count, (unsigned)(result == GU_INVENTORY_OK), ad102, ga102);
    return EFI_UNSUPPORTED; /* Never use the single global Gu for two GPUs. */
  }
  if (inventory.count != 1) return EFI_UNSUPPORTED;
  selected = &inventory.nvidia[0];
  if (!GuIsSupportedPciId(selected->pci_id)) return EFI_UNSUPPORTED;
  s = table->BootServices->HandleProtocol((EFI_HANDLE)selected->handle,
                                            &PciGuid, (VOID **)&io);
  if (EFI_ERROR(s) || io != selected->pci || !io ||
      !io->GetLocation || !io->Pci.Read) return EFI_UNSUPPORTED;
  s = io->GetLocation(io, &seg, &bus, &dev, &fun);
  if (EFI_ERROR(s) || seg != selected->segment || bus != selected->bus ||
      dev != selected->device || fun != selected->function) return EFI_UNSUPPORTED;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 0, 64, config);
  if (EFI_ERROR(s) || config[0] != selected->pci_id ||
      config[0x2c / 4] != selected->subsystem_id ||
      (config[2] >> 16) != selected->class_code ||
      !GuCheckConfig(config, seg, bus, dev, fun, &caps)) return EFI_UNSUPPORTED;

  GuInit(table->BootServices, io);
  Gu.image = image;
  Gu.handle = (EFI_HANDLE)selected->handle;
  Gu.device_id = (UINT16)(selected->pci_id >> 16);
  Gu.is_ampere = GuIsAmpereDeviceId(Gu.device_id);
  Gu.is_turing = GuIsTuringDeviceId(Gu.device_id);
  Gu.bar_base = (config[4] & ~0xfU);
  Gu.bar_size = 0x1000000;
  Gu.pm_cap = caps.pm;
  Gu.pcie_cap = caps.pcie;
  Gu.pcie_version = caps.pcie_version;
  Gu.msi_cap = caps.msi;
  Gu.msix_cap = caps.msix;
  return EFI_SUCCESS;
}
