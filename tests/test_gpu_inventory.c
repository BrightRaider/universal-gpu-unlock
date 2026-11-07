/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/gpu_inventory.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #c); return 1; } } while (0)

typedef struct {
  GU_GPU_RECORD rows[GU_INVENTORY_MAX_NVIDIA + 2];
  size_t count;
  size_t reads;
  size_t fail_at;
} MOCK;

static int Reader(void *ctx, size_t index, GU_GPU_RECORD *out) {
  MOCK *m = (MOCK *)ctx;
  m->reads++;
  if (index >= m->count || index == m->fail_at) return -1;
  *out = m->rows[index];
  return 0;
}
static GU_GPU_RECORD Record(GU_GPU_IDENTITY id, uintptr_t token) {
  GU_GPU_RECORD r;
  memset(&r, 0, sizeof(r));
  r.pci_id = id.pci_id;
  r.subsystem_id = id.subsystem_id;
  r.class_code = 0x0300;
  r.segment = id.segment; r.bus = id.bus;
  r.device = id.device; r.function = id.function;
  r.handle = (void *)(token * 2U + 1U);
  r.pci = (void *)(token * 2U + 2U);
  return r;
}
static void Pair(MOCK *m) {
  memset(m, 0, sizeof(*m));
  m->count = 2;
  m->fail_at = (size_t)-1;
  m->rows[0] = Record(GuPinnedAd102, 1);
  m->rows[1] = Record(GuPinnedGa102, 2);
}
static int ExpectRefusal(MOCK *m, int status) {
  GU_GPU_INVENTORY inventory;
  int actual = GuCollectGpuInventory(m->count, Reader, m, &inventory);
  CHECK(actual == status);
  CHECK(inventory.count == 0);
  return 0;
}
int main(void) {
  MOCK m;
  GU_GPU_INVENTORY inventory;
  GU_GPU_IDENTITY wrong;
  GU_GPU_RECORD swap;
  size_t ad = 99, ga = 99, selected = 99, i;

  Pair(&m);
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(m.reads == 2 && inventory.count == 2);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_OK);
  CHECK(ad == 0 && ga == 1);
  CHECK(GuFindExactGpu(&inventory, &GuPinnedGa102, &selected) == GU_INVENTORY_OK && selected == 1);
  swap = m.rows[0]; m.rows[0] = m.rows[1]; m.rows[1] = swap;
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_OK && ad == 1 && ga == 0);

  Pair(&m); m.rows[1].subsystem_id ^= 1;
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_NOT_FOUND);
  CHECK(GuFindExactGpu(&inventory, &GuPinnedGa102, &selected) == GU_INVENTORY_NOT_FOUND && selected == 0);

  Pair(&m); m.rows[1].bus = 5;
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_NOT_FOUND);

  Pair(&m); m.rows[1].pci_id = 0x221610deU;
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_NOT_FOUND);

  Pair(&m); m.rows[1].bus = m.rows[0].bus;
  CHECK(ExpectRefusal(&m, GU_INVENTORY_DUPLICATE) == 0);
  Pair(&m); m.rows[1].handle = m.rows[0].handle;
  CHECK(ExpectRefusal(&m, GU_INVENTORY_DUPLICATE) == 0);
  Pair(&m); m.rows[1].pci = m.rows[0].pci;
  CHECK(ExpectRefusal(&m, GU_INVENTORY_DUPLICATE) == 0);

  Pair(&m); m.rows[1].device = 32;
  CHECK(ExpectRefusal(&m, GU_INVENTORY_READ_FAILED) == 0);
  Pair(&m); m.rows[1].handle = NULL;
  CHECK(ExpectRefusal(&m, GU_INVENTORY_READ_FAILED) == 0);
  Pair(&m); m.fail_at = 1;
  CHECK(ExpectRefusal(&m, GU_INVENTORY_READ_FAILED) == 0);
  CHECK(m.reads == 2);

  Pair(&m); m.count = 3;
  wrong = GuPinnedAd102; wrong.pci_id = 0x270210deU; wrong.bus = 6;
  m.rows[2] = Record(wrong, 3);
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(inventory.count == 3);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_NOT_FOUND);

  Pair(&m); m.count = 3;
  wrong = GuPinnedGa102; wrong.pci_id = 0x164e1002U; wrong.bus = 7;
  m.rows[2] = Record(wrong, 3);
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(inventory.count == 2);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_OK);

  Pair(&m); m.count = 3;
  wrong = GuPinnedGa102; wrong.pci_id = 0x10f010deU; wrong.bus = 7;
  m.rows[2] = Record(wrong, 3); m.rows[2].class_code = 0x0403;
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(inventory.count == 2);

  Pair(&m); m.count = 1;
  CHECK(GuCollectGpuInventory(m.count, Reader, &m, &inventory) == GU_INVENTORY_OK);
  CHECK(inventory.count == 1);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ga) == GU_INVENTORY_NOT_FOUND);

  Pair(&m); m.count = GU_INVENTORY_MAX_NVIDIA + 1;
  for (i = 0; i < m.count; i++) {
    wrong = GuPinnedAd102; wrong.bus = (uint32_t)(i + 1);
    m.rows[i] = Record(wrong, (uintptr_t)i + 1U);
  }
  CHECK(ExpectRefusal(&m, GU_INVENTORY_TOO_MANY) == 0);

  Pair(&m);
  CHECK(GuCollectGpuInventory(0, Reader, &m, &inventory) == GU_INVENTORY_INVALID);
  CHECK(inventory.count == 0);
  CHECK(GuCollectGpuInventory(GU_INVENTORY_MAX_HANDLES + 1, Reader, &m, &inventory) == GU_INVENTORY_INVALID);
  CHECK(GuCollectGpuInventory(2, NULL, &m, &inventory) == GU_INVENTORY_INVALID);
  CHECK(GuCollectGpuInventory(2, Reader, &m, NULL) == GU_INVENTORY_INVALID);
  CHECK(GuFindExactGpu(NULL, &GuPinnedAd102, &selected) == GU_INVENTORY_INVALID);
  CHECK(GuFindExactGpu(&inventory, &GuPinnedAd102, NULL) == GU_INVENTORY_INVALID);
  CHECK(GuFindPinnedGpuPair(&inventory, &ad, &ad) == GU_INVENTORY_INVALID);

  /* A passing result must depend on both actual independent pins. */
  CHECK(GuPinnedAd102.pci_id != GuPinnedGa102.pci_id);
  CHECK(GuPinnedAd102.subsystem_id != GuPinnedGa102.subsystem_id);
  printf("PASS GPU inventory: %u checks; mock reads only, no GPU/EFI execution\n", checks);
  return 0;
}
