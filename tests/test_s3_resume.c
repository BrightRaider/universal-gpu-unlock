/* SPDX-License-Identifier: GPL-2.0-only */
#define GU_S3_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#undef NULL
#undef EPROTO
#undef EBADMSG
#undef EOPNOTSUPP
#undef ETIMEDOUT
#undef EOVERFLOW
#undef EALREADY

#include "../src/s3_save.h"
#include "../src/s3_facs.h"
#include "../src/s3_resume.h"
#include "../src/port.h"
#include "../src/compat/compat.h"
#include "../src/vendor/prep_610.h"
#include <Protocol/PciRootBridgeIo.h>
#include <stddef.h>

#include <stdarg.h>

GU_PLATFORM Gu;
GU_ASSET GuAssets[5];
static char g_log_history[8192] = {0};

BOOLEAN GuSafeToReturn(VOID) {
  return !Gu.retained && !Gu.allocations && !Gu.dma_count &&
         !Gu.master_attributes_pending &&
         (!Gu.started || (Gu.quiesced && !Gu.fault));
}

EFI_STATUS GuS3AllocateArena(VOID) {
  if (Gu.s3_arena_armed)
    return EFI_SUCCESS;
  if (!Gu.bs || !Gu.bs->AllocatePages)
    return EFI_NOT_READY;

  EFI_PHYSICAL_ADDRESS Phys = 0xFFFFFFFFULL;
  EFI_STATUS Status = Gu.bs->AllocatePages(AllocateMaxAddress, EfiReservedMemoryType,
                                          (UINTN)GU_S3_ARENA_PAGES, &Phys);
  if (EFI_ERROR(Status) || !Phys) {
    return Status;
  }

  Gu.s3_arena_phys = Phys;
  Gu.s3_arena_pages = (UINTN)GU_S3_ARENA_PAGES;
  Gu.s3_arena_used_pages = 0;
  Gu.s3_arena_armed = TRUE;
  return EFI_SUCCESS;
}

VOID GuLog(const char *Format, ...) {
  char temp[1024];
  va_list args;
  va_start(args, Format);
  vsnprintf(temp, sizeof(temp), Format, args);
  va_end(args);
  size_t cur = strlen(g_log_history);
  if (cur + strlen(temp) + 1 < sizeof(g_log_history)) {
    strcat(g_log_history, temp);
  }
}

static unsigned int g_checks = 0;
#define CHECK(cond, msg) do { \
  g_checks++; \
  if (!(cond)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    fflush(stderr); \
    exit(1); \
  } \
} while(0)

static void TestGuS3ResumeContextOffsets(void) {
  printf("[0] Testing GU_S3_RESUME_CONTEXT struct offsets & alignment...\n");
  CHECK(sizeof(GU_S3_RESUME_CONTEXT) == GU_S3_CTX_V2_SIZE, "GU_S3_RESUME_CONTEXT size must be 216 bytes (v1.9.0)");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, root_port_link_reg) == GU_S3_CTX_OFF_ROOT_PORT_LNK, "root_port_link_reg offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, gpu_pmcsr_reg) == GU_S3_CTX_OFF_GPU_PMCSR, "gpu_pmcsr_reg offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, magic) == GU_S3_CTX_OFF_MAGIC, "magic offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, bar_base) == GU_S3_CTX_OFF_BAR_BASE, "bar_base offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, is_ampere) == GU_S3_CTX_OFF_IS_AMPERE, "is_ampere offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, is_turing) == GU_S3_CTX_OFF_IS_TURING, "is_turing offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, pci_cmd_port_addr) == GU_S3_CTX_OFF_PCI_CMD_ADDR, "pci_cmd_port_addr offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, booter_dma) == GU_S3_CTX_OFF_BOOTER_DMA, "booter_dma offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, booter_size) == GU_S3_CTX_OFF_BOOTER_SZ, "booter_size offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, val_ss0) == GU_S3_CTX_OFF_VAL_SS0, "val_ss0 offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, val_ss1) == GU_S3_CTX_OFF_VAL_SS1, "val_ss1 offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, resume_status) == GU_S3_CTX_OFF_STATUS, "resume_status offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, resume_count) == GU_S3_CTX_OFF_COUNT, "resume_count offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, last_sec2_marker) == GU_S3_CTX_OFF_SEC2_MARKER, "last_sec2_marker offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, chip_id) == GU_S3_CTX_OFF_CHIP_ID, "chip_id offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, bridge_count) == GU_S3_CTX_OFF_BRIDGE_CNT, "bridge_count offset");
  CHECK(offsetof(GU_S3_RESUME_CONTEXT, bridges) == GU_S3_CTX_OFF_BRIDGES, "bridges offset");
}

/* Mock 16MB MMIO window for BAR0 */
#define MOCK_BAR_DWORDS (0x1000000 / 4)
static UINT32 g_mock_bar[MOCK_BAR_DWORDS];

static void TestGuS3ExecuteResumeUnlock_Validation(void) {
  printf("[1] Testing S3 Resume validation (NULL & bad magic)...\n");
  INTN ret = GuS3ExecuteResumeUnlock(NULL);
  CHECK(ret == -1, "NULL context must return -1");

  GU_S3_RESUME_CONTEXT bad_ctx;
  memset(&bad_ctx, 0, sizeof(bad_ctx));
  bad_ctx.magic = 0x12345678U;
  ret = GuS3ExecuteResumeUnlock(&bad_ctx);
  CHECK(ret == -1, "Bad magic must return -1");

  bad_ctx.magic = GU_S3_CONTEXT_MAGIC;
  bad_ctx.bar_base = 0;
  ret = GuS3ExecuteResumeUnlock(&bad_ctx);
  CHECK(ret == -1, "Zero bar_base must return -1");
}

static void TestGuS3ExecuteResumeUnlock_AdaAmpere(void) {
  printf("[2] Testing S3 Resume hardware unlock for Ada/Ampere...\n");
  memset(g_mock_bar, 0, sizeof(g_mock_bar));

  /* Simulate cold locked state: SS0=0, SS1=0, CPUCTL halt bit initially 0 */
  g_mock_bar[0x0082381c / 4] = 0;
  g_mock_bar[0x00823820 / 4] = 0;
  g_mock_bar[0x00840100 / 4] = 0x10; /* Set halt bit so loop terminates immediately */
  g_mock_bar[0x00840040 / 4] = 0x67b00001U; /* Simulated SEC2 marker */

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 1;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.pci_id = 0x268410deU; /* RTX 4090 */
  ctx.is_ampere = 0;
  ctx.is_turing = 0;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;
  ctx.booter_dma = 0xdc5d9000ULL;
  ctx.booter_size = 57672;

  INTN ret = GuS3ExecuteResumeUnlock(&ctx);
  CHECK(ret == 0, "Ada/Ampere unlock must succeed (return 0)");
  CHECK(ctx.resume_status == 1, "Resume status must be 1 (success)");
  CHECK(ctx.resume_count == 1, "Resume count must be incremented to 1");
  CHECK(ctx.last_sec2_marker == 0x67b00001U, "SEC2 marker must be captured");

  /* Verify register writes occurred correctly on the mock BAR */
  CHECK(g_mock_bar[0x0082381c / 4] == 0x88888888U, "BAR0 SS0 must be 0x88888888");
  CHECK(g_mock_bar[0x00823820 / 4] == 0x00000008U, "BAR0 SS1 must be 0x00000008");
  CHECK(g_mock_bar[0x00840110 / 4] == (UINT32)(ctx.booter_dma >> 8), "Booter DMA address must be programmed");
  CHECK(g_mock_bar[0x00840100 / 4] == 2, "CPUCTL must have been started with 2");
}

static void TestGuS3ExecuteResumeUnlock_Turing(void) {
  printf("[3] Testing S3 Resume hardware unlock for Turing...\n");
  memset(g_mock_bar, 0, sizeof(g_mock_bar));
  g_mock_bar[0x00840100 / 4] = 0x10;

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 1;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.pci_id = 0x1e0410deU; /* TU102 (RTX 2080 Ti) */
  ctx.is_ampere = 0;
  ctx.is_turing = 1;
  ctx.booter_dma = 0xdc5d9000ULL;

  INTN ret = GuS3ExecuteResumeUnlock(&ctx);
  CHECK(ret == 0, "Turing unlock must succeed (return 0)");
  CHECK(ctx.resume_status == 1, "Resume status must be 1 (success)");
  CHECK(ctx.resume_count == 1, "Resume count must be incremented to 1");
  CHECK(g_mock_bar[0x00840110 / 4] == (UINT32)(ctx.booter_dma >> 8), "Turing booter DMA programmed");
}

static void TestGuS3ResumeDispatch_Wrapper(void) {
  printf("[4] Testing S3 Resume Dispatch wrapper entrypoint...\n");
  memset(g_mock_bar, 0, sizeof(g_mock_bar));
  g_mock_bar[0x00840100 / 4] = 0x10;

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.bar_base = (UINT64)(uintptr_t)g_mock_bar;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;

  GuS3ResumeDispatch(NULL, &ctx);
  CHECK(ctx.resume_status == 1, "Dispatch must set resume_status to 1");
  CHECK(ctx.resume_count == 1, "Dispatch must increment resume_count to 1");

  /* Call a second time to simulate subsequent resume cycle */
  GuS3ResumeDispatch(NULL, &ctx);
  CHECK(ctx.resume_count == 2, "Second resume cycle must increment resume_count to 2");

  /* Call with context passed in This (RCX parameter fallback) */
  GuS3ResumeDispatch(&ctx, NULL);
  CHECK(ctx.resume_count == 3, "RCX parameter fallback must increment resume_count to 3");
}

static void TestGuS3GuidsAndOpcodes(void) {
  printf("[5] Testing UEFI PI Specification S3 Protocol GUIDs & Opcodes...\n");
  EFI_GUID s3_guid = EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  CHECK(s3_guid.Data1 == 0xe857caf6, "S3 Save State GUID Data1 must be 0xe857caf6 (UEFI PI standard)");
  CHECK(s3_guid.Data2 == 0xc046, "S3 Save State GUID Data2 must be 0xc046");
  CHECK(s3_guid.Data3 == 0x45dc, "S3 Save State GUID Data3 must be 0x45dc");

  EFI_GUID framework_guid = EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID;
  CHECK(framework_guid.Data1 == 0x470e1529, "Boot Script Save GUID Data1 must be 0x470e1529 (Framework standard)");

  CHECK(EFI_BOOT_SCRIPT_IO_WRITE_OPCODE == 0x00, "IO_WRITE must be 0x00");
  CHECK(EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE == 0x02, "MEM_WRITE must be 0x02");
  CHECK(EFI_BOOT_SCRIPT_DISPATCH_OPCODE == 0x08, "DISPATCH must be 0x08");
  CHECK(EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE == 0x09, "DISPATCH_2 must be 0x09");
}

static VOID *g_mock_ebs_observed_vendor_table = NULL;
static EFI_SYSTEM_TABLE mock_st;
static EFI_STATUS g_mock_ebs_return_status = EFI_SUCCESS;
static UINT8 g_mock_high_acpi[4096];
static UINT8 g_mock_telemetry_page[4096];
static UINT32 g_mock_s3_save_call_count = 0;
static VOID *g_mock_s3_save_observed_vendor_table = NULL;

static EFI_STATUS EFIAPI MockOriginalExitBootServices(EFI_HANDLE ImageHandle, UINTN MapKey) {
  (VOID)ImageHandle; (VOID)MapKey;
  if (mock_st.ConfigurationTable) {
    g_mock_ebs_observed_vendor_table = mock_st.ConfigurationTable[0].VendorTable;
  }
  return g_mock_ebs_return_status;
}

static EFI_STATUS EFIAPI MockCalculateCrc32(VOID *Data, UINTN DataSize, UINT32 *Crc32) {
  (VOID)Data; (VOID)DataSize;
  if (Crc32) *Crc32 = 0x12345678;
  return EFI_SUCCESS;
}

static UINT8 g_mock_nvs_buffer[256 * 1024];
static UINT8 g_mock_low_tramp[4096];
static BOOLEAN g_mock_fail_low_tramp_alloc = FALSE;
static EFI_PHYSICAL_ADDRESS g_mock_last_low_max_addr = 0;
static UINT32 g_mock_low_nvs_alloc_calls = 0;

static EFI_STATUS EFIAPI MockFreePages(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages) {
  (VOID)Memory; (VOID)Pages;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockAllocatePages(
    IN EFI_ALLOCATE_TYPE Type,
    IN EFI_MEMORY_TYPE MemoryType,
    IN UINTN Pages,
    IN OUT EFI_PHYSICAL_ADDRESS *Memory
) {
  if (!Memory) return EFI_INVALID_PARAMETER;
  if (MemoryType == EfiReservedMemoryType && Pages >= 20000) {
    *Memory = 0xDAD00000ULL;
  } else if (MemoryType == EfiRuntimeServicesData || MemoryType == EfiReservedMemoryType) {
    *Memory = (EFI_PHYSICAL_ADDRESS)(uintptr_t)g_mock_telemetry_page;
  } else if (MemoryType == EfiACPIMemoryNVS && Pages >= 64) {
    *Memory = (EFI_PHYSICAL_ADDRESS)(uintptr_t)g_mock_nvs_buffer;
  } else if (MemoryType == EfiACPIMemoryNVS && Type == AllocateMaxAddress && *Memory <= 0x000FFFFFULL) {
    g_mock_low_nvs_alloc_calls++;
    g_mock_last_low_max_addr = *Memory;
    if (g_mock_fail_low_tramp_alloc) {
      return EFI_OUT_OF_RESOURCES;
    }
    *Memory = (EFI_PHYSICAL_ADDRESS)(uintptr_t)g_mock_low_tramp;
  } else {
    *Memory = (EFI_PHYSICAL_ADDRESS)(uintptr_t)g_mock_high_acpi;
  }
  return EFI_SUCCESS;
}

/* Mock PCI IO & PCI Root Bridge IO Protocol infrastructure for multi-vendor / switch topology tests (v1.9.1) */
typedef struct {
  EFI_PCI_IO_PROTOCOL io;
  UINT8 bus;
  UINT8 dev;
  UINT8 func;
  UINT8 cfg[256];
} MOCK_PCI_DEV;

#define MAX_MOCK_PCI_DEVS 16
static MOCK_PCI_DEV g_mock_pci_devs[MAX_MOCK_PCI_DEVS];
static UINTN g_mock_pci_count = 0;

static MOCK_PCI_DEV g_mock_rb_devs[MAX_MOCK_PCI_DEVS];
static UINTN g_mock_rb_count = 0;
static BOOLEAN g_mock_rb_enabled = FALSE;
static UINT32 g_mock_rb_segment = 0;

static EFI_STATUS EFIAPI MockPciConfigRead(
    IN EFI_PCI_IO_PROTOCOL *This,
    IN EFI_PCI_IO_PROTOCOL_WIDTH Width,
    IN UINT32 Offset,
    IN UINTN Count,
    IN OUT VOID *Buffer
) {
  if (!This || !Buffer || Width != EfiPciIoWidthUint8 || Offset + Count > 256) {
    return EFI_INVALID_PARAMETER;
  }
  MOCK_PCI_DEV *dev = (MOCK_PCI_DEV *)This;
  memcpy(Buffer, &dev->cfg[Offset], Count);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockPciGetLocation(
    IN EFI_PCI_IO_PROTOCOL *This,
    OUT UINTN *SegmentNumber,
    OUT UINTN *BusNumber,
    OUT UINTN *DeviceNumber,
    OUT UINTN *FunctionNumber
) {
  if (!This) return EFI_INVALID_PARAMETER;
  MOCK_PCI_DEV *dev = (MOCK_PCI_DEV *)This;
  if (SegmentNumber) *SegmentNumber = 0;
  if (BusNumber) *BusNumber = dev->bus;
  if (DeviceNumber) *DeviceNumber = dev->dev;
  if (FunctionNumber) *FunctionNumber = dev->func;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockRootBridgePciRead(
    IN     EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL        *This,
    IN     EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH  Width,
    IN     UINT64                                 Address,
    IN     UINTN                                  Count,
    IN OUT VOID                                   *Buffer
) {
  (VOID)This;
  if (!Buffer || Count != 1) return EFI_INVALID_PARAMETER;
  UINT8 bus  = (UINT8)((Address >> 24) & 0xFFU);
  UINT8 dev  = (UINT8)((Address >> 16) & 0xFFU);
  UINT8 func = (UINT8)((Address >> 8) & 0xFFU);
  UINT32 reg = (UINT32)(Address & 0xFFU);
  UINTN elem_sz = (Width == EfiPciWidthUint8) ? 1U :
                  (Width == EfiPciWidthUint16) ? 2U :
                  (Width == EfiPciWidthUint32) ? 4U : 0U;
  if (elem_sz == 0U || reg + elem_sz > 256U) return EFI_INVALID_PARAMETER;

  for (UINTN i = 0; i < g_mock_rb_count; i++) {
    if (g_mock_rb_devs[i].bus == bus && g_mock_rb_devs[i].dev == dev && g_mock_rb_devs[i].func == func) {
      memcpy(Buffer, &g_mock_rb_devs[i].cfg[reg], elem_sz);
      return EFI_SUCCESS;
    }
  }
  memset(Buffer, 0xFF, elem_sz);
  return EFI_SUCCESS;
}

static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL g_mock_root_bridge_io;

static EFI_STATUS EFIAPI MockLocateHandleBuffer(
    IN EFI_LOCATE_SEARCH_TYPE SearchType,
    IN EFI_GUID *Protocol,
    IN VOID *SearchKey,
    IN OUT UINTN *NoHandles,
    OUT EFI_HANDLE **Buffer
) {
  (VOID)SearchType; (VOID)SearchKey;
  static const EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
  static const EFI_GUID rb_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
  if (!Protocol || !NoHandles || !Buffer) return EFI_INVALID_PARAMETER;

  if (memcmp(Protocol, &rb_guid, sizeof(EFI_GUID)) == 0 && g_mock_rb_enabled) {
    memset(&g_mock_root_bridge_io, 0, sizeof(g_mock_root_bridge_io));
    g_mock_root_bridge_io.Pci.Read = MockRootBridgePciRead;
    g_mock_root_bridge_io.SegmentNumber = g_mock_rb_segment;
    EFI_HANDLE *arr = (EFI_HANDLE *)calloc(1, sizeof(EFI_HANDLE));
    arr[0] = (EFI_HANDLE)&g_mock_root_bridge_io;
    *NoHandles = 1;
    *Buffer = arr;
    return EFI_SUCCESS;
  }

  if (memcmp(Protocol, &pci_guid, sizeof(EFI_GUID)) == 0 && g_mock_pci_count > 0) {
    EFI_HANDLE *arr = (EFI_HANDLE *)calloc(g_mock_pci_count, sizeof(EFI_HANDLE));
    for (UINTN i = 0; i < g_mock_pci_count; i++) {
      arr[i] = (EFI_HANDLE)&g_mock_pci_devs[i];
    }
    *NoHandles = g_mock_pci_count;
    *Buffer = arr;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockHandleProtocol(
    IN EFI_HANDLE Handle,
    IN EFI_GUID *Protocol,
    OUT VOID **Interface
) {
  static const EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
  static const EFI_GUID rb_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
  if (!Handle || !Protocol || !Interface) return EFI_INVALID_PARAMETER;
  if (memcmp(Protocol, &rb_guid, sizeof(EFI_GUID)) == 0 && Handle == (EFI_HANDLE)&g_mock_root_bridge_io) {
    *Interface = Handle;
    return EFI_SUCCESS;
  }
  if (memcmp(Protocol, &pci_guid, sizeof(EFI_GUID)) == 0) {
    *Interface = Handle;
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI MockFreePool(IN VOID *Buffer) {
  free(Buffer);
  return EFI_SUCCESS;
}

static void AddMockBridge(UINT8 bus, UINT8 dev, UINT8 func, UINT16 vendor, UINT16 device_id,
                          UINT8 pri, UINT8 sec, UINT8 sub, UINT8 pcie_cap_off) {
  if (g_mock_pci_count >= MAX_MOCK_PCI_DEVS) return;
  MOCK_PCI_DEV *d = &g_mock_pci_devs[g_mock_pci_count++];
  memset(d, 0, sizeof(*d));
  d->bus = bus; d->dev = dev; d->func = func;
  d->io.Pci.Read = MockPciConfigRead;
  d->io.GetLocation = MockPciGetLocation;
  *(UINT16 *)&d->cfg[0x00] = vendor;
  *(UINT16 *)&d->cfg[0x02] = device_id;
  d->cfg[0x0E] = 0x01; /* Type 1 PCI-to-PCI Bridge */
  d->cfg[0x18] = pri;
  d->cfg[0x19] = sec;
  d->cfg[0x1A] = sub;
  if (pcie_cap_off >= 0x40) {
    d->cfg[0x34] = pcie_cap_off;
    d->cfg[pcie_cap_off] = 0x10;
    d->cfg[pcie_cap_off + 1] = 0x00;
  }
}

static void AddMockRbEntry(UINT8 bus, UINT8 dev, UINT8 func, UINT16 vendor, UINT16 device_id,
                           UINT8 hdr_type, UINT8 pri, UINT8 sec, UINT8 sub, UINT8 pcie_cap_off) {
  if (g_mock_rb_count >= MAX_MOCK_PCI_DEVS) return;
  MOCK_PCI_DEV *d = &g_mock_rb_devs[g_mock_rb_count++];
  memset(d, 0, sizeof(*d));
  d->bus = bus; d->dev = dev; d->func = func;
  *(UINT16 *)&d->cfg[0x00] = vendor;
  *(UINT16 *)&d->cfg[0x02] = device_id;
  d->cfg[0x0E] = hdr_type;
  d->cfg[0x18] = pri;
  d->cfg[0x19] = sec;
  d->cfg[0x1A] = sub;
  if (pcie_cap_off >= 0x40) {
    d->cfg[0x34] = pcie_cap_off;
    d->cfg[pcie_cap_off] = 0x10;
    d->cfg[pcie_cap_off + 1] = 0x00;
  }
}

static EFI_STATUS EFIAPI MockOriginalS3Save(
    IN EFI_ACPI_S3_SAVE_PROTOCOL *This,
    IN VOID *Context
) {
  (VOID)This; (VOID)Context;
  g_mock_s3_save_call_count++;
  if (mock_st.ConfigurationTable && mock_st.NumberOfTableEntries > 0) {
    g_mock_s3_save_observed_vendor_table = mock_st.ConfigurationTable[0].VendorTable;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockS3SaveStateWrite(
    IN CONST EFI_S3_SAVE_STATE_PROTOCOL *This,
    IN UINTN OpCode,
    ...
) {
  (VOID)This; (VOID)OpCode;
  return EFI_SUCCESS;
}

static EFI_S3_SAVE_STATE_PROTOCOL g_mock_s3_save_state_protocol = {
  MockS3SaveStateWrite,
  NULL,
  NULL
};

static EFI_ACPI_S3_SAVE_PROTOCOL g_mock_s3_save_protocol = {
  MockOriginalS3Save
};

static EFI_STATUS EFIAPI MockLocateProtocol(
    IN EFI_GUID *Protocol,
    IN VOID *Registration,
    OUT VOID **Interface
) {
  (VOID)Registration;
  if (!Protocol || !Interface) return EFI_INVALID_PARAMETER;
  static EFI_GUID s3_save_guid = EFI_ACPI_S3_SAVE_PROTOCOL_GUID;
  if (memcmp(Protocol, &s3_save_guid, sizeof(EFI_GUID)) == 0) {
    *Interface = &g_mock_s3_save_protocol;
    return EFI_SUCCESS;
  }
  static EFI_GUID s3_save_state_guid = EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  if (memcmp(Protocol, &s3_save_state_guid, sizeof(EFI_GUID)) == 0) {
    *Interface = &g_mock_s3_save_state_protocol;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static void TestGuAcpiFacsDiscoveryAndPatching(void) {
  printf("[6] Testing Universal ACPI FACS & FADT Discovery and Shadow Patching...\n");

  /* Setup Mock ACPI hierarchy */
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE mock_facs;
  memset(&mock_facs, 0, sizeof(mock_facs));
  mock_facs.Signature = GU_ACPI_FACS_SIGNATURE; /* 'FACS' */
  mock_facs.Length = sizeof(mock_facs);
  mock_facs.HardwareSignature = 0x12345678U;
  mock_facs.FirmwareWakingVector = 0x00007C00U; /* Initial waking vector */

  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE mock_fadt;
  memset(&mock_fadt, 0, sizeof(mock_fadt));
  mock_fadt.Header.Signature = GU_ACPI_FADT_SIGNATURE; /* 'FACP' */
  mock_fadt.Header.Length = sizeof(mock_fadt);
  mock_fadt.Header.Revision = 3;
  mock_fadt.FirmwareCtrl = (UINT32)(uintptr_t)&mock_facs;
  mock_fadt.XFirmwareCtrl = (UINT64)(uintptr_t)&mock_facs;

  /* Calculate initial FADT checksum */
  mock_fadt.Header.Checksum = 0;
  UINT8 sum = 0;
  UINT8 *p = (UINT8 *)&mock_fadt;
  for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + p[i]);
  mock_fadt.Header.Checksum = (UINT8)(0 - sum);

  /* Verify initial FADT checksum is 0 mod 256 */
  sum = 0;
  for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Initial mock FADT checksum must be 0 mod 256");

  /* Mock RSDT containing 32-bit pointer to FADT */
#pragma pack(push, 1)
  struct {
    EFI_ACPI_DESCRIPTION_HEADER Header;
    UINT32 Entry[1];
  } mock_rsdt;
#pragma pack(pop)
  memset(&mock_rsdt, 0, sizeof(mock_rsdt));
  mock_rsdt.Header.Signature = 0x54445352U; /* 'RSDT' */
  mock_rsdt.Header.Length = sizeof(mock_rsdt);
  mock_rsdt.Entry[0] = (UINT32)(uintptr_t)&mock_fadt;
  mock_rsdt.Header.Checksum = 0;
  UINT8 r_sum = 0;
  UINT8 *r_p = (UINT8 *)&mock_rsdt;
  for (UINT32 i = 0; i < mock_rsdt.Header.Length; i++) r_sum = (UINT8)(r_sum + r_p[i]);
  mock_rsdt.Header.Checksum = (UINT8)(0 - r_sum);

  /* Mock XSDT containing 64-bit pointer to FADT */
#pragma pack(push, 1)
  struct {
    EFI_ACPI_DESCRIPTION_HEADER Header;
    UINT64 Entry[1];
  } mock_xsdt;
#pragma pack(pop)
  memset(&mock_xsdt, 0, sizeof(mock_xsdt));
  mock_xsdt.Header.Signature = 0x54445358U; /* 'XSDT' */
  mock_xsdt.Header.Length = sizeof(mock_xsdt);
  mock_xsdt.Entry[0] = (UINT64)(uintptr_t)&mock_fadt;
  mock_xsdt.Header.Checksum = 0;
  UINT8 x_sum = 0;
  UINT8 *x_p = (UINT8 *)&mock_xsdt;
  for (UINT32 i = 0; i < mock_xsdt.Header.Length; i++) x_sum = (UINT8)(x_sum + x_p[i]);
  mock_xsdt.Header.Checksum = (UINT8)(0 - x_sum);

  /* Mock RSDP */
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER mock_rsdp;
  memset(&mock_rsdp, 0, sizeof(mock_rsdp));
  mock_rsdp.Signature = GU_ACPI_RSDP_SIGNATURE;
  mock_rsdp.Length = sizeof(mock_rsdp);
  mock_rsdp.Revision = 2;
  mock_rsdp.RsdtAddress = (UINT32)(uintptr_t)&mock_rsdt;
  mock_rsdp.XsdtAddress = (UINT64)(uintptr_t)&mock_xsdt;

  /* Calculate initial mock RSDP checksums */
  mock_rsdp.Checksum = 0;
  UINT8 rsdp_init_sum20 = 0;
  UINT8 *p_mrsdp = (UINT8 *)&mock_rsdp;
  for (UINT32 i = 0; i < 20; i++) rsdp_init_sum20 = (UINT8)(rsdp_init_sum20 + p_mrsdp[i]);
  mock_rsdp.Checksum = (UINT8)(0 - rsdp_init_sum20);

  mock_rsdp.ExtendedChecksum = 0;
  UINT8 rsdp_init_sum36 = 0;
  for (UINT32 i = 0; i < mock_rsdp.Length; i++) rsdp_init_sum36 = (UINT8)(rsdp_init_sum36 + p_mrsdp[i]);
  mock_rsdp.ExtendedChecksum = (UINT8)(0 - rsdp_init_sum36);

  /* Save original checksums of untouched real tables */
  UINT8 orig_fadt_checksum = mock_fadt.Header.Checksum;
  UINT8 orig_xsdt_checksum = mock_xsdt.Header.Checksum;
  UINT8 orig_rsdt_checksum = mock_rsdt.Header.Checksum;
  UINT8 orig_rsdp_checksum = mock_rsdp.Checksum;
  UINT8 orig_rsdp_ext_checksum = mock_rsdp.ExtendedChecksum;

  /* Mock EFI System Table & Boot Services */
  EFI_BOOT_SERVICES mock_bs;
  memset(&mock_bs, 0, sizeof(mock_bs));
  mock_bs.Hdr.HeaderSize = sizeof(mock_bs);
  mock_bs.ExitBootServices = MockOriginalExitBootServices;
  mock_bs.CalculateCrc32 = MockCalculateCrc32;
  mock_bs.AllocatePages = MockAllocatePages;
  mock_bs.LocateProtocol = MockLocateProtocol;

  EFI_CONFIGURATION_TABLE config_tables[1];
  EFI_GUID acpi20_guid = GU_EFI_ACPI_20_TABLE_GUID;
  config_tables[0].VendorGuid = acpi20_guid;
  config_tables[0].VendorTable = &mock_rsdp;

  memset(&mock_st, 0, sizeof(mock_st));
  mock_st.NumberOfTableEntries = 1;
  mock_st.ConfigurationTable = config_tables;
  mock_st.BootServices = &mock_bs;

  /* Run Discovery */
  GU_ACPI_FACS_INFO info;
  EFI_STATUS s = GuAcpiLocateFadtAndFacs(&mock_st, &info);
  CHECK(s == EFI_SUCCESS, "GuAcpiLocateFadtAndFacs must return EFI_SUCCESS");
  CHECK(info.found == TRUE, "ACPI tables must be found");
  CHECK(info.fadt == &mock_fadt, "Discovered FADT must match mock_fadt");
  CHECK(info.real_facs == &mock_facs, "Discovered FACS must match mock_facs");
  CHECK(info.original_firmware_waking_vector == 0x00007C00U, "Original waking vector must be preserved");

  /* Setup mock 256 KB NVS buffer for Shadow FACS and Trampoline */
  UINT8 mock_nvs[4096];
  memset(mock_nvs, 0, sizeof(mock_nvs));
  EFI_PHYSICAL_ADDRESS mock_nvs_phys = (EFI_PHYSICAL_ADDRESS)(uintptr_t)mock_nvs;

  /* Setup mock 4 KB Low Trampoline Buffer (<1MB) */
  UINT8 mock_low_tramp[4096];
  memset(mock_low_tramp, 0, sizeof(mock_low_tramp));
  EFI_PHYSICAL_ADDRESS mock_low_tramp_phys = (EFI_PHYSICAL_ADDRESS)(uintptr_t)mock_low_tramp;

  /* Host-Test: Pre-populate mock telemetry in mock_low_tramp to verify it is NOT destroyed before reading */
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_MAGIC_OFF) = GU_TRAMP_TELEMETRY_MAGIC;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_ENTRY_STATUS_OFF) = 0xAA01;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_EXIT_STATUS_OFF) = 0xAA02;
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_CAPTURED_VEC_OFF) = 0x0009E000U;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_TARGET_PTR_OFF) = 0x0000;
  *(UINT16 *)(mock_low_tramp + GU_TRAMP_TARGET_PTR_OFF + 2) = 0x9E00;
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x00) = 0x10221483U; /* AMD Root Port */
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x08) = 0x002D2D00U;
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x0C) = 0x20430040U; /* Gen4 x16 DLLLA=1 */
  *(UINT32 *)(mock_low_tramp + GU_TRAMP_PCI_PROBE_RAW_OFF + 0x10) = 0x10DE2684U; /* RTX 4090 */

  g_log_history[0] = '\0';

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.bar_base = 0xfb000000ULL;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;
  ctx.pci_cmd_port_addr = 0x80000004U;

  /* Reset high ACPI mock memory, telemetry page and S3Save state */
  memset(g_mock_high_acpi, 0, sizeof(g_mock_high_acpi));
  memset(g_mock_telemetry_page, 0, sizeof(g_mock_telemetry_page));
  g_mock_s3_save_call_count = 0;
  g_mock_s3_save_observed_vendor_table = NULL;
  g_mock_s3_save_protocol.S3Save = MockOriginalS3Save;

  /* Install Shadow FACS - must read pre-populated telemetry before low_base is zeroed */
  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, mock_nvs, mock_nvs_phys, mock_low_tramp, mock_low_tramp_phys, &info);
  CHECK(s == EFI_SUCCESS, "GuAcpiInstallShadowFacs must return EFI_SUCCESS");
  CHECK(strstr(g_log_history, "S3 Prior Wake Telemetry") != NULL, "Prior wake telemetry must be read and logged");
  CHECK(strstr(g_log_history, "DLLLA=1") != NULL, "Link status telemetry must report DLLLA=1");

  /* Verify fresh re-invocation logs 'none found' */
  g_log_history[0] = '\0';
  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, mock_nvs, mock_nvs_phys, mock_low_tramp, mock_low_tramp_phys, &info);
  CHECK(s == EFI_SUCCESS, "Second GuAcpiInstallShadowFacs must return EFI_SUCCESS");
  CHECK(strstr(g_log_history, "S3 Prior Wake Telemetry: none found") != NULL, "Re-run must log 'none found'");
  CHECK(s == EFI_SUCCESS, "GuAcpiInstallShadowFacs must return EFI_SUCCESS");

  EFI_PHYSICAL_ADDRESS expected_high_acpi_phys = (EFI_PHYSICAL_ADDRESS)(uintptr_t)g_mock_high_acpi;
  EFI_PHYSICAL_ADDRESS expected_telemetry_phys = (EFI_PHYSICAL_ADDRESS)(uintptr_t)g_mock_telemetry_page;
  CHECK(info.high_acpi_phys == expected_high_acpi_phys, "High ACPI physical address must match allocated page");
  CHECK(info.telemetry_page_phys == expected_telemetry_phys, "Telemetry page physical address must match allocated page");
  CHECK(info.shadow_facs_phys == expected_high_acpi_phys + GU_HIGH_SHADOW_FACS_OFF, "Shadow FACS physical address must be high page + 0x400");
  CHECK(info.shadow_fadt_phys == expected_high_acpi_phys + GU_HIGH_SHADOW_FADT_OFF, "Shadow FADT physical address must be high page + 0x200");
  CHECK(info.shadow_rsdp_phys == expected_high_acpi_phys + GU_HIGH_SHADOW_RSDP_OFF, "Shadow RSDP physical address must be high page + 0x000");
  CHECK(info.shadow_xsdt_phys == expected_high_acpi_phys + GU_HIGH_SHADOW_XSDT_OFF, "Shadow XSDT physical address must be high page + 0x080");
  CHECK(info.trampoline_phys == mock_low_tramp_phys, "Trampoline physical address must be mock_low_tramp_phys (<1MB)");
  CHECK(info.s3_save_hook_installed == TRUE, "S3 Save hook must be installed");

  /* Verify LEAK 1: Low memory (<1MB) contains ZERO bytes of 'RSD PTR ' and ZERO bytes of 'FACS' */
  for (UINT32 i = 0; i <= sizeof(mock_low_tramp) - 8; i++) {
    CHECK(memcmp(mock_low_tramp + i, "RSD PTR ", 8) != 0, "Low trampoline memory must contain ZERO bytes of 'RSD PTR '");
  }
  for (UINT32 i = 0; i <= sizeof(mock_low_tramp) - 4; i++) {
    CHECK(memcmp(mock_low_tramp + i, "FACS", 4) != 0, "Low trampoline memory must contain ZERO bytes of 'FACS'");
  }
  /* Verify GDT + RM_IDTR at +0x0900 (v1.8.59 16->32->16 bridge), with zero FACS/RSDP */
  CHECK(*(UINT16 *)(mock_low_tramp + GU_TRAMP_RM_IDTR_OFF) == 0x03FF, "Low trampoline +0x0900 must contain RM_IDTR limit");
  CHECK(*(UINT32 *)(mock_low_tramp + GU_TRAMP_RM_IDTR_OFF + 2) == 0, "Low trampoline +0x0902 must contain RM_IDTR base 0");
  CHECK(*(UINT16 *)(mock_low_tramp + GU_TRAMP_PM_GDTR_OFF) == 0x0027, "Low trampoline +0x0908 must contain PM_GDTR limit 39");
  /* Verify unused buffer regions are zero-initialized and Bridge Table populated */
  for (UINT32 i = 0xA90; i < 0xB00; i++) {
    CHECK(mock_low_tramp[i] == 0, "Low trampoline memory +0xA90..+0xAFF must be zero-initialized");
  }
  for (UINT32 i = 0xB38; i < 0xB40; i++) {
    CHECK(mock_low_tramp[i] == 0, "Low trampoline memory +0xB38..+0xB3F must be zero-initialized");
  }
  for (UINT32 i = 0xB90; i < 0x1000; i++) {
    CHECK(mock_low_tramp[i] == 0, "Low trampoline memory +0xB90..+0xFFF must be zero-initialized");
  }
  CHECK(*(UINT32 *)(mock_low_tramp + GU_TRAMP_BRIDGE_TBL_OFF + GU_TRAMP_BRIDGE_BDF_OFF) == 0U,
        "Zero-bus context must NOT fabricate blind 0x80001900 bridge fallback");
  /* Verify Real FACS Mirror at +0x0880 */
  CHECK(*(UINT32 *)(mock_low_tramp + GU_TRAMP_REAL_FACS_MIRROR_OFF) == 0x00007C00U, "Real FACS mirror at +0x880");

  /* 1. Verify Plan C: Real ACPI tables remain 100% UNTOUCHED byte-for-byte */
  CHECK(mock_fadt.Header.Checksum == orig_fadt_checksum, "Real FADT checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_xsdt.Header.Checksum == orig_xsdt_checksum, "Real XSDT checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_rsdt.Header.Checksum == orig_rsdt_checksum, "Real RSDT checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_rsdp.Checksum == orig_rsdp_checksum, "Real RSDP 20-byte checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_rsdp.ExtendedChecksum == orig_rsdp_ext_checksum, "Real RSDP 36-byte extended checksum must be byte-for-byte UNTOUCHED");
  CHECK(mock_xsdt.Entry[0] == (UINT64)(uintptr_t)&mock_fadt, "Real XSDT Entry[0] must still point to Real FADT");
  CHECK(mock_rsdt.Entry[0] == (UINT32)(uintptr_t)&mock_fadt, "Real RSDT Entry[0] must still point to Real FADT");

  /* Verify Real FACS was hooked to point to Low Trampoline (<1MB) for AMI Aptio V */
  CHECK(mock_facs.FirmwareWakingVector == (UINT32)mock_low_tramp_phys, "Real FACS FirmwareWakingVector must point to Low Trampoline");
  CHECK(mock_facs.XFirmwareWakingVector == 0, "Real FACS XFirmwareWakingVector must be cleared");
  CHECK((mock_facs.Flags & 2U) == 0, "Real FACS 64-bit wake flag must be cleared");

  /* 2. Verify Plan C: Shadow FACS on high page + 0x400 */
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *shadow_facs =
      (EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *)(g_mock_high_acpi + GU_HIGH_SHADOW_FACS_OFF);
  CHECK(shadow_facs->Signature == GU_ACPI_FACS_SIGNATURE, "Shadow FACS signature must be 'FACS'");
  CHECK(shadow_facs->HardwareSignature == 0x12345678U, "Shadow FACS hardware signature must match original");
  CHECK(shadow_facs->FirmwareWakingVector == 0, "Shadow FACS FirmwareWakingVector initialized to 0");
  CHECK((shadow_facs->Flags & 2U) == 0, "Shadow FACS 64-bit wake flag must be cleared");

  /* 3. Verify Plan C: Shadow FADT at high page + 0x200 */
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *shadow_fadt =
      (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)(g_mock_high_acpi + GU_HIGH_SHADOW_FADT_OFF);
  CHECK(shadow_fadt->Header.Signature == GU_ACPI_FADT_SIGNATURE, "Shadow FADT signature must be 'FACP'");
  CHECK(shadow_fadt->FirmwareCtrl == (UINT32)(expected_high_acpi_phys + GU_HIGH_SHADOW_FACS_OFF), "Shadow FADT FirmwareCtrl must point to Shadow FACS on high page for Windows");
  CHECK(shadow_fadt->XFirmwareCtrl == 0, "Shadow FADT XFirmwareCtrl must be 0");

  sum = 0;
  p = (UINT8 *)shadow_fadt;
  for (UINT32 i = 0; i < shadow_fadt->Header.Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow FADT checksum must be 0 mod 256");

  /* 4. Verify Plan C: Shadow XSDT at high page + 0x080 */
  EFI_ACPI_DESCRIPTION_HEADER *shadow_xsdt =
      (EFI_ACPI_DESCRIPTION_HEADER *)(g_mock_high_acpi + GU_HIGH_SHADOW_XSDT_OFF);
  CHECK(shadow_xsdt->Signature == GU_ACPI_XSDT_SIGNATURE, "Shadow XSDT signature must be 'XSDT'");
  UINT64 *sxsdt_entries = (UINT64 *)((UINT8 *)shadow_xsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
  CHECK(sxsdt_entries[0] == (UINT64)(expected_high_acpi_phys + GU_HIGH_SHADOW_FADT_OFF), "Shadow XSDT Entry[0] must point to Shadow FADT on high page");

  sum = 0;
  p = (UINT8 *)shadow_xsdt;
  for (UINT32 i = 0; i < shadow_xsdt->Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow XSDT checksum must be 0 mod 256");

  /* 5. Verify Plan C: Shadow RSDP at high page + 0x000 */
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *shadow_rsdp =
      (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)(g_mock_high_acpi + GU_HIGH_SHADOW_RSDP_OFF);
  CHECK(shadow_rsdp->Signature == GU_ACPI_RSDP_SIGNATURE, "Shadow RSDP signature must be 'RSD PTR '");
  CHECK(shadow_rsdp->XsdtAddress == (UINT64)(expected_high_acpi_phys + GU_HIGH_SHADOW_XSDT_OFF), "Shadow RSDP XsdtAddress must point to Shadow XSDT");
  CHECK(shadow_rsdp->RsdtAddress == mock_rsdp.RsdtAddress, "Shadow RSDP RsdtAddress must remain pointing to Real RSDT");

  sum = 0;
  p = (UINT8 *)shadow_rsdp;
  for (UINT32 i = 0; i < 20; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow RSDP 20-byte legacy checksum must be 0 mod 256");

  sum = 0;
  for (UINT32 i = 0; i < shadow_rsdp->Length; i++) sum = (UINT8)(sum + p[i]);
  CHECK(sum == 0, "Shadow RSDP 36-byte extended checksum must be 0 mod 256");

  /* Verify Forensics Logging */
  CHECK(strstr(g_log_history, "RealFADT Forensic: FirmwareCtrl=") != NULL, "RealFADT forensic must be logged");
  CHECK(strstr(g_log_history, "UEFI ConfigurationTable Forensic") != NULL, "ConfigurationTable forensic must be logged");
  CHECK(strstr(g_log_history, "AcpiS3Save->S3Save Code Hex Dump") != NULL, "S3Save code hex dump must be logged");

  /* 6. Verify Deferred ConfigurationTable Patching */
  CHECK(mock_st.ConfigurationTable[0].VendorTable == &mock_rsdp,
        "SystemTable ConfigurationTable must initially remain Real RSDP after install (deferred apply)");

  /* Test late apply via GuAcpiApplyShadowConfigTable() */
  UINT32 applied = GuAcpiApplyShadowConfigTable();
  CHECK(applied > 0, "GuAcpiApplyShadowConfigTable must patch table entries");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == (VOID *)(UINTN)(expected_high_acpi_phys + GU_HIGH_SHADOW_RSDP_OFF),
        "SystemTable ConfigurationTable must point to Shadow RSDP on high page after late apply");

  /* Test S3Save Sandwich Hook */
  g_mock_s3_save_observed_vendor_table = NULL;
  g_mock_s3_save_call_count = 0;
  CHECK(g_mock_s3_save_protocol.S3Save != MockOriginalS3Save, "S3Save pointer must be hooked");
  s = g_mock_s3_save_protocol.S3Save(&g_mock_s3_save_protocol, NULL);
  CHECK(s == EFI_SUCCESS, "Hooked S3Save must return EFI_SUCCESS");
  CHECK(g_mock_s3_save_call_count == 1, "MockOriginalS3Save must be called once");
  CHECK(g_mock_s3_save_observed_vendor_table == &mock_rsdp, "Inside S3Save sandwich: AcpiS3Save must observe Real RSDP in ConfigurationTable");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == (VOID *)(UINTN)(expected_high_acpi_phys + GU_HIGH_SHADOW_RSDP_OFF),
        "After S3Save: ConfigurationTable must be restored to Shadow RSDP for OS loader");
  volatile UINT16 *tramp_words = (volatile UINT16 *)(mock_low_tramp + GU_TRAMP_RESTORED_COUNT_OFF);
  CHECK(tramp_words[1] == 1, "S3SaveCallCount at +0x502 must be 1");

  /* 7. Verify ExitBootServices Sandwich Hook */
  g_mock_ebs_observed_vendor_table = NULL;
  s = GuAcpiInstallEbsHook(&mock_st);
  CHECK(s == EFI_SUCCESS, "GuAcpiInstallEbsHook must return EFI_SUCCESS");
  CHECK(mock_st.BootServices->ExitBootServices != MockOriginalExitBootServices, "EBS pointer must be hooked");

  /* Invoke hooked EBS with EFI_SUCCESS */
  s = mock_st.BootServices->ExitBootServices((EFI_HANDLE)0x1, 0x1234);
  CHECK(s == EFI_SUCCESS, "Hooked EBS invocation must return EFI_SUCCESS");
  CHECK(g_mock_ebs_observed_vendor_table == &mock_rsdp,
        "Inside EBS sandwich: firmware/SMM must observe Real RSDP in ConfigurationTable");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == &mock_rsdp,
        "After successful EBS: ConfigurationTable must remain Real RSDP for SMM/firmware");

  /* Check diagnostic counters at LowTrampoline + 0x500 */
  CHECK(tramp_words[0] == 1, "RestoredCount at +0x500 must be 1");
  CHECK(tramp_words[1] == 1, "S3SaveCallCount at +0x502 must still be 1");
  volatile UINT32 *tramp_dwords = (volatile UINT32 *)(mock_low_tramp + GU_TRAMP_RESTORED_COUNT_OFF);
  CHECK(tramp_dwords[1] == (UINT32)(UINTN)&mock_rsdp, "Restored VendorTable lower 32 bits at +0x504 must match Real RSDP");
  volatile UINT16 *ebs_words = (volatile UINT16 *)(mock_low_tramp + GU_TRAMP_EBS_CALL_COUNT_OFF);
  CHECK(ebs_words[0] == 1, "EbsCallCount at +0x508 must be 1");
  CHECK(ebs_words[1] == 1, "EbsSuccessCount at +0x50A must be 1");
  CHECK(tramp_dwords[3] == 0, "EbsLastStatus at +0x50C must be 0 (EFI_SUCCESS)");

  /* Test retry behavior when EBS fails (e.g. EFI_INVALID_PARAMETER) */
  g_mock_ebs_return_status = EFI_INVALID_PARAMETER;
  s = mock_st.BootServices->ExitBootServices((EFI_HANDLE)0x1, 0x5678);
  CHECK(s == EFI_INVALID_PARAMETER, "Hooked EBS must pass through error status");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == (VOID *)(UINTN)(expected_high_acpi_phys + GU_HIGH_SHADOW_RSDP_OFF),
        "After failed EBS: ConfigurationTable must be restored to Shadow RSDP for winload retry");
  CHECK(ebs_words[0] == 2, "EbsCallCount at +0x508 must be 2 after retry");
  CHECK(ebs_words[1] == 1, "EbsSuccessCount at +0x50A must still be 1 after failed call");
  CHECK(tramp_dwords[3] == (UINT32)EFI_INVALID_PARAMETER, "EbsLastStatus at +0x50C must record EFI_INVALID_PARAMETER");
  g_mock_ebs_return_status = EFI_SUCCESS;

  /* Uninstall EBS Hook */
  GuAcpiUninstallEbsHook(&mock_st);
  CHECK(mock_st.BootServices->ExitBootServices == MockOriginalExitBootServices,
        "Uninstalling EBS hook must restore original BootServices pointer");
  CHECK(g_mock_s3_save_protocol.S3Save == MockOriginalS3Save,
        "Uninstalling EBS hook must restore original S3Save pointer");
  CHECK(mock_st.ConfigurationTable[0].VendorTable == &mock_rsdp,
        "Uninstalling EBS hook must restore ConfigurationTable to Real RSDP");

  /* 8. Verify warm reset recovery: re-running discovery recovers Real FADT & Real FACS */
  GU_ACPI_FACS_INFO info_warm;
  s = GuAcpiLocateFadtAndFacs(&mock_st, &info_warm);
  CHECK(s == EFI_SUCCESS, "Warm reset discovery must succeed");
  CHECK(info_warm.fadt == &mock_fadt, "Warm reset must recover original Real FADT from RSDT");
  CHECK(info_warm.real_facs == &mock_facs, "Warm reset must recover original Real FACS table");
  CHECK(info_warm.real_facs_phys == (EFI_PHYSICAL_ADDRESS)(uintptr_t)&mock_facs, "Warm reset must recover original Real FACS address");
  printf("[6] Discovery, Shadow Chain & EBS Sandwich (Plan C): OK!\n");
  fflush(stdout);
}

static void TestGuAcpiBuildTrampolineCode(void) {
  printf("[7] Testing S3 Resume Trampoline Machine Code Generation...\n");
  fflush(stdout);

  UINT8 buffer[4096];
  EFI_PHYSICAL_ADDRESS tramp_phys = 0x0009E000ULL;
  EFI_PHYSICAL_ADDRESS ctx_phys = 0xdc5d8000ULL;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = 0xdc617400ULL;
  EFI_PHYSICAL_ADDRESS telem_phys = 0xDA000000ULL;
  EFI_PHYSICAL_ADDRESS fwsec_dma_phys = 0xC7300000ULL;

  UINT32 bytes = GuAcpiBuildTrampolineCode(buffer, sizeof(buffer), tramp_phys, ctx_phys, shadow_facs_phys, telem_phys, 0xFB000000ULL, 0x80001960U, 0x802D0064U, fwsec_dma_phys);
  CHECK(bytes > 0, "Trampoline builder must generate non-zero bytes");
  CHECK(bytes <= GU_TRAMP_ENVELOPE_SIZE, "Trampoline machine code must fit within 2560 bytes (<0x0A00 telemetry area)");

  /* 1. Check Preamble: cli, cld */
  CHECK(buffer[0] == 0xFA, "Preamble must start with CLI (0xFA)");
  CHECK(buffer[1] == 0xFC, "Preamble must have CLD (0xFC)");

  /* 2. Check Segment Setup & Stack Save */
  CHECK(buffer[2] == 0x8C && buffer[3] == 0xC8, "mov ax, cs");
  CHECK(buffer[4] == 0x8E && buffer[5] == 0xD8, "mov ds, ax");
  CHECK(buffer[6] == 0x89 && buffer[7] == 0x26 && buffer[8] == (UINT8)(GU_TRAMP_BIOS_SP_OFF & 0xFFU) && buffer[9] == (UINT8)(GU_TRAMP_BIOS_SP_OFF >> 8), "mov [0x0A70], sp");
  CHECK(buffer[10] == 0x8C && buffer[11] == 0x16 && buffer[12] == (UINT8)(GU_TRAMP_BIOS_SS_OFF & 0xFFU) && buffer[13] == (UINT8)(GU_TRAMP_BIOS_SS_OFF >> 8), "mov [0x0A72], ss");

  /* 3. Check Telemetry Magic: mov dword [0x0A1C], 0x53335F54 */
  CHECK(buffer[14] == 0x66 && buffer[15] == 0xC7 && buffer[16] == 0x06 && buffer[17] == (UINT8)(GU_TRAMP_MAGIC_OFF & 0xFFU) && buffer[18] == (UINT8)(GU_TRAMP_MAGIC_OFF >> 8), "telemetry magic mov");
  CHECK(buffer[19] == 0x54 && buffer[20] == 0x5F && buffer[21] == 0x33 && buffer[22] == 0x53, "telemetry magic S3_T");

  /* 4. Check Entry Checkpoint: mov word [0x0A10], 0xAA01 */
  CHECK(buffer[23] == 0xC7 && buffer[24] == 0x06 && buffer[25] == (UINT8)(GU_TRAMP_ENTRY_STATUS_OFF & 0xFFU) && buffer[26] == (UINT8)(GU_TRAMP_ENTRY_STATUS_OFF >> 8), "entry checkpoint mov");
  CHECK(buffer[27] == 0x01 && buffer[28] == 0xAA, "entry checkpoint 0xAA01");

  /* 5. Check ljmp cs:[0x0A00] (0x2E, 0xFF, 0x2E, 0x00, 0x0A) */
  BOOLEAN found_ljmp_indirect = FALSE;
  for (UINT32 i = 0; i < bytes - 4; i++) {
    if (buffer[i] == 0x2E && buffer[i+1] == 0xFF && buffer[i+2] == 0x2E && buffer[i+3] == (UINT8)(GU_TRAMP_FAR_JUMP_OFF & 0xFFU) && buffer[i+4] == (UINT8)(GU_TRAMP_FAR_JUMP_OFF >> 8)) {
      found_ljmp_indirect = TRUE;
      break;
    }
  }
  CHECK(found_ljmp_indirect, "Real mode return stub must end with ljmp cs:[0x0A00]");

  /* Explicit Sanity Check 1: Verify all 16-bit real mode 32-bit I/O instructions have 0x66 prefix */
  UINT32 pm32_offset = 0;
  for (UINT32 i = 0; i < bytes - 6; i++) {
    if (buffer[i] == 0x66 && buffer[i+1] == 0xEA && buffer[i+6] == 0x08 && buffer[i+7] == 0x00) {
      pm32_offset = i + 8;
      break;
    }
  }
  CHECK(pm32_offset > 0, "Must locate 32-bit Protected Mode entry point");

  UINT32 pm16_offset = 0;
  for (UINT32 i = pm32_offset; i < bytes - 6; i++) {
    if (buffer[i] == 0xEA && buffer[i+5] == 0x18 && buffer[i+6] == 0x00) {
      pm16_offset = i;
      break;
    }
  }
  CHECK(pm16_offset > pm32_offset, "Must locate 16-bit Protected Mode transition point");

  UINT32 num_outs_16 = 0, num_ins_16 = 0;
  for (UINT32 i = 0; i < pm32_offset; i++) {
    if (buffer[i] == 0xEF) { /* out dx, eax */
      CHECK(i > 0 && buffer[i - 1] == 0x66, "16-bit out dx, eax must be preceded by 0x66 operand size prefix");
      num_outs_16++;
    } else if (buffer[i] == 0xED && i > 0 && buffer[i - 1] == 0x66) { /* in eax, dx preceded by 0x66 */
      num_ins_16++;
    }
  }
  CHECK(num_outs_16 >= 4, "16-bit trampoline must contain at least 4 32-bit OUT instructions (66 EF)");
  CHECK(num_ins_16 >= 2, "16-bit trampoline must contain at least 2 32-bit IN instructions (66 ED)");

  /* Explicit Sanity Check 2: Verify Root Port 0x18 (0x80001918) is NOT restored in 32-bit teardown (preserving bus 0x2D across wake) */
  BOOLEAN found_rp18_in_teardown = FALSE;
  for (UINT32 i = pm32_offset; i <= bytes - 5; i++) {
    if (buffer[i] == 0xB8 && *(UINT32 *)(buffer + i + 1) == 0x80001918U &&
        i + 8 < pm16_offset && buffer[i+8] == 0xA1) {
      found_rp18_in_teardown = TRUE;
      break;
    }
  }
  CHECK(!found_rp18_in_teardown, "Trampoline teardown must NOT restore Root Port Bus Numbers (0x80001918) to retain bus routing across S3 wake");

  /* Explicit Sanity Check 2b: Verify 16-bit trampoline is 100% stack-free (no push eax 66 50 or pop eax 66 58) */
  BOOLEAN found_stack_op = FALSE;
  for (UINT32 i = 0; i < pm32_offset; i++) {
    if (buffer[i] == 0x66 && i + 1 < pm32_offset && (buffer[i+1] == 0x50 || buffer[i+1] == 0x58)) {
      found_stack_op = TRUE;
      break;
    }
    if (buffer[i] == 0x50 || buffer[i] == 0x58) {
      found_stack_op = TRUE;
      break;
    }
  }
  CHECK(!found_stack_op, "16-bit trampoline must be 100% stack-free (no push eax or pop eax)");

  BOOLEAN found_mov_ecx_eax = FALSE;
  BOOLEAN found_mov_eax_ecx = FALSE;
  for (UINT32 i = 0; i < bytes - 2; i++) {
    if (buffer[i] == 0x66 && buffer[i+1] == 0x89 && buffer[i+2] == 0xC1) found_mov_ecx_eax = TRUE;
    if (buffer[i] == 0x66 && buffer[i+1] == 0x89 && buffer[i+2] == 0xC8) found_mov_eax_ecx = TRUE;
  }
  CHECK(found_mov_ecx_eax, "Trampoline must contain mov ecx, eax (66 89 C1)");
  CHECK(found_mov_eax_ecx || memchr(buffer, 0x91, pm32_offset) != NULL, "Trampoline must restore EAX from ECX stack-free (66 89 C8 or 66 91)");

  /* Explicit Sanity Check 3: Verify NO MSE/BME (0x0006) is written to Offset 0x04 */
  BOOLEAN found_write_04 = FALSE;
  for (UINT32 i = 0; i < bytes - 6; i++) {
    if (buffer[i] == 0x66 && buffer[i+1] == 0xB8 && buffer[i+2] == 0x04 && buffer[i+3] == 0x19 &&
        buffer[i+4] == 0x00 && buffer[i+5] == 0x80) {
      found_write_04 = TRUE;
      break;
    }
  }
  CHECK(!found_write_04, "Trampoline must NEVER write MSE/BME to Root Port Command Register 0x80001904");

  /* Explicit Sanity Check 4: Dynamic PMCSR discovery & Link Status table verification */
  UINT32 *tbl = (UINT32 *)(buffer + GU_TRAMP_PCI_PROBE_ADDRS_OFF);
  CHECK(tbl[0] == 0x80001900U, "Bridge DevID address at tbl[0]");
  CHECK(tbl[1] == 0x80001904U, "Bridge Cmd/Stat address at tbl[1]");
  CHECK(tbl[2] == 0x80001918U, "Bridge BusNum address at tbl[2]");
  CHECK(tbl[3] == 0x80001960U, "Bridge PCIe Link Status address at tbl[3] (Cap 0x10 + 0x10)");
  CHECK(tbl[4] == 0x802D0000U, "GPU DevID address at tbl[4]");
  CHECK(tbl[5] == 0x802D0004U, "GPU Cmd/Stat address at tbl[5]");
  CHECK(tbl[6] == 0x802D0010U, "GPU BAR0 address at tbl[6]");
  CHECK(tbl[7] == 0x802D0064U, "GPU PMCSR address at tbl[7] (Cap 0x01 + 0x04)");

  /* Explicit Sanity Check 5: Verify linear-to-real-mode segment:offset conversion for Windows HalpRMStub (0x00001000) */
  UINT32 win_stub_vec = 0x00001000U;
  UINT16 win_stub_ip = (UINT16)(win_stub_vec & 0x0FU);
  UINT16 win_stub_cs = (UINT16)(((win_stub_vec >> 4) | ((win_stub_vec >> 16) << 12)) & 0xFFFFU);
  CHECK(win_stub_ip == 0x0000, "HalpRMStub (0x00001000) offset must be 0x0000");
  CHECK(win_stub_cs == 0x0100, "HalpRMStub (0x00001000) segment must be 0x0100 (CS = vec >> 4)");
  CHECK(((UINT32)win_stub_cs << 4) + win_stub_ip == win_stub_vec, "Real mode CS:IP (0100:0000) must resolve to linear 0x00001000");

  /* Disassembly & Hex Dump Output for User Inspection */
  printf("\n==================== 16-BIT REAL-MODE TRAMPOLINE DISASSEMBLY & HEX DUMP ====================\n");
  printf("Trampoline Total Size: %u bytes (Execution envelope: 0x0000 - 0x09FF, 2560 bytes limit (<0x0A00))\n", bytes);
  printf("--------------------------------------------------------------------------------------------\n");
  const char *desc = "";
  BOOLEAN found_hlt = FALSE;
  for (UINT32 i = 0; i < bytes; ) {
    char hex[48] = {0};
    UINT32 instr_len = 1;
    desc = "";

    if (buffer[i] == 0xF4) { instr_len = 1; desc = "hlt"; found_hlt = TRUE; }
    else if (buffer[i] == 0xFA) { instr_len = 1; desc = "cli"; }
    else if (buffer[i] == 0xFC) { instr_len = 1; desc = "cld"; }
    else if (buffer[i] == 0x8C && buffer[i+1] == 0xC8) { instr_len = 2; desc = "mov ax, cs"; }
    else if (buffer[i] == 0x8E && buffer[i+1] == 0xD8) { instr_len = 2; desc = "mov ds, ax"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x26 && buffer[i+2] == 0x70 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov [0x0A70], sp               ; Save BIOS SP"; }
    else if (buffer[i] == 0x8C && buffer[i+1] == 0x16 && buffer[i+2] == 0x72 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov [0x0A72], ss               ; Save BIOS SS"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xC7 && buffer[i+2] == 0x06 && buffer[i+3] == 0x1C && buffer[i+4] == 0x0A) { instr_len = 9; desc = "mov dword [0x0A1C], 0x53335F54  ; Telemetry Magic 'S3_T'"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x06 && buffer[i+2] == 0x10 && buffer[i+3] == 0x0A && buffer[i+4] == 0x01) { instr_len = 6; desc = "mov word [0x0A10], 0xAA01        ; Checkpoint 1: Entry"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x06 && buffer[i+2] == 0x10 && buffer[i+3] == 0x0A && buffer[i+4] == 0xAD) { instr_len = 6; desc = "mov word [0x0A10], 0xDEAD        ; Fault: No Waking Vector"; }
    else if (buffer[i] == 0xBE && buffer[i+1] == 0x20 && buffer[i+2] == 0x0A) { instr_len = 3; desc = "mov si, 0x0A20                   ; Probe addresses table"; }
    else if (buffer[i] == 0xBE && buffer[i+1] == 0x30 && buffer[i+2] == 0x0A) { instr_len = 3; desc = "mov si, 0x0A30                   ; Post-arm probe addresses table"; }
    else if (buffer[i] == 0xBF && buffer[i+1] == 0x40 && buffer[i+2] == 0x0A) { instr_len = 3; desc = "mov di, 0x0A40                   ; Raw probe results destination"; }
    else if (buffer[i] == 0xBF && buffer[i+1] == 0x60 && buffer[i+2] == 0x0A) { instr_len = 3; desc = "mov di, 0x0A60                   ; Post-arm probe results destination"; }
    else if (buffer[i] == 0xB9 && buffer[i+1] == 0x08 && buffer[i+2] == 0x00) { instr_len = 3; desc = "mov cx, 8                         ; Probe count = 8 dwords"; }
    else if (buffer[i] == 0xB9 && buffer[i+1] == 0x04 && buffer[i+2] == 0x00) { instr_len = 3; desc = "mov cx, 4                         ; Probe count = 4 dwords"; }
    else if (buffer[i] == 0xE4 && buffer[i+1] == 0x80) { instr_len = 2; desc = "in al, 0x80                       ; ~1us I/O pacing read"; }
    else if (buffer[i] == 0xB9 && buffer[i+1] == 0x64 && buffer[i+2] == 0x00 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov ecx, 100                      ; GSP pre-delay (~100us)"; }
    else if (buffer[i] == 0xB9 && buffer[i+1] == 0x40 && buffer[i+2] == 0x42 && buffer[i+3] == 0x0F && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov ecx, 1000000                  ; GSP Halt Poll (up to ~1s)"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x8B && buffer[i+2] == 0x04) { instr_len = 3; desc = ".probe_loop: mov eax, [si]       ; Read target 0xCF8 address"; }
    else if (buffer[i] == 0xBA && buffer[i+1] == 0xF8 && buffer[i+2] == 0x0C && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov edx, 0x0CF8"; }
    else if (buffer[i] == 0xBA && buffer[i+1] == 0xFC && buffer[i+2] == 0x0C && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov edx, 0x0CFC"; }
    else if (buffer[i] == 0xBA && buffer[i+1] == 0xF8 && buffer[i+2] == 0x0C) { instr_len = 3; desc = "mov dx, 0x0CF8"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xEF) { instr_len = 2; desc = "out dx, eax                    ; [66 EF] Write PCI config address"; }
    else if (buffer[i] == 0xBA && buffer[i+1] == 0xFC && buffer[i+2] == 0x0C) { instr_len = 3; desc = "mov dx, 0x0CFC"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xED) { instr_len = 2; desc = "in eax, dx                     ; [66 ED] Read PCI config dword"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x89 && buffer[i+2] == 0x05) { instr_len = 3; desc = "mov [di], eax                   ; Store raw probe result"; }
    else if (buffer[i] == 0x83 && buffer[i+1] == 0xC6 && buffer[i+2] == 0x04) { instr_len = 3; desc = "add si, 4"; }
    else if (buffer[i] == 0x83 && buffer[i+1] == 0xC7 && buffer[i+2] == 0x04) { instr_len = 3; desc = "add di, 4"; }
    else if (buffer[i] == 0xE2) { instr_len = 2; desc = "loop .probe_loop                ; Loop 8 times"; }
    else if (buffer[i] == 0xA1 && buffer[i+1] == 0x40 && buffer[i+2] == 0x0A) { instr_len = 3; desc = "mov ax, [0x0A40]                 ; Bridge DevID vendor"; }
    else if (buffer[i] == 0x3D && buffer[i+1] == 0x22 && buffer[i+2] == 0x10) { instr_len = 3; desc = "cmp ax, 0x1022                   ; AMD Vendor ID?"; }
    else if (buffer[i] == 0x0F && buffer[i+1] == 0x85) {
      instr_len = 4;
      desc = "jne near .skip_bridge            ; Near jump if bridge not present";
    }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xA1 && buffer[i+2] == 0x48 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov eax, [0x0A48]                ; Load original Root Port Bus Numbers"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x25 && buffer[i+2] == 0x00 && buffer[i+5] == 0xFF) { instr_len = 6; desc = "and eax, 0xFF000000              ; Preserve Latency Timer (bits 31:24)"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x0D && buffer[i+2] == 0x00 && buffer[i+3] == 0x2D) { instr_len = 6; desc = "or eax, 0x002D2D00               ; Pri=0, Sec=0x2D, Sub=0x2D"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x89 && buffer[i+2] == 0xC1) { instr_len = 3; desc = "mov ecx, eax                    ; Save modified bus numbers in ECX (stack-free)"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xB8 && buffer[i+2] == 0x18 && buffer[i+3] == 0x19) { instr_len = 6; desc = "mov eax, 0x80001918              ; Root Port 0:3:1 Offset 0x18"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x89 && buffer[i+2] == 0xC8) { instr_len = 3; desc = "mov eax, ecx                    ; Restore modified bus numbers from ECX (stack-free)"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xB8 && buffer[i+2] == 0x00 && buffer[i+4] == 0x2D) { instr_len = 6; desc = "mov eax, 0x802D0000              ; GPU 45:0:0 DevID"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xA3 && buffer[i+2] == 0x60 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov [0x0A60], eax                ; Store Post-Arm GPU DevID"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xB8 && buffer[i+2] == 0x04 && buffer[i+4] == 0x2D) { instr_len = 6; desc = "mov eax, 0x802D0004              ; GPU 45:0:0 Cmd/Stat"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xA3 && buffer[i+2] == 0x64 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov [0x0A64], eax                ; Store Post-Arm GPU Cmd/Stat"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xB8 && buffer[i+2] == 0x10 && buffer[i+4] == 0x2D) { instr_len = 6; desc = "mov eax, 0x802D0010              ; GPU 45:0:0 BAR0"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xA3 && buffer[i+2] == 0x68 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov [0x0A68], eax                ; Store Post-Arm GPU BAR0"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xA1 && buffer[i+2] == 0x3C && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov eax, [0x0A3C]                ; Load dynamic GPU PMCSR address"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xA3 && buffer[i+2] == 0x6C && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov [0x0A6C], eax                ; Store Post-Arm GPU PMCSR"; }
    else if (buffer[i] == 0x2E && buffer[i+1] == 0x0F && buffer[i+2] == 0x01 && buffer[i+3] == 0x16 && buffer[i+4] == 0x08 && buffer[i+5] == 0x0B) { instr_len = 6; desc = ".skip_bridge: lgdt [cs:0x0B08]   ; Load 32-bit PM GDTR"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x0F && buffer[i+2] == 0x20 && buffer[i+3] == 0xC0) { instr_len = 4; desc = "mov eax, cr0"; }
    else if (buffer[i] == 0x0C && buffer[i+1] == 0x01) { instr_len = 2; desc = "or al, 1                         ; Set CR0.PE = 1 (Enable PM)"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x0F && buffer[i+2] == 0x22 && buffer[i+3] == 0xC0) { instr_len = 4; desc = "mov cr0, eax"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xEA) { instr_len = 7; desc = "jmp far 0x0008:pm32             ; JUMP TO 32-BIT FLAT PM!"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xB8 && buffer[i+2] == 0x10 && buffer[i+3] == 0x00) { instr_len = 4; desc = "pm32: mov ax, 0x0010            ; 32-bit Flat Data Selector"; }
    else if (buffer[i] == 0x8E && buffer[i+1] == 0xD8) { instr_len = 2; desc = "mov ds, ax"; }
    else if (buffer[i] == 0x8E && buffer[i+1] == 0xC0) { instr_len = 2; desc = "mov es, ax"; }
    else if (buffer[i] == 0x8E && buffer[i+1] == 0xE0) { instr_len = 2; desc = "mov fs, ax"; }
    else if (buffer[i] == 0x8E && buffer[i+1] == 0xE8) { instr_len = 2; desc = "mov gs, ax"; }
    else if (buffer[i] == 0x8E && buffer[i+1] == 0xD0) { instr_len = 2; desc = "mov ss, ax"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x05 && buffer[i+6] == 0x21 && buffer[i+7] == 0x4F && buffer[i+8] == 0x4B && buffer[i+9] == 0x32) { instr_len = 10; desc = "mov dword [HighAcpiPage+0x500], 0x324B4F21 ; Write Diagnostic Marker '!OK2'"; }
    else if (buffer[i] == 0x0F && buffer[i+1] == 0x01 && buffer[i+2] == 0x1D) { instr_len = 7; desc = "lidt [TrampolinePhys+0x0B00]        ; Load Real-Mode IDTR (Limit=0x3FF, Base=0)"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x35) { instr_len = 6; desc = "mov esi, [ShadowFACS+0x0C]     ; Read Windows Waking Vector from High Page"; }
    else if (buffer[i] == 0x85 && buffer[i+1] == 0xF6) { instr_len = 2; desc = "test esi, esi"; }
    else if (buffer[i] == 0x75 && buffer[i+1] == 0x05) { instr_len = 2; desc = "jnz .have_vec32"; }
    else if (buffer[i] == 0xBE && buffer[i+1] == 0x00 && buffer[i+2] == 0x10 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov esi, 0x00001000             ; Fallback to HalpRMStub"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x35) { instr_len = 6; desc = ".have_vec32: mov [0x0009EA14], esi ; Save captured vector to telemetry"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0xF0) { instr_len = 2; desc = "mov eax, esi"; }
    else if (buffer[i] == 0x83 && buffer[i+1] == 0xE0 && buffer[i+2] == 0x0F) { instr_len = 3; desc = "and eax, 0x0F                   ; Offset = AX & 0x0F"; }
    else if (buffer[i] == 0xC1 && buffer[i+1] == 0xEE && buffer[i+2] == 0x04) { instr_len = 3; desc = "shr esi, 4                      ; Segment = ESI >> 4"; }
    else if (buffer[i] == 0xC1 && buffer[i+1] == 0xE6 && buffer[i+2] == 0x10) { instr_len = 3; desc = "shl esi, 16                      ; Pack segment into high word"; }
    else if (buffer[i] == 0x09 && buffer[i+1] == 0xF0) { instr_len = 2; desc = "or eax, esi                       ; EAX = (Segment << 16) | Offset"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xA3) { instr_len = 5; desc = "mov [disp32], ax                ; Store target/telemetry offset"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x89 && buffer[i+2] == 0x35) { instr_len = 6; desc = "mov [disp32], si                ; Store target/telemetry segment"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0xC7 && buffer[i+2] == 0x05) { instr_len = 9; desc = "mov word [0x0009EA12], 0xAA02   ; Exit Checkpoint 0xAA02"; }
    else if (buffer[i] == 0xBE && buffer[i+1] == 0x04 && buffer[i+2] == 0x0A && buffer[i+3] == 0x09 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov esi, 0x0009EA04             ; Telemetry Source (+0x0A04)"; }
    else if (buffer[i] == 0xBF) { instr_len = 5; desc = "mov edi, HighAcpiPage+0x504     ; Telemetry Destination (+0x504)"; }
    else if (buffer[i] == 0xB9 && buffer[i+1] == 0x1F && buffer[i+2] == 0x00 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov ecx, 31                    ; 31 DWORDs = 124 bytes"; }
    else if (buffer[i] == 0xF3 && buffer[i+1] == 0xA5) { instr_len = 2; desc = "rep movsd                       ; MIRROR TELEMETRY TO HIGH MEMORY!"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x05 && buffer[i+6] == 0x4B && buffer[i+7] == 0x4C && buffer[i+8] == 0x4E && buffer[i+9] == 0x55) { instr_len = 10; desc = "mov dword [dest+0x80], 'UNLK'   ; S3 Wake Unlock Telemetry Magic"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x00 && buffer[i+3] == 0x02 && buffer[i+4] == 0x00 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x000200]             ; Read PMC_ENABLE (BAR0 + 0x200)"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0xC0 && buffer[i+3] == 0x03 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x8403C0]             ; Read SEC2_RESET (BAR0 + 0x8403C0)"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0xF4 && buffer[i+3] == 0x00 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x8400F4]             ; Read SEC2_HWCFG2 (BAR0 + 0x8400F4)"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x68 && buffer[i+3] == 0x16 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x841668]             ; Read FALCON2_BCR (BAR0 + 0x841668)"; }
    else if (buffer[i] == 0x0C && buffer[i+1] == 0x01) { instr_len = 2; desc = "or al, 0x01                         ; Assert SEC2 reset (or al, 1)"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0xC0 && buffer[i+3] == 0x03 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x8403C0], eax             ; Write SEC2_RESET (BAR0 + 0x8403C0)"; }
    else if (buffer[i] == 0xB9 && buffer[i+1] == 0x0A && buffer[i+2] == 0x00 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov ecx, 10                        ; 10 dummy read cycle counter"; }
    else if (buffer[i] == 0xB9 && buffer[i+1] == 0x50 && buffer[i+2] == 0xC3 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov ecx, 50000                     ; 50k scrub/BCR poll timeout"; }
    else if (buffer[i] == 0xA9 && buffer[i+1] == 0x00 && buffer[i+2] == 0x10 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "test eax, 0x00001000               ; Test HWCFG2 Bit 12 (Memory Scrubbing)"; }
    else if (buffer[i] == 0x74 && buffer[i+1] == 0x03) { instr_len = 2; desc = "jz <rel8>                         ; Jump if condition met (scrub/BCR done)"; }
    else if (buffer[i] == 0x31 && buffer[i+1] == 0xC0) { instr_len = 2; desc = "xor eax, eax                     ; Clear EAX (write 0 to BCR)"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x68 && buffer[i+3] == 0x16 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x841668], eax             ; Write 0 to FALCON2_BCR (exit Boot-ROM)"; }
    else if (buffer[i] == 0x80 && buffer[i+1] == 0xE2 && buffer[i+2] == 0x11) { instr_len = 3; desc = "and dl, 0x11                     ; Mask BCR mode bits (0x11)"; }
    else if (buffer[i] == 0x80 && buffer[i+1] == 0xFA && buffer[i+2] == 0x01) { instr_len = 3; desc = "cmp dl, 0x01                     ; Check BCR unmasked (BCR & 0x11 == 1)"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x84 && buffer[i+3] == 0x00 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840084], eax             ; Write Chip-ID to SEC2 (BAR0 + 0x840084)"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x00 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x840100]             ; Read SEC2 Falcon CPUCTL"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x24 && buffer[i+3] == 0x06 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x840624]             ; Read SEC2 FBIF Trans Enable"; }
    else if (buffer[i] == 0x0C && buffer[i+1] == 0x80) { instr_len = 2; desc = "or al, 0x80                         ; Enable SEC2 FBIF Trans"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x24 && buffer[i+3] == 0x06 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840624], eax             ; Write SEC2 FBIF Trans Enable"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x0C && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x84010C], eax             ; Clear Falcon IRQ Status (0x0084010C)"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x00 && buffer[i+3] == 0x06 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x840600]             ; Read SEC2 FBIF Trans Mode"; }
    else if (buffer[i] == 0x24 && buffer[i+1] == 0xF8) { instr_len = 2; desc = "and al, 0xF8                        ; Clear FBIF mode bits [2:0]"; }
    else if (buffer[i] == 0x0C && buffer[i+1] == 0x05) { instr_len = 2; desc = "or al, 0x05                         ; Set FBIF trans mode 5"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x00 && buffer[i+3] == 0x06 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840600], eax             ; Write SEC2 FBIF Trans Mode"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x10 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x840110], booter_dma ; Set SEC2 Booter DMA Address Low"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x10 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840110], eax             ; Set SEC2 Booter DMA Address Low (Base Reload)"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x28 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840128], eax             ; Set SEC2 Booter DMA Address High (0)"; }
    else if (buffer[i] == 0x31 && buffer[i+1] == 0xFF) { instr_len = 2; desc = "xor edi, edi                     ; Clear Chunk DMA Dest (0)"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0xBB && buffer[i+2] == 0x14 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840114], edi             ; Set Chunk DMA Dest"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x93 && buffer[i+2] == 0x1C && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x84011C], edx             ; Set Chunk DMA Offset"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0xB3 && buffer[i+2] == 0x18 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840118], esi             ; Set Chunk DMA Cmd & Kick"; }
    else if (buffer[i] == 0x81 && buffer[i+1] == 0xC7 && buffer[i+2] == 0x00 && buffer[i+3] == 0x01 && buffer[i+4] == 0x00 && buffer[i+5] == 0x00) { instr_len = 6; desc = "add edi, 256                     ; Advance Chunk DMA Dest by 256"; }
    else if (buffer[i] == 0x81 && buffer[i+1] == 0xC2 && buffer[i+2] == 0x00 && buffer[i+3] == 0x01 && buffer[i+4] == 0x00 && buffer[i+5] == 0x00) { instr_len = 6; desc = "add edx, 256                     ; Advance Chunk DMA Offset by 256"; }
    else if (buffer[i] == 0x81 && buffer[i+1] == 0xFE && buffer[i+2] == 0x14 && buffer[i+3] == 0x06 && buffer[i+4] == 0x00 && buffer[i+5] == 0x00) { instr_len = 6; desc = "cmp esi, 0x0614                   ; Check IMEM DMA transfer complete"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x81 && buffer[i+2] == 0xFE && buffer[i+3] == 0x14 && buffer[i+4] == 0x06) { instr_len = 5; desc = "cmp si, 0x0614                     ; Check IMEM DMA transfer complete (16-bit prefix)"; }
    else if (buffer[i] == 0xA8 && buffer[i+1] == 0x02) { instr_len = 2; desc = "test al, 0x02                       ; Test DMA Complete Bit"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x10 && buffer[i+3] == 0x12 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x841210], 16       ; Set Falcon2 sig_dmem = 16"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x9C && buffer[i+3] == 0x11 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x84119C], 1        ; Set Falcon2 engine_mask = 1"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x98 && buffer[i+3] == 0x11 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x841198], 3        ; Set Falcon2 ucode_id = 3"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x80 && buffer[i+3] == 0x11 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x841180], 1        ; Set Falcon2 config = 1"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x04 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x840104], 256      ; Set Falcon Entry Point Vector = 256"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x40 && buffer[i+3] == 0x00 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x840040], meta_dma ; Set Mailbox 0 = meta_dma"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && buffer[i+2] == 0x44 && buffer[i+3] == 0x00 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov [ebx+0x840044], eax             ; Clear Mailbox 1 (0)"; }
    else if (buffer[i] == 0x3D && buffer[i+1] == 0x01 && buffer[i+2] == 0x00 && buffer[i+3] == 0xB0 && buffer[i+4] == 0x67) { instr_len = 5; desc = "cmp eax, 0x67B00001               ; Check Mailbox 0 marker (AD102 unlock)"; }
    else if (buffer[i] == 0x3D && *(UINT32 *)(buffer + i + 1) == (UINT32)(ctx_phys + 0x2A000ULL)) { instr_len = 5; desc = "cmp eax, meta_dma                 ; Guard Halt Bit until Falcon Mailbox0 != meta_dma"; }
    else if (buffer[i] == 0xB2 && buffer[i+1] == 0xF8) { instr_len = 2; desc = "mov dl, 0xF8                     ; Fast switch to PCI CF8 port"; }
    else if (buffer[i] == 0xB2 && buffer[i+1] == 0xFC) { instr_len = 2; desc = "mov dl, 0xFC                     ; Fast switch to PCI CFC port"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x00 && buffer[i+3] == 0x01 && buffer[i+4] == 0x84 && buffer[i+5] == 0x00 && *(UINT32 *)(buffer + i + 6) == 2U) { instr_len = 10; desc = "mov dword [ebx+0x840100], 2          ; Start SEC2 Falcon CPU (CPUCTL = 2)"; }
    else if (buffer[i] == 0xB9 && *(UINT32 *)(buffer + i + 1) == 6000000U) { instr_len = 5; desc = "mov ecx, 6000000                  ; SEC2 Booter Poll (~5-6s budget)"; }
    else if (buffer[i] == 0xB9 && *(UINT32 *)(buffer + i + 1) == 100000U) { instr_len = 5; desc = "mov ecx, 100000                   ; SEC2 Halt Wait (100k Port-0x80)"; }
    else if (buffer[i] == 0xA8 && buffer[i+1] == 0x10) { instr_len = 2; desc = "test al, 0x10                       ; Test Falcon Halt Bit (CPUCTL & 0x10)"; }
    else if (buffer[i] == 0x49) { instr_len = 1; desc = "dec ecx                             ; Decrement loop/poll timeout"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x1C && buffer[i+3] == 0x38 && buffer[i+4] == 0x82 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x82381C], 0x88888888 ; Write SS0 Rate-Limiter Unlock"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && buffer[i+2] == 0x20 && buffer[i+3] == 0x38 && buffer[i+4] == 0x82 && buffer[i+5] == 0x00) { instr_len = 10; desc = "mov dword [ebx+0x823820], 0x00000008 ; Write SS1 Rate-Limiter Unlock"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x1C && buffer[i+3] == 0x38 && buffer[i+4] == 0x82 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x82381C]         ; Read SS0 Selector"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x20 && buffer[i+3] == 0x38 && buffer[i+4] == 0x82 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x823820]         ; Read SS1 Selector"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x04 && buffer[i+3] == 0x38 && buffer[i+4] == 0x82 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x823804]         ; Read PLM Permission Level Mask"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x18 && buffer[i+3] == 0x38 && buffer[i+4] == 0x82 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x823818]         ; Read Readout 1 Speed Status"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x24 && buffer[i+3] == 0xA8 && buffer[i+4] == 0x1F && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x001fa824]       ; Read pre-booter WPR2_LO"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x28 && buffer[i+3] == 0xA8 && buffer[i+4] == 0x1F && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x001fa828]       ; Read WPR2_HI (pre-booter or post-FLR)"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0xE0 && buffer[i+3] == 0x0C && buffer[i+4] == 0x10 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x00100ce0]       ; Read MMU Local Memory Range"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x34 && buffer[i+3] == 0x82 && buffer[i+4] == 0x11 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x00118234]       ; Read GFW-BOOT Completion"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0xF4 && buffer[i+3] == 0x00 && buffer[i+4] == 0x11 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x001100f4]       ; Read GSP_HWCFG2"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x00 && buffer[i+3] == 0x01 && buffer[i+4] == 0x11 && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x00110100]       ; Read GSP_CPUCTL"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x83) { instr_len = 10; desc = "mov dword [ebx+disp32], imm32"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0xBB) { instr_len = 6; desc = "mov [ebx+disp32], edi"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x93) { instr_len = 6; desc = "mov [ebx+disp32], edx"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0xB3) { instr_len = 6; desc = "mov [ebx+disp32], esi"; }
    else if (buffer[i] == 0x4D) { instr_len = 1; desc = "dec ebp"; }
    else if (buffer[i] == 0x31 && buffer[i+1] == 0xD2) { instr_len = 2; desc = "xor edx, edx"; }
    else if (buffer[i] == 0xA8) { instr_len = 2; desc = "test al, imm8"; }
    else if (buffer[i] == 0xB1) { instr_len = 2; desc = "mov cl, imm8"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83) { instr_len = 6; desc = "mov eax, [ebx+disp32]          ; Generic MMIO read"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0x83) { instr_len = 6; desc = "mov [ebx+disp32], eax          ; Generic MMIO write"; }
    else if (buffer[i] == 0xBB) { instr_len = 5; desc = "mov ebx, bar0_target           ; Set BAR0 Base Pointer"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x03) { instr_len = 2; desc = "mov eax, [ebx]                 ; Read GPU PMC Boot0 / Chip ID"; }
    else if (buffer[i] == 0x3D && buffer[i+1] == 0xFF && buffer[i+2] == 0xFF && buffer[i+3] == 0xFF && buffer[i+4] == 0xFF) { instr_len = 5; desc = "cmp eax, 0xFFFFFFFF           ; Check MMIO master abort"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0xC2) { instr_len = 2; desc = "mov edx, eax"; }
    else if (buffer[i] == 0x89 && buffer[i+1] == 0xC1) { instr_len = 2; desc = "mov ecx, eax"; }
    else if (buffer[i] == 0xC1 && buffer[i+1] == 0xEA && buffer[i+2] == 0x10) { instr_len = 3; desc = "shr edx, 16"; }
    else if (buffer[i] == 0x81 && buffer[i+1] == 0xFA && buffer[i+2] == 0xDF && buffer[i+3] == 0xBA && buffer[i+4] == 0x00 && buffer[i+5] == 0x00) { instr_len = 6; desc = "cmp edx, 0x0000BADF           ; Check 0xBADF fault"; }
    else if (buffer[i] == 0x0F && buffer[i+1] == 0x84) { instr_len = 6; desc = "jz near .fault                 ; Jump on MMIO error"; }
    else if (buffer[i] == 0x81 && buffer[i+1] == 0xF9 && buffer[i+2] == 0x88 && buffer[i+3] == 0x88 && buffer[i+4] == 0x88 && buffer[i+5] == 0x88) { instr_len = 6; desc = "cmp ecx, 0x88888888           ; Verify SS0 == 0x88888888"; }
    else if (buffer[i] == 0x83 && buffer[i+1] == 0xE2 && buffer[i+2] == 0x0F) { instr_len = 3; desc = "and edx, 0x0F"; }
    else if (buffer[i] == 0x83 && buffer[i+1] == 0xFA && buffer[i+2] == 0x08) { instr_len = 3; desc = "cmp edx, 0x08                 ; Verify (SS1 & 0xF) == 0x08"; }
    else if (buffer[i] == 0x0C && buffer[i+1] == 0x02) { instr_len = 2; desc = "or al, 0x02                    ; Enable Memory Space (MSE)"; }
    else if (buffer[i] == 0x0C && buffer[i+1] == 0x06) { instr_len = 2; desc = "or al, 0x06                    ; Enable MSE + BME"; }
    else if (buffer[i] == 0x85 && buffer[i+1] == 0xC0) { instr_len = 2; desc = "test eax, eax"; }
    else if (buffer[i] == 0x75) { instr_len = 2; desc = "jnz <rel8>"; }
    else if (buffer[i] == 0xEB) { instr_len = 2; desc = "jmp short .teardown"; }
    else if (buffer[i] == 0xBD) { instr_len = 5; desc = "mov ebp, ret_target             ; Link subroutine return address"; }
    else if (buffer[i] == 0xE9) { instr_len = 5; desc = "jmp near .sec2_sub_start        ; Call SEC2 reset subroutine"; }
    else if (buffer[i] == 0xFF && buffer[i+1] == 0xE5) { instr_len = 2; desc = "jmp ebp                         ; Return from SEC2 reset subroutine"; }
    else if (buffer[i] == 0xB1 && buffer[i+1] == 0x0A) { instr_len = 2; desc = "mov cl, 10                      ; 10 dummy read cycle counter"; }
    else if (buffer[i] == 0xB1 && buffer[i+1] == 0x64) { instr_len = 2; desc = "mov cl, 100                     ; 100 poll cycle counter"; }
    else if (buffer[i] == 0xFC) { instr_len = 1; desc = "cld                             ; Clear direction flag"; }
    else if (buffer[i] == 0x31 && buffer[i+1] == 0xC9) { instr_len = 2; desc = "xor ecx, ecx                     ; Clear ECX"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x04 && buffer[i+2] == 0x19 && buffer[i+3] == 0x00 && buffer[i+4] == 0x80) { instr_len = 5; desc = "mov eax, 0x80001904          ; Root Port 0:3:1 Cmd/Stat"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x20 && buffer[i+2] == 0x19 && buffer[i+3] == 0x00 && buffer[i+4] == 0x80) { instr_len = 5; desc = "mov eax, 0x80001920          ; Root Port 0:3:1 MemBase/Limit"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x18 && buffer[i+2] == 0x19 && buffer[i+3] == 0x00 && buffer[i+4] == 0x80) { instr_len = 5; desc = "mov eax, 0x80001918          ; Root Port 0:3:1 Bus Numbers"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x10 && buffer[i+2] == 0x00 && buffer[i+3] == 0x2D && buffer[i+4] == 0x80) { instr_len = 5; desc = "mov eax, 0x802D0010          ; GPU 45:0:0 BAR0"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x04 && buffer[i+2] == 0x00 && buffer[i+3] == 0x2D && buffer[i+4] == 0x80) { instr_len = 5; desc = "mov eax, 0x802D0004          ; GPU 45:0:0 Cmd/Stat"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x00 && buffer[i+2] == 0xFB && buffer[i+3] == 0xF0 && buffer[i+4] == 0xFB) { instr_len = 5; desc = "mov eax, 0xFBF0FB00          ; Bridge 16MB Mem Window 0xFB00..0xFBF0"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x01 && buffer[i+2] == 0x00 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov eax, 1                   ; Status = Success (1)"; }
    else if (buffer[i] == 0x48) { instr_len = 1; desc = "dec eax                         ; Status = Success (1)"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x02 && buffer[i+2] == 0x00 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "mov eax, 2                   ; Status = Mismatch (2)"; }
    else if (buffer[i] == 0x0D && buffer[i+1] == 0x00 && buffer[i+2] == 0x80 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) { instr_len = 5; desc = "or eax, 0x00008000             ; Initiate PCIe FLR (Bit 15)"; }
    else if (buffer[i] == 0xE4 && buffer[i+1] == 0x80) { instr_len = 2; desc = "in al, 0x80                     ; Port 0x80 I/O read delay (~1us)"; }
    else if (buffer[i] == 0x3D && buffer[i+1] == 0xDE && buffer[i+2] == 0x10 && buffer[i+3] == 0x84 && buffer[i+4] == 0x26) { instr_len = 5; desc = "cmp eax, 0x268410DE             ; Poll GPU Config VID/DID"; }
    else if (buffer[i] == 0x3D && buffer[i+1] == 0xA1 && buffer[i+2] == 0x00 && buffer[i+3] == 0x20 && buffer[i+4] == 0x19) { instr_len = 5; desc = "cmp eax, 0x192000A1             ; Poll GPU BAR0 Chip ID"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && buffer[i+2] == 0x28 && buffer[i+3] == 0xA8 && buffer[i+4] == 0x1F && buffer[i+5] == 0x00) { instr_len = 6; desc = "mov eax, [ebx+0x001fa828]       ; Read post-FLR WPR2_HI"; }
    else if (buffer[i] == 0xB8) { instr_len = 5; desc = "mov eax, imm32"; }
    else if (buffer[i] == 0xA3) { instr_len = 5; desc = "mov [disp32], eax"; }
    else if (buffer[i] == 0xA1) { instr_len = 5; desc = "mov eax, [disp32]"; }
    else if (buffer[i] == 0xEF) { instr_len = 1; desc = "out dx, eax (32-bit PM)"; }
    else if (buffer[i] == 0xED) { instr_len = 1; desc = "in eax, dx (32-bit PM)"; }
    else if (buffer[i] == 0xC7 && buffer[i+1] == 0x05) { instr_len = 10; desc = "mov dword [disp32], imm32"; }
    else if (buffer[i] == 0xEA && buffer[i+5] == 0x18 && buffer[i+6] == 0x00) { instr_len = 7; desc = "jmp far 0x0018:pm16             ; Drop to 16-bit Protected Mode"; }
    else if (buffer[i] == 0xB8 && buffer[i+1] == 0x20 && buffer[i+2] == 0x00) { instr_len = 3; desc = "pm16: mov ax, 0x0020            ; 16-bit PM Data Selector"; }
    else if (buffer[i] == 0x24 && buffer[i+1] == 0xFE) { instr_len = 2; desc = "and al, 0xFE                    ; Clear CR0.PE (Return to Real Mode)"; }
    else if (buffer[i] == 0xEA) { instr_len = 5; desc = "jmp far 0x9E00:rm_exit          ; Jump to 16-bit Real Mode!"; }
    else if (buffer[i] == 0x8E && buffer[i+1] == 0x16 && buffer[i+2] == 0x72 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "rm_exit: mov ss, [0x0A72]        ; Restore BIOS SS"; }
    else if (buffer[i] == 0x8B && buffer[i+1] == 0x26 && buffer[i+2] == 0x70 && buffer[i+3] == 0x0A) { instr_len = 4; desc = "mov sp, [0x0A70]                 ; Restore BIOS SP"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x31 && buffer[i+2] == 0xC0) { instr_len = 3; desc = "xor eax, eax                     ; Clean register state for OS"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x31 && buffer[i+2] == 0xDB) { instr_len = 3; desc = "xor ebx, ebx"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x31 && buffer[i+2] == 0xC9) { instr_len = 3; desc = "xor ecx, ecx"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x31 && buffer[i+2] == 0xD2) { instr_len = 3; desc = "xor edx, edx"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x31 && buffer[i+2] == 0xF6) { instr_len = 3; desc = "xor esi, esi"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x31 && buffer[i+2] == 0xFF) { instr_len = 3; desc = "xor edi, edi"; }
    else if (buffer[i] == 0x66 && buffer[i+1] == 0x31 && buffer[i+2] == 0xED) { instr_len = 3; desc = "xor ebp, ebp"; }
    else if (buffer[i] == 0x2E && buffer[i+1] == 0xFF && buffer[i+2] == 0x2E && buffer[i+3] == 0x00 && buffer[i+4] == 0x0A) {
      instr_len = 5;
      desc = "jmp far ptr cs:[0x0A00]          ; FAR JUMP TO Windows HalpRMStub!";
    }

    char *hptr = hex;
    for (UINT32 k = 0; k < instr_len && i + k < bytes; k++) {
      hptr += sprintf(hptr, "%02X ", buffer[i + k]);
    }
    printf("  [0x%04X]  %-24s %s\n", i, hex, desc);
    i += instr_len;
  }
  printf("============================================================================================\n\n");

  /* Explicit Sanity Check 6: 16->32->16 Flat Mode Bridge checks */
  /* Verify RM_IDTR and PM_GDTR at buffer + GU_TRAMP_RM_IDTR_OFF (0x0B00) (v1.8.66) */
  CHECK(buffer[GU_TRAMP_RM_IDTR_OFF] == 0xFF && buffer[GU_TRAMP_RM_IDTR_OFF + 1] == 0x03, "RM_IDTR limit must be 0x03FF");
  CHECK(*(UINT32 *)(buffer + GU_TRAMP_RM_IDTR_OFF + 2) == 0, "RM_IDTR base must be 0");
  CHECK(*(UINT16 *)(buffer + GU_TRAMP_PM_GDTR_OFF) == 0x0027, "PM_GDTR limit must be 39 (5 descriptors)");
  CHECK(*(UINT32 *)(buffer + GU_TRAMP_PM_GDTR_OFF + 2) == (UINT32)(tramp_phys + GU_TRAMP_GDT_BASE_OFF), "PM_GDTR base must be TrampolinePhys + 0x0B10");

  /* Verify GDT Descriptors at +0x0B10 */
  CHECK(buffer[GU_TRAMP_GDT_BASE_OFF + 0x0D] == 0x9A && buffer[GU_TRAMP_GDT_BASE_OFF + 0x0E] == 0xCF, "GDT[1] 32-bit Code Descriptor (0x08)");
  CHECK(buffer[GU_TRAMP_GDT_BASE_OFF + 0x15] == 0x92 && buffer[GU_TRAMP_GDT_BASE_OFF + 0x16] == 0xCF, "GDT[2] 32-bit Data Descriptor (0x10)");
  CHECK(buffer[GU_TRAMP_GDT_BASE_OFF + 0x1D] == 0x9A && buffer[GU_TRAMP_GDT_BASE_OFF + 0x1E] == 0x00, "GDT[3] 16-bit Code Descriptor (0x18)");
  CHECK(buffer[GU_TRAMP_GDT_BASE_OFF + 0x25] == 0x92 && buffer[GU_TRAMP_GDT_BASE_OFF + 0x26] == 0x00, "GDT[4] 16-bit Data Descriptor (0x20)");

  /* Verify Fallback in 32-bit PM (mov esi, 0x00001000) */
  BOOLEAN found_fallback = FALSE;
  for (UINT32 i = 0; i < bytes - 4; i++) {
    if (buffer[i] == 0xBE && buffer[i+1] == 0x00 && buffer[i+2] == 0x10 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) {
      found_fallback = TRUE;
      break;
    }
  }
  CHECK(found_fallback, "Trampoline must fall back to mov esi, 0x00001000 when waking vector is 0");

  /* Verify Diagnostic Marker 0x324B4F21 ('!OK2') write to telem_phys */
  BOOLEAN found_marker = FALSE;
  BOOLEAN found_marker_addr = FALSE;
  for (UINT32 i = 0; i < bytes - 9; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x05 && buffer[i+6] == 0x21 && buffer[i+7] == 0x4F && buffer[i+8] == 0x4B && buffer[i+9] == 0x32) {
      found_marker = TRUE;
      if (*(UINT32 *)(buffer + i + 2) == (UINT32)telem_phys) {
        found_marker_addr = TRUE;
      }
      break;
    }
  }
  CHECK(found_marker, "Trampoline must write diagnostic marker 0x324B4F21 (!OK2) to TelemetryPage");
  CHECK(found_marker_addr, "Marker write destination address must match telem_phys (0xDA000000)");

  /* Verify Telemetry Mirroring destination (EDI = telem_phys + 0x04) */
  BOOLEAN found_edi_addr = FALSE;
  for (UINT32 i = 0; i < bytes - 4; i++) {
    if (buffer[i] == 0xBF && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0x04)) {
      found_edi_addr = TRUE;
      break;
    }
  }
  CHECK(found_edi_addr, "rep movsd destination (EDI) must match telem_phys + 0x04 (0xDA000004)");

  /* Verify Telemetry Mirroring (rep movsd: F3 A5) */
  BOOLEAN found_rep_movsd = FALSE;
  for (UINT32 i = 0; i < bytes - 1; i++) {
    if (buffer[i] == 0xF3 && buffer[i+1] == 0xA5) {
      found_rep_movsd = TRUE;
      break;
    }
  }
  CHECK(found_rep_movsd, "Trampoline must contain rep movsd (F3 A5) to mirror telemetry to TelemetryPage");

  /* Explicit Sanity Check 7: S3 Wake Hardware Unlock via Temporary BAR0 */
  /* Verify BAR0 target write (0xFB000000) */
  BOOLEAN found_bar0_target = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 4; i++) {
    if (*(UINT32 *)(buffer + i) == 0xFB000000U) {
      found_bar0_target = TRUE;
      break;
    }
  }
  CHECK(found_bar0_target, "Trampoline must program temporary BAR0 to 0xFB000000");

  /* Verify SEC2 Diagnostics & Reset Cycle (v1.8.51 / v1.8.52) */
  BOOLEAN found_telem_pmc_enable = FALSE;
  BOOLEAN found_telem_sec2_reset = FALSE;
  BOOLEAN found_telem_sec2_hwcfg2 = FALSE;
  BOOLEAN found_telem_falcon2_bcr = FALSE;
  BOOLEAN found_reset_assert = FALSE;
  BOOLEAN found_reset_deassert = FALSE;
  BOOLEAN found_poll_scrub = FALSE;
  BOOLEAN found_bcr_write_zero = FALSE;
  BOOLEAN found_poll_bcr = FALSE;
  BOOLEAN found_telem_bcr_post = FALSE;
  BOOLEAN found_chip_id_write = FALSE;
  BOOLEAN found_telem_cpuctl_post = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 9; i++) {
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xC0)) {
      found_telem_pmc_enable = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xC4)) {
      found_telem_sec2_reset = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xC8)) {
      found_telem_sec2_hwcfg2 = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xCC)) {
      found_telem_falcon2_bcr = TRUE;
    }
    if (buffer[i] == 0x0C && buffer[i+1] == 0x01) {
      found_reset_assert = TRUE;
    }
    if (buffer[i] == 0x24 && buffer[i+1] == 0xFE) {
      found_reset_deassert = TRUE;
    }
    if ((buffer[i] == 0xA9 && buffer[i+1] == 0x00 && buffer[i+2] == 0x10 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) ||
        (buffer[i] == 0xF6 && buffer[i+1] == 0xC4 && buffer[i+2] == 0x10)) {
      found_poll_scrub = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00841668U) {
      found_bcr_write_zero = TRUE;
    }
    if (buffer[i] == 0x80 && buffer[i+1] == 0xE2 && buffer[i+2] == 0x11) {
      found_poll_bcr = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xD4)) {
      found_telem_bcr_post = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840084U) {
      found_chip_id_write = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xD0)) {
      found_telem_cpuctl_post = TRUE;
    }
  }
  CHECK(found_telem_pmc_enable, "Trampoline must save PMC_ENABLE to Telemetry +0xC0");
  CHECK(found_telem_sec2_reset, "Trampoline must save SEC2_RESET to Telemetry +0xC4");
  CHECK(found_telem_sec2_hwcfg2, "Trampoline must save SEC2_HWCFG2 to Telemetry +0xC8");
  CHECK(found_telem_falcon2_bcr, "Trampoline must save FALCON2_BCR to Telemetry +0xCC");
  CHECK(found_reset_assert, "Trampoline must assert SEC2 reset (or al, 0x01)");
  CHECK(found_reset_deassert, "Trampoline must release SEC2 reset (and al, 0xFE)");
  CHECK(found_poll_scrub, "Trampoline must poll HWCFG2 Bit 12 (Memory Scrubbing)");
  CHECK(found_bcr_write_zero, "Trampoline must write 0 to FALCON2_BCR (BAR0 + 0x841668) to exit Boot-ROM mode");
  CHECK(found_poll_bcr, "Trampoline must poll FALCON2_BCR & 0x11 == 1");
  CHECK(found_telem_bcr_post, "Trampoline must save post-switch FALCON2_BCR to Telemetry +0xD4");
  CHECK(found_chip_id_write, "Trampoline must write Chip-ID to SEC2 (BAR0 + 0x840084)");
  CHECK(found_telem_cpuctl_post, "Trampoline must save post-reset CPUCTL to Telemetry +0xD0");

  /* Verify FBIF & Chunked DMA Transfers, Falcon2 Config and CPU Start (v1.8.54 / v1.8.56) */
  BOOLEAN found_fbif_trans_read = FALSE;
  BOOLEAN found_fbif_trans_write = FALSE;
  BOOLEAN found_irq_clear = FALSE;
  BOOLEAN found_fbif_mode_write = FALSE;
  BOOLEAN found_booter_dma_low = FALSE;
  UINT32 booter_dma_low_write_count = 0;
  BOOLEAN found_booter_dma_high = FALSE;
  BOOLEAN found_dma_dest_write = FALSE;
  BOOLEAN found_dma_offset_write = FALSE;
  BOOLEAN found_dma_cmd_write = FALSE;
  BOOLEAN found_dma_poll_comp = FALSE;
  BOOLEAN found_dma_loop_reload = FALSE;
  BOOLEAN found_falcon2_sig = FALSE;
  BOOLEAN found_falcon2_eng = FALSE;
  BOOLEAN found_falcon2_ucode = FALSE;
  BOOLEAN found_falcon2_cfg = FALSE;
  BOOLEAN found_entry_point = FALSE;
  BOOLEAN found_mailbox0 = FALSE;
  BOOLEAN found_mailbox1 = FALSE;
  BOOLEAN found_falcon_cpu_start = FALSE;
  BOOLEAN found_poll_marker = FALSE;
  BOOLEAN found_poll_meta_guard = FALSE;
  BOOLEAN found_poll_falcon_halt = FALSE;
  BOOLEAN found_telem_booter_cpuctl = FALSE;
  BOOLEAN found_telem_booter_marker = FALSE;
  BOOLEAN found_telem_pre_wpr2_lo = FALSE;
  BOOLEAN found_telem_pre_wpr2_hi = FALSE;
  BOOLEAN found_telem_mmu_lmr = FALSE;
  BOOLEAN found_telem_gfw_boot = FALSE;
  BOOLEAN found_telem_gsp_hwcfg2 = FALSE;
  BOOLEAN found_telem_gsp_cpuctl = FALSE;
  BOOLEAN found_gsp_reset = FALSE;
  BOOLEAN found_gsp_bcr_zero = FALSE;
  BOOLEAN found_gsp_chip_id = FALSE;
  BOOLEAN found_gsp_fbif_trans = FALSE;
  BOOLEAN found_gsp_dma_low = FALSE;
  BOOLEAN found_gsp_falcon2_sig = FALSE;
  BOOLEAN found_gsp_cpu_start = FALSE;
  BOOLEAN found_telem_post_fwsec_wpr2_lo = FALSE;
  BOOLEAN found_telem_post_fwsec_wpr2_hi = FALSE;
  BOOLEAN found_telem_post_fwsec_cpuctl = FALSE;
  BOOLEAN found_telem_post_fwsec_mb0 = FALSE;
  BOOLEAN found_telem_post_fwsec_scratch = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 9; i++) {
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x001103C0U) {
      found_gsp_reset = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00111668U) {
      found_gsp_bcr_zero = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00110084U) {
      found_gsp_chip_id = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00110624U) {
      found_gsp_fbif_trans = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00110110U) {
      found_gsp_dma_low = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00111210U && *(UINT32 *)(buffer + i + 6) == 0x0B24U) {
      found_gsp_falcon2_sig = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00110100U && *(UINT32 *)(buffer + i + 6) == 2U) {
      found_gsp_cpu_start = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FWSEC_WPR2_LO_OFF)) {
      found_telem_post_fwsec_wpr2_lo = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FWSEC_WPR2_HI_OFF)) {
      found_telem_post_fwsec_wpr2_hi = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FWSEC_GSP_CPUCTL_OFF)) {
      found_telem_post_fwsec_cpuctl = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FWSEC_GSP_MAILBOX0_OFF)) {
      found_telem_post_fwsec_mb0 = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FWSEC_SCRATCH_OFF)) {
      found_telem_post_fwsec_scratch = TRUE;
    }
    if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840624U) {
      found_fbif_trans_read = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840624U) {
      found_fbif_trans_write = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x0084010CU) {
      found_irq_clear = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840600U) {
      found_fbif_mode_write = TRUE;
    }
    if ((buffer[i] == 0x89 || buffer[i] == 0xC7) && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840110U) {
      found_booter_dma_low = TRUE;
      booter_dma_low_write_count++;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840128U) {
      found_booter_dma_high = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0xBB && *(UINT32 *)(buffer + i + 2) == 0x00840114U) {
      found_dma_dest_write = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x93 && *(UINT32 *)(buffer + i + 2) == 0x0084011CU) {
      found_dma_offset_write = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0xB3 && *(UINT32 *)(buffer + i + 2) == 0x00840118U) {
      found_dma_cmd_write = TRUE;
    }
    if (buffer[i] == 0xA8 && buffer[i+1] == 0x02) {
      found_dma_poll_comp = TRUE;
    }
    if (buffer[i] == 0xEB && i >= 5 && buffer[i-5] == 0xBE && *(UINT32 *)(buffer + i - 4) == 0x0600U) {
      found_dma_loop_reload = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00841210U && *(UINT32 *)(buffer + i + 6) == 16U) {
      found_falcon2_sig = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x0084119CU && *(UINT32 *)(buffer + i + 6) == 1U) {
      found_falcon2_eng = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00841198U && *(UINT32 *)(buffer + i + 6) == 3U) {
      found_falcon2_ucode = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00841180U && *(UINT32 *)(buffer + i + 6) == 1U) {
      found_falcon2_cfg = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840104U && *(UINT32 *)(buffer + i + 6) == 256U) {
      found_entry_point = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840040U) {
      found_mailbox0 = TRUE;
    }
    if (buffer[i] == 0x89 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840044U) {
      found_mailbox1 = TRUE;
    }
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840100U && *(UINT32 *)(buffer + i + 6) == 2U) {
      found_falcon_cpu_start = TRUE;
    }
    if (buffer[i] == 0x3D && *(UINT32 *)(buffer + i + 1) == 0x67B00001U) {
      found_poll_marker = TRUE;
    }
    if (buffer[i] == 0x3D && *(UINT32 *)(buffer + i + 1) == (UINT32)(ctx_phys + 0x2A000ULL)) {
      found_poll_meta_guard = TRUE;
    }
    if (buffer[i] == 0xA8 && buffer[i+1] == 0x10) {
      found_poll_falcon_halt = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xD8)) {
      found_telem_booter_cpuctl = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + 0xDC)) {
      found_telem_booter_marker = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_PRE_BOOTER_WPR2_LO_OFF)) {
      found_telem_pre_wpr2_lo = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_PRE_BOOTER_WPR2_HI_OFF)) {
      found_telem_pre_wpr2_hi = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_MMU_LOCAL_MEM_RANGE_OFF)) {
      found_telem_mmu_lmr = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_GFW_BOOT_STATUS_OFF)) {
      found_telem_gfw_boot = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_GSP_HWCFG2_OFF)) {
      found_telem_gsp_hwcfg2 = TRUE;
    }
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_GSP_CPUCTL_OFF)) {
      found_telem_gsp_cpuctl = TRUE;
    }
  }
  CHECK(found_fbif_trans_read && found_fbif_trans_write, "Trampoline must enable FBIF Trans (0x00840624 |= 0x80)");
  CHECK(found_irq_clear, "Trampoline must clear Falcon IRQ Status (0x0084010C = 0)");
  CHECK(found_fbif_mode_write, "Trampoline must setup FBIF Trans Mode (0x00840600 = (val & ~7) | 5)");
  CHECK(found_booter_dma_low && booter_dma_low_write_count >= 2, "Trampoline must write Booter DMA Address Low to 0x00840110 twice (IMEM base and DMEM base reload)");
  CHECK(found_booter_dma_high, "Trampoline must write Booter DMA Address High to 0x00840128");
  CHECK(found_dma_dest_write && found_dma_offset_write && found_dma_cmd_write, "Trampoline must execute chunked DMA transfers to IMEM and DMEM");
  CHECK(found_dma_poll_comp && found_dma_loop_reload, "Trampoline must poll DMA completion (status & 2 == 2) for both IMEM and DMEM transfers");
  CHECK(found_falcon2_sig && found_falcon2_eng && found_falcon2_ucode && found_falcon2_cfg, "Trampoline must program Falcon2 registers (sig, eng, ucode, cfg)");
  CHECK(found_entry_point, "Trampoline must set Falcon Entry Point Vector (0x00840104 = 256)");
  CHECK(found_mailbox0 && found_mailbox1, "Trampoline must program Mailbox 0 (meta_dma) and Mailbox 1 (0)");
  CHECK(found_falcon_cpu_start, "Trampoline must start Falcon CPU (0x00840100 = 2)");
  CHECK(found_poll_marker && found_poll_meta_guard && found_poll_falcon_halt, "Trampoline must poll Marker (0x67B00001), guard Halt with meta_dma, and check Falcon Engine Halt (0x10)");
  CHECK(found_telem_booter_cpuctl, "Trampoline must save post-booter CPUCTL to Telemetry +0xD8");
  CHECK(found_telem_booter_marker, "Trampoline must save post-booter Marker to Telemetry +0xDC");
  CHECK(found_telem_pre_wpr2_lo, "Trampoline must save pre-booter WPR2_LO (0x001fa824) to Telemetry +0xF0");
  CHECK(found_telem_pre_wpr2_hi, "Trampoline must save pre-booter WPR2_HI (0x001fa828) to Telemetry +0xF4");
  CHECK(found_telem_mmu_lmr, "Trampoline must save MMU Local Memory Range (0x00100ce0) to Telemetry +0xF8");
  CHECK(found_telem_gfw_boot, "Trampoline must save GFW-BOOT Status (0x00118234) to Telemetry +0xFC");
  CHECK(found_telem_gsp_hwcfg2, "Trampoline must save GSP_HWCFG2 (0x001100f4) to Telemetry +0x100");
  CHECK(found_telem_gsp_cpuctl, "Trampoline must save GSP_CPUCTL (0x00110100) to Telemetry +0x104");
  CHECK(found_gsp_reset, "Trampoline must reset GSP engine (0x001103C0)");
  CHECK(found_gsp_bcr_zero, "Trampoline must switch GSP BCR to non-secure (0x00111668 = 0)");
  CHECK(found_gsp_chip_id, "Trampoline must write Chip-ID to GSP (0x00110084)");
  CHECK(found_gsp_fbif_trans, "Trampoline must enable GSP FBIF Trans (0x00110624 |= 0x80)");
  CHECK(found_gsp_dma_low, "Trampoline must set GSP DMA base address (0x00110110)");
  CHECK(found_gsp_falcon2_sig, "Trampoline must set GSP Falcon2 sig_dmem = 0x0B24");
  CHECK(found_gsp_cpu_start, "Trampoline must start GSP CPU (0x00110100 = 2)");
  CHECK(found_telem_post_fwsec_wpr2_lo, "Trampoline must save Post-FWSEC WPR2_LO (BAR0 + 0x001fa824) to Telemetry +0x108");
  CHECK(found_telem_post_fwsec_wpr2_hi, "Trampoline must save Post-FWSEC WPR2_HI (BAR0 + 0x001fa828) to Telemetry +0x10C");
  CHECK(found_telem_post_fwsec_cpuctl, "Trampoline must save Post-FWSEC GSP_CPUCTL (BAR0 + 0x00110100) to Telemetry +0x110");
  CHECK(found_telem_post_fwsec_mb0, "Trampoline must save Post-FWSEC GSP_MAILBOX0 (BAR0 + 0x00110040) to Telemetry +0x114");
  CHECK(found_telem_post_fwsec_scratch, "Trampoline must save Post-FWSEC Scratch (BAR0 + 0x00001438) to Telemetry +0x118");

  /* Verify MSE + BME (0x06) programmed into Root Port Command & GPU Command */
  BOOLEAN found_cmd_bme = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 2; i++) {
    if (buffer[i] == 0x0C && buffer[i+1] == 0x06) {
      found_cmd_bme = TRUE;
      break;
    }
  }
  CHECK(found_cmd_bme, "Trampoline must enable MSE and BME (or al, 0x06) on PCIe bridge and GPU");

  /* Verify SS0 write (0x88888888) */
  BOOLEAN found_ss0_unlock = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 9; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x0082381CU &&
        *(UINT32 *)(buffer + i + 6) == 0x88888888U) {
      found_ss0_unlock = TRUE;
      break;
    }
  }
  CHECK(found_ss0_unlock, "Trampoline must write SS0 = 0x88888888 to BAR0 + 0x82381C");

  /* Verify SS1 write (0x00000008) */
  BOOLEAN found_ss1_unlock = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 9; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00823820U &&
        *(UINT32 *)(buffer + i + 6) == 0x00000008U) {
      found_ss1_unlock = TRUE;
      break;
    }
  }
  CHECK(found_ss1_unlock, "Trampoline must write SS1 = 0x00000008 to BAR0 + 0x823820");

  /* Verify 'UNLK' magic write (0x554E4C4B) */
  BOOLEAN found_unlk_magic = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 9; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x05 && *(UINT32 *)(buffer + i + 2) == (UINT32)(telem_phys + 0x80) &&
        *(UINT32 *)(buffer + i + 6) == 0x554E4C4BU) {
      found_unlk_magic = TRUE;
      break;
    }
  }
  CHECK(found_unlk_magic, "Trampoline must write 'UNLK' (0x554E4C4B) to TelemetryPage + 0x80");

  /* Verify PCIe FLR execution and post-FLR containment (v1.8.59, 1:1 with unlock_610.c:1750-1840) */
  BOOLEAN found_flr_trigger = FALSE;
  BOOLEAN found_flr_stall = FALSE;
  BOOLEAN found_flr_vid_poll = FALSE;
  BOOLEAN found_flr_bar0_poll = FALSE;
  BOOLEAN found_post_flr_sec2 = FALSE;
  BOOLEAN found_telem_post_wpr2 = FALSE;
  BOOLEAN found_telem_post_ss0 = FALSE;
  BOOLEAN found_telem_post_ss1 = FALSE;
  BOOLEAN found_telem_post_plm = FALSE;

  for (UINT32 i = pm32_offset; i < pm16_offset - 8; i++) {
    /* PCIe FLR trigger: or eax, 0x00008000 (0D 00 80 00 00) */
    if (buffer[i] == 0x0D && buffer[i+1] == 0x00 && buffer[i+2] == 0x80 && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) {
      found_flr_trigger = TRUE;
    }
    /* Port 0x80 stall: in al, 0x80 (E4 80) */
    if (buffer[i] == 0xE4 && buffer[i+1] == 0x80) {
      found_flr_stall = TRUE;
    }
    /* Bounded poll on GPU VID/DID: cmp eax, 0x268410DE (3D DE 10 84 26) */
    if (buffer[i] == 0x3D && buffer[i+1] == 0xDE && buffer[i+2] == 0x10 && buffer[i+3] == 0x84 && buffer[i+4] == 0x26) {
      found_flr_vid_poll = TRUE;
    }
    /* Bounded poll on BAR0 ready: cmp eax, 0x192000A1 (3D A1 00 20 19) */
    if (buffer[i] == 0x3D && buffer[i+1] == 0xA1 && buffer[i+2] == 0x00 && buffer[i+3] == 0x20 && buffer[i+4] == 0x19) {
      found_flr_bar0_poll = TRUE;
    }
    /* Post-FLR hold_sec2_reset call: mov ebp, imm32; jmp near .sec2_sub_start (BD xx xx xx xx E9) */
    if (buffer[i] == 0xBD && buffer[i+5] == 0xE9) {
      found_post_flr_sec2 = TRUE;
    }
    /* Post-FLR WPR2_HI telemtry write: mov [dest + 0xE0], eax (A3 xx xx xx xx) */
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FLR_WPR2_OFF)) {
      found_telem_post_wpr2 = TRUE;
    }
    /* Post-FLR SS0 telemtry write: mov [dest + 0xE4], eax (A3 xx xx xx xx) */
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FLR_SS0_OFF)) {
      found_telem_post_ss0 = TRUE;
    }
    /* Post-FLR SS1 telemtry write: mov [dest + 0xE8], eax (A3 xx xx xx xx) */
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FLR_SS1_OFF)) {
      found_telem_post_ss1 = TRUE;
    }
    /* Post-FLR PLM telemtry write: mov [dest + 0xEC], eax (A3 xx xx xx xx) */
    if (buffer[i] == 0xA3 && *(UINT32 *)(buffer + i + 1) == (UINT32)(telem_phys + GU_TELEM_POST_FLR_PLM_OFF)) {
      found_telem_post_plm = TRUE;
    }
  }
  CHECK(found_flr_trigger, "Trampoline must trigger PCIe FLR via DevCtl Bit 15 (0x00008000)");
  CHECK(found_flr_stall, "Trampoline must stall ~120ms via Port 0x80 reads (in al, 0x80)");
  CHECK(found_flr_vid_poll, "Trampoline must poll GPU VID/DID 0x268410DE post-FLR");
  CHECK(found_flr_bar0_poll, "Trampoline must poll GPU BAR0 0x192000A1 post-FLR");
  CHECK(found_post_flr_sec2, "Trampoline must execute hold_sec2_reset() subroutine post-FLR");
  CHECK(found_telem_post_wpr2, "Trampoline must save post-FLR WPR2_HI to high_telem_dest + 0xE0");
  CHECK(found_telem_post_ss0, "Trampoline must save post-FLR SS0 to high_telem_dest + 0xE4");
  CHECK(found_telem_post_ss1, "Trampoline must save post-FLR SS1 to high_telem_dest + 0xE8");
  CHECK(found_telem_post_plm, "Trampoline must save post-FLR PLM to high_telem_dest + 0xEC");

  /* Verify 4-Stage PCI teardown restoration (GPU Cmd, GPU BAR0, Leaf->Root Bridge MemWindow & Cmd loop)
   * Stage 5 (Bridge BusNum 0x18) is INTENTIONALLY OMITTED to preserve bridge bus routing for Windows!
   */
  BOOLEAN found_gpu_cmd_restore = FALSE;
  BOOLEAN found_gpu_bar0_restore = FALSE;
  BOOLEAN found_br_mem_restore = FALSE;
  BOOLEAN found_br_cmd_restore = FALSE;
  BOOLEAN found_br_tbl_loop = FALSE;
  BOOLEAN found_rp_bus_restore = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 8; i++) {
    if (buffer[i] == 0xB8 && *(UINT32 *)(buffer + i + 1) == 0x802D0004U) {
      found_gpu_cmd_restore = TRUE;
    }
    if (buffer[i] == 0xB8 && *(UINT32 *)(buffer + i + 1) == 0x802D0010U) {
      found_gpu_bar0_restore = TRUE;
    }
    if (buffer[i] == 0xBE && *(UINT32 *)(buffer + i + 1) >= (UINT32)(tramp_phys + GU_TRAMP_BRIDGE_TBL_OFF)) {
      found_br_tbl_loop = TRUE;
    }
    if (buffer[i] == 0x0C && buffer[i+1] == 0x20 && buffer[i+3] == 0xB2 && buffer[i+4] == 0xFC &&
        buffer[i+5] == 0x8B && buffer[i+6] == 0x46 && buffer[i+7] == (UINT8)GU_TRAMP_BRIDGE_SAVED_MEM_OFF) {
      found_br_mem_restore = TRUE;
    }
    if (buffer[i] == 0x0C && buffer[i+1] == 0x04 && buffer[i+3] == 0xB2 && buffer[i+4] == 0xFC &&
        buffer[i+5] == 0x8B && buffer[i+6] == 0x46 && buffer[i+7] == (UINT8)GU_TRAMP_BRIDGE_SAVED_CMD_OFF) {
      found_br_cmd_restore = TRUE;
    }
    if (buffer[i] == 0x0C && buffer[i+1] == 0x18) {
      found_rp_bus_restore = TRUE;
    }
  }
  CHECK(found_gpu_cmd_restore, "Trampoline teardown Stage 1 must restore GPU Command (0x802D0004)");
  CHECK(found_gpu_bar0_restore, "Trampoline teardown Stage 2 must restore GPU BAR0 (0x802D0010)");
  CHECK(found_br_tbl_loop, "Trampoline teardown must walk dynamic bridge table (0x0B40) Leaf->Root");
  CHECK(found_br_mem_restore, "Trampoline teardown Stage 3 must restore Bridge Memory Base/Limit (0x20 from [esi+12])");
  CHECK(found_br_cmd_restore, "Trampoline teardown Stage 4 must restore Bridge Command Register (0x04 from [esi+8])");
  CHECK(!found_rp_bus_restore, "Trampoline teardown Stage 5 must NOT restore Bridge Bus Numbers (0x18) to preserve bus routing");

  /* Verify Pre-FWSEC GFW-BOOT / VRAM Gate (0x00118234 & 0xFF == 0xFF && 0x00100CE0 != 0) */
  BOOLEAN found_gfw_vram_gate = FALSE;
  for (UINT32 i = pm32_offset; i + 30 < pm16_offset; i++) {
    if (buffer[i] == 0xB9 && *(UINT32 *)(buffer + i + 1) == 2000000U &&
        buffer[i+5] == 0x8B && buffer[i+6] == 0x83 && *(UINT32 *)(buffer + i + 7) == 0x00118234U &&
        buffer[i+11] == 0x83 && buffer[i+12] == 0xF8 && buffer[i+13] == 0xFF &&
        buffer[i+16] == 0x3C && buffer[i+17] == 0xFF &&
        buffer[i+20] == 0x8B && buffer[i+21] == 0x83 && *(UINT32 *)(buffer + i + 22) == 0x00100CE0U) {
      found_gfw_vram_gate = TRUE;
      break;
    }
  }
  CHECK(found_gfw_vram_gate, "Trampoline must include pre-FWSEC Port-0x80-paced GFW-BOOT (0x118234) & VRAM (0x100CE0) gate");

  /* Verify Telemetry Mirroring and collision check (v1.8.59: 28 DWORDs at +0x0880) */
  BOOLEAN found_telem_low_rep_movsd = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 1; i++) {
    if (buffer[i] == 0xF3 && buffer[i+1] == 0xA5) {
      found_telem_low_rep_movsd = TRUE;
      break;
    }
  }
  CHECK(found_telem_low_rep_movsd, "Trampoline teardown must mirror telemetry using rep movsd (F3 A5)");
  CHECK(GU_TRAMP_REAL_FACS_MIRROR_OFF + (28U * 4U) <= GU_TRAMP_RM_IDTR_OFF, "Telemetry mirror buffer (+0x0A80..+0x0AEF) must not collide with RM_IDTR (+0x0B00)");

  /* Verify 5-byte port address loads in 32-bit flat mode (no 2-byte phase shift) */
  BOOLEAN found_5byte_port_cf8 = FALSE;
  BOOLEAN found_port_cfc_switch = FALSE;
  for (UINT32 i = pm32_offset; i < pm16_offset - 5; i++) {
    if (buffer[i] == 0xBA && buffer[i+1] == 0xF8 && buffer[i+2] == 0x0C && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) {
      found_5byte_port_cf8 = TRUE;
    }
    if ((buffer[i] == 0xBA && buffer[i+1] == 0xFC && buffer[i+2] == 0x0C && buffer[i+3] == 0x00 && buffer[i+4] == 0x00) ||
        (buffer[i] == 0xB2 && buffer[i+1] == 0xFC)) {
      found_port_cfc_switch = TRUE;
    }
  }
  CHECK(found_5byte_port_cf8, "32-bit PM must use 5-byte mov edx, 0x00000CF8 (BA F8 0C 00 00)");
  CHECK(found_port_cfc_switch, "32-bit PM must switch to 0x0CFC cleanly (5-byte BA FC 0C 00 00 or mov dl, 0xFC)");

  /* Verify Return to Real Mode (CR0.PE clear and far jump) */
  BOOLEAN found_cr0_clear = FALSE;
  for (UINT32 i = 0; i < bytes - 1; i++) {
    if (buffer[i] == 0x24 && buffer[i+1] == 0xFE) {
      found_cr0_clear = TRUE;
      break;
    }
  }
  CHECK(found_cr0_clear, "Trampoline must clear CR0.PE (and al, 0xFE) to return to Real Mode");

  CHECK(!found_hlt, "Trampoline must contain ZERO HLT instructions");

  printf("[7] Trampoline Generation (including 0x66 Prefix, Bridge Restore & Disassembly): OK!\n");
}

static void TestGuS3Prepare_BooterUnpackAndPatch(void) {
  printf("[8] Testing GuS3PrepareAndRegister Booter container unpacking & fuse signature patching...\n");

  /* Setup mock EFI boot services and system table */
  EFI_BOOT_SERVICES mock_bs;
  memset(&mock_bs, 0, sizeof(mock_bs));
  mock_bs.Hdr.HeaderSize = sizeof(mock_bs);
  mock_bs.AllocatePages = MockAllocatePages;
  mock_bs.FreePages = MockFreePages;
  mock_bs.LocateProtocol = MockLocateProtocol;

  EFI_SYSTEM_TABLE local_st;
  memset(&local_st, 0, sizeof(local_st));
  local_st.BootServices = &mock_bs;

  /* Initialize mock GPU platform */
  Gu.bar_base = 0xFB000000ULL;
  Gu.device_id = 0x2684;
  Gu.is_ampere = FALSE;
  Gu.is_turing = FALSE;
  Gu.fuse_version = 1;

  /* Build synthetic 57672-byte AD102 booter container (72-byte header + 56832-byte image + 768-byte sigs) */
  UINT32 image_size = 56832;
  UINT32 sig_bytes = 768;
  UINT32 container_size = 72 + image_size + sig_bytes;
  UINT8 *container = (UINT8 *)malloc(container_size);
  CHECK(container != NULL, "Mock booter container allocation must succeed");
  memset(container, 0, container_size);

  /* Container Header */
  *(UINT32 *)(container + 0) = 0x314C5442U; /* 'BTL1' */
  *(UINT32 *)(container + 4) = 1;           /* Version 1 */

  struct gpu_unlock_booter_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.os_code_off = 0;
  hdr.os_code_size = 256;
  hdr.os_data_off = 32256;
  hdr.os_data_size = 24576;
  hdr.num_apps = 1;
  hdr.app_code_off = 256;
  hdr.app_code_size = 32000;
  hdr.app_data_off = 256;
  hdr.app_data_size = 0;
  hdr.patch_loc = 32272;
  hdr.ucode_id = 3;
  hdr.engine_id = 1;
  hdr.num_sigs = 2;
  hdr.sig_bytes = 768;

  memcpy(container + 8, &hdr, sizeof(hdr));
  *(UINT32 *)(container + 8 + sizeof(hdr)) = image_size;
  *(UINT32 *)(container + 12 + sizeof(hdr)) = sig_bytes;

  /* Image markers: put 0xA5 at app_code_off (offset 256 within image -> 72 + 256 of container) */
  memset(container + 72, 0x12, image_size);
  container[72 + 256] = 0xA5;

  /* Signatures: slot 0 (384 bytes of 0x77 for fuse_ver=1), slot 1 (384 bytes of 0x88 for fuse_ver=0) */
  memset(container + 72 + image_size, 0x77, 384);
  memset(container + 72 + image_size + 384, 0x88, 384);

  GuAssets[0].firmware.data = container;
  GuAssets[0].firmware.size = container_size;

  /* Run GuS3PrepareAndRegister */
  EFI_STATUS status = GuS3PrepareAndRegister(&local_st);
  CHECK(status == EFI_SUCCESS, "GuS3PrepareAndRegister must return EFI_SUCCESS");

  /* Verify in NVS buffer (g_mock_nvs_buffer) */
  GU_S3_RESUME_CONTEXT *ctx = (GU_S3_RESUME_CONTEXT *)g_mock_nvs_buffer;
  CHECK(ctx->magic == GU_S3_CONTEXT_MAGIC, "Context magic in NVS must match");
  CHECK(ctx->booter_size == image_size, "Booter size in NVS context must be unpacked image_size (56832), NOT container size (57672)");

  UINT8 *nvs_booter = g_mock_nvs_buffer + 0x01000;
  /* First 4 bytes must NOT be 'BTL1' container header */
  CHECK(*(UINT32 *)nvs_booter != 0x314C5442U, "NVS booter at +0x01000 must NOT contain 'BTL1' container header");

  /* App code offset 256 must match unpacked image byte */
  CHECK(nvs_booter[256] == 0xA5, "NVS booter at +0x01000 + 256 must match unpacked app code byte 0xA5");

  /* Patch loc 32272 must have received signature for fuse_ver=1 (0x77) */
  CHECK(nvs_booter[32272] == 0x77, "NVS booter at patch_loc (32272) must contain patched fuse_ver=1 signature (0x77)");

  free(container);
  GuAssets[0].firmware.data = NULL;
  GuAssets[0].firmware.size = 0;
  printf("[8] GuS3PrepareAndRegister Booter container unpacking & fuse signature patching: OK!\n");
}

static void TestGuS3PersistentMemoryRetention(void) {
  printf("[9] Testing S3 Persistent Radix & GSP Image Memory Retention (EfiReservedMemoryType)...\n");

  /* 1. Test GuSafeToReturn() behavior with S3 persistence */
  memset(&Gu, 0, sizeof(Gu));
  Gu.armed = TRUE;
  Gu.quiesced = TRUE;
  Gu.started = TRUE;

  /* Pending DMA allocations MUST ALWAYS block safe return (DMA_live must be 0 before Windows handover) */
  GU_DMA dummy_dma;
  memset(&dummy_dma, 0, sizeof(dummy_dma));
  Gu.allocations = &dummy_dma;
  Gu.dma_count = 20604;
  Gu.s3_persist_armed = FALSE;
  CHECK(!GuSafeToReturn(), "Pending DMA allocations without s3_persist_armed must NOT be safe to return");

  Gu.s3_persist_armed = TRUE;
  CHECK(!GuSafeToReturn(), "Pending DMA allocations with s3_persist_armed MUST NOT be safe to return (DMA_live must be 0)");

  /* With 0 DMA allocations (DMA_live == 0), GuSafeToReturn() succeeds */
  Gu.allocations = NULL;
  Gu.dma_count = 0;
  CHECK(GuSafeToReturn(), "Zero DMA allocations (DMA_live = 0) with s3_persist_armed MUST be safe to return");

  /* A real fault or unverified live engine must still block return */
  Gu.fault = TRUE;
  CHECK(!GuSafeToReturn(), "Hardware fault with s3_persist_armed must still NOT be safe to return");
  Gu.fault = FALSE;
  Gu.retained = TRUE;
  CHECK(!GuSafeToReturn(), "Retained error flag must still NOT be safe to return");
  Gu.retained = FALSE;

  /* 2. Test S3 Contiguous Reserved Arena allocation & single-descriptor retention */
  EFI_BOOT_SERVICES arena_bs;
  memset(&arena_bs, 0, sizeof(arena_bs));
  arena_bs.AllocatePages = MockAllocatePages;
  arena_bs.FreePages = MockFreePages;
  Gu.bs = &arena_bs;

  EFI_STATUS arena_st = GuS3AllocateArena();
  CHECK(arena_st == EFI_SUCCESS, "GuS3AllocateArena must succeed");
  CHECK(Gu.s3_arena_armed == TRUE, "Gu.s3_arena_armed must be TRUE");
  CHECK(Gu.s3_arena_phys == 0xDAD00000ULL, "Gu.s3_arena_phys must match 0xDAD00000");
  CHECK(Gu.s3_arena_pages == GU_S3_ARENA_PAGES, "Gu.s3_arena_pages must match GU_S3_ARENA_PAGES");
  CHECK(Gu.s3_arena_used_pages == 0, "Gu.s3_arena_used_pages must start at 0");

  /* 3. Test WPR metadata host RAM pointer retention */
  Gu.bar_base = 0xFB000000ULL;
  Gu.device_id = 0x2684;
  Gu.s3_persist_armed = TRUE;

  UINT8 mock_wpr_meta[256];
  memset(mock_wpr_meta, 0, 256);
  *(UINT64 *)(mock_wpr_meta + 0) = 0xdc3aae21371a60b3ULL;  /* Magic */
  *(UINT64 *)(mock_wpr_meta + 8) = 1ULL;                   /* Version */
  *(UINT64 *)(mock_wpr_meta + 16) = 0xDAD00000ULL;         /* Radix table DMA in EfiReservedMemoryType */
  *(UINT64 *)(mock_wpr_meta + 24) = 0x05053000ULL;         /* Image size (84 MB) */
  *(UINT64 *)(mock_wpr_meta + 32) = 0xDC639000ULL;         /* boot_dma */
  *(UINT64 *)(mock_wpr_meta + 72) = 0xDC629000ULL;         /* signature_dma */
  *(UINT64 *)(mock_wpr_meta + 80) = 64000ULL;              /* signature_size */

  memcpy(Gu.wpr_meta, mock_wpr_meta, 256);
  Gu.wpr_meta_valid = TRUE;

  EFI_STATUS reg_status = GuS3PrepareAndRegister(&mock_st);
  CHECK(reg_status == EFI_SUCCESS, "GuS3PrepareAndRegister must succeed with valid persistent metadata");

  UINT8 *nvs_meta = g_mock_nvs_buffer + 0x2A000;
  UINT64 magic_val = *(UINT64 *)(nvs_meta + 0);
  UINT64 radix_dma_val = *(UINT64 *)(nvs_meta + 16);
  UINT64 img_size_val = *(UINT64 *)(nvs_meta + 24);
  UINT64 boot_dma_val = *(UINT64 *)(nvs_meta + 32);
  UINT64 sig_dma_val = *(UINT64 *)(nvs_meta + 72);
  UINT64 sig_sz_val = *(UINT64 *)(nvs_meta + 80);

  CHECK(magic_val == 0xdc3aae21371a60b3ULL, "WPR metadata magic must match");
  CHECK(radix_dma_val == 0xDAD00000ULL, "radix_dma must point to persistent EfiReservedMemoryType (0xDAD00000)");
  CHECK(img_size_val == 0x05053000ULL, "img_size must match 84 MB GSP firmware (0x05053000)");
  CHECK(boot_dma_val == (UINT64)(uintptr_t)(g_mock_nvs_buffer + 0x21000), "boot_dma must point to NVS staging buffer");
  CHECK(sig_dma_val == (UINT64)(uintptr_t)(g_mock_nvs_buffer + 0x11000), "sig_dma must point to NVS V67 signature");
  CHECK(sig_sz_val == 64000ULL, "sig_size must be 64000 bytes");

  /* Verify no pointer points to null / zero */
  CHECK(radix_dma_val != 0, "radix_dma must not be 0");
  CHECK(boot_dma_val != 0, "boot_dma must not be 0");
  CHECK(sig_dma_val != 0, "sig_dma must not be 0");

  printf("[9] S3 Persistent Radix & GSP Image Memory Retention (EfiReservedMemoryType): OK!\n");
}

static void TestGuS3ArenaConsolidation(void) {
  printf("[10] Testing S3 Full 6 GPU-DMA Buffer Consolidation into Reserved Arena (v1.8.65)...\n");
  fflush(stdout);

  EFI_SYSTEM_TABLE mock_st;
  memset(&mock_st, 0, sizeof(mock_st));

  /* Setup mock table and protocol */
  EFI_BOOT_SERVICES mock_bs;
  memset(&mock_bs, 0, sizeof(mock_bs));
  mock_bs.AllocatePages = MockAllocatePages;
  mock_bs.LocateProtocol = MockLocateProtocol;
  mock_st.BootServices = &mock_bs;

  /* Reset and configure Gu state */
  memset(&Gu, 0, sizeof(Gu));
  Gu.bs = &mock_bs;
  Gu.bar_base = 0xFB000000ULL;
  Gu.device_id = 0x2684;
  Gu.s3_persist_armed = TRUE;
  Gu.s3_arena_armed = TRUE;
  Gu.s3_booter_dma = 0xDAD01000ULL;
  Gu.s3_booter_size = 57672U;
  Gu.s3_v67_dma = 0xDAD11000ULL;
  Gu.s3_boot_dma = 0xDAD21000ULL;
  Gu.s3_boot_size = 36864U;
  Gu.s3_meta_dma = 0xDAD31000ULL;
  Gu.s3_fwsec_dma = 0xDAD41000ULL;
  Gu.s3_fwsec_size = 65536U;
  Gu.wpr_meta_valid = TRUE;

  EFI_STATUS status = GuS3PrepareAndRegister(&mock_st);
  CHECK(status == EFI_SUCCESS, "GuS3PrepareAndRegister must succeed with arena consolidation");

  GU_S3_RESUME_CONTEXT *ctx = (GU_S3_RESUME_CONTEXT *)g_mock_nvs_buffer;
  CHECK(ctx->booter_dma == 0xDAD01000ULL, "ctx->booter_dma must match Gu.s3_booter_dma");
  CHECK(ctx->booter_size == 57672U, "ctx->booter_size must match Gu.s3_booter_size");
  CHECK(ctx->v67_dma == 0xDAD11000ULL, "ctx->v67_dma must match Gu.s3_v67_dma");
  CHECK(ctx->boot_dma == 0xDAD21000ULL, "ctx->boot_dma must match Gu.s3_boot_dma");
  CHECK(ctx->boot_size == 36864U, "ctx->boot_size must match Gu.s3_boot_size");
  CHECK(ctx->meta_dma == 0xDAD31000ULL, "ctx->meta_dma must match Gu.s3_meta_dma");
  CHECK(ctx->meta_size == 256U, "ctx->meta_size must be 256");

  /* Test Trampoline code builder with arena pointers */
  UINT8 buffer[4096];
  EFI_PHYSICAL_ADDRESS tramp_phys = 0x0009E000ULL;
  EFI_PHYSICAL_ADDRESS ctx_phys = 0xdc5d8000ULL;
  EFI_PHYSICAL_ADDRESS shadow_facs_phys = 0xdc617400ULL;
  EFI_PHYSICAL_ADDRESS telem_phys = 0xDA000000ULL;
  UINT32 bytes = GuAcpiBuildTrampolineCode(buffer, sizeof(buffer), tramp_phys, ctx_phys,
                                          shadow_facs_phys, telem_phys, 0xFB000000ULL,
                                          0x80001960U, 0x802D0064U, Gu.s3_fwsec_dma);
  CHECK(bytes > 0 && bytes <= GU_TRAMP_ENVELOPE_SIZE, "Trampoline builder must generate <= 2560 bytes with arena pointers");

  /* Verify Mailbox 0 in trampoline code points to Gu.s3_meta_dma (0xDAD31000) */
  BOOLEAN found_arena_meta = FALSE;
  for (UINT32 i = 0; i < bytes - 8; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840040U &&
        *(UINT32 *)(buffer + i + 6) == 0xDAD31000U) {
      found_arena_meta = TRUE;
      break;
    }
  }
  CHECK(found_arena_meta, "Trampoline Mailbox 0 write must use arena meta_dma (0xDAD31000)");

  /* Verify GSP kickoff pre-delay (~100 Port-0x80 reads) & 1M Port-0x80-paced Halt Poll */
  BOOLEAN found_gsp_pre_delay = FALSE;
  BOOLEAN found_gsp_halt_gate = FALSE;
  for (UINT32 i = 0; i + 24 < bytes; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00110100U &&
        *(UINT32 *)(buffer + i + 6) == 2U &&
        buffer[i+10] == 0xB9 && *(UINT32 *)(buffer + i + 11) == 100U &&
        buffer[i+15] == 0xE4 && buffer[i+16] == 0x80 && buffer[i+17] == 0x49 && buffer[i+18] == 0x75) {
      found_gsp_pre_delay = TRUE;
    }
    if (buffer[i] == 0xB9 && *(UINT32 *)(buffer + i + 1) == 1000000U &&
        buffer[i+5] == 0x8B && buffer[i+6] == 0x83 && *(UINT32 *)(buffer + i + 7) == 0x00110100U &&
        buffer[i+11] == 0xA8 && buffer[i+12] == 0x10 && buffer[i+13] == 0x75 &&
        buffer[i+15] == 0xE4 && buffer[i+16] == 0x80 && buffer[i+17] == 0x49 && buffer[i+18] == 0x75) {
      found_gsp_halt_gate = TRUE;
    }
  }
  CHECK(found_gsp_pre_delay, "Trampoline must issue ~100 Port-0x80 pre-delay reads after GSP FWSEC kickoff");
  CHECK(found_gsp_halt_gate, "Trampoline must gate on GSP Halt (0x00110100 & 0x10) for up to 1,000,000 Port-0x80 iterations");

  /* Verify SEC2 kickoff pre-delay (~100 Port-0x80), 6M Port-0x80 Booter Poll, and post-marker Halt Wait (v1.8.67) */
  BOOLEAN found_sec2_pre_delay = FALSE;
  BOOLEAN found_sec2_booter_poll = FALSE;
  BOOLEAN found_sec2_halt_wait = FALSE;
  for (UINT32 i = 0; i + 40 <= bytes; i++) {
    if (buffer[i] == 0xC7 && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840100U &&
        *(UINT32 *)(buffer + i + 6) == 2U &&
        buffer[i+10] == 0xB9 && *(UINT32 *)(buffer + i + 11) == 100U &&
        buffer[i+15] == 0xE4 && buffer[i+16] == 0x80 && buffer[i+17] == 0x49 && buffer[i+18] == 0x75) {
      found_sec2_pre_delay = TRUE;
    }
    if (buffer[i] == 0xB9 && *(UINT32 *)(buffer + i + 1) == 6000000U &&
        buffer[i+5] == 0x8B && buffer[i+6] == 0x83 && *(UINT32 *)(buffer + i + 7) == 0x00840040U &&
        buffer[i+11] == 0x3D && *(UINT32 *)(buffer + i + 12) == 0x67B00001U &&
        buffer[i+16] == 0x74 &&
        buffer[i+35] == 0xE4 && buffer[i+36] == 0x80 && buffer[i+37] == 0x49 && buffer[i+38] == 0x75) {
      found_sec2_booter_poll = TRUE;
    }
    if (buffer[i] == 0xB9 && *(UINT32 *)(buffer + i + 1) == 100000U &&
        buffer[i+5] == 0x8B && buffer[i+6] == 0x83 && *(UINT32 *)(buffer + i + 7) == 0x00840100U &&
        buffer[i+11] == 0xA8 && buffer[i+12] == 0x10 && buffer[i+13] == 0x75 &&
        buffer[i+15] == 0xE4 && buffer[i+16] == 0x80 && buffer[i+17] == 0x49 && buffer[i+18] == 0x75) {
      found_sec2_halt_wait = TRUE;
    }
  }
  CHECK(found_sec2_pre_delay, "Trampoline must issue ~100 Port-0x80 pre-delay reads after SEC2 Booter kickoff");
  CHECK(found_sec2_booter_poll, "Trampoline must poll SEC2 Booter (0x00840040) for up to 6,000,000 Port-0x80 iterations");
  CHECK(found_sec2_halt_wait, "Trampoline must wait for SEC2 CPUCTL Halt (0x00840100 & 0x10) with Port-0x80 pacing after marker");

  /* Verify +0x14C direct [meta_dma] host-RAM probe */
  BOOLEAN found_meta_probe = FALSE;
  for (UINT32 i = 0; i + 10 <= bytes; i++) {
    if (buffer[i] == 0xA1 && *(UINT32 *)(buffer + i + 1) == 0xDAD31000U &&
        buffer[i+5] == 0xA3 && *(UINT32 *)(buffer + i + 6) == (UINT32)(telem_phys + GU_TELEM_META_MAGIC_LO_OFF)) {
      found_meta_probe = TRUE;
      break;
    }
  }
  CHECK(found_meta_probe, "Trampoline must probe [meta_dma] directly into telemetry +0x14C before SEC2 kickoff");

  /* Verify Mailbox 1 save to +0x11C */
  BOOLEAN found_mb1_save = FALSE;
  for (UINT32 i = 0; i < bytes - 8; i++) {
    if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == 0x00840044U &&
        buffer[i+6] == 0xA3 && *(UINT32 *)(buffer + i + 7) == (UINT32)(telem_phys + 0x11CU)) {
      found_mb1_save = TRUE;
      break;
    }
  }
  CHECK(found_mb1_save, "Trampoline must record SEC2 Mailbox 1 at telemetry +0x11C");

  /* Verify v1.8.66 extended diagnostic reads (+0x120..+0x148) */
  static const struct {
    UINT32 reg_off;
    UINT32 telem_off;
  } v66_diags[11] = {
    { 0x00840094U, GU_TELEM_SEC2_DEBUGINFO_OFF },
    { 0x00840008U, GU_TELEM_SEC2_IRQSTAT_OFF },
    { 0x0084140CU, GU_TELEM_SEC2_TRACEPC_OFF },
    { 0x00841500U, GU_TELEM_SEC2_PRIV_ERR_OFF },
    { 0x00841510U, GU_TELEM_SEC2_HUB_ERR_OFF },
    { 0x00000600U, GU_TELEM_PMC_DEV_ENABLE_OFF },
    { 0x00111388U, GU_TELEM_GSP_RISCV_CPUCTL_OFF },
    { 0x00111668U, GU_TELEM_GSP_BCR_OFF },
    { 0x00841504U, GU_TELEM_SEC2_PRIV_INFO_OFF },
    { 0x00841508U, GU_TELEM_SEC2_PRIV_ADDR_OFF },
    { 0x00110094U, GU_TELEM_GSP_DEBUGINFO_OFF }
  };
  for (UINTN d = 0; d < 11; d++) {
    BOOLEAN found_diag = FALSE;
    for (UINT32 i = 0; i + 11 <= bytes; i++) {
      if (buffer[i] == 0x8B && buffer[i+1] == 0x83 && *(UINT32 *)(buffer + i + 2) == v66_diags[d].reg_off &&
          buffer[i+6] == 0xA3 && *(UINT32 *)(buffer + i + 7) == (UINT32)(telem_phys + v66_diags[d].telem_off)) {
        found_diag = TRUE;
        break;
      }
    }
    CHECK(found_diag, "Trampoline must capture v1.8.66 diagnostic register into +0x120..+0x148");
  }

  printf("[10] S3 Full 6 GPU-DMA Buffer Consolidation into Reserved Arena: OK!\n");
}

static void TestGuS3UniversalTopologiesAndHardening(void) {
  printf("[11] Testing v1.9.0 Universal Topologies (Intel, 2-Level Switch, RTX 4080/BAR0, <0xA0000, S0ix)...\n");
  GuAcpiResetShadowStateForTest();

  /* Setup reusable mock ACPI hierarchy */
  EFI_ACPI_2_0_FIRMWARE_ACPI_CONTROL_STRUCTURE mock_facs;
  memset(&mock_facs, 0, sizeof(mock_facs));
  mock_facs.Signature = GU_ACPI_FACS_SIGNATURE;
  mock_facs.Length = sizeof(mock_facs);
  mock_facs.FirmwareWakingVector = 0x00007C00U;
  mock_facs.XFirmwareWakingVector = 0x12345678ULL;

  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE mock_fadt;
  memset(&mock_fadt, 0, sizeof(mock_fadt));
  mock_fadt.Header.Signature = GU_ACPI_FADT_SIGNATURE;
  mock_fadt.Header.Length = sizeof(mock_fadt);
  mock_fadt.Header.Revision = 5;
  mock_fadt.Flags = GU_ACPI_FADT_FLAG_LOW_POWER_S0_IDLE;
  mock_fadt.FirmwareCtrl = (UINT32)(uintptr_t)&mock_facs;
  mock_fadt.XFirmwareCtrl = (UINT64)(uintptr_t)&mock_facs;
  UINT8 sum = 0;
  for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_fadt)[i]);
  mock_fadt.Header.Checksum = (UINT8)(0 - sum);

#pragma pack(push, 1)
  struct { EFI_ACPI_DESCRIPTION_HEADER Header; UINT64 Entry[1]; } mock_xsdt;
#pragma pack(pop)
  memset(&mock_xsdt, 0, sizeof(mock_xsdt));
  mock_xsdt.Header.Signature = 0x54445358U;
  mock_xsdt.Header.Length = sizeof(mock_xsdt);
  mock_xsdt.Entry[0] = (UINT64)(uintptr_t)&mock_fadt;
  sum = 0;
  for (UINT32 i = 0; i < mock_xsdt.Header.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_xsdt)[i]);
  mock_xsdt.Header.Checksum = (UINT8)(0 - sum);

  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER mock_rsdp;
  memset(&mock_rsdp, 0, sizeof(mock_rsdp));
  mock_rsdp.Signature = GU_ACPI_RSDP_SIGNATURE;
  mock_rsdp.Length = sizeof(mock_rsdp);
  mock_rsdp.Revision = 2;
  mock_rsdp.XsdtAddress = (UINT64)(uintptr_t)&mock_xsdt;
  sum = 0;
  for (UINT32 i = 0; i < 20; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.Checksum = (UINT8)(0 - sum);
  sum = 0;
  for (UINT32 i = 0; i < mock_rsdp.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.ExtendedChecksum = (UINT8)(0 - sum);

  EFI_BOOT_SERVICES mock_bs;
  memset(&mock_bs, 0, sizeof(mock_bs));
  mock_bs.AllocatePages = MockAllocatePages;
  mock_bs.FreePages = MockFreePages;
  mock_bs.LocateProtocol = MockLocateProtocol;
  mock_bs.CalculateCrc32 = MockCalculateCrc32;
  mock_bs.LocateHandleBuffer = MockLocateHandleBuffer;
  mock_bs.HandleProtocol = MockHandleProtocol;
  mock_bs.FreePool = MockFreePool;

  EFI_CONFIGURATION_TABLE cfg[1];
  EFI_GUID acpi20_guid = GU_EFI_ACPI_20_TABLE_GUID;
  cfg[0].VendorGuid = acpi20_guid;
  cfg[0].VendorTable = &mock_rsdp;
  memset(&mock_st, 0, sizeof(mock_st));
  mock_st.BootServices = &mock_bs;
  mock_st.NumberOfTableEntries = 1;
  mock_st.ConfigurationTable = cfg;

  /* --- Scenario A1: AMI Aptio V (0 PciIo bridges, multifunction 0:3:0 + 0:3:1 in RootBridgeIo, Bus 45) --- */
  g_mock_pci_count = 0; /* NO EFI_PCI_IO_PROTOCOL handles on bridges (matches AMI Aptio V hardware!) */
  g_mock_rb_count = 0;
  g_mock_rb_segment = 0;
  g_mock_rb_enabled = TRUE;
  AddMockRbEntry(0, 3, 0, 0x1022, 0x1482, 0x80, 0, 0, 0, 0x00);      /* Multifunction host bridge func 0 */
  AddMockRbEntry(0, 3, 1, 0x1022, 0x1483, 0x01, 0, 45, 45, 0x50);    /* AMD Root Port 0:3:1 -> Bus 45, Cap 0x50 */

  memset(&Gu, 0, sizeof(Gu));
  Gu.bs = &mock_bs;
  Gu.device_id = 0x2684;
  Gu.chip_id = 0x192000A1U;
  Gu.s3_fwsec_dma = 0xDAD41000ULL;

  GU_S3_RESUME_CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 2;
  ctx.bar_base = 0xFB000000ULL;
  ctx.pci_id = 0x268410DEU;
  ctx.chip_id = 0x192000A1U;
  ctx.bus = 45; ctx.device = 0; ctx.function = 0;
  ctx.pci_cmd_port_addr = 0x802D0004U;
  ctx.booter_dma = 0xDAD01000ULL;
  ctx.meta_dma = 0xDAD31000ULL;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;

  UINT8 nvs_page[4096];
  UINT8 low_page[4096];
  memset(nvs_page, 0, sizeof(nvs_page));
  memset(low_page, 0, sizeof(low_page));
  memset(g_mock_high_acpi, 0, sizeof(g_mock_high_acpi));
  g_log_history[0] = '\0';

  GU_ACPI_FACS_INFO info;
  EFI_PHYSICAL_ADDRESS low_phys = 0x0009D000ULL;
  EFI_STATUS s = GuAcpiInstallShadowFacs(&mock_st, &ctx, nvs_page, 0xDC500000ULL, low_page, low_phys, &info);
  CHECK(s == EFI_SUCCESS, "AMI Aptio V RootBridgeIo GuAcpiInstallShadowFacs must succeed with 0 PciIo bridges");
  CHECK(ctx.bridge_count == 1, "AMI Aptio V must discover 0:3:1 dynamically via RootBridgeIo");
  CHECK(ctx.bridges[0].bdf_base == 0x80001900U, "AMI Aptio V Root Port BDF must be 0:3:1");
  CHECK(ctx.bridges[0].bus_val == 0x002D2D00U, "AMI Aptio V Root Port BusCfg must be 0x002D2D00");
  CHECK(ctx.bridges[0].link_reg == 0x80001960U, "AMI Aptio V Root Port LinkReg must be 0x80001960");
  CHECK(ctx.root_port_link_reg == 0x80001960U, "ctx.root_port_link_reg must match discovered LinkReg");
  CHECK(strstr(g_log_history, "S3 PCIe Bridge[0]: BDF=0x80001900 (pri=0 sec=45 sub=45)") != NULL,
        "Log must explicitly record discovered bridge pri/sec/sub");

  /* --- Scenario A2 + E: Intel Root Port (0x8086, Bus 1, 0:1:0 via RootBridgeIo, 0 PciIo), BAR0 0xD0000000, S0ix + XFirmwareWakingVector --- */
  mock_rsdp.XsdtAddress = (UINT64)(uintptr_t)&mock_xsdt;
  sum = 0; for (UINT32 i = 0; i < 20; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.Checksum = (UINT8)(0 - sum);
  sum = 0; for (UINT32 i = 0; i < mock_rsdp.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.ExtendedChecksum = (UINT8)(0 - sum);
  mock_fadt.FirmwareCtrl = (UINT32)(uintptr_t)&mock_facs;
  mock_fadt.XFirmwareCtrl = (UINT64)(uintptr_t)&mock_facs;
  mock_fadt.Header.Checksum = 0;
  sum = 0; for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_fadt)[i]);
  mock_fadt.Header.Checksum = (UINT8)(0 - sum);
  mock_facs.FirmwareWakingVector = 0x00007C00U;
  mock_facs.XFirmwareWakingVector = 0x12345678ULL;
  GuAcpiResetShadowStateForTest();

  g_mock_pci_count = 0;
  g_mock_rb_count = 0;
  g_mock_rb_enabled = TRUE;
  AddMockRbEntry(0, 1, 0, 0x8086, 0xA70D, 0x01, 0, 1, 1, 0xA0); /* Intel Root Port 0:1:0 -> Bus 1, LinkReg = 0x800008B0 */

  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 2;
  ctx.bar_base = 0xD0000000ULL;
  ctx.pci_id = 0x268410DEU;
  ctx.chip_id = 0x192000A1U;
  ctx.bus = 1; ctx.device = 0; ctx.function = 0;
  ctx.pci_cmd_port_addr = 0x80010004U;
  ctx.booter_dma = 0xDAD01000ULL;
  ctx.meta_dma = 0xDAD31000ULL;
  ctx.val_ss0 = 0x88888888U;
  ctx.val_ss1 = 0x00000008U;

  memset(nvs_page, 0, sizeof(nvs_page));
  memset(low_page, 0, sizeof(low_page));
  memset(g_mock_high_acpi, 0, sizeof(g_mock_high_acpi));
  g_log_history[0] = '\0';

  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, nvs_page, 0xDC500000ULL, low_page, low_phys, &info);
  CHECK(s == EFI_SUCCESS, "Intel Root Port GuAcpiInstallShadowFacs must succeed via RootBridgeIo");
  CHECK(ctx.bridge_count == 1, "Intel topology must discover 1 upstream bridge");
  CHECK(ctx.bridges[0].bdf_base == 0x80000800U, "Intel Root Port BDF must be 0:1:0 (0x80000800)");
  CHECK(ctx.bridges[0].bus_val == 0x00010100U, "Intel Root Port BusCfg must be 0x00010100");
  CHECK(ctx.bridges[0].link_reg == 0x800008B0U, "Intel Root Port LinkReg must be 0x800008B0");
  CHECK(*(UINT32 *)(low_page + GU_TRAMP_BRIDGE_TBL_OFF + GU_TRAMP_BRIDGE_BDF_OFF) == 0x80000800U, "Low bridge table[0] BDF");
  CHECK(*(UINT32 *)(low_page + GU_TRAMP_BRIDGE_TBL_OFF + GU_TRAMP_BRIDGE_BUS_OFF) == 0x00010100U, "Low bridge table[0] BusVal");
  CHECK(*(UINT32 *)(low_page + GU_TRAMP_BRIDGE_TBL_OFF + GU_TRAMP_BRIDGE_ENTRY_SIZE) == 0U, "Low bridge table terminator");

  UINT32 *intel_tbl = (UINT32 *)(low_page + GU_TRAMP_PCI_PROBE_ADDRS_OFF);
  CHECK(intel_tbl[0] == 0x80000800U, "Intel probe[0] must point to 0:1:0");
  CHECK(intel_tbl[3] == 0x800008B0U, "Intel probe[3] must point to 0:1:0 LinkReg");
  CHECK(intel_tbl[4] == 0x80010000U, "Intel probe[4] must point to GPU 1:0:0");

  /* Verify 0xD0F0D000 inclusive 16MB window for 0xD0000000 BAR0 and dynamic GPU 1:0:0 config ops */
  BOOLEAN found_d0_win = FALSE, found_gpu_1_0_0_cmd = FALSE, found_gpu_1_0_0_bar = FALSE;
  for (UINT32 i = 0; i + 5 <= GU_TRAMP_ENVELOPE_SIZE; i++) {
    if (low_page[i] == 0xB8 && *(UINT32 *)(low_page + i + 1) == 0xD0F0D000U) found_d0_win = TRUE;
    if (low_page[i] == 0xB8 && *(UINT32 *)(low_page + i + 1) == 0x80010004U) found_gpu_1_0_0_cmd = TRUE;
    if (low_page[i] == 0xB8 && *(UINT32 *)(low_page + i + 1) == 0x80010010U) found_gpu_1_0_0_bar = TRUE;
  }
  CHECK(found_d0_win, "Trampoline must emit 0xD0F0D000 16MB bridge memory window for BAR0=0xD0000000");
  CHECK(found_gpu_1_0_0_cmd, "Trampoline must address dynamic GPU BDF 1:0:0 Cmd (0x80010004)");
  CHECK(found_gpu_1_0_0_bar, "Trampoline must address dynamic GPU BDF 1:0:0 BAR0 (0x80010010)");

  /* Verify Scenario E: S0ix FADT Bit 21 & non-zero XFirmwareWakingVector warnings */
  CHECK(info.s0ix_warning == TRUE, "FADT Flags Bit 21 must set s0ix_warning");
  CHECK(info.fadt_flags == GU_ACPI_FADT_FLAG_LOW_POWER_S0_IDLE, "info.fadt_flags must capture Bit 21");
  CHECK(info.original_xfirmware_waking_vector == 0x12345678ULL, "Original 64-bit XFirmwareWakingVector must be preserved in Info");
  CHECK(mock_facs.XFirmwareWakingVector == 0ULL, "Real FACS XFirmwareWakingVector must be cleared to 0");
  CHECK(strstr(g_log_history, "LOW_POWER_S0_IDLE_CAPABLE") != NULL, "Log must emit S0ix Modern Standby preflight warning");
  CHECK(strstr(g_log_history, "XFirmwareWakingVector") != NULL, "Log must emit XFirmwareWakingVector preflight warning");

  /* --- Scenario B: 2-Level PCIe Switch (Root 0:1:0 WITHOUT PciIo + Switch ports WITH/WITHOUT PciIo; sibling excluded) --- */
  g_mock_pci_count = 0;
  AddMockBridge(1, 0, 0, 0x10B5, 0x8747, 1, 2, 5, 0x70); /* Only Switch ports in PciIo; Root Port 0:1:0 deliberately omitted! */
  AddMockBridge(2, 4, 0, 0x10B5, 0x8747, 2, 3, 3, 0x70);
  g_mock_rb_count = 0;
  g_mock_rb_enabled = TRUE;
  AddMockRbEntry(0, 1, 0, 0x8086, 0xA70D, 0x01, 0, 1, 5, 0x40); /* Root Port (LinkReg=0x80000850) */
  AddMockRbEntry(1, 0, 0, 0x10B5, 0x8747, 0x01, 1, 2, 5, 0x60); /* Switch Upstream (LinkReg=0x80010070) */
  AddMockRbEntry(2, 3, 0, 0x10B5, 0x8747, 0x01, 2, 4, 4, 0x70); /* Sibling Downstream -> MUST BE EXCLUDED */
  AddMockRbEntry(2, 4, 0, 0x10B5, 0x8747, 0x01, 2, 3, 3, 0x70); /* Target Downstream (LinkReg=0x80022080) */

  /* Recover Real RSDP/FADT/FACS cleanly after Scenario A installed Shadow state */
  mock_rsdp.XsdtAddress = (UINT64)(uintptr_t)&mock_xsdt;
  sum = 0; for (UINT32 i = 0; i < 20; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.Checksum = (UINT8)(0 - sum);
  sum = 0; for (UINT32 i = 0; i < mock_rsdp.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.ExtendedChecksum = (UINT8)(0 - sum);
  mock_fadt.FirmwareCtrl = (UINT32)(uintptr_t)&mock_facs;
  mock_fadt.XFirmwareCtrl = (UINT64)(uintptr_t)&mock_facs;
  mock_fadt.Header.Checksum = 0;
  sum = 0; for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_fadt)[i]);
  mock_fadt.Header.Checksum = (UINT8)(0 - sum);
  mock_facs.FirmwareWakingVector = 0x00007C00U;

  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC;
  ctx.version = 2;
  ctx.bar_base = 0xC8000000ULL;
  ctx.bus = 3; ctx.device = 0; ctx.function = 0;
  ctx.pci_cmd_port_addr = 0x80030004U;
  ctx.booter_dma = 0xDAD01000ULL;
  ctx.meta_dma = 0xDAD31000ULL;
  memset(low_page, 0, sizeof(low_page));
  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, nvs_page, 0xDC500000ULL, low_page, low_phys, &info);
  CHECK(s == EFI_SUCCESS, "2-level PCIe switch GuAcpiInstallShadowFacs must succeed");
  CHECK(ctx.bridge_count == 3, "2-level switch topology must discover all 3 bridges (Root 0:1:0 not missed!)");
  CHECK(ctx.bridges[0].bdf_base == 0x80000800U && ctx.bridges[0].bus_val == 0x00050100U, "Switch[0] Root Port 0:1:0");
  CHECK(ctx.bridges[1].bdf_base == 0x80010000U && ctx.bridges[1].bus_val == 0x00050201U, "Switch[1] Upstream 1:0:0");
  CHECK(ctx.bridges[2].bdf_base == 0x80022000U && ctx.bridges[2].bus_val == 0x00030302U, "Switch[2] Downstream 2:4:0");
  CHECK(ctx.root_port_link_reg == 0x80022080U, "Direct parent Downstream Port LinkReg (2:4:0 -> 0x80022080) must be selected");
  CHECK(*(UINT32 *)(low_page + GU_TRAMP_PCI_PROBE_ADDRS_OFF + 3U * 4U) == 0x80022080U, "probe[3] must probe direct parent LinkReg");
  CHECK(*(UINT32 *)(low_page + GU_TRAMP_BRIDGE_TBL_OFF + 2U * GU_TRAMP_BRIDGE_ENTRY_SIZE) == 0x80022000U,
        "Bridge table[2] must be Leaf Downstream 2:4:0");
  CHECK(*(UINT32 *)(low_page + GU_TRAMP_BRIDGE_TBL_OFF + 3U * GU_TRAMP_BRIDGE_ENTRY_SIZE) == 0U,
        "Bridge table[3] must be null terminator");

  BOOLEAN found_leaf_ptr = FALSE, found_cnt_3 = FALSE;
  UINT32 last_br_lin = (UINT32)(low_phys + GU_TRAMP_BRIDGE_TBL_OFF + (2U * GU_TRAMP_BRIDGE_ENTRY_SIZE));
  for (UINT32 i = 0; i + 10 <= GU_TRAMP_ENVELOPE_SIZE; i++) {
    if (low_page[i] == 0xBE && *(UINT32 *)(low_page + i + 1) == last_br_lin &&
        low_page[i+5] == 0xB9 && *(UINT32 *)(low_page + i + 6) == 3U) {
      found_leaf_ptr = TRUE;
      found_cnt_3 = TRUE;
      break;
    }
  }
  CHECK(found_leaf_ptr && found_cnt_3, "Teardown must initialize ESI at leaf bridge (2:4:0) and ECX=3");

  /* --- Scenario F: PciIo Fallback when RootBridgeIo is unavailable --- */
  g_mock_rb_enabled = FALSE;
  g_mock_pci_count = 0;
  AddMockBridge(0, 1, 0, 0x8086, 0xA70D, 0, 1, 1, 0xA0);
  mock_rsdp.XsdtAddress = (UINT64)(uintptr_t)&mock_xsdt;
  sum = 0; for (UINT32 i = 0; i < 20; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.Checksum = (UINT8)(0 - sum);
  sum = 0; for (UINT32 i = 0; i < mock_rsdp.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_rsdp)[i]);
  mock_rsdp.ExtendedChecksum = (UINT8)(0 - sum);
  mock_fadt.FirmwareCtrl = (UINT32)(uintptr_t)&mock_facs;
  mock_fadt.XFirmwareCtrl = (UINT64)(uintptr_t)&mock_facs;
  mock_fadt.Header.Checksum = 0;
  sum = 0; for (UINT32 i = 0; i < mock_fadt.Header.Length; i++) sum = (UINT8)(sum + ((UINT8 *)&mock_fadt)[i]);
  mock_fadt.Header.Checksum = (UINT8)(0 - sum);
  mock_facs.FirmwareWakingVector = 0x00007C00U;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC; ctx.version = 2; ctx.bar_base = 0xD0000000ULL;
  ctx.bus = 1; ctx.device = 0; ctx.function = 0; ctx.pci_cmd_port_addr = 0x80010004U;
  ctx.booter_dma = 0xDAD01000ULL; ctx.meta_dma = 0xDAD31000ULL;
  s = GuAcpiInstallShadowFacs(&mock_st, &ctx, nvs_page, 0xDC500000ULL, low_page, low_phys, &info);
  CHECK(s == EFI_SUCCESS && ctx.bridge_count == 1 && ctx.bridges[0].bdf_base == 0x80000800U,
        "PciIo fallback must still work cleanly when RootBridgeIo is absent");

  /* --- Scenario G: Clean Abort when 0 Upstream Bridges exist for target_gpu_bus > 0 (NO 0x80001900 fallback!) --- */
  g_mock_pci_count = 0;
  g_mock_rb_count = 0;
  g_mock_rb_enabled = TRUE;
  AddMockRbEntry(0, 2, 0, 0x8086, 0x1234, 0x01, 0, 10, 10, 0x40); /* Unrelated bridge (bus 10 != 45) */
  mock_facs.FirmwareWakingVector = 0x00007C00U;
  memset(&ctx, 0, sizeof(ctx));
  ctx.magic = GU_S3_CONTEXT_MAGIC; ctx.version = 2; ctx.bar_base = 0xFB000000ULL;
  ctx.bus = 45; ctx.device = 0; ctx.function = 0; ctx.pci_cmd_port_addr = 0x802D0004U;
  memset(low_page, 0, sizeof(low_page));
  g_log_history[0] = '\0';
  EFI_STATUS no_br_s = GuAcpiInstallShadowFacs(&mock_st, &ctx, nvs_page, 0xDC500000ULL, low_page, low_phys, &info);
  CHECK(no_br_s == EFI_NOT_FOUND, "GuAcpiInstallShadowFacs must abort cleanly with EFI_NOT_FOUND when 0 bridges match bus > 0");
  CHECK(ctx.bridge_count == 0, "bridge_count must remain 0 when no upstream bridge matches");
  CHECK(mock_facs.FirmwareWakingVector == 0x00007C00U, "Clean abort must NOT modify RealFACS waking vector");
  CHECK(*(UINT32 *)(low_page + GU_TRAMP_BRIDGE_TBL_OFF) == 0U, "Low bridge table must NOT contain 0x80001900 fallback");
  CHECK(strstr(g_log_history, "ERROR") != NULL, "Missing bridge abort must log an explicit ERROR");
  UINT32 abort_bytes = GuAcpiBuildTrampolineCodeEx(
      low_page, sizeof(low_page), low_phys, 0xDC5D8000ULL, 0xDC617400ULL,
      0xDA000000ULL, 0xFB000000ULL, 0U, 0x802D0064U, 0xDAD41000ULL, &ctx);
  CHECK(abort_bytes == 0U, "GuAcpiBuildTrampolineCodeEx must return 0 when bus > 0 and 0 bridges without RootPortLinkReg");
  g_mock_rb_enabled = FALSE;
  g_mock_rb_count = 0;

  /* --- Scenario C: RTX 4080 (0x270410DE) + Chip ID 0x193000A1 + Custom FWSEC/SEC2 Geometries --- */
  GU_S3_RESUME_CONTEXT ctx4080;
  memset(&ctx4080, 0, sizeof(ctx4080));
  ctx4080.magic = GU_S3_CONTEXT_MAGIC;
  ctx4080.version = 2;
  ctx4080.bar_base = 0xF0000000ULL;
  ctx4080.pci_id = 0x270410DEU; /* RTX 4080 AD103 */
  ctx4080.chip_id = 0x193000A1U;
  ctx4080.bus = 7; ctx4080.device = 0; ctx4080.function = 0;
  ctx4080.pci_cmd_port_addr = 0x80070004U;
  ctx4080.booter_dma = 0xDAD01000ULL;
  ctx4080.meta_dma = 0xDAD31000ULL;
  ctx4080.fwsec_imem_chunks = 210U;
  ctx4080.fwsec_dmem_chunks = 12U;
  ctx4080.fwsec_data_off = 0xD200U;
  ctx4080.fwsec_sig_dmem = 0x09A0U;
  ctx4080.fwsec_engine_mask = 0x0400U;
  ctx4080.fwsec_ucode_id = 10U;
  ctx4080.booter_app_code_off = 512U;
  ctx4080.booter_imem_chunks = 110U;
  ctx4080.booter_os_data_off = 0x7200U;
  ctx4080.booter_dmem_chunks = 80U;
  ctx4080.booter_sig_dmem = 0x20U;
  ctx4080.booter_engine_mask = 1U;
  ctx4080.booter_ucode_id = 4U;

  UINT8 buf4080[4096];
  UINT32 bytes4080 = GuAcpiBuildTrampolineCodeEx(
      buf4080, sizeof(buf4080), 0x0009E000ULL, 0xDC5D8000ULL, 0xDC617400ULL,
      0xDA000000ULL, 0xF0000000ULL, 0x800008B0U, 0x80070064U, 0xDAD41000ULL, &ctx4080);
  CHECK(bytes4080 > 0 && bytes4080 <= GU_TRAMP_ENVELOPE_SIZE, "RTX 4080 custom geometry trampoline must fit within 0x0A00");

  BOOLEAN f_pci = FALSE, f_chip = FALSE, f_fw_imem = FALSE, f_fw_dmem = FALSE, f_fw_sig = FALSE, f_fw_uc = FALSE;
  BOOLEAN f_bt_app = FALSE, f_bt_imem = FALSE, f_bt_dmem = FALSE, f_bt_sig = FALSE, f_bt_uc = FALSE, f_bt_vec = FALSE;
  for (UINT32 i = 0; i + 10 <= bytes4080; i++) {
    if (buf4080[i] == 0x3D && *(UINT32 *)(buf4080 + i + 1) == 0x270410DEU) f_pci = TRUE;
    if (buf4080[i] == 0x3D && *(UINT32 *)(buf4080 + i + 1) == 0x193000A1U) f_chip = TRUE;
    if (buf4080[i] == 0xBD && *(UINT32 *)(buf4080 + i + 1) == 210U) f_fw_imem = TRUE;
    if (buf4080[i] == 0xBD && *(UINT32 *)(buf4080 + i + 1) == 12U) f_fw_dmem = TRUE;
    if (buf4080[i] == 0xC7 && buf4080[i+1] == 0x83 && *(UINT32 *)(buf4080 + i + 2) == 0x00111210U && *(UINT32 *)(buf4080 + i + 6) == 0x09A0U) f_fw_sig = TRUE;
    if (buf4080[i] == 0xC7 && buf4080[i+1] == 0x83 && *(UINT32 *)(buf4080 + i + 2) == 0x00111198U && *(UINT32 *)(buf4080 + i + 6) == 10U) f_fw_uc = TRUE;
    if (buf4080[i] == 0xBA && *(UINT32 *)(buf4080 + i + 1) == 512U) f_bt_app = TRUE;
    if (buf4080[i] == 0xBD && *(UINT32 *)(buf4080 + i + 1) == 110U) f_bt_imem = TRUE;
    if (buf4080[i] == 0xBD && *(UINT32 *)(buf4080 + i + 1) == 80U) f_bt_dmem = TRUE;
    if (buf4080[i] == 0xC7 && buf4080[i+1] == 0x83 && *(UINT32 *)(buf4080 + i + 2) == 0x00841210U && *(UINT32 *)(buf4080 + i + 6) == 0x20U) f_bt_sig = TRUE;
    if (buf4080[i] == 0xC7 && buf4080[i+1] == 0x83 && *(UINT32 *)(buf4080 + i + 2) == 0x00841198U && *(UINT32 *)(buf4080 + i + 6) == 4U) f_bt_uc = TRUE;
    if (buf4080[i] == 0xC7 && buf4080[i+1] == 0x83 && *(UINT32 *)(buf4080 + i + 2) == 0x00840104U && *(UINT32 *)(buf4080 + i + 6) == 512U) f_bt_vec = TRUE;
  }
  CHECK(f_pci && f_chip, "RTX 4080 PCI ID (0x270410DE) and Chip ID (0x193000A1) must be emitted");
  CHECK(f_fw_imem && f_fw_dmem && f_fw_sig && f_fw_uc, "Custom FWSEC chunk counts/sig/ucode must be emitted");
  CHECK(f_bt_app && f_bt_imem && f_bt_dmem && f_bt_sig && f_bt_uc && f_bt_vec, "Custom SEC2 Booter geometry/code_va must be emitted");

  /* --- Scenario D: Strict Low-RAM < 0x000A0000 Enforcement (NO 0x000FFFFF VGA/ROM Fallback) --- */
  g_mock_pci_count = 0;
  g_mock_fail_low_tramp_alloc = TRUE;
  g_mock_low_nvs_alloc_calls = 0;
  g_mock_last_low_max_addr = 0;
  Gu.bar_base = 0xFB000000ULL;
  Gu.s3_persist_armed = TRUE;
  Gu.s3_arena_armed = TRUE;
  Gu.s3_booter_dma = 0xDAD01000ULL; Gu.s3_booter_size = 57672U;
  Gu.s3_v67_dma = 0xDAD11000ULL;
  Gu.s3_boot_dma = 0xDAD21000ULL; Gu.s3_boot_size = 36864U;
  Gu.s3_meta_dma = 0xDAD31000ULL;
  Gu.s3_fwsec_dma = 0xDAD41000ULL; Gu.s3_fwsec_size = 65536U;
  Gu.wpr_meta_valid = TRUE;
  EFI_STATUS fail_s = GuS3PrepareAndRegister(&mock_st);
  g_mock_fail_low_tramp_alloc = FALSE;
  CHECK(fail_s == EFI_OUT_OF_RESOURCES, "GuS3PrepareAndRegister must return EFI_OUT_OF_RESOURCES when <0xA0000 low RAM unavailable");
  CHECK(g_mock_low_nvs_alloc_calls == 1, "Must NOT retry low trampoline allocation into 0x000A0000..0x000FFFFF VGA/ROM zone");
  CHECK(g_mock_last_low_max_addr <= 0x0009FFFFULL, "Low trampoline allocation cap must remain strictly <= 0x0009FFFF");

  printf("[11] v1.9.0 Universal Topologies & Hardening (Intel/Switch/4080/<0xA0000/S0ix): OK!\n");
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== Universal S3 Persistence Resume Engine Test Suite ===\n");
  fflush(stdout);
  TestGuS3ResumeContextOffsets();
  TestGuS3ExecuteResumeUnlock_Validation();
  TestGuS3ExecuteResumeUnlock_AdaAmpere();
  TestGuS3ExecuteResumeUnlock_Turing();
  TestGuS3ResumeDispatch_Wrapper();
  TestGuS3GuidsAndOpcodes();
  TestGuAcpiFacsDiscoveryAndPatching();
  TestGuAcpiBuildTrampolineCode();
  TestGuS3Prepare_BooterUnpackAndPatch();
  TestGuS3PersistentMemoryRetention();
  TestGuS3ArenaConsolidation();
  TestGuS3UniversalTopologiesAndHardening();

  printf("=== ALL S3 RESUME TESTS PASSED (%u checks) [OK] ===\n", g_checks);
  fflush(stdout);
  return 0;
}
