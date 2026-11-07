/* SPDX-License-Identifier: GPL-2.0-only */
#define GU_DISCOVERY_HOST_TEST 1
#include "gpu_discovery_mock.h"
#include "../src/gpu_inventory.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

typedef struct {
  EFI_PCI_IO_PROTOCOL protocol;
  UINT32 dwords[64];
  UINTN segment, bus, device, function;
  int read_fail, location_fail, mutate_at_full_read;
} MOCK_DEVICE;
static MOCK_DEVICE gpu[4];
static EFI_HANDLE handles[4];
static size_t count;
static unsigned init_calls, free_calls, log_calls, resolve_calls;
static int fail_pool_free, redirect_on_refresh;
GU_PLATFORM Gu;

void GuInit(EFI_BOOT_SERVICES *bs, EFI_PCI_IO_PROTOCOL *pci) {
  init_calls++; memset(&Gu, 0, sizeof(Gu)); Gu.bs = bs; Gu.pci = pci;
}
void GuLog(const char *format, ...) { (void)format; log_calls++; }
int GuIsSupportedPciId(UINT32 pci) {
  return pci == 0x268410deU || pci == 0x220a10deU;
}
int GuIsAmpereDeviceId(UINT16 dev) { return dev == 0x220aU; }
int GuIsTuringDeviceId(UINT16 dev) { (void)dev; return 0; }
int GuCheckConfig(const UINT32 c[64], UINTN seg, UINTN bus, UINTN dev, UINTN fun, GU_CAPS *caps) {
  if (!c || !caps || seg != 0 || bus > 255 || dev > 31 || fun > 7 ||
      !GuIsSupportedPciId(c[0]) || (c[2] >> 16) != 0x0300) return 0;
  memset(caps, 0, sizeof(*caps)); caps->pm = 0x40; caps->pcie = 0x60;
  return 1;
}
static EFI_STATUS Read(EFI_PCI_IO_PROTOCOL *io, int width, UINT32 offset, UINTN n, VOID *out) {
  MOCK_DEVICE *m = (MOCK_DEVICE *)io->context;
  if (m->read_fail || width != EfiPciIoWidthUint32 || !out ||
      (offset & 3) || offset / 4 + n > 64) return 3;
  memcpy(out, m->dwords + offset / 4, n * sizeof(UINT32));
  if (m->mutate_at_full_read && n == 64)
    ((UINT32 *)out)[0x2c / 4] ^= 1U;
  return EFI_SUCCESS;
}
static EFI_STATUS GetLocation(EFI_PCI_IO_PROTOCOL *io, UINTN *seg, UINTN *bus, UINTN *dev, UINTN *fun) {
  MOCK_DEVICE *m = (MOCK_DEVICE *)io->context;
  if (m->location_fail) return 3;
  *seg = m->segment; *bus = m->bus; *dev = m->device; *fun = m->function;
  return EFI_SUCCESS;
}
static EFI_STATUS Locate(int type, EFI_GUID *guid, VOID *key, UINTN *outCount, EFI_HANDLE **outHandles) {
  (void)type; (void)guid; (void)key;
  *outCount = count; *outHandles = handles; return EFI_SUCCESS;
}
static EFI_STATUS Resolve(EFI_HANDLE h, EFI_GUID *guid, VOID **io) {
  (void)guid; resolve_calls++;
  *io = (redirect_on_refresh && resolve_calls > count) ? handles[1] : h;
  return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {
  free_calls++; if (p != handles || fail_pool_free) return 3;
  return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES services = { Locate, Resolve, Free };
static EFI_SYSTEM_TABLE system_table = { &services };
static void Populate(size_t i, GU_GPU_IDENTITY id) {
  MOCK_DEVICE *m = &gpu[i]; memset(m, 0, sizeof(*m));
  m->protocol.GetLocation = GetLocation; m->protocol.Pci.Read = Read;
  m->protocol.context = m;
  m->dwords[0] = id.pci_id;
  m->dwords[2] = 0x03000000U;
  m->dwords[0x2c / 4] = id.subsystem_id;
  m->dwords[4] = 0xfb000000U + (UINT32)(i * 0x1000000U);
  m->segment = id.segment; m->bus = id.bus;
  m->device = id.device; m->function = id.function;
  handles[i] = &m->protocol;
}
static void ResetPair(void) {
  memset(&Gu, 0, sizeof(Gu)); init_calls = free_calls = log_calls = 0;
  fail_pool_free = redirect_on_refresh = 0; resolve_calls = 0; count = 2;
  Populate(0, GuPinnedAd102); Populate(1, GuPinnedGa102);
}
#include "../src/gpu_discovery.c"

int main(void) {
  EFI_STATUS status;
  ResetPair();
  status = GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table);
  CHECK(status == EFI_UNSUPPORTED && init_calls == 0 && free_calls == 1 && log_calls >= 3);

  ResetPair();
  Populate(0, GuPinnedGa102); Populate(1, GuPinnedAd102);
  status = GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table);
  CHECK(status == EFI_UNSUPPORTED && init_calls == 0 && free_calls == 1);

  ResetPair(); gpu[1].dwords[0x2c / 4] ^= 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); gpu[1].bus = gpu[0].bus;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); gpu[0].read_fail = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); gpu[0].location_fail = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); fail_pool_free = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 3;
  Populate(2, GuPinnedAd102); gpu[2].bus = 6;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_SUCCESS && init_calls == 1);
  CHECK(Gu.handle == handles[0] && Gu.device_id == 0x2684 && !Gu.is_ampere);

  ResetPair(); count = 1; gpu[0].mutate_at_full_read = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); count = 1; redirect_on_refresh = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 1; gpu[0].dwords[0] = 0x123410deU;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 2; gpu[1].dwords[0] = 0x164e1002U;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_SUCCESS && init_calls == 1);
  CHECK(Gu.handle == handles[0]);

  printf("PASS UEFI discovery adapter: %u checks; mocked protocols only\n", checks);
  return 0;
}
