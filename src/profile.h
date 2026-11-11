/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include "vendor/vbios_610.h"
BOOLEAN GuAssetsValid(VOID);
BOOLEAN GuPromPlausible(const VOID *, UINTN);
BOOLEAN GuFwsecMatches(const struct gpu_unlock_fwsec *);
/* Prepare is read-only on the GPU. It validates ROM, assets, exact PCI/card
 * state and creates the immutable selector latch. No driver release here. */
EFI_STATUS GuPrepare(VOID);
BOOLEAN GuColdEntry(VOID);
EFI_STATUS GuDiagnosticScan(VOID);

typedef struct {
  const CHAR16 *path;
  const char   *name;
  const char   *hash;
  UINTN         bytes;
} GU_ASSET_ENTRY;

#define GU_ASSET_COUNT 5

typedef struct {
  const char     *arch_name;
  const CHAR16   *alt_gsp_path;
  GU_ASSET_ENTRY  entries[GU_ASSET_COUNT];
} GU_ARCH_ASSETS;

const GU_ARCH_ASSETS *GuGetArchAssets(VOID);

typedef struct {
  UINTN   rom_offset;
  UINTN   rom_bytes;
  UINTN   payload_offset;
  UINTN   payload_bytes;
  UINT16  machine_type;
  UINT16  subsystem;
  BOOLEAN compressed;
} GU_VBIOS_EFI_IMAGE;

BOOLEAN GuFindVbiosEfiOptionRom(const UINT8 *rom, UINTN length, GU_VBIOS_EFI_IMAGE *out);
BOOLEAN GuGetStagedVbiosEfiImage(const UINT8 **payload, UINTN *bytes, UINT16 *machine_type, BOOLEAN *compressed);
VOID GuReleaseStagedVbiosEfiImage(VOID);

/* Legacy compatibility wrappers for vendor/ code */
BOOLEAN GuAd104AssetsValid(VOID);
BOOLEAN GuAd104PromPlausible(const VOID *rom, UINTN sz);
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *fw);
EFI_STATUS GuAd104Prepare(VOID);
BOOLEAN GuAd104ColdEntry(VOID);
