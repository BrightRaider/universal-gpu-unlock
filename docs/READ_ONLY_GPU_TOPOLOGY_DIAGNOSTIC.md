# Read-only PCI/GOP topology diagnostic (AD102 + GA102)

This is a standalone, **diagnostic-only EFI application**, not a GPU unlocker.
It extends the shared `gpu_inventory` selector already used by both legacy EFI
entrypoints. It checks the exact pinned B850 cards:

| Card | PCI ID | Subsystem ID | Segment:BDF |
|---|---|---|---|
| AD102 RTX 4090 | `10de:2684` | `1043:88e6` | `0000:01:00.0` |
| GA102 RTX 3080 | `10de:220a` | `19da:b612` | `0000:04:00.0` |

These are observed identities and **not generic model claims**. The result
remains `PARTIAL` or `ERROR` if PCI handles, locations, path prefixes, bridge
bus ranges, root-port types, GOP paths, or `ConOut` device paths are missing,
changed or contradictory. In particular, a Windows device number does not prove
its pre-boot PCI bus will remain the same across firmware configuration changes.

## Diagnostic observations and limitations

- Enumerates all EFI `PCI_IO` endpoints using bounded `LocateHandleBuffer`,
  `HandleProtocol`, `GetLocation`, 256-byte PCI config reads and device paths.
  Re-resolves each PCI protocol and re-reads its entire config and device path
  for stability. A read/FreePool failure makes the capture incomplete.
- Identifies exact GPUs through **the same** `GuCollectGpuInventory` and
  `GuFindPinnedGpuPair` routine as the existing source, not by array position.
- Validates nearest PCI bridge parents using complete path-prefix ancestry;
  bridge bus windows must contain the GPU bus. Each terminal PCI path node must agree with its BDF device/function. A PCIe Root Port requires PCIe
  capability Port Type 4. Roots must be distinct. Missing handles cannot be
  invented from PCI bus numbers.
- Enumerates `EFI_GRAPHICS_OUTPUT_PROTOCOL` handles and associates each GOP
  device path with its longest matching display-controller PCI path. Both
  NVIDIA GOP associations and successful PCI owner queries are required for
  a complete topology observation. Logs `OpenProtocolInformation` BY_DRIVER counts; an owner count is not proof of
  a particular NVIDIA driver's private GOP context or of a safe Stop operation.
- Reads `ConOut` device path(s) from the UEFI global variable **without
  writing it**; demands an AMD display GOP association and no NVIDIA ConOut
  path before declaring *observed console-path separation*. That still does
  **not** prove which physical cable/monitor is lit, GOP destructor safety,
  firmware ownership teardown, or NVIDIA hardware recovery.
- `TOPOLOGY=0` / `STATUS=COMPLETE` means only the limited **read-only
  observations** were consistent. It is **never actuation eligibility**.
  The EFI output always says `CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0`.

No vendor unlock sources, `app.c`, `dxe_main.c`, `msi_release.c`, DMA, BAR0
MMIO or firmware transactions are linked. The default image only outputs
verbose Unicode text through `ConOut` and does not write a disk or NVRAM.
**Optional** `-SaveLog` intentionally creates a *new* `GPU-TOPOLOGY-DIAGNOSTIC.TXT`
on the boot application's EFI volume; it refuses to overwrite an existing file.
This optional log file is the only persistent mutation by the application.

## Build, inspect and run

From a Windows Developer PowerShell with Visual Studio C++ Build Tools installed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\src\Build-Gpu-Topology-Diagnostic.ps1 -SaveLog
```

Without the optional disk write, omit `-SaveLog`. The output is
`src\build_diagnostic\GPU-TOPOLOGY-DIAGNOSTIC.EFI` and a printed SHA-256.
The generated PE/COFF binary is **unsigned**. It is deliberately **not** the
repository's prebuilt `bin\BOOTX64.EFI`, is not a DXE driver, cannot be used
as an active unlock, and is not automatically installed. Verify the image's
EFI application subsystem, imports, hash, and Secure Boot policy before copying
it to a removable EFI filesystem and running it manually in UEFI Shell.
Do not enroll new Secure Boot keys merely to run a diagnostic.

On removable media with UEFI Shell, run the image by its explicitly chosen
path, e.g. `fs0:\GPU-TOPOLOGY-DIAGNOSTIC.EFI`. Filesystem mappings vary.
With `-SaveLog`, archive or delete the **diagnostic log file** before running
again; the program refuses overwrites to preserve original evidence.
Capture all `PCI[n]`, `GPU[n]`, `GOP_*`, `CONOUT_*` and final `STATUS` lines,
and retain the SHA-256 and build arguments. Never interpret `STATUS=COMPLETE`
as permission to detach a driver or proceed to a firmware transaction.

## Host verification and fault boundaries

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic -Isrc tests/test_gpu_topology.c src/gpu_topology.c src/gpu_inventory.c -o /tmp/test_gpu_topology && /tmp/test_gpu_topology
```

The host tests validate reversed PCI enumeration, exact card pins, wrong PCI
IDs/BDF/subsystem, duplicated handles/BDF/protocols, absent or shared root
ports, bridge windows, broken parent paths, malformed GOP and ConOut paths,
NVIDIA ConOut ownership, missing AMD GOP and console observations, and bounds.
They cannot prove genuine firmware methods, GOP driver safety or display
ownership. See `Windows-GPU-UNLOCK`'s read-only `research/pci-decode/efi-capture.*`,
`internal-boot/boot/probe-parent.c` and `research/gop/nvgop_stop_model.py`
for the basis of the ancestry observations and for modeled Stop-related
failure boundaries. The latter model is host-only and must **not** be used as
proof that `DisconnectController()` is safe.

## Current status

- Pure topology host checks are implemented. EFI source and isolated build
  recipe are provided; physical execution remains **not tested**.
- No changes are made to the legacy active entrypoint's strict multi-GPU
  refusal. No dual-GPU write-enabled path is introduced.
- Revalidate all build and host results against a specific commit and hardware
  capture before treating any output as an implementation qualification.
