/* SPDX-License-Identifier: GPL-2.0-only */
#include "s3_resume.h"
#include "s3_facs.h"
#include "port.h"
#include "profile.h"
#include "compat/compat.h"
#include "vendor/prep_610.h"

#define GU_BOOTER_CONTAINER_MAGIC 0x314C5442U /* 'BTL1' */

extern GU_ASSET GuAssets[5];

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

  if (ctx->bar_base == 0) {
    ctx->resume_status = 2;
    return;
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
    return;
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
}

INTN GuS3ExecuteResumeUnlock(GU_S3_RESUME_CONTEXT *ctx) {
  if (!ctx || ctx->magic != GU_S3_CONTEXT_MAGIC || ctx->bar_base == 0) {
    return -1;
  }
  GuS3ResumeDispatch(NULL, ctx);
  return (ctx->resume_status == 1) ? 0 : -2;
}

EFI_STATUS GuS3PrepareAndRegister(EFI_SYSTEM_TABLE *t) {
  if (!t || !t->BootServices) return EFI_INVALID_PARAMETER;
  if (!Gu.bar_base) return EFI_NOT_READY;

  /* 1. Allocate 64 pages (256 KB) of EfiACPIMemoryNVS.
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
    Gu.s3_registered = FALSE;
    Gu.s3_status = alloc_status;
    return alloc_status;
  }

  UINT8 *nvs_base = (UINT8 *)(UINTN)nvs_phys;
  memset(nvs_base, 0, 64 * 4096);

  /* Allocate 1 low page (<1MB) of EfiACPIMemoryNVS for S3 Resume Trampoline */
  EFI_PHYSICAL_ADDRESS low_tramp_phys = 0x0009FFFFULL;
  EFI_STATUS tramp_alloc = t->BootServices->AllocatePages(
      AllocateMaxAddress,
      EfiACPIMemoryNVS,
      1,
      &low_tramp_phys
  );
  if (EFI_ERROR(tramp_alloc) || !low_tramp_phys) {
    low_tramp_phys = 0x000FFFFFULL;
    tramp_alloc = t->BootServices->AllocatePages(
        AllocateMaxAddress,
        EfiACPIMemoryNVS,
        1,
        &low_tramp_phys
    );
  }
  if (EFI_ERROR(tramp_alloc) || !low_tramp_phys) {
    GuLog("GuS3PrepareAndRegister: Failed to allocate low (<1MB) ACPI NVS page (%#llx)\n",
          (unsigned long long)tramp_alloc);
    t->BootServices->FreePages(nvs_phys, 64);
    Gu.s3_registered = FALSE;
    Gu.s3_status = tramp_alloc;
    return tramp_alloc;
  }

  UINT8 *low_tramp_base = (UINT8 *)(UINTN)low_tramp_phys;

  GU_S3_RESUME_CONTEXT *ctx = (GU_S3_RESUME_CONTEXT *)nvs_base;
  ctx->magic = GU_S3_CONTEXT_MAGIC;
  ctx->version = 1;
  ctx->bar_base = Gu.bar_base;
  ctx->pci_id = Gu.device_id;
  ctx->is_ampere = Gu.is_ampere;
  ctx->is_turing = Gu.is_turing;
  ctx->val_ss0 = 0x88888888U;
  ctx->val_ss1 = 0x00000008U;

  /* Resolve PCI BDF location to configure Command register upon S3 wake */
  if (Gu.pci && Gu.pci->GetLocation) {
    UINTN seg = 0, bus = 0, dev = 0, fun = 0;
    EFI_STATUS s_loc = Gu.pci->GetLocation(Gu.pci, &seg, &bus, &dev, &fun);
    if (!EFI_ERROR(s_loc)) {
      ctx->segment = (UINT16)seg;
      ctx->bus = (UINT8)bus;
      ctx->device = (UINT8)dev;
      ctx->function = (UINT8)fun;
      ctx->pci_cmd_port_addr = 0x80000004U |
                               ((UINT32)bus << 16) |
                               ((UINT32)dev << 11) |
                               ((UINT32)fun << 8);
    }
  }

  if (Gu.s3_arena_armed && Gu.s3_meta_dma && Gu.s3_booter_dma) {
    /*
     * S3 Persistence Mode (v1.8.65):
     * All 6 GPU-DMA buffers (radix table, GSP image, FWSEC image, booter, V67 payload,
     * boot image, and pristine WPR metadata) are consolidated in the single contiguous
     * EfiReservedMemoryType Arena (<4GB). They retain their cold-boot DMA addresses
     * directly without copying to ACPI NVS or pointer retargeting.
     */
    ctx->booter_dma = Gu.s3_booter_dma;
    ctx->booter_size = Gu.s3_booter_size;
    ctx->v67_dma = Gu.s3_v67_dma;
    ctx->v67_size = GU_V67_SIZE;
    ctx->boot_dma = Gu.s3_boot_dma;
    ctx->boot_size = Gu.s3_boot_size;
    ctx->meta_dma = Gu.s3_meta_dma;
    ctx->meta_size = 256;
    GuLog("GuS3PrepareAndRegister: 6 GPU-DMA buffers consolidated in Arena (EfiReservedMemoryType):\n"
          "  booter_dma=%#llx (%u bytes), v67_dma=%#llx, boot_dma=%#llx (%u bytes), meta_dma=%#llx\n",
          (unsigned long long)ctx->booter_dma, ctx->booter_size,
          (unsigned long long)ctx->v67_dma,
          (unsigned long long)ctx->boot_dma, ctx->boot_size,
          (unsigned long long)ctx->meta_dma);
    if (Gu.s3_fwsec_dma) {
      GuLog("GuS3PrepareAndRegister: Persistent FWSEC: dma=%#llx sz=%#x (EfiReservedMemoryType)\n",
            (unsigned long long)Gu.s3_fwsec_dma, (unsigned)Gu.s3_fwsec_size);
    }
  } else {
    /* Legacy NVS staging fallback (for non-arena / Turing / test environments) */
    ctx->booter_dma = nvs_phys + 0x01000;
    ctx->v67_dma = nvs_phys + 0x11000;
    ctx->boot_dma = nvs_phys + 0x21000;
    ctx->meta_dma = nvs_phys + 0x2A000;

    if (GuAssets[0].firmware.data && GuAssets[0].firmware.size) {
      const UINT8 *bdata = (const UINT8 *)GuAssets[0].firmware.data;
      UINTN bsize = GuAssets[0].firmware.size;
      if (bsize >= 72 && *(const UINT32 *)bdata == GU_BOOTER_CONTAINER_MAGIC) {
        /* BooterLoad container: unpack 72-byte header, extract executable image and patch fuse signature */
        struct gpu_unlock_booter_hdr hdr;
        memcpy(&hdr, bdata + 8, sizeof(hdr));
        UINT32 image_size = *(const UINT32 *)(bdata + 8 + sizeof(hdr));
        UINT32 sig_bytes = *(const UINT32 *)(bdata + 12 + sizeof(hdr));

        if (image_size && image_size <= 65536 - 4096 &&
            72 + image_size + sig_bytes <= bsize) {
          memcpy(nvs_base + 0x01000, bdata + 72, image_size);
          ctx->booter_size = image_size;

          /* Resolve fuse version: use cold-boot sampled version if available, or read GPU BAR0 */
          UINT32 fuse_ver = Gu.fuse_version ? Gu.fuse_version : 1;
          if (!Gu.fuse_version && !Gu.is_turing && Gu.bar_base) {
            UINT32 index = (hdr.ucode_id > 0) ? (hdr.ucode_id - 1) : 0;
            UINT32 fuse_raw = *(volatile UINT32 *)(UINTN)(Gu.bar_base + GU_FUSE_SEC2_UCODE1 + index * 4);
            if (fuse_raw != 0xFFFFFFFFU && (fuse_raw & 0xFFFF0000U) != 0xBADF0000U) {
              fuse_ver = gpu_unlock_fuse_version(fuse_raw);
            }
          }
          gpu_unlock_booter_patch(nvs_base + 0x01000, image_size, &hdr,
                                  bdata + 72 + image_size, sig_bytes, fuse_ver);
        } else {
          ctx->booter_size = (UINT32)bsize;
          memcpy(nvs_base + 0x01000, bdata, ctx->booter_size);
        }
      } else {
        ctx->booter_size = (UINT32)bsize;
        memcpy(nvs_base + 0x01000, bdata, ctx->booter_size);
      }
    }
    if (GuAssets[1].firmware.data && GuAssets[1].firmware.size) {
      ctx->v67_size = (UINT32)GuAssets[1].firmware.size;
      memcpy(nvs_base + 0x11000, GuAssets[1].firmware.data, ctx->v67_size);
    }
    if (GuAssets[3].firmware.data && GuAssets[3].firmware.size) {
      ctx->boot_size = (UINT32)GuAssets[3].firmware.size;
      memcpy(nvs_base + 0x21000, GuAssets[3].firmware.data, ctx->boot_size);
    }
    if (Gu.wpr_meta_valid) {
      ctx->meta_size = 256;
      memcpy(nvs_base + 0x2A000, Gu.wpr_meta, 256);
      /* Re-target internal DMA buffer pointers inside the persistent WPR metadata:
       * +32: boot_dma (8 bytes) -> nvs_phys + 0x21000
       * +72: signature_dma (8 bytes) -> nvs_phys + 0x11000
       * +80: signature_size (8 bytes) -> 64000
       */
      UINT8 *meta_pkt = nvs_base + 0x2A000;
      UINT64 boot_dma_val = (UINT64)(nvs_phys + 0x21000);
      UINT64 sig_dma_val = (UINT64)(nvs_phys + 0x11000);
      UINT64 sig_size_val = 64000ULL;
      memcpy(meta_pkt + 32, &boot_dma_val, sizeof(UINT64));
      memcpy(meta_pkt + 72, &sig_dma_val, sizeof(UINT64));
      memcpy(meta_pkt + 80, &sig_size_val, sizeof(UINT64));

      UINT64 radix_dma_val = 0;
      memcpy(&radix_dma_val, meta_pkt + 16, sizeof(UINT64));
      UINT64 img_size_val = 0;
      memcpy(&img_size_val, meta_pkt + 24, sizeof(UINT64));
      GuLog("GuS3PrepareAndRegister: Persistent WPR Meta: radix_dma=%#llx (%s) img_sz=%#llx boot_dma=%#llx sig_dma=%#llx\n",
            (unsigned long long)radix_dma_val,
            Gu.s3_persist_armed ? "EfiReservedMemoryType" : "EfiBootServicesData",
            (unsigned long long)img_size_val,
            (unsigned long long)boot_dma_val,
            (unsigned long long)sig_dma_val);
      if (Gu.s3_fwsec_dma) {
        GuLog("GuS3PrepareAndRegister: Persistent FWSEC: dma=%#llx sz=%#x (EfiReservedMemoryType)\n",
              (unsigned long long)Gu.s3_fwsec_dma, (unsigned)Gu.s3_fwsec_size);
      }
    } else if (GuAssets[4].firmware.data && GuAssets[4].firmware.size) {
      ctx->meta_size = (UINT32)GuAssets[4].firmware.size;
      memcpy(nvs_base + 0x2A000, GuAssets[4].firmware.data, ctx->meta_size);
      /* Ensure WPR magic is populated for AD102 even in fallback */
      UINT64 wpr_magic = 0xdc3aae21371a60b3ULL;
      memcpy(nvs_base + 0x2A000, &wpr_magic, sizeof(UINT64));
    }
  }

  /* Copy the self-contained GuS3ResumeDispatch PIC machine code into the ACPI NVS buffer.
   * This guarantees that the executable code survives Windows ExitBootServices() and memory
   * reclamation even if EfiBootServicesCode / EfiLoaderCode is overwritten by the OS. */
  memcpy(nvs_base + 0x00800, (const void *)(uintptr_t)GuS3ResumeDispatch, 0x800);

  /* 2. Tier 1: Try standard UEFI S3 Save State Protocol */
  static EFI_GUID S3SaveStateGuid = EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  static EFI_GUID BootScriptSaveGuid = EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID;
  EFI_S3_SAVE_STATE_PROTOCOL *s3 = NULL;
  EFI_STATUS s3_status = t->BootServices->LocateProtocol(&S3SaveStateGuid, NULL, (VOID **)&s3);
  if (EFI_ERROR(s3_status) || !s3 || !s3->Write) {
    s3_status = t->BootServices->LocateProtocol(&BootScriptSaveGuid, NULL, (VOID **)&s3);
  }

  BOOLEAN boot_script_ok = FALSE;
  if (!EFI_ERROR(s3_status) && s3 && s3->Write) {
    EFI_PHYSICAL_ADDRESS dispatch_entry = nvs_phys + 0x00800;
    EFI_STATUS s_dispatch = s3->Write(s3, EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE, dispatch_entry, nvs_phys);
    if (EFI_ERROR(s_dispatch)) {
      s_dispatch = s3->Write(s3, EFI_BOOT_SCRIPT_DISPATCH_OPCODE, dispatch_entry);
    }

    UINT32 val_ss0 = ctx->val_ss0;
    UINT32 val_ss1 = ctx->val_ss1;
    UINT64 addr_ss0 = (UINT64)ctx->bar_base + 0x0082381cULL;
    UINT64 addr_ss1 = (UINT64)ctx->bar_base + 0x00823820ULL;
    EFI_STATUS s_mmio0 = s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, addr_ss0, (UINTN)1, &val_ss0);
    EFI_STATUS s_mmio1 = s3->Write(s3, EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE, EfiBootScriptWidthUint32, addr_ss1, (UINTN)1, &val_ss1);

    if (!EFI_ERROR(s_dispatch) || (!EFI_ERROR(s_mmio0) && !EFI_ERROR(s_mmio1))) {
      boot_script_ok = TRUE;
      GuLog("S3 Persistence: Registered via UEFI Boot Script Protocol (NVS=%#llx Dispatch=%#llx)\n",
            (unsigned long long)nvs_phys, (unsigned long long)s_dispatch);
      Gu.s3_registered = TRUE;
      Gu.s3_status = EFI_SUCCESS;
      return EFI_SUCCESS;
    } else {
      GuLog("S3 Persistence: UEFI Boot Script Write locked/rejected (%#llx). SMM LockBox active.\n",
            (unsigned long long)s_dispatch);
    }
  }

  /* 3. Tier 2: Universal ACPI FACS Shadow Trampoline Intercept
   * Engages when SMM LockBox is sealed before BDS (e.g. AMI Aptio V on AMD AM4/AM5).
   * Rewrites FADT->FirmwareCtrl to Shadow FACS and hooks Real FACS->FirmwareWakingVector. */
  GuLog("S3 Persistence: Engaging Universal ACPI FACS Shadow Trampoline Intercept...\n");
  GU_ACPI_FACS_INFO facs_info;
  EFI_STATUS facs_status = GuAcpiInstallShadowFacs(t, ctx, nvs_base, nvs_phys, low_tramp_base, low_tramp_phys, &facs_info);
  if (!EFI_ERROR(facs_status) && facs_info.found) {
    GuLog("S3 Persistence: Universal ACPI FACS Trampoline armed successfully!\n");
    Gu.s3_registered = TRUE;
    Gu.s3_status = EFI_SUCCESS;
    return EFI_SUCCESS;
  }

  GuLog("GuS3PrepareAndRegister: ACPI FACS Trampoline installation failed (%#llx)\n",
        (unsigned long long)facs_status);
  Gu.s3_registered = FALSE;
  Gu.s3_status = facs_status;
  return facs_status;
}
