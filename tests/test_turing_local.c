/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int      BOOLEAN;
#define TRUE  1
#define FALSE 0

#include "../src/gpu_device_ids.h"

#define GU_DEV_MATCH(id) (dev == (id)) ||

static inline BOOLEAN GuIsAdaDeviceId(UINT16 dev) {
  return (GU_ADA_DEVICE_IDS(GU_DEV_MATCH) FALSE);
}
static inline BOOLEAN GuIsAmpereDeviceId(UINT16 dev) {
  return (GU_AMPERE_DEVICE_IDS(GU_DEV_MATCH) FALSE);
}
static inline BOOLEAN GuIsTuringDeviceId(UINT16 dev) {
  return (GU_TURING_DEVICE_IDS(GU_DEV_MATCH) FALSE);
}
static inline BOOLEAN GuIsSupportedDeviceId(UINT16 dev) {
  return GuIsAdaDeviceId(dev) || GuIsAmpereDeviceId(dev) || GuIsTuringDeviceId(dev);
}
static inline BOOLEAN GuIsSupportedPciId(UINT32 id) {
  if ((id & 0xffffU) != 0x10deU) return FALSE;
  return GuIsSupportedDeviceId((UINT16)(id >> 16));
}

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
      return 1; \
    } \
  } while (0)

static long CheckFileSize(const char *rel_path) {
  char full_path[512];
  const char *repo_env = getenv("GU_REPO_ROOT");
  if (repo_env) {
    snprintf(full_path, sizeof(full_path), "%s/%s", repo_env, rel_path);
    FILE *f = fopen(full_path, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fclose(f);
      return sz;
    }
  }
  FILE *f = fopen(rel_path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fclose(f);
  return sz;
}

int main(void) {
  printf("=== Turing (TU102/TU104/TU106) Local Architecture & Asset Test ===\n");

  /* 1. Device ID Identification Tests */
  printf("[1] Testing Turing Device IDs...\n");
  CHECK(GuIsTuringDeviceId(0x1e04U), "RTX 2080 Ti (1e04) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1e07U), "RTX 2080 Ti OEM (1e07) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1e02U), "TITAN RTX (1e02) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1e81U), "RTX 2080 SUPER (1e81) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1e82U), "RTX 2080 (1e82) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1e84U), "RTX 2070 SUPER (1e84) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1f02U), "RTX 2070 (1f02) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1f06U), "RTX 2060 SUPER (1f06) must match Turing");
  CHECK(GuIsTuringDeviceId(0x1f08U), "RTX 2060 (1f08) must match Turing");

  /* Non-Turing negative tests */
  CHECK(!GuIsTuringDeviceId(0x2684U), "RTX 4090 must NOT match Turing");
  CHECK(!GuIsTuringDeviceId(0x2206U), "RTX 3080 must NOT match Turing");
  CHECK(!GuIsTuringDeviceId(0x1234U), "Unknown device must NOT match Turing");

  /* Full PCI ID matching */
  CHECK(GuIsSupportedPciId(0x1e0410deU), "RTX 2080 Ti 0x1e0410de must be supported");
  CHECK(GuIsSupportedPciId(0x268410deU), "RTX 4090 0x268410de must be supported");
  CHECK(GuIsSupportedPciId(0x220610deU), "RTX 3080 0x220610de must be supported");
  CHECK(!GuIsSupportedPciId(0x1e041002U), "Non-NVIDIA vendor must be rejected");

  /* 2. TU102 Asset Verification */
  printf("[2] Verifying TU102-ASSETS directory and payload files...\n");
  long sz_booter = CheckFileSize("TU102-ASSETS/booter_load_tu102.bin");
  long sz_desc   = CheckFileSize("TU102-ASSETS/gsp_boot_desc_tu102.bin");
  long sz_boot   = CheckFileSize("TU102-ASSETS/gsp_boot_tu102.bin");
  long sz_v67    = CheckFileSize("TU102-ASSETS/v67-tu102-610.43.03.bin");
  long sz_gsp    = CheckFileSize("TU102-ASSETS/gsp_tu10x.bin");

  CHECK(sz_booter == 59976L, "booter_load_tu102.bin must be exactly 59976 bytes");
  CHECK(sz_desc == 76L,      "gsp_boot_desc_tu102.bin must be exactly 76 bytes");
  CHECK(sz_boot == 4096L,    "gsp_boot_tu102.bin must be exactly 4096 bytes");
  CHECK(sz_v67 == 64000L,    "v67-tu102-610.43.03.bin must be exactly 64000 bytes");
  CHECK(sz_gsp > 20000000L,  "gsp_tu10x.bin must exist and be > 20 MB");

  printf("  -> booter_load_tu102.bin   : %ld bytes [OK]\n", sz_booter);
  printf("  -> gsp_boot_desc_tu102.bin : %ld bytes [OK]\n", sz_desc);
  printf("  -> gsp_boot_tu102.bin      : %ld bytes [OK]\n", sz_boot);
  printf("  -> v67-tu102-610.43.03.bin : %ld bytes [OK]\n", sz_v67);
  printf("  -> gsp_tu10x.bin           : %ld bytes [OK]\n", sz_gsp);

  printf("=== ALL TURING ARCHITECTURE & ASSET CHECKS PASSED [OK] ===\n");
  return 0;
}
