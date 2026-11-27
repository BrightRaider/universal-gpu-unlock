/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MSVC host test for non-identity PciIo->Map() S3 arena conversion (refs #25).
 * Verifies:
 *  1) GuDmaAllocate tracks non-identity arena IOVAs and logs once instead of spamming 20k+ lines.
 *  2) s3_repack_arena_physical repacks all radix tables, WPR metadata, and Gu.s3_*_dma
 *     to <4 GB CPU-physical arena addresses after FLR quiescence.
 *  3) free_coherent unmaps every original DMA mapping cleanly (using work->s3_meta_dma)
 *     while preserving the EfiReservedMemoryType arena and its repacked contents.
 */
#define GU_S3_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#undef NULL
#undef EPROTO
#undef EBADMSG
#undef EOPNOTSUPP
#undef ETIMEDOUT
#undef EOVERFLOW
#undef EALREADY

#ifdef _WIN32
__declspec(dllimport) void * __stdcall VirtualAlloc(void *lpAddress, size_t dwSize,
						    unsigned long flAllocationType,
						    unsigned long flProtect);
__declspec(dllimport) int __stdcall VirtualFree(void *lpAddress, size_t dwSize,
						  unsigned long dwFreeType);
#define TEST_MEM_COMMIT   0x00001000UL
#define TEST_MEM_RESERVE  0x00002000UL
#define TEST_MEM_RELEASE  0x00008000UL
#define TEST_PAGE_RW      0x04UL
#endif

#include "../src/vendor/unlock_610.c"

static int g_failures = 0;
static unsigned int g_checks = 0;
#define CHECK(cond) do { \
	g_checks++; \
	if (!(cond)) { \
		printf("FAIL line %d: %s\n", __LINE__, #cond); \
		g_failures++; \
	} \
} while (0)

static char g_log_buf[32768];
static size_t g_log_len = 0;
static int g_warning_count = 0;

VOID GuLog(const char *Format, ...) {
	char line[1024];
	va_list ap;
	int n;

	va_start(ap, Format);
	n = vsnprintf(line, sizeof(line), Format, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if (strstr(line, "WARNING: PciIo->Map non-identity"))
		g_warning_count++;
	if (g_log_len + (size_t)n < sizeof(g_log_buf)) {
		memcpy(g_log_buf + g_log_len, line, (size_t)n);
		g_log_len += (size_t)n;
		g_log_buf[g_log_len] = '\0';
	}
}

/* Stubs required by unlock_610.c + runtime references when linked standalone */
BOOLEAN GuAd104AssetsValid(VOID) { return TRUE; }
BOOLEAN GuAd104ColdEntry(VOID) { return TRUE; }
BOOLEAN GuAd104PromPlausible(const UINT8 *Prom, UINTN Size) { (void)Prom; (void)Size; return TRUE; }
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *Fwsec) { (void)Fwsec; return TRUE; }
BOOLEAN GuTestPreflightDisplayOk(VOID) { return TRUE; }

static void *g_arena_backing = NULL;
static size_t g_arena_backing_bytes = 0;
static int g_use_non_identity = 0;
static UINT64 g_next_iova = 0x00204000ULL;
static int g_map_calls = 0;
static int g_unmap_calls = 0;
static int g_alloc_buf_calls = 0;
static int g_free_buf_calls = 0;

static void *alloc_low4gb_arena(size_t bytes) {
#ifdef _WIN32
	uintptr_t cand;
	for (cand = 0x30000000ULL; cand < 0x70000000ULL; cand += 0x01000000ULL) {
		void *p = VirtualAlloc((void *)cand, bytes, TEST_MEM_COMMIT | TEST_MEM_RESERVE, TEST_PAGE_RW);
		if (p && (UINT64)(UINTN)p + bytes - 1ULL <= 0xffffffffULL)
			return p;
		if (p)
			VirtualFree(p, 0, TEST_MEM_RELEASE);
	}
#endif
	return NULL;
}

static void free_low4gb_arena(void *p) {
#ifdef _WIN32
	if (p)
		VirtualFree(p, 0, TEST_MEM_RELEASE);
#else
	free(p);
#endif
}

static EFI_STATUS EFIAPI MockAllocatePages(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemType,
					   UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory) {
	size_t bytes = Pages * 4096ULL;
	(void)Type;
	(void)MemType;
	if (!Memory || !Pages)
		return EFI_INVALID_PARAMETER;
	if (!g_arena_backing || g_arena_backing_bytes < bytes)
		return EFI_OUT_OF_RESOURCES;
	*Memory = (EFI_PHYSICAL_ADDRESS)(UINTN)g_arena_backing;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockAllocatePool(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer) {
	(void)PoolType;
	if (!Buffer)
		return EFI_INVALID_PARAMETER;
	*Buffer = calloc(1, Size ? Size : 1);
	return *Buffer ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI MockFreePool(VOID *Buffer) {
	free(Buffer);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockPciAllocateBuffer(EFI_PCI_IO_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
						EFI_MEMORY_TYPE MemoryType, UINTN Pages,
						VOID **HostAddress, UINT64 Attributes) {
	void *p;
	(void)This; (void)Type; (void)MemoryType; (void)Attributes;
	p = alloc_low4gb_arena(Pages * 4096ULL);
	if (!p)
		return EFI_OUT_OF_RESOURCES;
	*HostAddress = p;
	g_alloc_buf_calls++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockPciFreeBuffer(EFI_PCI_IO_PROTOCOL *This, UINTN Pages, VOID *HostAddress) {
	(void)This; (void)Pages;
	g_free_buf_calls++;
	free_low4gb_arena(HostAddress);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockPciMap(EFI_PCI_IO_PROTOCOL *This, EFI_PCI_IO_PROTOCOL_OPERATION Op,
				    VOID *HostAddress, UINTN *NumberOfBytes,
				    EFI_PHYSICAL_ADDRESS *DeviceAddress, VOID **Mapping) {
	UINTN bytes;
	(void)This; (void)Op;
	if (!HostAddress || !NumberOfBytes || !DeviceAddress || !Mapping)
		return EFI_INVALID_PARAMETER;
	bytes = *NumberOfBytes;
	g_map_calls++;
	if (g_use_non_identity) {
		*DeviceAddress = g_next_iova;
		g_next_iova += ((bytes + 4095ULL) & ~4095ULL);
	} else {
		*DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
	}
	*Mapping = (VOID *)(UINTN)(0xA0000000ULL + (UINT64)g_map_calls);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockPciUnmap(EFI_PCI_IO_PROTOCOL *This, VOID *Mapping) {
	(void)This; (void)Mapping;
	g_unmap_calls++;
	return EFI_SUCCESS;
}

static EFI_BOOT_SERVICES g_mock_bs;
static EFI_PCI_IO_PROTOCOL g_mock_pci;
static struct pci_dev g_mock_pdev;
static struct firmware g_mock_gsp_fw;
static u64 g_dma_mask = DMA_BIT_MASK(32);

static u64 read_le64_at(const u8 *p) {
	u64 v = 0;
	unsigned int i;
	for (i = 0; i < 8; i++)
		v |= ((u64)p[i]) << (8U * i);
	return v;
}

static void setup_env(int non_identity) {
	memset(&g_mock_bs, 0, sizeof(g_mock_bs));
	memset(&g_mock_pci, 0, sizeof(g_mock_pci));
	memset(&g_mock_pdev, 0, sizeof(g_mock_pdev));
	memset(g_log_buf, 0, sizeof(g_log_buf));
	g_log_len = 0;
	g_warning_count = 0;
	g_use_non_identity = non_identity;
	g_next_iova = 0x00204000ULL;
	g_map_calls = 0;
	g_unmap_calls = 0;
	g_alloc_buf_calls = 0;
	g_free_buf_calls = 0;

	g_mock_bs.AllocatePages = MockAllocatePages;
	g_mock_bs.AllocatePool = MockAllocatePool;
	g_mock_bs.FreePool = MockFreePool;

	g_mock_pci.AllocateBuffer = MockPciAllocateBuffer;
	g_mock_pci.FreeBuffer = MockPciFreeBuffer;
	g_mock_pci.Map = MockPciMap;
	g_mock_pci.Unmap = MockPciUnmap;

	GuInit(&g_mock_bs, &g_mock_pci);
	Gu.profile_verified = TRUE;
	Gu.armed = TRUE;
	Gu.display_ok = TRUE;
	Gu.owned = TRUE;
	Gu.s3_persist_armed = TRUE;

	g_dma_mask = DMA_BIT_MASK(32);
	g_mock_pdev.vendor = 0x10de;
	g_mock_pdev.device = 0x2684;
	g_mock_pdev.dev.dma_mask = &g_dma_mask;
	g_mock_pdev.dev.coherent_dma_mask = DMA_BIT_MASK(32);
}

static void init_mock_work(struct unlock_transaction_work *work, u8 *gsp_img, size_t gsp_bytes) {
	memset(work, 0, sizeof(*work));
	work->pdev = &g_mock_pdev;
	g_mock_gsp_fw.data = gsp_img;
	g_mock_gsp_fw.size = gsp_bytes;
	work->gsp_fw = &g_mock_gsp_fw;
	work->image_span.offset = 0;
	work->image_span.size = gsp_bytes;
	work->layout_in.usable_fb = 24ULL << 30;
	work->layout_in.vga_base = (24ULL << 30) - (16ULL << 20);
	work->layout_in.frts_size = 0x100000ULL;
	work->layout_in.boot_size = 36864;
	work->layout_in.image_size = gsp_bytes;
	work->layout_in.nonwpr_size = 0x100000ULL;
	work->layout_in.heap_request = 0x1000000ULL;
	work->boot_desc.image_size = 36864;
	work->boot_desc.code_offset = 0x100;
	work->boot_desc.data_offset = 0x200;
	work->boot_desc.manifest_offset = 0x300;
	CHECK(gpu_unlock_layout(&work->layout_in, &work->layout) == 0);
	CHECK(gpu_unlock_radix_plan(work->image_span.size, &work->radix) == 0);
}

static void run_arena_scenario(int non_identity) {
	static u8 fake_gsp[64 * 4096];
	struct unlock_transaction_work work;
	u8 *saved_fwsec_cpu, *saved_booter_cpu, *saved_v67_cpu, *saved_boot_cpu;
	u8 *saved_radix0_cpu, *saved_s3_meta_cpu;
	u8 *saved_leaf_cpu[4] = {0};
	u32 i;
	u32 table_pages;
	u32 leaf_count;

	for (i = 0; i < sizeof(fake_gsp); i++)
		fake_gsp[i] = (u8)(i ^ 0x5Au);

	setup_env(non_identity);
	init_mock_work(&work, fake_gsp, sizeof(fake_gsp));

	CHECK(GuS3AllocateArena() == EFI_SUCCESS);
	CHECK(Gu.s3_arena_armed == TRUE);

	Gu.s3_arena_active = TRUE;
	work.fwsec_size = 4096;
	work.fwsec_cpu = diag_dma_alloc(&work, "fwsec", work.fwsec_size, &work.fwsec_dma, GFP_KERNEL);
	work.booter_image_size = 56832;
	work.booter_cpu = diag_dma_alloc(&work, "booter", work.booter_image_size, &work.booter_dma, GFP_KERNEL);
	work.v67_cpu = diag_dma_alloc(&work, "v67", GU_V67_SIZE, &work.v67_dma, GFP_KERNEL);
	work.boot_cpu = diag_dma_alloc(&work, "boot-image", work.boot_desc.image_size, &work.boot_dma, GFP_KERNEL);
	Gu.s3_arena_active = FALSE;
	work.meta_cpu = diag_dma_alloc(&work, "metadata", GU_WPR_META_SIZE, &work.meta_dma, GFP_KERNEL);
	work.stock_size = 4096;
	work.stock_cpu = diag_dma_alloc(&work, "stock", work.stock_size, &work.stock_dma, GFP_KERNEL);

	CHECK(work.fwsec_cpu && work.booter_cpu && work.v67_cpu && work.boot_cpu);
	CHECK(work.meta_cpu && work.stock_cpu);

	Gu.s3_fwsec_dma = work.fwsec_dma;
	Gu.s3_fwsec_size = work.fwsec_size;
	Gu.s3_booter_dma = work.booter_dma;
	Gu.s3_booter_size = work.booter_image_size;
	Gu.s3_v67_dma = work.v67_dma;
	Gu.s3_boot_dma = work.boot_dma;
	Gu.s3_boot_size = work.boot_desc.image_size;

	CHECK(unlock_transaction_radix(&work) == 0);
	CHECK(work.pages_allocated == work.page_count);
	CHECK(pack_wpr_meta(&work, work.meta_cpu, false) == 0);
	memcpy(Gu.wpr_meta, work.meta_cpu, GU_WPR_META_SIZE);
	Gu.wpr_meta_valid = TRUE;

	Gu.s3_arena_active = TRUE;
	Gu.s3_meta_cpu = diag_dma_alloc(&work, "s3-meta", GU_WPR_META_SIZE, &Gu.s3_meta_dma, GFP_KERNEL);
	Gu.s3_arena_active = FALSE;
	CHECK(Gu.s3_meta_cpu != NULL);
	work.s3_meta_dma = Gu.s3_meta_dma;
	memcpy(Gu.s3_meta_cpu, work.meta_cpu, GU_WPR_META_SIZE);

	saved_fwsec_cpu = (u8 *)work.fwsec_cpu;
	saved_booter_cpu = (u8 *)work.booter_cpu;
	saved_v67_cpu = (u8 *)work.v67_cpu;
	saved_boot_cpu = (u8 *)work.boot_cpu;
	saved_radix0_cpu = (u8 *)work.pages[0].cpu;
	saved_s3_meta_cpu = (u8 *)Gu.s3_meta_cpu;
	table_pages = (u32)work.radix.table_pages;
	leaf_count = work.page_count - table_pages;
	for (i = 0; i < 4 && i < work.page_count; i++)
		saved_leaf_cpu[i] = (u8 *)work.pages[i].cpu;

	if (non_identity) {
		CHECK(Gu.s3_arena_non_identity == TRUE);
		CHECK(g_warning_count == 1);
		CHECK(Gu.s3_arena_non_identity_count > 10U);
		/* Before repack, persisted S3 addresses still hold the IOVAs != CPU phys */
		CHECK(Gu.s3_booter_dma == work.booter_dma);
		CHECK(Gu.s3_booter_dma != (UINT64)(UINTN)saved_booter_cpu);
		CHECK(read_le64_at(saved_s3_meta_cpu + 16) == work.pages[0].dma);
		CHECK(read_le64_at(saved_s3_meta_cpu + 32) == work.boot_dma);
		CHECK(read_le64_at(saved_s3_meta_cpu + 72) == work.v67_dma);
	} else {
		CHECK(Gu.s3_arena_non_identity == FALSE);
		CHECK(g_warning_count == 0);
		CHECK(Gu.s3_arena_non_identity_count == 0);
	}

	/* Simulate post-FLR quiescence before unwind */
	work.firmware_started = true;
	work.quiesced = true;
	GuStarted();
	GuQuiesced();

	if (Gu.s3_arena_non_identity) {
		CHECK(s3_repack_arena_physical(&work) == 0);
	}

	free_coherent(&work);
	CHECK(Gu.retained == FALSE);
	CHECK(Gu.allocations == NULL);
	CHECK(Gu.dma_count == 0);
	CHECK(Gu.s3_meta_cpu == NULL);
	CHECK(g_unmap_calls == g_map_calls);
	CHECK(g_free_buf_calls == 2); /* Only non-arena metadata + stock freed */

	/* Verify final persisted S3 addresses and intact CPU-phys DRAM tables after unmap */
	CHECK(Gu.s3_fwsec_dma == (UINT64)(UINTN)saved_fwsec_cpu);
	CHECK(Gu.s3_booter_dma == (UINT64)(UINTN)saved_booter_cpu);
	CHECK(Gu.s3_v67_dma == (UINT64)(UINTN)saved_v67_cpu);
	CHECK(Gu.s3_boot_dma == (UINT64)(UINTN)saved_boot_cpu);
	CHECK(Gu.s3_meta_dma == (UINT64)(UINTN)saved_s3_meta_cpu);
	CHECK(Gu.s3_radix_dma == (UINT64)(UINTN)saved_radix0_cpu);

	CHECK(read_le64_at(saved_s3_meta_cpu + 16) == (u64)(UINTN)saved_radix0_cpu);
	CHECK(read_le64_at(saved_s3_meta_cpu + 32) == (u64)(UINTN)saved_boot_cpu);
	CHECK(read_le64_at(saved_s3_meta_cpu + 72) == (u64)(UINTN)saved_v67_cpu);
	CHECK(read_le64_at(Gu.wpr_meta + 16) == (u64)(UINTN)saved_radix0_cpu);

	/* Verify top-level radix page points to next-level arena CPU phys addresses */
	CHECK(table_pages >= 3U);
	CHECK(leaf_count == 64U);
	CHECK(read_le64_at(saved_leaf_cpu[0] + 0) == (u64)(UINTN)saved_leaf_cpu[1]);
	CHECK(read_le64_at(saved_leaf_cpu[1] + 0) == (u64)(UINTN)saved_leaf_cpu[2]);
	CHECK(read_le64_at(saved_leaf_cpu[2] + 0) == (u64)(UINTN)saved_leaf_cpu[3]);
}

int main(void) {
	u64 phys_out = 0;

	g_arena_backing_bytes = (size_t)GU_S3_ARENA_PAGES * 4096ULL;
	g_arena_backing = alloc_low4gb_arena(g_arena_backing_bytes);
	CHECK(g_arena_backing != NULL);
	if (!g_arena_backing)
		return 1;

	/* Reject >4 GB or NULL physical addresses */
	CHECK(s3_arena_cpu_phys32(NULL, &phys_out) == -ERANGE);
	CHECK(s3_arena_cpu_phys32((const void *)(UINTN)0x100000000ULL, &phys_out) == -ERANGE);

	run_arena_scenario(0);
	run_arena_scenario(1);

	free_low4gb_arena(g_arena_backing);
	printf("test_s3_non_identity: %u checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
