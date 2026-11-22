# Technische Dokumentation: Universal GPU Unlock (Multi-Gen)

> **Tool:** Universal GPU Unlock  
> **Version:** v1.8.28  
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

### 2.1 Speicherplatzbedarf der EFI-Systempartition (ESP) bei Option `[I]`
Standardmäßig erstellt das Windows-Setup eine exakt 100 MB große EFI-Systempartition (ESP), von der Windows selbst bereits 30–50 MB belegt.
* **Speicherplatzbedarf der Firmware-Assets:**
  * **Ada Lovelace (RTX 40) & Ampere (RTX 30):** Benötigen zwingend NVIDIAs signierte GSP-Firmware `gsp_ga10x.bin` (**80,37 MB**). Auf einer 100-MB-ESP reicht der freie Platz oft nicht aus. Erkennt `[I]` unzureichenden Speicherplatz, registriert der Bootloader die Option sicher auf dem USB-Laufwerk, statt eine unvollständige Installation auf der ESP zu hinterlassen.
  * **Turing (RTX 20 / CMP):** Benötigt nur `TU102-ASSETS` (**28,1 MB**). Da der Bootloader im einheitlichen Release-Paket automatisch erkennt, welche Architektur verbaut ist, kopiert Option `[I]` auf Turing-Karten ausschließlich die kompakten Turing-Assets. Dadurch passt die Installation direkt auf standardmäßige 100-MB-Partitionen, ganz ohne manuelles Partitionieren.
* **Lösungswege & Best Practices:**
  1. **Option `[P]` (S3 Sleep Persistence - Empfohlen):** Wer den Rechner nicht herunterfährt, sondern den Standby (S3 Suspend-to-RAM) nutzt, benötigt `[I]` überhaupt nicht. Einmalig per USB entsperren – der im ACPI NVS residente Dispatcher stellt den Unlock beim Aufwachen in unter 3 Sekunden wieder her, ganz ohne ESP-Speicherplatzbedarf.
  2. **Eigene FAT32-Partition (1-Klick-Tool `Create-Unlock-Partition.bat`):** Mit dem beigelegten 1-Klick-Skript `Create-Unlock-Partition.bat` (oder direkt im Universal Control Center) wird eine interne SSD gefahrlos um 512 MB verkleinert und als FAT32 formatiert. Das UEFI-BIOS mountet diese Partition beim Booten automatisch, und Option `[I]` legt Treiber und Assets dort ab (in [#19](https://github.com/BrightRaider/universal-gpu-unlock/issues/19) verifiziert).
  3. **ESP vergrößern:** Die bestehende ESP mit einem Partitionstool von 100 MB auf 500 MB erweitern.
* **Automatische Erkennung in Setup & Control Center:** Sowohl `Setup.bat` (`Setup-AdaUnlock.ps1`) als auch das Universal Control Center (in den Reitern *Pre-Flight Safety*, *USB Unlock Deployer* und *Internal ESP Setup*) erkennen die Größe der Windows-ESP automatisch und weisen bei einer Standard-100-MB-Partition direkt auf Option `[P]` hin.

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
   - **Autarke PIC-Code-Spiegelung:** Da normaler Executable Code in `EfiBootServicesCode` oder `EfiLoaderCode` nach `ExitBootServices()` von Windows freigegeben und überschrieben werden kann, kopiere ich den positionsunabhängigen Maschinencode der Dispatch-Routine `GuS3ResumeDispatch` (331 Bytes, 0 Relocations) direkt in den geschützten NVS-Puffer (`nvs_base + 0x00800`).
   - Der Dispatch-2-Eintrag verweist auf `nvs_phys + 0x00800`. Dadurch ist garantiert, dass der auszuführende Code auch bei maximaler Windows-Speicherauslastung niemals überschrieben wird.
   - **Parameter-Resilienz:** Die Dispatch-Routine unterstützt sowohl den Standard-UEFI-PI-Aufruf (`PeiServices` in `RCX`, `Context` in `RDX`) als auch BIOS-Varianten, die den Kontext direkt im ersten Parameter (`RCX`) übergeben.
   - Als Rückfallebene werden zusätzlich MMIO-Writes für SS0 und SS1 registriert, falls ein BIOS Dispatch-2 nicht ausführt.

3. **Autarke Resume-Engine (`GuS3ExecuteResumeUnlock`):**
   - Beim Aufwachen des Systems führt das Motherboard-BIOS die Routine noch vor der Wiederübergabe an das Betriebssystem aus.
   - Die Engine setzt den SEC2-Falcon-Controller zurück, konfiguriert das FBIF-Interface, stößt den Booter per DMA an, pollt das Falcon-Halt-Signal und verriegelt anschließend die Rate-Limiter-Selektoren `0x88888888` und `0x00000008`.
   - Nach erfolgreicher Verriegelung wird der SEC2-Controller in einen sauberen Zustand versetzt und der Resume-Status im NVS-Header protokolliert (`resume_status = 1`, inkrementierter `resume_count`).

---

## 7. Sicherheitsarchitektur & Containment (Code-Review Zusammenfassung)

### 7.1 Zweistufige Hardware-Containment-Logik
- **Keine Mutation (`!GuUnlockMutated()`):** Bei unpassenden Karten oder Preflight-Verweigerung bleibt die GPU unberührt. Der Bootloader informiert den Benutzer und kehrt sicher nach 5 Sekunden zum BIOS/Bootmenü zurück.
- **Aktive Mutation (`GuUnlockMutated()`):** Sollte während der Hardware-Transaktion ein Fehler auftreten, entzieht der Bootloader dem Gerät sofort das Bus-Mastering (`GuClearMaster()`), setzt PCI-Attribute zurück und hält das System kontrolliert an (`GuBootHalt()`), damit Windows keinen inkonsistenten DMA-Zustand erbt.

### 7.2 CSM Video Isolation & GOP Recovery
- Erkennt Legacy-CSM-Video-Adapter (`pci_attributes & VGA_MEMORY/VGA_IO`).
- Verhindert das Aufrufen von `ConnectController` auf dem Legacy-Handle.
- **Layer 1:** Lädt und bindet den nativen UEFI GOP-Treiber aus dem VBIOS Option ROM.
- **Layer 2:** Installiert bei Bedarf einen sicheren Fallback-GOP auf der EFI ConsoleOut-Handle.

### 7.3 DXE Selbstdeaktivierungsschutz (`AdaUnlockDxe.efi`)
- Tritt bei der permanenten DXE-Treiber-Ausführung ein Fehler auf, deaktiviert sich der Treiber vor dem Herunterfahren automatisch in der NVRAM-Variable `DriverOrder`.
- Dadurch bootet der Rechner beim nächsten Start garantiert sauber in normales Windows, ohne in einer Boot-Schleife festzustecken.

### 7.4 Host-Testabdeckung (8 Test-Suiten)
Alle Kernkomponenten werden über gemockte Firmware-Services automatisiert validiert (`tests/Run-MsvcHostTests.ps1`):
1. `test_driver_nvram`: NVRAM Load-Optionen & DriverOrder-Verwaltung.
2. `test_boot_select`: Countdown-Timer & Tastatureingaben.
3. `test_csm_handover`: CSM-Erkennung & zweistufige GOP-Wiederherstellung.
4. `test_gpu_inventory`: Multi-GPU Inventory Parser (57 Checks).
5. `test_gpu_topology`: Negative Topology Isolation (44 Checks).
6. `test_gpu_discovery`: PCI I/O Protocol Adapter & Memory-Decode-Aktivierung (21 Checks).
7. `test_turing_local`: Turing TU102/TU104/TU106 Device-IDs & Binär-Hashes.
8. `test_s3_resume`: ACPI NVS Dispatcher & Wake-Handshake (18 Checks).

---

## 8. Multi-GPU Sequential Unlock Engine (ab v1.8.28)

### 8.1 Herausforderung bei Multi-GPU-Systemen
In Workstations und Multi-GPU-Setups (z. B. Dual RTX 4090 oder RTX 4090 + RTX 3090) verarbeitete der UEFI-Bootloader in älteren Versionen lediglich den primären Anzeige-Controller (`gpu[0]`). Sekundäre Karten blieben im uninitialisierten Werkszustand mit aktivem Rate-Limiter zurück.

### 8.2 Die sequentielle Unlock-Schleife
Ab v1.8.28 nutzen sowohl die Bootloader-Anwendung (`src/app.c`) als auch der permanente DXE-Treiber (`src/dxe_main.c`) dieselbe Multi-GPU-Schleife `GuUnlockAllGpus` (`src/multi_gpu.c`):
1. **Sequentielles Binding:** Der Bootloader iteriert über alle erkannten unterstützten NVIDIA-Grafikkarten (`0 .. device_count - 1`) und bindet jede Karte per `GuBindDevice`.
2. **Dynamischer Asset-Reload:** Bei heterogenen Kombinationen (z. B. Ada Lovelace + Ampere) erkennt der Bootloader den Architekturwechsel (`is_ampere` / `is_turing`) und lädt die spezifischen Firmware-Payloads (`AD102-ASSETS` vs `GA102-ASSETS`) nach, sobald sich die Architektur gegenüber den zuletzt geladenen Assets ändert.
3. **Fehlerbehandlung:** Schlagen bei einer sekundären Karte das Laden der Assets oder die nur lesende Vorprüfung (`GuPrepare`) fehl, wird sie übersprungen und bleibt im Werkszustand. Schlägt die eigentliche Freischaltung einer Karte fehl, bricht der Start über den normalen Fehlerpfad ab, weil die Karte dann schon verändert sein kann.
4. **S3 Boot Script Registrierung:** Nach jeder erfolgreich entsperrten GPU wird das S3-Boot-Script registriert (beim Bootloader nur im Modus `[P]`).
5. **Plan-Reset zwischen GPUs (`GuUnlockPrepareNextGpu`):** Da `Plan` in `unlock_release.c` datei-statisch ist, verblieben nach der ersten GPU gesetzte Statusflags (`Plan.mutated = TRUE`, `Plan.ready = TRUE`). Ohne Reset brach die Vorprüfung der zweiten Karte mit `EFI_ALREADY_STARTED` (`0x8000000000000014`) ab. `GuUnlockPrepareNextGpu()` setzt `Plan` und `GuHardwareVerified` vor der nächsten GPU nur dann sicher zurück, wenn die vorherige Karte vollständig, sicher und hardware-verifiziert abgeschlossen wurde oder unberührt blieb (refs #19).
6. **Reihenfolge bei Headless-Sekundärkarten:** Headless-GPUs (ohne eigenes Display) erfordern zur Vorprüfung einen aktiven GOP-Framebuffer einer anderen Karte. Bei mehreren GPUs wird die primäre Anzeige-GPU an das Ende der Abarbeitungsliste rotiert (`devices[device_count - 1]`). Dadurch werden sekundäre Headless-Karten entsperrt, solange die primäre Anzeige noch stabil aktiv ist.
7. **Console-Reconnect:** Erst nach Abschluss aller GPU-Transaktionen wird der primäre Display-Controller (`devices[device_count - 1]`) wieder mit dem UEFI-Console-Out-System verbunden.

---

## 9. Das 60-Sekunden Quick-Start Prinzip & Bereinigung des Script-Wildwuchses

### 9.1 Warum Skript-Wildwuchs schadet
Ein Kernversprechen des Projekts ist der **60-Second Quick Start**: Ein Benutzer muss innerhalb von einer Minute vom Download zur voll entsperrten GPU gelangen.
Durch das Hinzufügen vieler einzelner Werkzeuge (`Setup-FastResume.bat`, `Toggle-Sleep-Button.bat`, `Toggle-Turing-GSP.bat`, `Universal-Control-Center.cmd`) entstand im Wurzelverzeichnis ein unübersichtlicher Skript-Dschungel, der Verwirrung stiftete.

### 9.2 Die bereinigte Struktur
Ab v1.8.28 ist das Wurzelverzeichnis auf die wesentlichen 1-Klick-Einstiegspunkte reduziert:
- **`Setup.bat`:** 1-Klick Einrichtung (erstellt den USB-Stick oder installiert den DXE-Treiber).
- **`Check-GPU-Unlock.bat`:** 1-Klick CLI-Verifikation (Multi-GPU-fähig, zeigt TFLOPS, CUDA-Verhältnis, ReBAR, PCIe-Link und den letzten Aufwachzeitpunkt von Windows).
- **`Launch-Dashboard.bat`:** 1-Klick GUI Cockpit (vereint alle Statusanzeigen, Multi-GPU-Auswahl, Benchmark und Direkt-Buttons für alle Werkzeuge).
- **`Update-USB.bat`:** 1-Klick Schnell-Aktualisierung für bestehende USB-Sticks (schlägt den ersten Wechseldatenträger vor, fragt nach und aktualisiert dann Binärdateien & Skripte).

Alle spezifischen Hilfsskripte wurden sauber nach `tools/` ausgelagert. Im Dashboard stehen sie als direkte 1-Klick-Schaltflächen zur Verfügung, sodass kein Benutzer mehr manuell nach Hilfsskripten suchen muss. Im Wurzelverzeichnis verbleiben lediglich transparente 2-Zeilen-Forwarder für vollständige Abwärtskompatibilität.

---

## 10. Turing-Architektur & Rate-Limiter Realität (Issue #15 Analyse)

### 10.1 Analyse des TU102-Assets
Im Verzeichnis `TU102-ASSETS/` ist die Datei `v67-tu102-610.43.03.bin` mit `GA102-ASSETS/v67-ga102-610.43.03.bin` byte-identisch (identischer SHA-256-Hash).

### 10.2 Hardware-Befund
1. **Keine SS0/SS1 MSR-Register auf Turing:**
   Die Speed-Select-MSR-Register an den Adressen `0x0082381c` (SS0) und `0x00823820` (SS1) existieren auf der Turing-Mikroarchitektur physikalisch nicht (Leseoperationen liefern `0xbadf5040`). NVIDIA führte die hardwareseitige PLM-Drosselung dieser Register erst mit Ampere (GA102) und Ada Lovelace (AD102) ein.
2. **Ursache des Verhaltens in Issue #15:**
   Auf TU102 (RTX 2080 Ti / TITAN RTX) läuft der SEC2-Falcon-Handshake fehlerfrei durch, der Booter meldet Erfolg. Da jedoch keine SS0/SS1-Register existieren, bewirkt die Ampere-Payload auf Turing keine Freischaltung von Hardware-MSRs – die GEMM-Performance bleibt bei den Werks-54-TFLOPS.
3. **Der tatsächliche Turing-Boost:**
   Auf Turing wird der Leistungssprung (>85 bis 105 TFLOPS) primär durch die Aktivierung des modernen GSP-Firmware-Modus im Windows-Treiber erzielt:
   ```cmd
   reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000" /v EnableGpuFirmware /t REG_DWORD /d 1 /f
   ```
   Dies bindet `nvlddmkm.sys` an den integrierten RISC-V GSP-Coprozessor statt an den langsameren CPU-RM-Modus.
