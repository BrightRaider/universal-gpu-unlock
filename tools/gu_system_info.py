"""
Shared system checks and GPU profiles for quick_check.py and gpu_dashboard.py.
SPDX-License-Identifier: GPL-2.0-only
"""
import ctypes
import os
import re
import subprocess

# Number CUDA devices in PCI bus order, the same order nvidia-smi uses, so a
# torch device index can be passed to "nvidia-smi -i". Must be set before torch
# initializes CUDA; both tools import this module first.
os.environ.setdefault("CUDA_DEVICE_ORDER", "PCI_BUS_ID")

def get_firmware_mode():
    try:
        ft = ctypes.c_uint()
        if hasattr(ctypes, "windll") and hasattr(ctypes.windll, "kernel32"):
            if ctypes.windll.kernel32.GetFirmwareType(ctypes.byref(ft)):
                if ft.value == 2:
                    return "UEFI (Pure)", True
                elif ft.value == 1:
                    return "Legacy BIOS (CSM Active)", False
    except Exception:
        pass
    if os.path.exists("/sys/firmware/efi"):
        return "UEFI", True
    return "Unknown", None

def get_rebar_info(gpu_index=0):
    candidates = [
        "nvidia-smi",
        r"C:\Windows\System32\nvidia-smi.exe",
        r"C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe",
    ]
    for exe in candidates:
        try:
            cmd = [exe, "-i", str(gpu_index), "-q", "-d", "MEMORY"]
            out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
            m_bar = re.search(r"BAR1 Memory Usage\s*\n\s*Total\s*:\s*(\d+)\s*MiB", out)
            m_fb = re.search(r"FB Memory Usage\s*\n\s*Total\s*:\s*(\d+)\s*MiB", out)
            bar1 = int(m_bar.group(1)) if m_bar else None
            fb = int(m_fb.group(1)) if m_fb else None
            if bar1 is not None:
                is_active = bar1 > 1024 or (fb is not None and bar1 >= fb)
                return bar1, fb, is_active
        except Exception:
            continue
    return None, None, None

CARD_PROFILES = {
    "4090": {"min_unlocked": 200.0, "threshold": 175.0, "stock_typical": 135.0},
    "4080": {"min_unlocked": 115.0, "threshold": 98.0, "stock_typical": 80.0},
    "4070 ti super": {"min_unlocked": 100.0, "threshold": 88.0, "stock_typical": 70.0},
    "4070 ti": {"min_unlocked": 95.0, "threshold": 82.0, "stock_typical": 64.0},
    "4070 super": {"min_unlocked": 88.0, "threshold": 76.0, "stock_typical": 60.0},
    "4070": {"min_unlocked": 78.0, "threshold": 68.0, "stock_typical": 52.0},
    "4060 ti": {"min_unlocked": 55.0, "threshold": 48.0, "stock_typical": 36.0},
    "4060": {"min_unlocked": 40.0, "threshold": 34.0, "stock_typical": 26.0},
    "3090 ti": {"min_unlocked": 125.0, "threshold": 105.0, "stock_typical": 78.0},
    "3090": {"min_unlocked": 115.0, "threshold": 95.0, "stock_typical": 72.0},
    "3080 ti": {"min_unlocked": 110.0, "threshold": 92.0, "stock_typical": 70.0},
    "3080": {"min_unlocked": 95.0, "threshold": 80.0, "stock_typical": 62.0},
    "3070 ti": {"min_unlocked": 75.0, "threshold": 62.0, "stock_typical": 48.0},
    "3070": {"min_unlocked": 65.0, "threshold": 54.0, "stock_typical": 40.0},
    "3060 ti": {"min_unlocked": 55.0, "threshold": 46.0, "stock_typical": 34.0},
    "3060": {"min_unlocked": 40.0, "threshold": 32.0, "stock_typical": 24.0},
    "3050": {"min_unlocked": 28.0, "threshold": 22.0, "stock_typical": 15.0},
    # Turing Architecture (sm_75): 2nd-gen Tensor Cores with cuBLAS FP32 Accumulator
    # Theoretical Peak = SMs * 512 ops/cycle * Boost GHz (FP32 accumulate HMMA)
    "titan rtx": {"min_unlocked": 50.0, "threshold": 45.0, "stock_typical": 32.0},
    "2080 ti": {"min_unlocked": 45.0, "threshold": 40.0, "stock_typical": 28.0},
    "2080 super": {"min_unlocked": 35.0, "threshold": 31.0, "stock_typical": 22.0},
    "2080": {"min_unlocked": 32.0, "threshold": 28.0, "stock_typical": 20.0},
    "2070 super": {"min_unlocked": 28.0, "threshold": 25.0, "stock_typical": 18.0},
    "2070": {"min_unlocked": 24.0, "threshold": 21.0, "stock_typical": 15.0},
    "2060 super": {"min_unlocked": 23.0, "threshold": 20.0, "stock_typical": 14.0},
    "2060": {"min_unlocked": 20.0, "threshold": 18.0, "stock_typical": 13.0},
    "cmp 50hx": {"min_unlocked": 34.0, "threshold": 30.0, "stock_typical": 22.0},
}


def get_last_wake_time():
    """Time of the last wake from sleep or hibernation, or None.

    This only says that Windows woke up at some point. It does not show that the
    GPU unlock survived the sleep."""
    if os.name != "nt":
        return None
    try:
        cmd = [
            "powershell", "-NoProfile", "-Command",
            "(Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-Power-Troubleshooter'} -MaxEvents 1 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty TimeCreated).ToString('yyyy-MM-dd HH:mm:ss')"
        ]
        res = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
        return res or None
    except Exception:
        return None
