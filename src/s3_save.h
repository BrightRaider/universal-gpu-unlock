/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>

#define EFI_S3_SAVE_STATE_PROTOCOL_GUID \
  { 0xe73935aa, 0x3651, 0x4302, { 0xab, 0xdc, 0x4f, 0x30, 0xb0, 0xce, 0x53, 0x39 } }

#define EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID \
  { 0x32b304f4, 0xeb48, 0x4f3f, { 0x88, 0x99, 0xad, 0x8d, 0x52, 0xf9, 0x82, 0xa1 } }

typedef enum {
  EfiBootScriptWidthUint8 = 0,
  EfiBootScriptWidthUint16 = 1,
  EfiBootScriptWidthUint32 = 2,
  EfiBootScriptWidthUint64 = 3,
  EfiBootScriptWidthMaximum
} EFI_BOOT_SCRIPT_WIDTH;

#define EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE 0x00
#define EFI_BOOT_SCRIPT_MEM_READ_WRITE_OPCODE 0x01
#define EFI_BOOT_SCRIPT_PCI_CONFIG_WRITE_OPCODE 0x02
#define EFI_BOOT_SCRIPT_PCI_CONFIG_READ_WRITE_OPCODE 0x03
#define EFI_BOOT_SCRIPT_SMBUS_EXECUTE_OPCODE 0x04
#define EFI_BOOT_SCRIPT_STALL_OPCODE 0x05
#define EFI_BOOT_SCRIPT_DISPATCH_OPCODE 0x06
#define EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE 0x07
#define EFI_BOOT_SCRIPT_INFORMATION_OPCODE 0x08

typedef struct _EFI_S3_SAVE_STATE_PROTOCOL EFI_S3_SAVE_STATE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_S3_SAVE_STATE_WRITE)(
  IN CONST EFI_S3_SAVE_STATE_PROTOCOL *This,
  IN UINTN OpCode,
  ...
);

struct _EFI_S3_SAVE_STATE_PROTOCOL {
  EFI_S3_SAVE_STATE_WRITE Write;
  VOID *InsertAt;
  VOID *Compare;
};

#define GU_S3_CONTEXT_MAGIC 0x53335F50U /* "S3_P" */

#pragma pack(push, 1)
typedef struct {
  UINT32 magic;             /* GU_S3_CONTEXT_MAGIC */
  UINT32 version;           /* Context layout version (1) */
  UINT64 bar_base;          /* GPU BAR0 physical base address */
  UINT32 pci_id;            /* Device / Vendor ID */
  UINT16 segment;           /* PCI Segment */
  UINT8  bus;               /* PCI Bus */
  UINT8  device;            /* PCI Device */
  UINT8  function;          /* PCI Function */
  UINT8  is_ampere;         /* 1 if Ampere, 0 if Ada, 0 if Turing */
  UINT8  is_turing;         /* 1 if Turing */
  UINT8  reserved[5];

  /* Staged ACPI NVS payload physical DMA addresses & sizes */
  EFI_PHYSICAL_ADDRESS booter_dma;
  UINT32 booter_size;
  EFI_PHYSICAL_ADDRESS v67_dma;
  UINT32 v67_size;
  EFI_PHYSICAL_ADDRESS meta_dma;
  UINT32 meta_size;
  EFI_PHYSICAL_ADDRESS boot_dma;
  UINT32 boot_size;

  /* Target rate-limiter selectors */
  UINT32 val_ss0;
  UINT32 val_ss1;

  /* Status written back by S3 Resume Dispatch Handler */
  UINT32 resume_status;     /* 1 = Success, 0 = Pending, negative = Error */
  UINT32 resume_count;      /* Number of S3 resume cycles handled */
  UINT32 last_sec2_marker;  /* Readback of SEC2 marker register */
} GU_S3_RESUME_CONTEXT;
#pragma pack(pop)
