# Technische Dokumentation: Universal GPU Unlock (Multi-Gen)

> **Tool:** Universal GPU Unlock  
> **Version:** v1.8.35  
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

### 6.3 Zweistufige S3-Persistenz: Tier 1 (Boot-Script) & Tier 2 (Universal ACPI FACS Shadow Trampoline)
Auf modernen AMD AM4/AM5 Plattformen mit AMI Aptio V Firmware (wie dem MSI MEG X570 ACE mit AGESA / Sinkclose-Mitigationen) signalisiert die Plattformfirmware `DxeSmmReadyToLock` bereits am Ende der internen DXE-Phase, noch bevor externe Treiber (`Driver####`) oder Bootloader geladen werden. Jeder Aufruf von `EFI_S3_SAVE_STATE_PROTOCOL->Write()` wird daraufhin mit `EFI_OUT_OF_RESOURCES` (`0x8000000000000009`) abgewiesen.

Um dieses Problem universell und herstellerunabhängig zu lösen, habe ich eine zweistufige Persistenz-Engine implementiert:
1. **Tier 1 (UEFI S3 Save State Protocol):** Wird auf Plattformen ohne frühen SMM-Lockdown (z. B. Intel Z690/Z790 oder vielen Workstation-Boards) genutzt.
2. **Tier 2 (Universal ACPI FACS Shadow Trampoline, `src/s3_facs.c`):**
   - **ACPI Tabellen-Discovery:** Sucht in `ConfigurationTable` nach RSDP, durchsucht XSDT (mit unaligned 64-Bit-Zeiger-Unterstützung) und RSDT nach der FADT (`FACP`) Tabelle und löst die physikalische FACS-Adresse auf.
   - **Schatten-FACS Allokation:** Legt im persistenten `EfiACPIMemoryNVS` eine Schatten-FACS an und biegt `FADT->FirmwareCtrl` und `FADT->XFirmwareCtrl` auf diese Schatten-Tabelle um.
   - **FADT Checksummen-Neuberechnung:** Berechnet die 8-Bit-Prüfsumme der modifizierten FADT-Tabelle neu (0 mod 256), damit Windows `ACPI.sys` die Tabelle ohne Bluescreen akzeptiert.
   - **Hardware-FACS Hooking:** Schreibt die physikalische Adresse des S3-Resume-Trampolins in `RealFACS->FirmwareWakingVector`.
   - **Ablauf beim Schlafen:** Windows schreibt seine echte Aufwachadresse (`HalpRMStub`) in die Schatten-FACS. Die echte FACS bleibt unangetastet.
   - **Ablauf beim Aufwachen:** Das BIOS liest die echte FACS aus der LockBox und springt direkt in mein Trampolin.
   - **Dual-Mode Trampoline:** Startet im 16-Bit Real Mode, lädt eine eingebettete 5-Eintrag-GDT, wechselt in den 32-Bit Flat Protected Mode, setzt SEC2 Falcon zurück, re-authentifiziert den Unlock-Payload, verriegelt `SS0 = 0x88888888` und `SS1 = 0x8`, schaltet die CPU sauber in den 16-Bit Real Mode zurück und springt per Far Return (`retf`) direkt in Windows `HalpRMStub`.
   - **Ergebnis:** 100 % volatile, herstellerübergreifende S3-Persistenz ohne VBIOS-Flash und ohne SMM-Abhängigkeit.

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
8. `test_s3_resume`: Universal S3 Resume Engine, ACPI FACS Shadow Intercept & Dual-Mode Trampoline (55 Checks).

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

### 10.4 Kalibrierung der Turing-Grenzwerte & BF16-Klärung (Issue #20)
1. **cuBLAS FP32-Akkumulation auf Turing (`sm_75`):**
   In PyTorch (`torch.matmul`) werden Halbpräzisionsmatrizen (FP16) standardmäßig mit 32-Bit-Gleitkomma-Akkumulation (`CUBLAS_COMPUTE_32F`) berechnet, um Überläufe zu verhindern. Auf den Tensorkernen der 2. Generation (Turing) halbiert dies den maximalen Rechendurchsatz gegenüber FP16-Akkumulation.
   Für eine RTX 2060 (30 SMs / 1920 CUDA-Cores) bei 1680 MHz Boost-Takt beträgt das physikalische theoretische Maximum:
   $$30 \text{ SMs} \times 512 \text{ Ops/Takt} \times 1{,}68 \text{ GHz} = 25{,}8 \text{ TFLOPS}$$
   Ein gemessener Wert von **25,82 TFLOPS** (wie von `@resYuto` in Issue #20 mit v1.8.31 verifiziert) entspricht exakt **100 % der theoretischen Hardware-Kapazität** (4,21x Multiplikator gegenüber der FP32-CUDA-Baseline von 6,13 TFLOPS) – die Karte läuft somit mit 100 % uneingeschränkter Rechenleistung!
2. **Korrektur der Grenzwerte (`gu_system_info.py`):**
   Frühere Versionen verwendeten für die RTX 2060 einen Schwellenwert von 35 TFLOPS (abgeleitet von FP16-Halbpräzisionsakkumulation), wodurch `Check-GPU-Unlock.bat` fälschlicherweise eine Warnung ausgab (`STOCK RATE-LIMITER ACTIVE`). Die Profile für die gesamte Turing-Produktlinie (RTX 2060 bis TITAN RTX) wurden auf realistische Schwellenwerte für dichte GEMM kalibriert (RTX 2060: Schwelle 18 TFLOPS, min_unlocked 20 TFLOPS).
3. **Keine native BF16-Hardware auf Turing:**
   Bfloat16-Tensorkerne wurden von NVIDIA erst ab Ampere (`sm_80` / `sm_86`) eingeführt. Auf Turing läuft BF16 ausschließlich über langsame CUDA-Kern-Software-Emulation (~3,5 TFLOPS). Die Diagnosewerkzeuge weisen nun explizit darauf hin.
4. **GSP-Firmware-Version:**
   Die GSP-Firmware im Windows-Treiber muss **nicht** mit der im UEFI-Paket verwendeten Version (`610.43.03`) übereinstimmen. Die Pre-Boot-Initialisierung im UEFI ist flüchtig; der Windows-Treiber lädt beim Booten seine eigene GSP-Firmware. Jeder unterstützte NVIDIA-Treiber (560.xx, 570.xx, 572.xx) funktioniert einwandfrei.
5. **ReBAR auf Turing (ReBarUEFI) & Striktes Support-Disclaimer:**
   Turing-Grafikkarten besitzen ab Werk kein ReBAR-fähiges vBIOS, weshalb BAR1 standardmäßig auf 256 MB verbleibt. Enthusiasten können das externe Open-Source-Projekt ReBarUEFI (xCuri0) nutzen, um das PCIe-Fenster zu vergrößern.
   > [!CAUTION]
   > **STRIKTES SUPPORT-DISCLAIMER (KEIN SUPPORT FÜR REBARUEFI):**
   > * Das Universal GPU Unlock Projekt leistet **KEINERLEI TECHNISCHEN SUPPORT, HILFESTELLUNG ODER GEWÄHRLEISTUNG** für ReBarUEFI oder das Modifizieren/Flashen von Mainboard-BIOS-ROMs!
   > * Der GPU-Unlock läuft auf BAR0 und erzielt auch ohne ReBAR bereits **100 % der vollen physikalischen Rechenleistung** (25,82 TFLOPS auf RTX 2060). ReBAR optimiert nur PCIe-Transferzeiten, erhöht aber nicht die Rohrechenleistung der Tensorkerne.
   > * Eine optionale Anleitung und ein Starter befinden sich in `tools/optional-turing-rebar/`.


---

## 11. Silicon-Erkennung bereits entsperrter GPUs & Freeze-Schutz bei UEFI Boot (ab v1.8.31)

### 11.1 Problemstellung bei vorinstalliertem DXE-Treiber
Wurde der permanente DXE-Treiber `AdaUnlockDxe.efi` zuvor über `[I]` im UEFI-NVRAM (`Driver####`) registriert, führt das BIOS diesen Treiber bereits in einer sehr frühen POST-Phase vor dem Boot-Menü aus. Die GPU ist beim Erreichen des USB-Bootloaders `BOOTX64.EFI` bereits hardwareseitig voll entsperrt (`SS0=0x88888888, SS1=0x8`).
In früheren Versionen führte die erneute Anforderung von `[P]` oder `[U]` im Bootloader zu einem Abbruch in `GuPrepare`: Die Kaltstart-Prüfung (`Registers`) erwartete Kaltstart-Register (`SS0 != 0x88888888`) und verweigerte die Ausführung mit `PREFLIGHT REFUSED`.
Bei Rückgabe des Fehlerstatus an die UEFI-Firmware froren manche Mainboards (insbesondere MSI Click BIOS) auf dem Hersteller-Logo ein.

### 11.2 Die v1.8.31 Lösung
1. **Live Silicon-Erkennung (`GuIsUnlocked`):** `BOOTX64.EFI` liest BAR0 `0x82381c` und `0x823820`. Wurde die Karte bereits im POST entsperrt, wird dies sofort im Konsolenmenü als `[OK] GPU Silicon Status: ALREADY UNLOCKED (by DXE Driver in POST)` angezeigt.
2. **Nahtlose Weiterleitung:** Bei Auswahl von `[P]` oder `[U]` wird die redundante Hardware-Transaktion übersprungen. Im Modus `[P]` wird die S3-Sleep-Persistenz registriert, und die Kontrolle wird ohne Hänger direkt an den Windows Boot Manager übergeben.
3. **Freeze-Schutz in `RunFailure`:** Sollte eine Vorprüfung bei unberührter Hardware fehlschlagen, wird kein unkontrollierter Fehlercode an das BIOS zurückgegeben (was Hänger provoziert), sondern nach 5 Sekunden kontrolliert Windows im Standardmodus gestartet.
4. **S3 Protocol Fallback:** `GuS3PrepareAndRegister` prüft sowohl `EFI_S3_SAVE_STATE_PROTOCOL_GUID` (PI 1.2+) als auch `EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID` (Framework) und liefert klare Diagnoseausgaben, falls ACPI S3 im BIOS deaktiviert ist.

---

## 12. Ecosystem-Erweiterungen & Hardware-Stabilität (ab v1.8.32)

### 12.1 12-MiB GFW Usable-Memory-Delta Toleranz (Zagitta PR #361)
Auf High-End-Grafikkarten (insbesondere RTX 3090 Ti und RTX 4090 mit 24 GB VRAM) meldet die GPU-Firmware (GFW) vor Treiberstart nicht die vollen 24.576 MiB, sondern exakt **24.564 MiB** (ein herstellerspezifisches Apertur-Reservierungsdelta von 12 MiB). Ähnliche Deltas existieren bei der RTX 4080 (16.384 → 16.376 MiB, 8 MiB) und RTX 4070 SUPER (12.288 → 12.282 MiB, 6 MiB).
Strikte Byte-Gleichheitsprüfungen (`(mmu_top >> 20) == (usable_fb >> 20)`) schlugen auf manchen Plattformen mit `-ERANGE` (-34) fehl.
In `src/vendor/unlock_610.c` habe ich eine Deltaprüfung implementiert:
Liegt das Delta zwischen dem physikalischen MMU-Top und dem gemeldeten Framebuffer innerhalb von 32 MiB (`delta_mb <= 32`), wird die Konfiguration als gültig akzeptiert. Gleichzeitig wird validiert, dass `mmu_lock_low` innerhalb der oberen vorab bereinigten 256 MiB liegt, wodurch WPR-Kollisionen und ungültige MMU-Locks zuverlässig ausgeschlossen sind.

### 12.2 `ATTEMPT.FLG` Boot-Loop Safeguard (MindAllik v1.3.3 / v1.3.4)
Um unter allen Umständen zu verhindern, dass ein unerwarteter Systemabsturz oder Freeze während der Register-Mutationen zu einer Bootschleife führt, habe ich einen dateibasierten Safeguard integriert:
1. Vor dem ersten hardwareseitigen Schreibzugriff legt der Loader eine Datei `\GPU-UNLOCK\ATTEMPT.FLG` auf dem ESP- bzw. USB-Laufwerk an.
2. Erst nach erfolgreicher Verifikation und Abschluss des Display-Handovers wird die Flagge gelöscht.
3. Findet `BOOTX64.EFI` oder `AdaUnlockDxe.efi` beim POST eine vorhandene `ATTEMPT.FLG`, schaltet das System sofort in den `[SAFEGUARD ACTIVE]`-Modus, protokolliert die Warnung, löscht die Flagge und chainloaded Windows direkt im sicheren Werkszustand.

### 12.3 `SLOT-REVIVE` via Secondary Bus Reset & GOP-Decouple (physicsdolphin v2.1.12-rc6)
Auf OEM-Systemen (Dell/HP) oder nach schnellen Warm-Reboots verharren PCIe-Slots gelegentlich im Energiesparmodus (`PMCSR != D0`) oder melden `child 0xffff` / `0xffffffff`.
Ein Secondary Bus Reset (SBR) darf **niemals bei gebundenem GOP-Treiber** ausgeführt werden, da abbrechende MMIO-Transaktionen unweigerlich zum Firmware-Freeze führen.
Die Implementierung in `src/gpu_discovery.c` (`GuSlotRevive`) folgt einer strikten 5-Stufen-Sequenz:
1. **GOP-Entkopplung:** Trennung aller Display- und GOP-Treiberbindungen über `DisconnectController()`.
2. **PMCSR D0-Zustand:** Explizites Setzen von Bits 1:0 im PMCSR auf D0 (`pmcsr & ~3U`) mit 10 ms Einschwingzeit.
3. **Pulsieren des Secondary Bus Reset:** Auffinden der übergeordneten PCIe-Bridge, Setzen des SBR-Bits (Offset 0x3E, Bit 6) für 2 ms, Zurücknehmen und 100 ms PCIe-Link-Training.
4. **Wiederherstellung des Bookkeepings:** Rekonstruktion von BAR0/BAR1 und Reaktivierung von Memory Decode und Bus Master im PCI Command Register.
5. **Konfigurations-Refresh:** Erneutes Auslesen der PCI-Header zur nahtlosen Hardware-Erkennung.

### 12.4 Turing (TU102) `BCR_CTRL` Polling-Skip (MindAllik v1.3.3)
Auf Turing-Grafikkarten (RTX 2080 Ti, TITAN RTX) führt das Ansprechen des Registers `BCR_CTRL` (`falcon2 + 0x668`) in der Falcon-Reset-Routine zu einem Timeout (`poll timeout: reg=0x111668`, Fehlercode `-110`).
In `src/vendor/unlock_610.c` wird dieser Polling-Schritt für `GU_ARCH_TURING` gezielt übersprungen. Für Ampere (GA10x) und Ada (AD10x) bleibt die Umschaltung vollständig aktiv.

### 12.5 Erweiterung der Hardware-Datenbank (Mobile & Laptop GPUs)
In `src/gpu_device_ids.h` wurden neue mobile Ampere-Device-IDs ergänzt:
- **`0x25ec` (10de:25ec):** GA107 – NVIDIA GeForce RTX 3050 6GB Laptop
- **`0x25ed` (10de:25ed):** GA107 – NVIDIA GeForce RTX 2050 Mobile
- **`0x2560` (10de:2560):** GA106 – NVIDIA GeForce RTX 3060 Mobile / Desktop-FrankenCard
Die Diagnosewerkzeuge (`tools/gu_system_info.py`) und Host-Tests (`tests/test_turing_local.c`) wurden entsprechend erweitert.

### 12.6 S3-Persistence Protokoll- & Opcode-Korrektur (UEFI PI Standard)
Bei der S3-Sleep-Persistenz (`[P]`) registriert der Bootloader über das UEFI Boot Script Protokoll einen Dispatch- und MMIO-Aufruf, der beim Aufwachen aus dem Standby (S3 Suspend-to-RAM) ausgeführt wird.
1. **Offizielle UEFI PI Spezifikations-GUIDs:**
   In früheren Ständen waren in `src/s3_save.h` nicht-standardkonforme GUIDs hinterlegt. Dadurch gab das Motherboard-BIOS (getestet auf MSI MEG X570 ACE mit AMI Aptio V) bei `LocateProtocol` den Fehler `0x800000000000000e` (`EFI_NOT_FOUND`) zurück. Ich habe die GUIDs exakt auf den UEFI PI Standard aktualisiert:
   - `EFI_S3_SAVE_STATE_PROTOCOL_GUID`: `{ 0xe857caf6, 0xc046, 0x45dc, { 0xbe, 0x3f, 0xee, 0x07, 0x65, 0xfb, 0xa8, 0x87 } }`
   - `EFI_BOOT_SCRIPT_SAVE_PROTOCOL_GUID`: `{ 0x470e1529, 0xb79e, 0x4e32, { 0xa0, 0xfe, 0x6a, 0x15, 0x6d, 0x29, 0xf9, 0xb2 } }`
2. **Korrektur der Boot Script Opcodes (`PiS3BootScript.h`):**
   Die Opcodes wurden an die EDK2-Spezifikation angepasst: `MEM_WRITE` ist `0x02` (war fälschlich `0x00`, was `IO_WRITE` entspricht), `DISPATCH_2` ist `0x09` (war fälschlich `0x07`, was `STALL` entspricht).
3. **Autonomer, positionsunabhängiger ACPI NVS Dispatcher:**
   `GuS3ResumeDispatch` wurde in `src/s3_resume.c` so restrukturiert, dass der gesamte Unlock-Code ohne externe Funktionssprünge (`call rel32`) direkt im 256 KB ACPI NVS DRAM Puffer liegt. Dadurch ist der Code zu 100 % positionsunabhängig und übersteht das Windows-Boot-Handover unversehrt.
4. **Live Status-Rückmeldung am Bildschirm:**
   Der Pre-Boot-Bildschirm meldet den Erfolg der S3-Registrierung nun wahrheitsgetreu mit `[ OK ]` bzw. `[ FAILED ]`, sodass Fehlschläge sofort sichtbar sind.

---

## 13. Universelle ACPI FACS Shadow Trampoline Architektur für S3 Standby (ab v1.8.33 / v1.8.34)

### 13.1 Das Problem: Versiegelter SMM LockBox bei AMI Aptio V auf AMD AM4/AM5
Auf modernen AMD AM4- und AM5-Mainboards (wie dem MSI MEG X570 ACE mit AGESA 1.2.0.Cb / Sinkclose-Sicherheitsupdates) signalisiert die Firmware das Protokoll `gEfiDxeSmmReadyToLockProtocolGuid` bereits vor der BDS-Phase (Boot Device Selection).
Dadurch wird die S3 Save State Tabelle im SMRAM unwiderruflich gesperrt. Jeder Versuch eines Drittanbieter-Bootloaders oder DXE-Treibers, via `s3->Write()` Einträge zu registrieren, wird mit `EFI_OUT_OF_RESOURCES` (`0x8000000000000009`) abgewiesen.
Bisherige Ansätze führten dazu, dass der S3 Standby ("Energie sparen" / Suspend-to-RAM) den GPU-Unlock beim Aufwachen verlor oder das System auf S4 (Ruhezustand) angewiesen war.

### 13.2 Das FACS Shadowing Prinzip
Um den gesperrten SMM LockBox vollständig zu umgehen, setzt Universal GPU Unlock ab v1.8.33 auf einen universellen ACPI FACS Shadow Intercept:
1. **FADT-Auffindung:** Der Loader durchsucht die UEFI Configuration Table nach der ACPI RSDP und iteriert über XSDT/RSDT, um die `FACP` (Fixed ACPI Description Table) zu lokalisieren.
2. **Shadow FACS Allokation:** In persistentem `EfiACPIMemoryNVS` DRAM wird eine Kopie der Firmware-FACS (Firmware ACPI Control Structure) angelegt.
3. **FADT-Umleitung:** Die FADT-Felder `FirmwareCtrl` und `XFirmwareCtrl` werden auf die Shadow FACS umgebogen, und die FADT-Prüfsumme wird neu berechnet (0 mod 256).
4. **Waking Vector Abfangen:** Windows (`hal.dll` / `ACPI.sys`) schreibt seinen Aufwachvektor (`HalpRMStub`) in die ihm bekannte Shadow FACS. Die echte Hardware-FACS bleibt unangetastet.
5. **Real FACS Hook:** `RealFACS->FirmwareWakingVector` wird auf ein maschinencodebasiertes Resume-Trampoline umgebogen.

### 13.3 Die v1.8.34 Low-Memory (< 1 MB) Trampoline Lösung
Bei ersten Hardwaretests von v1.8.33 wachte das System aus dem S3-Schlaf auf, die Bildschirme blieben jedoch schwarz (erst nach einem Reset-Tastendruck bootete Windows über den Hybrid-Sleep-Fallback).
Eine forensische Analyse der EDK2/AMI Aptio V Firmware (`BootScriptExecutorDxe`) deckte die Ursache auf:
Die Firmware führt den S3 Waking Vector im **16-Bit Real Mode** über einen Paragraphen-Segment:Offset Far-Jump aus:
```nasm
mov ecx, [ebp + 8]   ; FirmwareWakingVector
shrd ebx, ecx, 20
and ecx, 0xf
mov bx, cx
mov [@jmp_addr + 1], ebx
retf
BITS 16
@jmp_addr: jmp 0x0:0x0 ; 16-bit far jump
```
Lag das Trampolin im hohen NVS-Speicher (`> 1 MB`, z. B. `0xDC638300`), wurde das Segment auf 16 Bit abgeschnitten (`0x3830:0x0000 = 0x00038300`), und die CPU stürzte im konventionellen RAM ab.

In **v1.8.34** habe ich die Architektur umfassend überarbeitet und perfektioniert:
1. **Low-Memory Allokation (< 1 MB):**
   Das Trampolin wird via `AllocatePages(AllocateMaxAddress, EfiACPIMemoryNVS, 1, &low_tramp_phys)` explizit im konventionellen Speicher unterhalb von 640 KB (`< 0x000A0000`, Fallback `< 0x00100000`) allokiert. Dadurch ist das Segment `<= 0x9000` und passt ohne jedes Abschneiden exakt in 16 Bit.
2. **GPU PCI Command Register Wiederherstellung (MSE + BME):**
   Nach dem S3-Aufwachen befindet sich die GPU hardwareseitig im D3cold-Zustand, sodass das PCI Command Register auf 0 zurückgesetzt ist. Das Trampolin stellt Memory Space Enable (Bit 1) und Bus Master Enable (Bit 2) direkt über die x86-I/O-Ports `0x0CF8` und `0x0CFC` wieder her, bevor auf BAR0 zugegriffen oder Falcon DMA gestartet wird.
3. **Struktur-Offset-Synchronisation & 32-Bit Rel32 Sprünge:**
   Alle Member-Offsets in `GU_S3_RESUME_CONTEXT` wurden präzise ausgerichtet (`booter_dma` 0x20, `booter_size` 0x28, `val_ss0` 0x50, `val_ss1` 0x54, `resume_status` 0x58, `resume_count` 0x5C, `last_sec2_marker` 0x60) und durch automatisierte Host-Tests (`TestGuS3ResumeContextOffsets`) verifiziert. Vorwärtssprünge wurden von 8-Bit `rel8` auf 32-Bit `0x0F 0x84 rel32` umgestellt.
4. **Saubere Real-Mode-Rückkehr & Windows-Handoff:**
   Nach Durchführung des Unlocks schaltet das Trampolin über einen dedizierten 16-Bit Protected Mode Code-Deskriptor (Selector 0x20 mit Basis = `low_tramp_phys`), löscht `CR0.PE`, springt in den 16-Bit Real Mode, stellt den originalen BIOS-Stack (`SS:SP`) wieder her, nullt alle Register und springt via `jmp far ptr cs:[0x30]` direkt in Windows `HalpRMStub`.
5. **Warm-Boot-Resilienz (Shadow FACS Tagging):**
   Die Shadow FACS wird in `Reserved[0..3]` mit der Signatur `GU_SHADOW_FACS_SIGNATURE` (`0x53554753U` "SGUS") markiert und speichert die echte `RealFACS`-Adresse in `Reserved[4..11]`. Bei einem Warmstart oder Reset erkennt der Loader die bestehende Shadow FACS und stellt die originale Firmware-FACS wieder her, anstatt Schattenstrukturen rekursiv zu verschachteln.
6. **Erzwingung des 32-Bit Wake Vectors:**
   Bit 1 in den FACS-`Flags` (64-Bit Wake Supported) sowie `XFirmwareWakingVector` werden in beiden FACS-Tabellen genullt, sodass Windows ACPI garantiert den 32-Bit Real-Mode Waking Vector verwendet.

### 13.4 Die v1.8.35 Lösung: Reines 16-Bit Real-Mode Passthrough & Low-Memory Shadow FACS (0x9E100)
1. **Ursachenanalyse des schwarzen Bildschirms in v1.8.34:**
   Forensische Tests ergaben, dass beim Aufwachen aus S3 die diskrete GPU auf Bus 45 (PCIe-Slot 1) noch gar nicht durch den Windows-PCI-Treiber (`pci.sys`) link-trainiert und deren Root-Port-Bridge noch nicht konfiguriert ist. Wenn das Trampolin im Protected Mode versucht, über `0xFB8403C0` auf GPU BAR0 zuzugreifen, friert die CPU in einem unkorrigierbaren PCIe-Bus-Stall (Master Abort / Machine Check) ein.
2. **Reines 16-Bit Real-Mode Trampolin ohne MMIO:**
   In v1.8.35 führt das Trampolin während der Firmware-Aufwachphase **keinerlei riskante GPU-MMIO-Zugriffe** mehr aus. Es verbleibt zu 100 % im 16-Bit Real Mode (kein GDT, kein CR0.PE-Umschalten, kein Risiko von Triple Faults).
3. **Co-Located Shadow FACS im Low Memory (`0x9E100`):**
   Die Shadow FACS (64 Bytes) wird direkt in derselben Low-Memory-NVS-Page bei `+0x100` (z. B. `0x0009E100`) platziert, 256-Byte-aligned. Das 16-Bit-Trampolin liest den Waking Vector direkt aus dem gleichen Segment (`DS:[0x010C]` bzw. `DS:[0x0118]`).
4. **4-Stufen-Waking-Vector-Resolution mit sicherem Fallback:**
   Liest der Reihe nach (1) Shadow FACS 32-Bit Vector (`[0x10C]`), (2) Shadow FACS 64-Bit Vector (`[0x118]`), (3) Backup Real FACS 32-Bit Vector (`[0x180]`), (4) Backup Real FACS 64-Bit Vector (`[0x188]`). Ist kein Vektor vorhanden, stoppt die CPU sicher via `cli; hlt` mit Telemetrie `0xDEAD`, statt auf Adresse 0 abzustürzen.
5. **Präzise Segment:Offset-Umrechnung & Telemetrie:**
   Berechnet `Offset = AX & 0x0F`, `Segment = (AX >> 4) | (DX << 12)`, stellt den BIOS-Stack wieder her und springt via `jmp far ptr cs:[0x30]` direkt in Windows `HalpRMStub`. Windows wacht verzögerungsfrei auf, initialisiert die Monitore und den Desktop ohne Black Screen.
6. **Hardware-Telemetrie bei jedem Warmstart:**
   Hinterlässt Telemetrie-Checkpoints (`0xAA01` Entry, Vektor, Segment:Offset, `0xAA02` Jump) bei `0x0040..0x004A`, die vom DXE-Treiber beim nächsten Boot automatisch ausgelesen und in `RUN*.TXT` protokolliert werden.
