# Firmware Microcode Assets (`assets/`)

This directory groups the pre-validated NVIDIA microcode payloads (`AD102-ASSETS/`, `GA102-ASSETS/`, `TU102-ASSETS/`) to keep the repository root clean.

- **Automatic USB / ESP Setup (`Setup.bat`):** `tools\Deploy-UsbCommon.ps1` copies these folders directly to `\AD102-ASSETS\`, `\GA102-ASSETS\`, and `\TU102-ASSETS\` at the root of your USB drive (and onto internal ESPs for `[I]` stickless auto-unlock).
- **Manual USB Copy (if not using `Setup.bat`):** Copy the three inner folders (`AD102-ASSETS/`, `GA102-ASSETS/`, `TU102-ASSETS/`) directly to the root of your FAT32 USB drive (`USB:\AD102-ASSETS\`, etc.) — not nested inside `USB:\assets\`.
