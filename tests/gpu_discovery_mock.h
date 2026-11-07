/* SPDX-License-Identifier: GPL-2.0-only */
/* Host definitions for compiling the actual UEFI discovery adapter only. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef void VOID;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef size_t UINTN;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef struct { uint32_t a; uint16_t b,c; uint8_t d[8]; } EFI_GUID;
#define EFI_PCI_IO_PROTOCOL_GUID { 1, 0, 0, {0,0,0,0,0,0,0,0} }
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED 3
#define EFI_INVALID_PARAMETER 2
#define EFI_ERROR(s) ((s) != EFI_SUCCESS)
#define ByProtocol 2
#define EfiPciIoWidthUint32 1

typedef struct MOCK_PCI_PROTOCOL EFI_PCI_IO_PROTOCOL;
typedef struct {
  EFI_STATUS (*Read)(EFI_PCI_IO_PROTOCOL *, int, UINT32, UINTN, VOID *);
} GU_PCI_ACCESS;
struct MOCK_PCI_PROTOCOL {
  EFI_STATUS (*GetLocation)(EFI_PCI_IO_PROTOCOL *, UINTN *, UINTN *, UINTN *, UINTN *);
  GU_PCI_ACCESS Pci;
  void *context;
};
typedef struct MOCK_BOOT_SERVICES {
  EFI_STATUS (*LocateHandleBuffer)(int, EFI_GUID *, VOID *, UINTN *, EFI_HANDLE **);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
typedef struct { EFI_BOOT_SERVICES *BootServices; } EFI_SYSTEM_TABLE;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  EFI_HANDLE image, handle;
  UINT16 device_id, pm_cap, pcie_cap, msi_cap, msix_cap;
  UINT8 is_ampere, is_turing, pcie_version;
  uint64_t bar_base, bar_size;
} GU_PLATFORM;
typedef struct {
  UINT16 pm, pcie, msi, msix;
  UINT8 pcie_version;
} GU_CAPS;
extern GU_PLATFORM Gu;
void GuInit(EFI_BOOT_SERVICES *, EFI_PCI_IO_PROTOCOL *);
void GuLog(const char *, ...);
int GuIsSupportedPciId(UINT32);
int GuIsAmpereDeviceId(UINT16);
int GuIsTuringDeviceId(UINT16);
int GuCheckConfig(const UINT32[64], UINTN, UINTN, UINTN, UINTN, GU_CAPS *);
