/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>

/* Official UEFI PI Specification EFI_S3_SAVE_STATE_PROTOCOL GUID (MdePkg/Include/Protocol/S3SaveState.h) */
#define EFI_S3_SAVE_STATE_PROTOCOL_GUID \
  { 0xe857caf6, 0xc046, 0x45dc, { 0xbe, 0x3f, 0xee, 0x07, 0x65, 0xfb, 0xa8, 0x87 } }

/* Official Intel Framework EFI_BOOT_SCRIPT_SAVE_PROTOCOL GUID (IntelFrameworkPkg/Include/Protocol/BootScriptSave.h) */
#define EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID \
  { 0x470e1529, 0xb79e, 0x4e32, { 0xa0, 0xfe, 0x6a, 0x15, 0x6d, 0x29, 0xf9, 0xb2 } }

typedef enum {
  EfiBootScriptWidthUint8 = 0,
  EfiBootScriptWidthUint16 = 1,
  EfiBootScriptWidthUint32 = 2,
  EfiBootScriptWidthUint64 = 3,
  EfiBootScriptWidthMaximum
} EFI_BOOT_SCRIPT_WIDTH;

/* Official UEFI PI Specification Boot Script Opcodes (PiS3BootScript.h) */
#define EFI_BOOT_SCRIPT_IO_WRITE_OPCODE 0x00
#define EFI_BOOT_SCRIPT_IO_READ_WRITE_OPCODE 0x01
#define EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE 0x02
#define EFI_BOOT_SCRIPT_MEM_READ_WRITE_OPCODE 0x03
#define EFI_BOOT_SCRIPT_PCI_CONFIG_WRITE_OPCODE 0x04
#define EFI_BOOT_SCRIPT_PCI_CONFIG_READ_WRITE_OPCODE 0x05
#define EFI_BOOT_SCRIPT_SMBUS_EXECUTE_OPCODE 0x06
#define EFI_BOOT_SCRIPT_STALL_OPCODE 0x07
#define EFI_BOOT_SCRIPT_DISPATCH_OPCODE 0x08
#define EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE 0x09
#define EFI_BOOT_SCRIPT_INFORMATION_OPCODE 0x0A

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
  UINT32 pci_cmd_port_addr; /* Precalculated 0xCF8 address for GPU PCI Command Register */
  UINT8  reserved[1];       /* Reserved alignment padding */

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

/* Context field byte offsets for machine code generation */
#define GU_S3_CTX_OFF_MAGIC         0x00
#define GU_S3_CTX_OFF_BAR_BASE      0x08
#define GU_S3_CTX_OFF_IS_AMPERE     0x19
#define GU_S3_CTX_OFF_IS_TURING     0x1A
#define GU_S3_CTX_OFF_PCI_CMD_ADDR  0x1B
#define GU_S3_CTX_OFF_BOOTER_DMA    0x20
#define GU_S3_CTX_OFF_BOOTER_SZ     0x28
#define GU_S3_CTX_OFF_VAL_SS0       0x50
#define GU_S3_CTX_OFF_VAL_SS1       0x54
#define GU_S3_CTX_OFF_STATUS        0x58
#define GU_S3_CTX_OFF_COUNT         0x5C
#define GU_S3_CTX_OFF_SEC2_MARKER   0x60
