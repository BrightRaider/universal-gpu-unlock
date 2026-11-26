# Temporary Pre-Release Test Patch (v2.0.0 - Issue #23)

Pre-release EFI test patch on top of `v2.0.0` lowering the PE32+ EFI chainloader size floor in `ReadWindowsBinary()` (4096 bytes min + full PE32+ AMD64 EFI Application header check) so **VeraCrypt `DcsBoot.efi`** (`\EFI\Microsoft\Boot\bootmgfw.efi`, `~24 KiB`) stages cleanly (efs #23).
- File: `v2.0.0-veracrypt-efi-patch.zip` (`303.8 KB`, `311068` bytes)
- SHA-256: `49B16F4B2AAF7280BBE792C6EA96F9ABDF28D0D9E0DC313DB4A61577607B3C4D`
