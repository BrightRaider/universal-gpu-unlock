/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_inventory.h"
#include <string.h>

const GU_GPU_IDENTITY GuPinnedAd102 = {
  0x268410deU, 0x88e61043U, 0U, 1U, 0U, 0U
};
const GU_GPU_IDENTITY GuPinnedGa102 = {
  0x220a10deU, 0xb61219daU, 0U, 4U, 0U, 0U
};

static int SameLocation(const GU_GPU_RECORD *a, const GU_GPU_RECORD *b) {
  return a->segment == b->segment && a->bus == b->bus &&
         a->device == b->device && a->function == b->function;
}

int GuCollectGpuInventory(size_t handle_count, GU_GPU_READER reader,
                          void *context, GU_GPU_INVENTORY *inventory) {
  size_t i, j;
  GU_GPU_RECORD record;
  if (!inventory) return GU_INVENTORY_INVALID;
  memset(inventory, 0, sizeof(*inventory));
  if (!reader || !handle_count || handle_count > GU_INVENTORY_MAX_HANDLES)
    return GU_INVENTORY_INVALID;
  for (i = 0; i < handle_count; i++) {
    memset(&record, 0, sizeof(record));
    if (reader(context, i, &record)) goto read_failed;
    if (!record.handle || !record.pci) goto read_failed;
    if ((record.pci_id & 0xffffU) != 0x10deU ||
        (record.class_code != 0x0300U && record.class_code != 0x0302U))
      continue;
    if (record.segment > 0xffffU || record.bus > 255U ||
        record.device > 31U || record.function > 7U)
      goto read_failed;
    for (j = 0; j < inventory->count; j++) {
      const GU_GPU_RECORD *prior = &inventory->nvidia[j];
      if (SameLocation(prior, &record) || prior->handle == record.handle ||
          prior->pci == record.pci) {
        memset(inventory, 0, sizeof(*inventory));
        return GU_INVENTORY_DUPLICATE;
      }
    }
    if (inventory->count == GU_INVENTORY_MAX_NVIDIA) {
      memset(inventory, 0, sizeof(*inventory));
      return GU_INVENTORY_TOO_MANY;
    }
    inventory->nvidia[inventory->count++] = record;
  }
  return GU_INVENTORY_OK;
read_failed:
  memset(inventory, 0, sizeof(*inventory));
  return GU_INVENTORY_READ_FAILED;
}

int GuFindExactGpu(const GU_GPU_INVENTORY *inventory,
                   const GU_GPU_IDENTITY *expected, size_t *index) {
  size_t i;
  if (index) *index = 0;
  if (!inventory || !expected || !index ||
      inventory->count > GU_INVENTORY_MAX_NVIDIA)
    return GU_INVENTORY_INVALID;
  for (i = 0; i < inventory->count; i++) {
    const GU_GPU_RECORD *p = &inventory->nvidia[i];
    if (p->pci_id == expected->pci_id &&
        p->subsystem_id == expected->subsystem_id &&
        p->segment == expected->segment && p->bus == expected->bus &&
        p->device == expected->device && p->function == expected->function) {
      *index = i;
      return GU_INVENTORY_OK;
    }
  }
  return GU_INVENTORY_NOT_FOUND;
}

int GuFindPinnedGpuPair(const GU_GPU_INVENTORY *inventory,
                        size_t *ad102_index, size_t *ga102_index) {
  size_t ad, ga;
  if (ad102_index) *ad102_index = 0;
  if (ga102_index) *ga102_index = 0;
  if (!inventory || !ad102_index || !ga102_index ||
      ad102_index == ga102_index)
    return GU_INVENTORY_INVALID;
  if (inventory->count != 2 ||
      GuFindExactGpu(inventory, &GuPinnedAd102, &ad) != GU_INVENTORY_OK ||
      GuFindExactGpu(inventory, &GuPinnedGa102, &ga) != GU_INVENTORY_OK ||
      ad == ga)
    return GU_INVENTORY_NOT_FOUND;
  *ad102_index = ad;
  *ga102_index = ga;
  return GU_INVENTORY_OK;
}
