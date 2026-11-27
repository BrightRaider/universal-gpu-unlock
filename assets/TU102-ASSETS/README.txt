# TU102 / TU104 / TU106 Firmware Assets (Turing Architecture)

This directory is the staging area for NVIDIA Turing (GeForce RTX 20 Series & CMP 50HX) rate-limiter unlock assets.

Required microcode assets (driver version 610.43.03):
1. booter_load_tu102.bin        <- SEC2 boot loader for TU102
2. v67-tu102-610.43.03.bin      <- Patched V67 firmware payload
3. gsp_boot_tu102.bin           <- GSP boot image (~24 KB)
4. gsp_boot_desc_tu102.bin      <- GSP boot descriptor (84 bytes)
5. gsp_ga10x.bin / gsp_tu10x.bin <- Shared GSP firmware blob (~84 MB, automatically resolved from AD102-ASSETS)

Supported Turing Hardware:
- TU102: RTX 2080 Ti, TITAN RTX, CMP 50HX, Quadro RTX 6000 / 8000
- TU104: RTX 2080 SUPER, RTX 2080, RTX 2070 SUPER, Quadro RTX 5000 / 4000
- TU106: RTX 2070, RTX 2060 SUPER, RTX 2060 12GB, RTX 2060 6GB
