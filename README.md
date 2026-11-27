# Temporary Pre-Release Test Patch (v2.0.0 - Issue #25)

Pre-release EFI test patch on top of `v2.0.0` fixing non-identity `PciIo->Map()` DXE IOVA addresses in the S3 Persistent Memory Arena (e.g., `MSI MPG X670E Carbon WIFI` / `IoMmuDxe` / Pre-Boot DMA Protection) so `Gu.s3_*_dma`, `Gu.s3_meta_cpu`, and all 3-level GSP radix table pages are converted back to genuine `<4 GB` CPU-physical DRAM addresses before handover (`refs #25`, commit `7e3f836`).
- File: `v2.0.0-x670e-s3-patch.zip` (334.1 KB, `342086` bytes)
- SHA-256: `E933B6743D6BFCA26A0F15D550888189EAB9D78EEBD69AF2EA843E49B2195AC9`
