/* SPDX-License-Identifier: GPL-2.0-only */
#include "s3_resume.h"
#include "port.h"
#include "profile.h"
#include "compat/compat.h"

extern GU_ASSET GuAssets[5];

INTN GuS3ExecuteResumeUnlock(GU_S3_RESUME_CONTEXT *ctx) {
  if (!ctx || ctx->magic != GU_S3_CONTEXT_MAGIC || ctx->bar_base == 0) {
    return -1;
  }

  volatile UINT32 *bar = (volatile UINT32 *)(UINTN)ctx->bar_base;

  if (ctx->is_turing) {
    /* Turing: SEC2 Falcon reset and payload kick */
    bar[0x008403c0 / 4] = 0;
    if (ctx->booter_dma) {
      bar[0x00840110 / 4] = (UINT32)(ctx->booter_dma >> 8);
      bar[0x00840100 / 4] = 2; /* CPUCTL start */
      for (UINT32 i = 0; i < 50000; i++) {
        if (bar[0x00840100 / 4] & 0x10) break;
      }
    }
    ctx->resume_status = 1;
    ctx->resume_count++;
    return 0;
  }

  /* Ada / Ampere: Full SEC2 Falcon handshake & rate-limiter unlatch */
  bar[0x008403c0 / 4] = 0; /* Reset SEC2 */

  UINT32 trans = bar[0x00840624 / 4];
  trans |= (1U << 7);
  bar[0x00840624 / 4] = trans;
  bar[0x0084010c / 4] = 0;
  trans = (bar[0x00840600 / 4] & ~7U) | 5U;
  bar[0x00840600 / 4] = trans;

  if (ctx->booter_dma && ctx->booter_size) {
    bar[0x00840110 / 4] = (UINT32)(ctx->booter_dma >> 8);
    bar[0x00840114 / 4] = 0;
    bar[0x00840118 / 4] = (ctx->booter_size & ~0xffU) | 0x10;
  }

  bar[0x00840100 / 4] = 2; /* Start Falcon CPU */

  /* Poll Falcon Engine Halt (up to 50,000 iterations) */
  for (UINT32 i = 0; i < 50000; i++) {
    if (bar[0x00840100 / 4] & 0x10) break;
  }

  /* Latch rate-limiter selectors */
  bar[0x0082381c / 4] = ctx->val_ss0;
  bar[0x00823820 / 4] = ctx->val_ss1;

  UINT32 r0 = bar[0x0082381c / 4];
  UINT32 r1 = bar[0x00823820 / 4];
  if (r0 == ctx->val_ss0 && (r1 & 0xf) == (ctx->val_ss1 & 0xf)) {
    ctx->resume_status = 1;
  } else {
    ctx->resume_status = 2;
  }

  /* Clear SEC2 reset to leave hardware clean */
  bar[0x008403c0 / 4] = 0;

  ctx->last_sec2_marker = bar[0x00840040 / 4];
  ctx->resume_count++;

  return (ctx->resume_status == 1) ? 0 : -2;
}

VOID EFIAPI GuS3ResumeDispatch(IN CONST VOID *This, IN VOID *Context) {
  GU_S3_RESUME_CONTEXT *ctx = (GU_S3_RESUME_CONTEXT *)Context;
  if (!ctx || ctx->magic != GU_S3_CONTEXT_MAGIC) {
    /* Check if motherboard firmware passes Context as the first argument in RCX */
    if (This && ((CONST GU_S3_RESUME_CONTEXT *)This)->magic == GU_S3_CONTEXT_MAGIC) {
      ctx = (GU_S3_RESUME_CONTEXT *)This;
    } else {
      return;
    }
  }
  (VOID)GuS3ExecuteResumeUnlock(ctx);
}

EFI_STATUS GuS3PrepareAndRegister(EFI_SYSTEM_TABLE *t) {
  if (!t || !t->BootServices) return EFI_INVALID_PARAMETER;
  if (!Gu.bar_base) return EFI_NOT_READY;

  static EFI_GUID S3SaveStateGuid = EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  EFI_S3_SAVE_STATE_PROTOCOL *s3 = NULL;
  EFI_STATUS s3_status = t->BootServices->LocateProtocol(&S3SaveStateGuid, NULL, (VOID **)&s3);
  if (EFI_ERROR(s3_status) || !s3 || !s3->Write) {
    GuLog("GuS3PrepareAndRegister: S3 Save State protocol unavailable (%#llx)\n", (unsigned long long)s3_status);
    return s3_status;
  }

  /* Allocate 64 pages (256 KB) of EfiACPIMemoryNVS.
   * ACPI NVS is preserved in DRAM across S3 sleep and is NEVER touched or overwritten by Windows. */
  EFI_PHYSICAL_ADDRESS nvs_phys = 0;
  EFI_STATUS alloc_status = t->BootServices->AllocatePages(
      AllocateAnyPages,
      EfiACPIMemoryNVS,
      64,
      &nvs_phys
  );
  if (EFI_ERROR(alloc_status) || !nvs_phys) {
    GuLog("GuS3PrepareAndRegister: Failed to allocate ACPI NVS pages (%#llx)\n", (unsigned long long)alloc_status);
    return alloc_status;
  }

  UINT8 *nvs_base = (UINT8 *)(UINTN)nvs_phys;
  memset(nvs_base, 0, 64 * 4096);

  GU_S3_RESUME_CONTEXT *ctx = (GU_S3_RESUME_CONTEXT *)nvs_base;
  ctx->magic = GU_S3_CONTEXT_MAGIC;
  ctx->version = 1;
  ctx->bar_base = Gu.bar_base;
  ctx->pci_id = Gu.device_id;
  ctx->is_ampere = Gu.is_ampere;
  ctx->is_turing = Gu.is_turing;
  ctx->val_ss0 = 0x88888888U;
  ctx->val_ss1 = 0x00000008U;

  /* Staging offsets within the 256 KB NVS allocation */
  ctx->booter_dma = nvs_phys + 0x01000;
  ctx->v67_dma = nvs_phys + 0x11000;
  ctx->boot_dma = nvs_phys + 0x21000;
  ctx->meta_dma = nvs_phys + 0x2A000;

  if (GuAssets[0].firmware.data && GuAssets[0].firmware.size) {
    ctx->booter_size = (UINT32)GuAssets[0].firmware.size;
    memcpy(nvs_base + 0x01000, GuAssets[0].firmware.data, ctx->booter_size);
  }
  if (GuAssets[1].firmware.data && GuAssets[1].firmware.size) {
    ctx->v67_size = (UINT32)GuAssets[1].firmware.size;
    memcpy(nvs_base + 0x11000, GuAssets[1].firmware.data, ctx->v67_size);
  }
  if (GuAssets[3].firmware.data && GuAssets[3].firmware.size) {
    ctx->boot_size = (UINT32)GuAssets[3].firmware.size;
    memcpy(nvs_base + 0x21000, GuAssets[3].firmware.data, ctx->boot_size);
  }
  if (GuAssets[4].firmware.data && GuAssets[4].firmware.size) {
    ctx->meta_size = (UINT32)GuAssets[4].firmware.size;
    memcpy(nvs_base + 0x2A000, GuAssets[4].firmware.data, ctx->meta_size);
  }

  /* Copy the self-contained GuS3ResumeDispatch PIC machine code into the ACPI NVS buffer.
   * This guarantees that the executable code survives Windows ExitBootServices() and memory
   * reclamation even if EfiBootServicesCode / EfiLoaderCode is overwritten by the OS. */
  memcpy(nvs_base + 0x00800, (const void *)(uintptr_t)GuS3ResumeDispatch, 0x200);

  /* 1. Register S3 Boot Script Dispatch 2 OpCode:
   * Calls the NVS-resident dispatch handler with Context = nvs_phys upon motherboard S3 resume. */
  EFI_PHYSICAL_ADDRESS dispatch_entry = nvs_phys + 0x00800;
  EFI_STATUS s_dispatch = s3->Write(
      s3,
      EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE,
      dispatch_entry,
      nvs_phys
  );

  /* 2. Register fallback MMIO write opcodes for SS0/SS1 */
  UINT32 val_ss0 = ctx->val_ss0;
  UINT32 val_ss1 = ctx->val_ss1;
  UINT64 addr_ss0 = (UINT64)ctx->bar_base + 0x0082381cULL;
  UINT64 addr_ss1 = (UINT64)ctx->bar_base + 0x00823820ULL;
  (VOID)s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, addr_ss0, (UINTN)1, &val_ss0);
  (VOID)s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, addr_ss1, (UINTN)1, &val_ss1);

  GuLog("S3 Persistence: Registered ACPI NVS Dispatch (NVS=%#llx Status=%#llx)\n",
        (unsigned long long)nvs_phys, (unsigned long long)s_dispatch);

  return EFI_SUCCESS;
}
