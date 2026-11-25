# ⚠️ OPTIONAL TURING RESIZABLE BAR (ReBarUEFI) - EXPERIMENTAL GUIDE

> [!CAUTION]
> # ‼️‼️ ABSOLUTELY NO SUPPORT PROVIDED! USE STRICTLY AT YOUR OWN RISK! ‼️‼️
> 
> * **NO SUPPORT:** The Universal GPU Unlock project, its author, and contributors provide **ZERO TECHNICAL SUPPORT, ASSISTANCE, OR GUARANTEES** for third-party Resizable BAR modifications or motherboard BIOS flashing.
> * **DO NOT OPEN GITHUB ISSUES FOR REBARUEFI:** Any issues opened on this repository regarding motherboard BIOS flashing errors, boot loops, black screens, or ReBarUEFI configuration will be **closed immediately without support**.
> * **RISK OF MOTHERBOARD BRICK:** Modifying motherboard BIOS ROMs with third-party DXE drivers carries a real risk of soft-bricking your motherboard. Only attempt this if your motherboard features physical **USB BIOS Flashback** or a dual-BIOS switch!
> * **YOUR GPU UNLOCK IS ALREADY 100% ACTIVE:** Your GPU compute rate-limiter bypass operates on BAR0 and is **already running at 100% full hardware compute capacity** (e.g. 25.82 TFLOPS on RTX 2060). ReBAR does NOT increase raw tensor compute TFLOPS; it only optimizes PCIe texture/weight transfer speeds.
> * **DO NOT COMBINE WITH WINDOWS S4 HIBERNATE:** Pre-boot ReBAR overrides (`ReBarDxe` / `0x610`/`0xBC0` aperture overrides) combined with Windows S4 Hibernation (`Setup-FastResume.bat` / `shutdown /h`) can trigger hard `Kernel-Power 41` hangs. Disable Hibernation (`powercfg /h off`) if testing BIOS ReBarUEFI modifications, or keep S4 without pre-boot ReBAR overrides.

---

## 🔍 Technical Background: Why Turing Cards Lack Official ReBAR

NVIDIA hardware from the Turing generation (RTX 2060, 2070, 2080, 2080 Ti) physically supports Resizable BAR in the silicon controller, but NVIDIA **never released official ReBAR vBIOS updates** for the RTX 20 series. 

Even if you enable *"Above 4G Decoding"* and *"Re-Size BAR Support"* in your motherboard BIOS, the Turing factory vBIOS descriptor hardcodes a request for only **256 MiB** (`BAR1`). Consequently, your motherboard firmware respects the GPU descriptor and leaves the aperture at 256 MiB.

---

## 🛠️ The Third-Party Solution: xCuri0's ReBarUEFI Project

The open-source project [ReBarUEFI by xCuri0](https://github.com/xCuri0/ReBarUEFI) provides a DXE driver (`ReBarDxe`) that intercepts the motherboard's PCI host bridge initialization during boot and forces a large BAR1 aperture (e.g. 6 GB, 8 GB, 11 GB).

### Important Considerations Before Proceeding:
1. **Motherboard BIOS Injection Required:**  
   Because `ReBarDxe` must execute before the motherboard allocates PCI resources, it **cannot** be chainloaded in Windows or from a simple USB bootloader. It typically requires injecting `ReBarDxe.ffs` directly into your motherboard's BIOS ROM using `UEFITool` and flashing it via USB BIOS Flashback.
2. **Motherboard Firmware Compatibility:**  
   Many OEM boards (ASUS, MSI, Gigabyte, ASRock) enforce cryptographic signature checks on BIOS ROM updates. Flashing a modified BIOS may be blocked or require specialized recovery flashers.

---

## 🔗 Official Upstream Resources

If you are an advanced enthusiast who understands the risks and wants to configure ReBarUEFI:
* **Official Upstream Repository:** [https://github.com/xCuri0/ReBarUEFI](https://github.com/xCuri0/ReBarUEFI)
* **ReBarUEFI Releases & Downloads:** [https://github.com/xCuri0/ReBarUEFI/releases](https://github.com/xCuri0/ReBarUEFI/releases)
* **Setup Guide & Documentation:** [https://github.com/xCuri0/ReBarUEFI#usage](https://github.com/xCuri0/ReBarUEFI#usage)

---

> [!NOTE]
> Again: Universal GPU Unlock is completely independent of ReBarUEFI. Our compute unlock works flawlessly on stock 256 MiB BAR1 configurations. Proceed with ReBarUEFI only if you assume full responsibility for your system hardware.
