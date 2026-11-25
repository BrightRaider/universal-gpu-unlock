---
name: Bug Report & Log Submission
about: Report a pre-boot halt, crash, or unexpected behavior (RUNxxxx.TXT required)
title: "[BUG]: "
labels: ["bug"]
assignees: []
---

### 🖥️ Hardware & Platform Information
- **GPU Model:** (e.g. RTX 4090, RTX 3080 10GB, RTX 2080 Ti)
- **Motherboard / Chipset:** (e.g. ASUS ROG X670E, MSI Z790, Gigabyte B550)
- **Release Version:** (e.g. v2.0.0)
- **Storage Configuration:** (e.g. 1x NVMe M.2, 2x SSDs, etc.)
- **Display Setup:** (e.g. 1x DP on target GPU, multi-monitor, etc.)

---

### 📝 Problem Description
A clear and concise description of what happened (e.g. halted at pre-flight, screen behavior, boot loop, etc.).

---

### 📄 Mandatory Diagnostic Log (`RUNxxxx.TXT`)
> [!IMPORTANT]
> **Please paste the FULL contents of your latest `RUN000x.TXT` log from the root of your USB flash drive below.**  
> We diagnose and resolve issues directly in code, but we require the exact MMIO register states, stage markers, and error return codes from your log to investigate!

```text
PASTE FULL CONTENTS OF RUNxxxx.TXT HERE
```

---

### 🚀 Community Benchmarks (Once Resolved / "Little Thank-You")
> [!TIP]
> **Once your issue is resolved and your unlock is running:**  
> As a small thank-you to the project and community, feel free to run **`Check-GPU-Unlock.bat`** (or your favorite PyTorch / ComfyUI / FLUX benchmark) and post a quick screenshot or your TFLOPS numbers here! It helps everyone see real-world performance gains across different hardware setups.
