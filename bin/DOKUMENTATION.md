# Technische Dokumentation: Universal GPU Unlock (Multi-Gen)

> **Tool:** Universal GPU Unlock  
> **Version:** v1.8.27  
> **Architekturen:** NVIDIA Ada Lovelace (RTX 40), Ampere (RTX 30), Turing (RTX 20 / CMP)  
> **Unterstützte Betriebssysteme:** Windows 10, Windows 11, WSL2 / Ubuntu CUDA Container  

---

## 1. Einleitung & Funktionsweise

### 1.1 Was ist der Universal GPU Unlock?
NVIDIA beschränkt Consumer- und Mining-Grafikkarten in ihrer Firmware bei FP16- und BF16-Tensor-Berechnungen (GEMM) über einen künstlichen Rate-Limiter auf einen Bruchteil des physikalisch möglichen Durchsatzes. Der *Universal GPU Unlock* hebt diese Drosselung auf UEFI-Ebene vor dem Laden des Windows-Kernels im RAM auf. Dadurch wird die volle Enterprise-Rechenleistung für AI-, Deep-Learning- und LLM-Workloads (PyTorch, Stable Diffusion, ComfyUI, vLLM) freigeschaltet.

### 1.2 Grenzen & Gaming-Verhalten
- **Kein Gaming-Boost:** Der Unlock beeinflusst weder Shader-, Rasterizer- noch DLSS-Leistung in Spielen. In reinen PC-Spielen beträgt der Performance-Gewinn 0 %.
- **100 % RAM-Only / Reversibel:** Es findet kein permanenter Flash des Grafik-VBIOS statt. Nach vollständigem Ausschalten des Systems (Kaltstart) befindet sich die GPU wieder im Originalzustand.
- **Kompatibel mit Secure Boot & Anti-Cheat:** Durch Authenticode-Signierung bleibt Secure Boot aktiv (wichtig für Riot Vanguard / Windows 11).

---

## 2. Bootloader-Modi & Auswahlmöglichkeiten

Beim Booten vom USB-Stick erscheint ein 30-Sekunden-Countdown:

| Taste | Modus | Beschreibung |
| :---: | :--- | :--- |
| **`[U]`** | **Standard Unlock** | Bewährter, robuster Unlock im RAM. Kehrt beim Kaltstart sauber in den Originalzustand zurück. |
| **`[P]`** | **S3 Persistence Unlock** | Experimenteller Boot-Script-Modus, der den Unlock-Status auch über Sleep/Standby (S3) aufrechterhält. |
| **`[I]`** | **Install to NVRAM** | Kopiert den `AdaUnlockDxe.efi`-Treiber in die interne EFI-Partition (`\EFI\Drivers\`) und trägt ihn in die UEFI-NVRAM-Bootreihenfolge ein (automatischer stickloser Unlock bei jedem Start). |
| **`[R]`** | **Remove DXE Driver** | Bereinigt den DXE-Treiber vollständig aus dem UEFI-NVRAM und der internen ESP-Partition. |
| **`[S]`** | **Stock Windows** | Führt Hardwarescans und Logging auf den USB-Stick durch, überspringt den Unlock und bootet normales Windows. |
| **`[Leertaste]`** | **Pause Countdown** | Hält den 30-Sekunden-Timer an, um das Menü in Ruhe lesen und bedienen zu können. |

---

## 3. Headless Compute & Sekundär-Grafikkarten (z. B. NVIDIA CMP 40HX)

### 3.1 Das Problem uninitialisierter PCI-Command-Register
Grafikkarten ohne Display-Ausgänge (wie die NVIDIA CMP 40HX auf TU106-Basis) oder Sekundär-GPUs in Systemen mit integrierter AMD/Intel-Grafik (iGPU als primärer Display-Adapter) werden vom UEFI-POST beim Systemstart oft nicht als aktive Anzeigecontroller initialisiert. Das Motherboard-BIOS lässt das PCI Command Register auf `0x0000` (Memory Space Decode disabled, `rom=0x0`).

### 3.2 Die Lösung ab v1.8.27
1. **Lockerung der Vorprüfung (`GuCheckConfig` in `src/checks.c`):** Uninitialisierte Beschleuniger werden nicht mehr abgewiesen, nur weil das Memory-Decode-Bit im Command Register noch inaktiv ist.
2. **Dynamische PCI Memory Space Aktivierung (`src/gpu_discovery.c`):** Vor dem Auslesen der MMIO-Register (BAR0 + 0x300000 VBIOS-ROM) aktiviert der Bootloader über das `EFI_PCI_IO_PROTOCOL` dynamisch das Memory-Decoding (`EFI_PCI_IO_ATTRIBUTE_MEMORY`) und setzt das Command-Register bei Bedarf direkt auf Hardware-Ebene.

---

## 4. Secure Boot Konfiguration

Die Binärdateien (`BOOTX64.EFI` und `AdaUnlockDxe.efi`) sind mit einem dedizierten Authenticode-Zertifikat signiert.

- **Option A (Einfachste Methode):** Secure Boot im BIOS vorübergehend auf **Disabled** stellen.
- **Option B (Secure Boot aktiv lassen):**
  1. Im BIOS Setup unter **Security ➔ Secure Boot ➔ Key Management** (bzw. *Custom Key Management*).
  2. Unter **Authorized Signatures (`db`)** auf **Append Key** / **Enroll Signature** klicken.
  3. Auf den USB-Stick navigieren und **`AdaUnlock_SecureBoot.cer`** auswählen.
  4. Mit **F10** speichern und neu starten.

---

## 5. Troubleshooting & Best Practices

- **CSM zwingend deaktivieren:** CSM (Compatibility Support Module) muss im BIOS deaktiviert sein (Pure UEFI Mode), um legacy VGA-Konflikte (Fehlercode 43) zu vermeiden.
- **Above 4G Decoding & Resizable BAR:** Im BIOS aktivieren, um den Flaschenhals des 256-MB-PCIe-Fensters aufzuheben (+10 % bis +15 % Durchsatzgewinn).
- **Turing / CMP Karten:** Einmalig `Toggle-Turing-GSP.bat` als Administrator in Windows ausführen, um den Treiber an den GSP-Firmware-Modus zu binden.

---

## 6. S3 Persistence Architektur & Boot-Script Dispatch (`[P]`-Modus)

### 6.1 Die technische Herausforderung beim S3 Sleep (Suspend-to-RAM)
Beim Wechsel in den Standby-Zustand (ACPI S3) schaltet das Betriebssystem und das Motherboard-Power-Management die GPU stromlos (Power Gating). Beim Aufwachen (S3 Resume) erfährt die GPU einen hardwareseitigen Power-On-Reset:
- Alle internen Steuer- und Konfigurationsregister der GPU werden auf Werkseinstellungen zurückgesetzt.
- Der hardwarebasierte Rate-Limiter an den Adressen `0x0082381c` (SS0) und `0x00823820` (SS1) wird hardwaremäßig gesperrt.
- Einfache MMIO-Schreiboperationen (`EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE`) werden von der GPU nach einem Reset ignoriert, weil der NVIDIA SEC2 Falcon Microcontroller zuerst die kryptografische Authentifizierung des V67-Microcodes abschließen muss.
- Ohne erneute Ausführung des Handshakes startet der Grafiktreiber nach dem Standby wieder mit gedrosselter Tensor-Core-Leistung.

### 6.2 Die ACPI NVS Boot-Script Dispatch Architektur
Um den Unlock auch über Standby-Zyklen hinweg ohne Neustart aufrechtzuerhalten, implementiert der `[P]`-Modus folgende mehrstufige Architektur:

1. **Persistentes Memory Staging (`EfiACPIMemoryNVS`):**
   - Standardmäßige UEFI-Boot-Services-Speicherbereiche (`EfiBootServicesCode`, `EfiBootServicesData`) werden von Windows nach `ExitBootServices()` freigegeben und überschrieben.
   - Ich allokiere daher 64 Seiten (256 KB) persistenten `EfiACPIMemoryNVS`-Speicher (Non-Volatile Sleeping Memory). Dieser Speicherbereich ist im ACPI-Memory-Map als reserviert markiert, bleibt während des S3-Schlafs im DRAM mit Strom versorgt und wird vom Windows-Kernel niemals angetastet.
   - In diesen Puffer werden alle benötigten Firmware-Payloads (Booter ~60 KB, V67 Microcode 64 KB, Boot Descriptor und Metadaten) sowie die `GU_S3_RESUME_CONTEXT`-Struktur kopiert.

2. **Boot-Script Dispatch Registrierung (`EFI_BOOT_SCRIPT_DISPATCH_2_OPCODE`):**
   - Über das `EFI_S3_SAVE_STATE_PROTOCOL` registriere ich den Dispatch-2-Opcode im Motherboard-Boot-Script.
   - Dieser Eintrag weist die UEFI-Firmware des Motherboards an, beim Aufwachen aus S3 direkt die autarke Dispatcher-Routine `GuS3ResumeDispatch` mit dem NVS-Kontext als Parameter aufzurufen.
   - Als Rückfallebene werden zusätzlich MMIO-Writes für SS0 und SS1 registriert, falls ein BIOS Dispatch-2 nicht ausführt.

3. **Autarke Resume-Engine (`GuS3ExecuteResumeUnlock`):**
   - Beim Aufwachen des Systems führt das Motherboard-BIOS die Routine noch vor der Wiederübergabe an das Betriebssystem aus.
   - Die Engine setzt den SEC2-Falcon-Controller zurück, konfiguriert das FBIF-Interface, stößt den Booter per DMA an, pollt das Falcon-Halt-Signal und verriegelt anschließend die Rate-Limiter-Selektoren `0x88888888` und `0x00000008`.
   - Nach erfolgreicher Verriegelung wird der SEC2-Controller in einen sauberen Zustand versetzt und der Resume-Status im NVS-Header protokolliert (`resume_status = 1`, inkrementierter `resume_count`).
