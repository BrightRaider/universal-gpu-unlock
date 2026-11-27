"""
Shared system checks, Power Limit control, S3 persistence detection, and GPU profiles
for quick_check.py and gpu_dashboard.py.
SPDX-License-Identifier: GPL-2.0-only
"""
import ctypes
from contextlib import nullcontext
from ctypes import wintypes
import json
import os
import re
import shutil
import subprocess
import time

# Number CUDA devices in PCI bus order, the same order nvidia-smi uses, so a
# torch device index can be passed to "nvidia-smi -i". Must be set before torch
# initializes CUDA; both tools import this module first.
os.environ.setdefault("CUDA_DEVICE_ORDER", "PCI_BUS_ID")

WAKE_TASK_NAME = "UniversalGpuUnlock_ReapplyPL"
EXTERNAL_LOAD_THRESHOLD = 15.0


def find_nvidia_smi():
    """Return the first usable full nvidia-smi executable path, or None."""
    candidates = []
    which_path = shutil.which("nvidia-smi")
    if which_path:
        candidates.append(which_path)
    candidates.extend([
        r"C:\Windows\System32\nvidia-smi.exe",
        r"C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe",
        "nvidia-smi",
    ])
    for exe in candidates:
        try:
            _run_hidden([exe, "--version"])
            return exe
        except Exception:
            continue
    return None


def _run_hidden(cmd):
    """Run subprocess without flashing a console window on Windows."""
    kwargs = {"text": True, "stderr": subprocess.PIPE}
    if os.name == "nt":
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        kwargs["startupinfo"] = si
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return subprocess.check_output(cmd, **kwargs)


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
    exe = find_nvidia_smi()
    if not exe:
        return None, None, None
    try:
        out = _run_hidden([exe, "-i", str(gpu_index), "-q", "-d", "MEMORY"])
        m_bar = re.search(r"BAR1 Memory Usage\s*\n\s*Total\s*:\s*(\d+)\s*MiB", out)
        m_fb = re.search(r"FB Memory Usage\s*\n\s*Total\s*:\s*(\d+)\s*MiB", out)
        bar1 = int(m_bar.group(1)) if m_bar else None
        fb = int(m_fb.group(1)) if m_fb else None
        if bar1 is not None:
            is_active = bar1 > 1024 or (fb is not None and bar1 >= fb)
            return bar1, fb, is_active
    except Exception:
        pass
    return None, None, None


def _parse_float(val):
    try:
        s = str(val).strip()
        if not s or s.upper() in ("N/A", "[N/A]", "NOT SUPPORTED"):
            return None
        return float(s)
    except Exception:
        return None


def get_power_limit_info(gpu_index=0):
    """Query draw, current/default/min/max power limit and compute Eco/Stock/Max markers."""
    result = {
        "draw_w": None,
        "current_w": None,
        "default_w": None,
        "min_w": None,
        "max_w": None,
        "eco_w": None,
        "stock_w": None,
        "adjustable": False,
        "error": None,
    }
    exe = find_nvidia_smi()
    if not exe:
        result["error"] = "nvidia-smi not found"
        return result

    try:
        out = _run_hidden([
            exe, "-i", str(gpu_index),
            "--query-gpu=power.draw,power.limit,power.default_limit,power.min_limit,power.max_limit",
            "--format=csv,noheader,nounits",
        ])
        parts = [p.strip() for p in out.strip().splitlines()[0].split(",")]
        if len(parts) >= 5:
            result["draw_w"] = _parse_float(parts[0])
            result["current_w"] = _parse_float(parts[1])
            result["default_w"] = _parse_float(parts[2])
            result["min_w"] = _parse_float(parts[3])
            result["max_w"] = _parse_float(parts[4])
    except Exception:
        pass

    # Fallback to "-q -d POWER" when CSV fields are unavailable on older drivers.
    if result["current_w"] is None or result["max_w"] is None:
        try:
            q_out = _run_hidden([exe, "-i", str(gpu_index), "-q", "-d", "POWER"])
            patterns = {
                "draw_w": r"(?:Average Power Draw|Power Draw)\s*:\s*([0-9.]+)\s*W",
                "current_w": r"(?:Current Power Limit|Power Limit)\s*:\s*([0-9.]+)\s*W",
                "default_w": r"Default Power Limit\s*:\s*([0-9.]+)\s*W",
                "min_w": r"Min Power Limit\s*:\s*([0-9.]+)\s*W",
                "max_w": r"Max Power Limit\s*:\s*([0-9.]+)\s*W",
            }
            for key, pat in patterns.items():
                if result[key] is None:
                    m = re.search(pat, q_out)
                    if m:
                        result[key] = _parse_float(m.group(1))
        except Exception as e:
            result["error"] = str(e)

    min_w = result["min_w"]
    max_w = result["max_w"]
    def_w = result["default_w"] if result["default_w"] is not None else result["current_w"]
    if min_w is not None and max_w is not None and max_w > min_w:
        result["adjustable"] = True
        base_w = def_w if def_w is not None else (min_w + max_w) / 2.0
        stock_w = max(int(round(min_w)), min(int(round(max_w)), int(round(base_w))))
        eco_w = max(int(round(min_w)), min(int(round(max_w)), int(round(base_w * 0.78))))
        result["stock_w"] = stock_w
        result["eco_w"] = eco_w
    elif def_w is not None:
        result["stock_w"] = int(round(def_w))

    return result


def _is_windows_admin():
    if os.name != "nt":
        return os.geteuid() == 0 if hasattr(os, "geteuid") else False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


class _SHELLEXECUTEINFOW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("fMask", wintypes.ULONG),
        ("hwnd", wintypes.HWND),
        ("lpVerb", wintypes.LPCWSTR),
        ("lpFile", wintypes.LPCWSTR),
        ("lpParameters", wintypes.LPCWSTR),
        ("lpDirectory", wintypes.LPCWSTR),
        ("nShow", ctypes.c_int),
        ("hInstApp", wintypes.HINSTANCE),
        ("lpIDList", ctypes.c_void_p),
        ("lpClass", wintypes.LPCWSTR),
        ("hkeyClass", wintypes.HKEY),
        ("dwHotKey", wintypes.DWORD),
        ("hMonitor", wintypes.HANDLE),
        ("hProcess", wintypes.HANDLE),
    ]


def _run_elevated_exe(exe_path, params, timeout_ms=20000):
    """Run an executable directly with UAC elevation via ShellExecuteExW (no shell quoting issues).

    Returns (launched: bool, exit_code: int|None, cancelled: bool, err: str).
    """
    if os.name != "nt":
        return False, None, False, "Elevation only supported on Windows."

    SEE_MASK_NOCLOSEPROCESS = 0x00000040
    SEE_MASK_FLAG_NO_UI = 0x00000400
    ERROR_CANCELLED = 1223

    sei = _SHELLEXECUTEINFOW()
    sei.cbSize = ctypes.sizeof(_SHELLEXECUTEINFOW)
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI
    sei.hwnd = None
    sei.lpVerb = "runas"
    sei.lpFile = str(exe_path)
    sei.lpParameters = str(params)
    sei.lpDirectory = None
    sei.nShow = 0  # SW_HIDE

    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(_SHELLEXECUTEINFOW)]
    shell32.ShellExecuteExW.restype = wintypes.BOOL
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    ok = shell32.ShellExecuteExW(ctypes.byref(sei))
    if not ok:
        err_code = ctypes.get_last_error()
        if err_code == ERROR_CANCELLED:
            return False, None, True, "UAC elevation was cancelled."
        return False, None, False, f"ShellExecuteExW failed (WinError {err_code})."

    exit_code = None
    if sei.hProcess:
        try:
            kernel32.WaitForSingleObject(sei.hProcess, timeout_ms)
            code = wintypes.DWORD(0)
            if kernel32.GetExitCodeProcess(sei.hProcess, ctypes.byref(code)):
                exit_code = int(code.value)
        finally:
            kernel32.CloseHandle(sei.hProcess)
    return True, exit_code, False, ""


def _run_nvidia_smi_batch(cmd_arg_lists, allow_uac=True, action_label="GPU tuning"):
    """Execute one or more nvidia-smi invocations cleanly, elevating once via UAC if needed."""
    exe = find_nvidia_smi()
    if not exe:
        return False, "nvidia-smi not found."
    if not cmd_arg_lists:
        return True, "No changes requested."

    clean_lists = [[str(tok) for tok in args] for args in cmd_arg_lists]
    if os.name != "nt" or _is_windows_admin():
        try:
            kwargs = {"capture_output": True, "text": True}
            if os.name == "nt":
                si = subprocess.STARTUPINFO()
                si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                kwargs["startupinfo"] = si
                kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            for args in clean_lists:
                proc = subprocess.run([exe] + args, **kwargs)
                if proc.returncode != 0:
                    detail = (proc.stderr or proc.stdout or f"nvidia-smi exited with code {proc.returncode}").strip()
                    return False, detail
            return True, ""
        except Exception as e:
            return False, str(e)

    if os.name == "nt" and allow_uac:
        if len(clean_lists) == 1:
            params = " ".join(clean_lists[0])
            launched, exit_code, cancelled, uac_err = _run_elevated_exe(exe, params)
            if cancelled:
                return False, "Administrator permission (UAC) was cancelled."
            if launched and (exit_code is None or exit_code == 0):
                return True, ""
            return False, uac_err or f"Elevated nvidia-smi exited with code {exit_code}."

        cfg_dir = get_config_dir()
        os.makedirs(cfg_dir, exist_ok=True)
        batch_path = os.path.join(cfg_dir, "elevated_tune.cmd")
        lines = ["@echo off"]
        for args in clean_lists:
            lines.append(f'"{exe}" ' + " ".join(args))
            lines.append("if errorlevel 1 exit /b 1")
        with open(batch_path, "w", encoding="ascii", errors="replace") as f:
            f.write("\r\n".join(lines) + "\r\n")
        try:
            comspec = os.environ.get("ComSpec") or r"C:\Windows\System32\cmd.exe"
            launched, exit_code, cancelled, uac_err = _run_elevated_exe(
                comspec, f'/d /c "{batch_path}"'
            )
            if cancelled:
                return False, "Administrator permission (UAC) was cancelled."
            if launched and (exit_code is None or exit_code == 0):
                return True, ""
            return False, uac_err or f"Elevated {action_label} exited with code {exit_code}."
        finally:
            try:
                if os.path.exists(batch_path):
                    os.remove(batch_path)
            except Exception:
                pass

    return False, f"Administrator permissions required for {action_label}."


def set_power_limit(gpu_index, watts, allow_uac=True):
    """Clamp watts to [min_w, max_w], apply via nvidia-smi -pl, and read back."""
    info = get_power_limit_info(gpu_index)
    if not info["adjustable"]:
        return False, info, "Power limit adjustment is locked or unavailable on this GPU."

    min_w = int(round(info["min_w"]))
    max_w = int(round(info["max_w"]))
    target_w = max(min_w, min(max_w, int(round(float(watts)))))

    applied, err_msg = _run_nvidia_smi_batch(
        [["-i", str(int(gpu_index)), "-pl", str(target_w)]],
        allow_uac=allow_uac,
        action_label="power limit adjustment",
    )
    if not applied and "cancelled" in err_msg.lower():
        return False, info, err_msg

    readback = get_power_limit_info(gpu_index)
    actual_w = readback.get("current_w")
    if actual_w is not None and abs(actual_w - target_w) <= 2.0 and applied:
        return True, readback, f"Power limit set to {actual_w:.0f} W (readback verified)."

    detail = err_msg or f"Readback is {actual_w} W instead of {target_w} W (admin permissions may be required)."
    return False, readback, detail


def _parse_supported_graphics_clocks(raw_text):
    """Parse unique sorted Graphics MHz steps from nvidia-smi -q -d SUPPORTED_CLOCKS."""
    vals = []
    seen = set()
    for m in re.finditer(r"^\s*Graphics\s*:\s*(\d+)\s*MHz\s*$", raw_text or "", re.MULTILINE):
        mhz = int(m.group(1))
        if mhz > 0 and mhz not in seen:
            seen.add(mhz)
            vals.append(mhz)
    vals.sort()
    return vals


def snap_clock_mhz(target_mhz, supported_clocks, min_mhz, max_mhz):
    """Clamp target MHz and snap to nearest supported discrete clock step if known."""
    target = max(int(min_mhz), min(int(max_mhz), int(round(float(target_mhz)))))
    if supported_clocks:
        return min(supported_clocks, key=lambda c: (abs(c - target), c))
    step = 15
    return max(int(min_mhz), min(int(max_mhz), int(round(target / float(step))) * step))


def resolve_clock_cap_presets(gpu_name, min_mhz, max_mhz, supported_clocks=None):
    """Compute architecture-aware Max-Efficiency and Speed-Sweetspot SM clock caps."""
    low_name = (gpu_name or "").lower()
    is_ada = any(tok in low_name for tok in ("rtx 40", "4090", "4080", "4070", "4060", "rtx 6000 ada", "rtx 5000 ada", "rtx 4000 ada", "l40"))
    if is_ada and max_mhz >= 2500:
        eff_raw = 2300
        sweet_raw = 2450
    elif any(tok in low_name for tok in ("rtx 30", "3090", "3080", "3070", "3060", "3050", "cmp 90", "rtx 20", "titan rtx", "2080", "2070", "2060", "cmp 40", "cmp 50")):
        eff_raw = int(round(max_mhz * 0.78))
        sweet_raw = int(round(max_mhz * 0.85))
    else:
        eff_raw = int(round(max_mhz * 0.76))
        sweet_raw = int(round(max_mhz * 0.82))

    eff = snap_clock_mhz(eff_raw, supported_clocks, min_mhz, max_mhz)
    sweet = snap_clock_mhz(sweet_raw, supported_clocks, min_mhz, max_mhz)
    if sweet < eff:
        sweet = eff
    return {
        "efficiency_mhz": eff,
        "sweetspot_mhz": sweet,
        "is_ada": is_ada,
    }


def get_clock_cap_info(gpu_index=0, gpu_name=""):
    """Query live SM clock, SW Power Cap state, supported clock range, and saved lock state."""
    info = {
        "name": gpu_name or "",
        "current_sm_mhz": None,
        "max_sm_mhz": None,
        "min_sm_mhz": None,
        "sw_power_cap": None,
        "supported_clocks": [],
        "adjustable": False,
        "efficiency_mhz": None,
        "sweetspot_mhz": None,
        "is_ada": False,
        "lock_enabled": False,
        "locked_min_mhz": None,
        "locked_max_mhz": None,
        "error": None,
    }
    exe = find_nvidia_smi()
    if not exe:
        info["error"] = "nvidia-smi not found"
        return info

    try:
        out = _run_hidden([
            exe, "-i", str(int(gpu_index)),
            "--query-gpu=name,clocks.current.sm,clocks.max.sm,clocks_event_reasons.sw_power_cap",
            "--format=csv,noheader,nounits",
        ])
        parts = [p.strip() for p in out.strip().splitlines()[0].split(",")]
        if len(parts) >= 4:
            if not info["name"] and parts[0]:
                info["name"] = parts[0]
            cur_f = _parse_float(parts[1])
            max_f = _parse_float(parts[2])
            if cur_f is not None:
                info["current_sm_mhz"] = int(round(cur_f))
            if max_f is not None:
                info["max_sm_mhz"] = int(round(max_f))
            cap_str = parts[3].strip().lower()
            if cap_str == "active":
                info["sw_power_cap"] = True
            elif cap_str in ("not active", "none"):
                info["sw_power_cap"] = False
    except Exception:
        pass

    try:
        sup_out = _run_hidden([exe, "-i", str(int(gpu_index)), "-q", "-d", "SUPPORTED_CLOCKS"])
        clocks = _parse_supported_graphics_clocks(sup_out)
        if clocks:
            info["supported_clocks"] = clocks
            info["min_sm_mhz"] = clocks[0]
            info["max_sm_mhz"] = clocks[-1]
    except Exception:
        pass

    if info["min_sm_mhz"] is None and info["max_sm_mhz"] is not None and info["max_sm_mhz"] > 400:
        info["min_sm_mhz"] = 210

    if info["min_sm_mhz"] is not None and info["max_sm_mhz"] is not None and info["max_sm_mhz"] > info["min_sm_mhz"]:
        info["adjustable"] = True
        presets = resolve_clock_cap_presets(
            info["name"], info["min_sm_mhz"], info["max_sm_mhz"], info["supported_clocks"]
        )
        info["efficiency_mhz"] = presets["efficiency_mhz"]
        info["sweetspot_mhz"] = presets["sweetspot_mhz"]
        info["is_ada"] = presets["is_ada"]

    cfg = load_user_config()
    clk_cfg = cfg.get("clock_lock", {})
    if clk_cfg.get("enabled") and int(clk_cfg.get("gpu_index", 0)) == int(gpu_index):
        info["lock_enabled"] = True
        info["locked_min_mhz"] = clk_cfg.get("min_mhz")
        info["locked_max_mhz"] = clk_cfg.get("max_mhz")

    return info


def _save_clock_lock_state(gpu_index, enabled, min_mhz=None, max_mhz=None, reapply_on_wake=None):
    cfg = load_user_config()
    clk_cfg = cfg.setdefault("clock_lock", {})
    clk_cfg["gpu_index"] = int(gpu_index)
    clk_cfg["enabled"] = bool(enabled)
    if min_mhz is not None:
        clk_cfg["min_mhz"] = int(min_mhz)
    if max_mhz is not None:
        clk_cfg["max_mhz"] = int(max_mhz)
    if reapply_on_wake is not None:
        clk_cfg["reapply_on_wake"] = bool(reapply_on_wake)
    save_user_config(cfg)


def set_sm_clock_cap(gpu_index, max_mhz, min_mhz=None, allow_uac=True):
    """Lock GPU SM clock ceiling via nvidia-smi -lgc <min>,<max> while keeping low idle clocks."""
    info = get_clock_cap_info(gpu_index)
    if not info["adjustable"]:
        return False, info, "SM clock capping is unavailable on this GPU."

    floor_mhz = int(min_mhz) if min_mhz is not None else int(info["min_sm_mhz"])
    floor_mhz = snap_clock_mhz(floor_mhz, info["supported_clocks"], info["min_sm_mhz"], info["max_sm_mhz"])
    ceil_mhz = snap_clock_mhz(max_mhz, info["supported_clocks"], floor_mhz, info["max_sm_mhz"])

    ok, err_msg = _run_nvidia_smi_batch(
        [["-i", str(int(gpu_index)), "-lgc", f"{floor_mhz},{ceil_mhz}"]],
        allow_uac=allow_uac,
        action_label="SM clock cap",
    )
    if ok:
        _save_clock_lock_state(gpu_index, True, min_mhz=floor_mhz, max_mhz=ceil_mhz)
        readback = get_clock_cap_info(gpu_index)
        return True, readback, f"SM clock cap locked to {floor_mhz}-{ceil_mhz} MHz (-lgc)."
    return False, get_clock_cap_info(gpu_index), err_msg


def reset_sm_clock_cap(gpu_index, allow_uac=True):
    """Reset any SM clock lock via nvidia-smi -rgc."""
    ok, err_msg = _run_nvidia_smi_batch(
        [["-i", str(int(gpu_index)), "-rgc"]],
        allow_uac=allow_uac,
        action_label="SM clock reset",
    )
    if ok:
        _save_clock_lock_state(gpu_index, False)
        readback = get_clock_cap_info(gpu_index)
        return True, readback, "SM clock lock cleared (-rgc); full dynamic boost active."
    return False, get_clock_cap_info(gpu_index), err_msg


def apply_efficiency_profile(gpu_index, target_watts=None, clock_max_mhz=None, unlock_clocks=False, allow_uac=True):
    """Apply power limit and SM clock cap/reset together in a single elevated operation."""
    pl_info = get_power_limit_info(gpu_index)
    clk_info = get_clock_cap_info(gpu_index)
    cmds = []

    final_w = None
    if target_watts is not None and pl_info.get("adjustable"):
        min_w = int(round(pl_info["min_w"]))
        max_w = int(round(pl_info["max_w"]))
        final_w = max(min_w, min(max_w, int(round(float(target_watts)))))
        cmds.append(["-i", str(int(gpu_index)), "-pl", str(final_w)])

    final_min_clk = None
    final_max_clk = None
    if unlock_clocks and clk_info.get("adjustable"):
        cmds.append(["-i", str(int(gpu_index)), "-rgc"])
    elif clock_max_mhz is not None and clk_info.get("adjustable"):
        floor = snap_clock_mhz(
            clk_info["min_sm_mhz"], clk_info["supported_clocks"],
            clk_info["min_sm_mhz"], clk_info["max_sm_mhz"]
        )
        ceil = snap_clock_mhz(
            clock_max_mhz, clk_info["supported_clocks"],
            floor, clk_info["max_sm_mhz"]
        )
        final_min_clk = floor
        final_max_clk = ceil
        cmds.append(["-i", str(int(gpu_index)), "-lgc", f"{floor},{ceil}"])

    if not cmds:
        return False, pl_info, clk_info, "Neither power limit nor SM clock controls are adjustable on this GPU."

    ok, err_msg = _run_nvidia_smi_batch(cmds, allow_uac=allow_uac, action_label="efficiency profile")
    if ok:
        if unlock_clocks:
            _save_clock_lock_state(gpu_index, False)
        elif final_max_clk is not None:
            _save_clock_lock_state(gpu_index, True, min_mhz=final_min_clk, max_mhz=final_max_clk)

    new_pl = get_power_limit_info(gpu_index)
    new_clk = get_clock_cap_info(gpu_index)
    if not ok:
        return False, new_pl, new_clk, err_msg

    parts = []
    if final_w is not None:
        parts.append(f"PL={final_w} W")
    if unlock_clocks:
        parts.append("Clocks=Unlocked (-rgc)")
    elif final_max_clk is not None:
        parts.append(f"SM Cap={final_min_clk}-{final_max_clk} MHz")
    return True, new_pl, new_clk, "Applied " + ", ".join(parts) + "."


def get_config_dir():
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "UniversalGpuUnlock")


def load_user_config():
    path = os.path.join(get_config_dir(), "config.json")
    try:
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
                if isinstance(data, dict):
                    return data
    except Exception:
        pass
    return {}


def save_user_config(data):
    cfg_dir = get_config_dir()
    os.makedirs(cfg_dir, exist_ok=True)
    path = os.path.join(cfg_dir, "config.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return path


def save_benchmark_baseline(gpu_index, gpu_name, metrics):
    cfg = load_user_config()
    baselines = cfg.setdefault("baselines", {})
    baselines[str(gpu_index)] = {
        "gpu_name": gpu_name,
        "saved_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "fp16": round(float(metrics.get("fp16", 0.0)), 2),
        "bf16": round(float(metrics.get("bf16", 0.0)), 2),
        "tf32": round(float(metrics.get("tf32", 0.0)), 2),
        "fp32": round(float(metrics.get("fp32", 0.0)), 2),
    }
    save_user_config(cfg)
    return baselines[str(gpu_index)]


def load_benchmark_baseline(gpu_index):
    cfg = load_user_config()
    return cfg.get("baselines", {}).get(str(gpu_index))


def configure_wake_reapply_task(enable, repo_root, gpu_index=0, watts=None, clock_max_mhz=None, clock_min_mhz=None, clock_enabled=None):
    """Save wake tuning preferences (PL and/or SM clock cap) and manage Wake Scheduled Task."""
    cfg = load_user_config()
    pl_cfg = cfg.setdefault("power_limit", {})
    pl_cfg["reapply_on_wake"] = bool(enable)
    if watts is not None:
        pl_cfg["target_watts"] = int(round(float(watts)))
    pl_cfg["gpu_index"] = int(gpu_index)

    clk_cfg = cfg.setdefault("clock_lock", {})
    clk_cfg["reapply_on_wake"] = bool(enable)
    clk_cfg["gpu_index"] = int(gpu_index)
    if clock_enabled is not None:
        clk_cfg["enabled"] = bool(clock_enabled)
    if clock_min_mhz is not None:
        clk_cfg["min_mhz"] = int(clock_min_mhz)
    if clock_max_mhz is not None:
        clk_cfg["max_mhz"] = int(clock_max_mhz)
    save_user_config(cfg)

    if os.name != "nt":
        return True, "Saved wake power-limit preference locally."

    quick_check = os.path.join(repo_root, "tools", "quick_check.py")
    if not os.path.exists(quick_check):
        quick_check = os.path.join(os.path.dirname(os.path.abspath(__file__)), "quick_check.py")

    import sys
    py_exe = sys.executable or "python.exe"
    if py_exe.lower().endswith("python.exe"):
        pyw = py_exe[:-10] + "pythonw.exe"
        if os.path.exists(pyw):
            py_exe = pyw

    if enable:
        xml_content = f"""<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Triggers>
    <EventTrigger>
      <Enabled>true</Enabled>
      <Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="System"&gt;&lt;Select Path="System"&gt;*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and (EventID=1)]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription>
    </EventTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <LogonType>InteractiveToken</LogonType>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <Enabled>true</Enabled>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>{py_exe}</Command>
      <Arguments>"{quick_check}" --reapply-pl</Arguments>
    </Exec>
  </Actions>
</Task>"""
        xml_path = os.path.join(get_config_dir(), "wake_task.xml")
        with open(xml_path, "w", encoding="utf-16") as f:
            f.write(xml_content)
        schtasks_exe = shutil.which("schtasks") or r"C:\Windows\System32\schtasks.exe"
        proc = subprocess.run(
            [schtasks_exe, "/Create", "/TN", WAKE_TASK_NAME, "/XML", xml_path, "/F"],
            capture_output=True, text=True
        )
        if proc.returncode != 0 and not _is_windows_admin():
            launched, exit_code, cancelled, uac_err = _run_elevated_exe(
                schtasks_exe, f'/Create /TN "{WAKE_TASK_NAME}" /XML "{xml_path}" /F'
            )
            if cancelled:
                return False, "Scheduled task registration cancelled (dashboard wake listener stays active while open)."
            check = subprocess.run([schtasks_exe, "/Query", "/TN", WAKE_TASK_NAME], capture_output=True, text=True)
            if check.returncode == 0:
                return True, "Wake power-limit scheduled task registered."
            return False, uac_err or f"Failed to register scheduled task (exit {exit_code})."
        return proc.returncode == 0, (
            "Wake power-limit scheduled task registered." if proc.returncode == 0
            else (proc.stderr or proc.stdout or "Failed to register scheduled task.").strip()
        )
    else:
        schtasks_exe = shutil.which("schtasks") or r"C:\Windows\System32\schtasks.exe"
        exists = subprocess.run(
            [schtasks_exe, "/Query", "/TN", WAKE_TASK_NAME],
            capture_output=True, text=True
        )
        if exists.returncode != 0:
            return True, "Wake power-limit scheduled task disabled."
        proc = subprocess.run(
            [schtasks_exe, "/Delete", "/TN", WAKE_TASK_NAME, "/F"],
            capture_output=True, text=True
        )
        if proc.returncode != 0 and "cannot find" not in (proc.stderr or "").lower():
            if not _is_windows_admin():
                _run_elevated_exe(schtasks_exe, f'/Delete /TN "{WAKE_TASK_NAME}" /F')
        return True, "Wake power-limit scheduled task disabled."


def apply_saved_power_limits():
    """Apply saved wake power limit and/or SM clock cap if enabled (used by --reapply-pl)."""
    cfg = load_user_config()
    pl_cfg = cfg.get("power_limit", {})
    clk_cfg = cfg.get("clock_lock", {})

    want_pl = bool(pl_cfg.get("reapply_on_wake")) and pl_cfg.get("target_watts") is not None
    want_clk = bool(clk_cfg.get("reapply_on_wake")) and bool(clk_cfg.get("enabled")) and clk_cfg.get("max_mhz") is not None
    if not want_pl and not want_clk:
        return False, "Wake tuning reapply is disabled or no saved settings."

    msgs = []
    overall_ok = True
    if want_pl:
        ok_pl, _, msg_pl = set_power_limit(pl_cfg.get("gpu_index", 0), pl_cfg["target_watts"], allow_uac=False)
        overall_ok = overall_ok and ok_pl
        msgs.append(msg_pl)
    if want_clk:
        ok_clk, _, msg_clk = set_sm_clock_cap(
            clk_cfg.get("gpu_index", 0),
            clk_cfg["max_mhz"],
            min_mhz=clk_cfg.get("min_mhz"),
            allow_uac=False,
        )
        overall_ok = overall_ok and ok_clk
        msgs.append(msg_clk)
    return overall_ok, " | ".join(msgs)


def read_live_acpi_fadt():
    """Read live Windows kernel ACPI FADT table bytes from HKLM\\HARDWARE\\ACPI\\FADT."""
    if os.name != "nt":
        return None
    try:
        import winreg
        base = r"HARDWARE\ACPI\FADT"
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as root:
            oem = winreg.EnumKey(root, 0)
            with winreg.OpenKey(root, oem) as k1:
                tbl = winreg.EnumKey(k1, 0)
                with winreg.OpenKey(k1, tbl) as k2:
                    rev = winreg.EnumKey(k2, 0)
                    with winreg.OpenKey(k2, rev) as k3:
                        raw, _ = winreg.QueryValueEx(k3, "00000000")
                        if isinstance(raw, (bytes, bytearray)) and len(raw) >= 116:
                            fw_ctrl = int.from_bytes(raw[36:40], "little")
                            chk = raw[9]
                            flags = int.from_bytes(raw[112:116], "little")
                            return {
                                "firmware_ctrl": fw_ctrl,
                                "checksum": chk,
                                "flags": flags,
                                "s0ix_fadt": bool(flags & (1 << 21)),
                                "length": len(raw),
                            }
    except Exception:
        pass
    return None


def get_s3_capabilities():
    """Return kernel ACPI S3 availability and Modern Standby (S0ix) status."""
    s3_supported = None
    s4_supported = None
    s0ix_active = False

    fadt = read_live_acpi_fadt()
    if fadt:
        s0ix_active = fadt["s0ix_fadt"]

    if os.name == "nt":
        try:
            buf = ctypes.create_string_buffer(128)
            if ctypes.windll.powrprof.GetPwrCapabilities(buf):
                raw = buf.raw
                # winnt.h SYSTEM_POWER_CAPABILITIES: SystemS3 at offset 5, SystemS4 at offset 6
                s3_supported = bool(raw[5])
                s4_supported = bool(raw[6])
                if not s3_supported:
                    s0ix_active = True
        except Exception:
            pass
    return {
        "s3_supported": s3_supported,
        "s4_supported": s4_supported,
        "s0ix_active": s0ix_active,
        "fadt": fadt,
    }


def _get_windows_boot_timestamp():
    if os.name != "nt":
        return None
    try:
        uptime_ms = ctypes.windll.kernel32.GetTickCount64()
        return time.time() - (uptime_ms / 1000.0)
    except Exception:
        return None


def _list_drive_roots():
    if os.name != "nt":
        return []
    roots = []
    try:
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        for i in range(26):
            if mask & (1 << i):
                roots.append(f"{chr(65 + i)}:\\")
    except Exception:
        pass
    return roots


def _enumerate_hidden_fat32_roots():
    """Return unmounted/hidden FAT32 \\\\?\\Volume{...}\\ roots (e.g. GPU_UNLOCK) without requiring admin."""
    if os.name != "nt":
        return []
    roots = []
    k32 = ctypes.windll.kernel32
    k32.SetErrorMode.argtypes = [wintypes.UINT]
    k32.SetErrorMode.restype = wintypes.UINT
    k32.FindFirstVolumeW.argtypes = [wintypes.LPWSTR, wintypes.DWORD]
    k32.FindFirstVolumeW.restype = wintypes.HANDLE
    k32.FindNextVolumeW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD]
    k32.FindNextVolumeW.restype = wintypes.BOOL
    k32.FindVolumeClose.argtypes = [wintypes.HANDLE]
    k32.FindVolumeClose.restype = wintypes.BOOL
    k32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
    k32.GetDriveTypeW.restype = wintypes.UINT
    k32.GetVolumeNameForVolumeMountPointW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    k32.GetVolumeNameForVolumeMountPointW.restype = wintypes.BOOL
    k32.GetVolumeInformationW.argtypes = [
        wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        wintypes.LPWSTR, wintypes.DWORD,
    ]
    k32.GetVolumeInformationW.restype = wintypes.BOOL

    mounted_guids = set()
    for drv in _list_drive_roots():
        try:
            vbuf = ctypes.create_unicode_buffer(256)
            if k32.GetVolumeNameForVolumeMountPointW(drv, vbuf, 256):
                mounted_guids.add(vbuf.value.upper())
        except Exception:
            pass

    invalid = wintypes.HANDLE(-1).value
    handle = invalid
    old_mode = k32.SetErrorMode(0x0001)  # SEM_FAILCRITICALERRORS
    try:
        buf = ctypes.create_unicode_buffer(1024)
        handle = k32.FindFirstVolumeW(buf, 1024)
        if handle == invalid:
            return roots
        while True:
            vol = buf.value
            if vol and vol.upper() not in mounted_guids:
                if k32.GetDriveTypeW(vol) in (2, 3):  # DRIVE_REMOVABLE / DRIVE_FIXED
                    lbl = ctypes.create_unicode_buffer(256)
                    fs = ctypes.create_unicode_buffer(64)
                    if k32.GetVolumeInformationW(vol, lbl, 256, None, None, None, fs, 64):
                        if fs.value.upper() in ("FAT32", "FAT", "EXFAT"):
                            roots.append((vol, lbl.value or ""))
            if not k32.FindNextVolumeW(handle, buf, 1024):
                break
    except Exception:
        pass
    finally:
        if handle != invalid:
            try:
                k32.FindVolumeClose(handle)
            except Exception:
                pass
        try:
            k32.SetErrorMode(old_mode)
        except Exception:
            pass
    return roots


def _list_log_search_roots():
    """Return (root_path, volume_label, is_hidden) tuples across drive letters and hidden FAT32 volumes."""
    entries = [(root, "", False) for root in _list_drive_roots()]
    for vol, lbl in _enumerate_hidden_fat32_roots():
        entries.append((vol, lbl, True))
    return entries


def sync_boot_lang_cfg(lang):
    """Best-effort sync of EFI\\BOOT\\lang.cfg ('en'/'zh'/'de') across USB & hidden FAT32 boot volumes."""
    raw = str(lang or "en").strip().lower()
    if raw.startswith("zh"):
        code = "zh"
    elif raw.startswith("de"):
        code = "de"
    else:
        code = "en"
    updated = []
    seen = set()
    for root, _label, _hidden in _list_log_search_roots():
        norm_root = os.path.normcase(os.path.normpath(root))
        if norm_root in seen:
            continue
        seen.add(norm_root)
        boot_dir = os.path.join(root, "EFI", "BOOT")
        boot_efi = os.path.join(boot_dir, "BOOTX64.EFI")
        try:
            if not os.path.isfile(boot_efi):
                continue
            cfg_path = os.path.join(boot_dir, "lang.cfg")
            with open(cfg_path, "w", encoding="ascii", newline="\n") as f:
                f.write(code + "\n")
            updated.append(cfg_path)
        except Exception:
            pass
    return updated


def _collect_run_log_candidates():
    candidates = []
    for root, label, is_hidden in _list_log_search_roots():
        for sub in ("GPU-UNLOCK", "AD102-UNLOCK", "GA102-UNLOCK", "RTX3090-UNLOCK"):
            folder = os.path.join(root, sub)
            try:
                if not os.path.isdir(folder):
                    continue
                for fn in os.listdir(folder):
                    up = fn.upper()
                    if up.startswith("RUN") and up.endswith(".TXT"):
                        full = os.path.join(folder, fn)
                        st = os.stat(full)
                        candidates.append({
                            "mtime": st.st_mtime,
                            "path": full,
                            "name": fn,
                            "folder": folder,
                            "volume_label": label,
                            "is_hidden": is_hidden,
                        })
            except Exception:
                continue
    candidates.sort(key=lambda item: (item["mtime"], item["name"]), reverse=True)
    return candidates


def get_boot_log_export_dir(repo_dir=None):
    """Return writable Boot-Logs export directory (prefer repo_dir/Boot-Logs, fallback LOCALAPPDATA)."""
    if repo_dir:
        cand = os.path.join(repo_dir, "Boot-Logs")
        try:
            os.makedirs(cand, exist_ok=True)
            probe = os.path.join(cand, ".write_test")
            with open(probe, "w", encoding="utf-8") as f:
                f.write("ok")
            os.remove(probe)
            return cand
        except Exception:
            pass
    local_app = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    cand = os.path.join(local_app, "UniversalGpuUnlock", "Boot-Logs")
    os.makedirs(cand, exist_ok=True)
    return cand


def export_latest_boot_logs(repo_dir=None, max_files=10, allow_uac=True):
    """Copy newest RUN*.TXT logs from USB / hidden GPU_UNLOCK (or elevated ESP) into Boot-Logs.

    Returns (ok, latest_exported_path, status_message).
    """
    dest_dir = get_boot_log_export_dir(repo_dir)
    candidates = _collect_run_log_candidates()
    if candidates:
        exported = []
        attempt_found = False
        for item in candidates[:max_files]:
            dst = os.path.join(dest_dir, item["name"])
            if os.path.exists(dst):
                try:
                    st_dst = os.stat(dst)
                    st_src = os.stat(item["path"])
                    if st_dst.st_size != st_src.st_size or int(st_dst.st_mtime) != int(st_src.st_mtime):
                        stamp = time.strftime("%Y%m%d_%H%M%S", time.localtime(item["mtime"]))
                        base, ext = os.path.splitext(item["name"])
                        dst = os.path.join(dest_dir, f"{base}_{stamp}{ext}")
                except Exception:
                    pass
            shutil.copy2(item["path"], dst)
            exported.append(dst)
            if os.path.exists(os.path.join(item["folder"], "ATTEMPT.FLG")):
                attempt_found = True

        src_tag = (
            f"hidden partition ({candidates[0]['volume_label'] or 'FAT32'})"
            if candidates[0]["is_hidden"]
            else candidates[0]["folder"]
        )
        msg = f"Exported {len(exported)} log(s) from {src_tag} to Boot-Logs ({os.path.basename(exported[0])})."
        if attempt_found:
            msg += " [ATTEMPT.FLG present]"
        return True, exported[0], msg

    if not allow_uac or os.name != "nt":
        return False, None, "No RUN*.TXT logs found on mounted drives or hidden FAT32 volumes."

    script_candidates = []
    if repo_dir:
        script_candidates.append(os.path.join(repo_dir, "tools", "Export-Boot-Log.ps1"))
    script_candidates.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "Export-Boot-Log.ps1"))
    ps_script = next((p for p in script_candidates if os.path.exists(p)), None)
    if not ps_script:
        return False, None, "Export-Boot-Log.ps1 not found for protected ESP fallback."

    params = f'-NoProfile -ExecutionPolicy Bypass -File "{ps_script}" -Destination "{dest_dir}" -MaxFiles {int(max_files)} -Quiet -Elevated'
    launched, exit_code, cancelled, uac_err = _run_elevated_exe("powershell.exe", params, timeout_ms=30000)
    if not launched:
        return False, None, uac_err or ("Administrator elevation cancelled." if cancelled else "Failed to launch elevated log exporter.")
    if exit_code != 0:
        return False, None, "No RUN*.TXT logs were found on USB, hidden GPU_UNLOCK, or Windows ESP."

    exported_files = []
    try:
        for fn in os.listdir(dest_dir):
            if fn.upper().startswith("RUN") and fn.upper().endswith(".TXT"):
                full = os.path.join(dest_dir, fn)
                exported_files.append((os.stat(full).st_mtime, full))
    except Exception:
        pass
    if not exported_files:
        return False, None, "Elevated ESP export finished, but no RUN*.TXT file appeared in Boot-Logs."
    exported_files.sort(key=lambda x: x[0], reverse=True)
    latest_path = exported_files[0][1]
    return True, latest_path, f"Exported {len(exported_files)} log(s) from EFI System Partition to Boot-Logs ({os.path.basename(latest_path)})."


def decode_plm_mask(plm_val):
    """Return a concise human-readable PLM decode string (read L0-L3, write L0-L3)."""
    if plm_val is None:
        return None
    if (plm_val & 0xFFFF0000) == 0xBADF0000:
        return "badf-pri (PRI error)"
    rmask = plm_val & 0x0F
    wmask = (plm_val >> 4) & 0x0F
    if plm_val == 0xFFFFFFFF:
        tag = "OPEN (L0-3 write)"
    elif plm_val == 0xFFFFFF8F:
        tag = "STOCK-LOCK (L3-only write)"
    elif plm_val == 0xFFFFFFEF:
        tag = "RELOCK-L0 (Host/L0 locked, L1-3 write)"
    elif plm_val == 0xFFFFFFCF:
        tag = "DEEP-LOCK (L2-3 write only)"
    elif (wmask & 0x01) == 0:
        tag = f"L0-LOCKED (R:0x{rmask:x} W:0x{wmask:x})"
    else:
        tag = f"CUSTOM (R:0x{rmask:x} W:0x{wmask:x})"
    return tag


def find_latest_run_log():
    """Find the most recent RUN*.TXT log across mounted drives & hidden FAT32 volumes and parse S3 markers."""
    boot_ts = _get_windows_boot_timestamp()
    candidates = _collect_run_log_candidates()
    if not candidates:
        return None

    top = candidates[0]
    mtime, full_path, name = top["mtime"], top["path"], top["name"]
    from_current_boot = (boot_ts is None) or (mtime >= boot_ts - 600.0)

    info = {
        "path": full_path,
        "name": name,
        "mtime": mtime,
        "from_current_boot": from_current_boot,
        "is_hidden": top["is_hidden"],
        "volume_label": top["volume_label"],
        "s3_tier1": False,
        "s3_tier2": False,
        "unlock_ok": False,
        "stock_mode": False,
        "shadow_facs": None,
        "real_facs": None,
        "plm_hex": None,
        "plm_summary": None,
        "relock_detected": False,
    }
    try:
        with open(full_path, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
        info["s3_tier1"] = "Registered via UEFI Boot Script Protocol" in txt
        info["s3_tier2"] = (
            "Universal ACPI FACS Trampoline armed successfully" in txt
            or "GuAcpiRefreshShadowChain" in txt
            or "ShadowFACS=" in txt
        )
        info["unlock_ok"] = (
            "100% UNLOCKED" in txt
            or "Fully Unlocked" in txt
            or "TRANSACTION OK" in txt
            or "post-FLR SEC2 reset released" in txt
        )
        info["stock_mode"] = "Stock Windows" in txt or "skipping unlock" in txt.lower()
        m_sf = re.search(r"ShadowFACS=(0x[0-9a-fA-F]+)", txt)
        if m_sf:
            info["shadow_facs"] = int(m_sf.group(1), 16)
        m_rf = re.search(r"RealFACS=(0x[0-9a-fA-F]+)", txt)
        if m_rf:
            info["real_facs"] = int(m_rf.group(1), 16)
        plm_matches = re.findall(r"\b(?:PLM|plm)=(0x[0-9a-fA-F]+)\b", txt)
        if plm_matches:
            plm_val = int(plm_matches[-1], 16)
            info["plm_hex"] = f"0x{plm_val:08x}"
            info["plm_summary"] = decode_plm_mask(plm_val)
            info["relock_detected"] = plm_val in (0xFFFFFFEF, 0xFFFFFF8F, 0xFFFFFFCF)
    except Exception:
        pass
    return info


def get_s3_armed_status():
    """Determine live ACPI S3 capability and whether S3 Persistence is currently armed."""
    caps = get_s3_capabilities()
    fadt = caps.get("fadt")
    fw_ctrl = fadt["firmware_ctrl"] if fadt else None
    log = find_latest_run_log()

    cfg = load_user_config()
    known_real_facs = cfg.get("acpi", {}).get("real_facs")
    if log and log.get("real_facs") and known_real_facs != log["real_facs"]:
        cfg.setdefault("acpi", {})["real_facs"] = log["real_facs"]
        save_user_config(cfg)
        known_real_facs = log["real_facs"]

    if caps["s0ix_active"] or caps["s3_supported"] is False:
        return {
            "code": "S0IX_WARN",
            "label": "S3 WARN (Modern Standby S0ix)",
            "detail": "BIOS/Windows uses Modern Standby (S0ix); switch BIOS to classic ACPI S3 for [P].",
            "s3_supported": caps["s3_supported"],
            "s0ix_active": True,
            "firmware_ctrl": fw_ctrl,
            "shadow_facs": log.get("shadow_facs") if log else None,
            "log_name": log.get("name") if log else None,
        }

    # Positive live confirmation from current boot log + kernel FADT check
    if log and log["from_current_boot"]:
        if log["s3_tier2"]:
            sf = log.get("shadow_facs")
            if fw_ctrl is not None and sf is not None and fw_ctrl == sf:
                return {
                    "code": "ARMED",
                    "label": "S3 ARMED (ShadowFACS Active)",
                    "detail": f"Tier 2 ShadowFACS 0x{fw_ctrl:08X} active in kernel FADT ({log['name']}).",
                    "s3_supported": caps["s3_supported"],
                    "s0ix_active": False,
                    "firmware_ctrl": fw_ctrl,
                    "shadow_facs": sf,
                    "log_name": log["name"],
                }
            if fw_ctrl is not None and known_real_facs is not None and fw_ctrl == known_real_facs:
                return {
                    "code": "NOT_ARMED",
                    "label": "S3 NOT ARMED (RealFACS)",
                    "detail": f"Kernel FADT points to RealFACS 0x{fw_ctrl:08X}. Boot [P] or [I] via UEFI.",
                    "s3_supported": caps["s3_supported"],
                    "s0ix_active": False,
                    "firmware_ctrl": fw_ctrl,
                    "shadow_facs": sf,
                    "log_name": log["name"],
                }
            return {
                "code": "ARMED",
                "label": "S3 ARMED (Plan C Logged)",
                "detail": f"S3 Trampoline armed in {log['name']} (FADT FirmwareCtrl=0x{fw_ctrl:08X})." if fw_ctrl else f"S3 Trampoline armed in {log['name']}.",
                "s3_supported": caps["s3_supported"],
                "s0ix_active": False,
                "firmware_ctrl": fw_ctrl,
                "shadow_facs": sf,
                "log_name": log["name"],
            }
        if log["s3_tier1"]:
            return {
                "code": "ARMED",
                "label": "S3 ARMED (Boot-Script Tier 1)",
                "detail": f"UEFI Boot-Script Dispatch armed in {log['name']}.",
                "s3_supported": caps["s3_supported"],
                "s0ix_active": False,
                "firmware_ctrl": fw_ctrl,
                "shadow_facs": None,
                "log_name": log["name"],
            }
        if log["unlock_ok"] or log["stock_mode"]:
            return {
                "code": "NOT_ARMED",
                "label": "COLD-BOOT ONLY ([U] / Stock)",
                "detail": f"Current session ({log['name']}) booted without S3 persistence.",
                "s3_supported": caps["s3_supported"],
                "s0ix_active": False,
                "firmware_ctrl": fw_ctrl,
                "shadow_facs": None,
                "log_name": log["name"],
            }

    # Hidden partition / USB unplugged fallback using known RealFACS baseline
    if fw_ctrl is not None and known_real_facs is not None and fw_ctrl != known_real_facs:
        return {
            "code": "ARMED",
            "label": "S3 ARMED (ShadowFACS in FADT)",
            "detail": f"Kernel FADT FirmwareCtrl=0x{fw_ctrl:08X} differs from RealFACS 0x{known_real_facs:08X}.",
            "s3_supported": caps["s3_supported"],
            "s0ix_active": False,
            "firmware_ctrl": fw_ctrl,
            "shadow_facs": fw_ctrl,
            "log_name": None,
        }

    return {
        "code": "UNVERIFIED",
        "label": "S3 READY (Log Not Mounted)",
        "detail": (
            f"ACPI S3 supported (FADT FirmwareCtrl=0x{fw_ctrl:08X}); current-boot RUN*.TXT not mounted."
            if fw_ctrl is not None
            else "ACPI S3 supported; current-boot RUN*.TXT not mounted."
        ),
        "s3_supported": caps["s3_supported"],
        "s0ix_active": False,
        "firmware_ctrl": fw_ctrl,
        "shadow_facs": None,
        "log_name": None,
    }


CARD_PROFILES = {
    # Laptop variants MUST appear before desktop substrings ("4090 laptop" before "4090", etc.).
    "4090 laptop": {"min_unlocked": 140.0, "threshold": 120.0, "stock_typical": 90.0},
    "4080 laptop": {"min_unlocked": 100.0, "threshold": 82.0, "stock_typical": 60.0},
    "4070 laptop": {"min_unlocked": 56.0, "threshold": 46.0, "stock_typical": 33.0},
    "4060 laptop": {"min_unlocked": 42.0, "threshold": 32.0, "stock_typical": 24.0},
    "4090": {"min_unlocked": 200.0, "threshold": 175.0, "stock_typical": 135.0},
    "4080": {"min_unlocked": 115.0, "threshold": 98.0, "stock_typical": 80.0},
    "4070 ti super": {"min_unlocked": 100.0, "threshold": 88.0, "stock_typical": 70.0},
    "4070 ti": {"min_unlocked": 95.0, "threshold": 82.0, "stock_typical": 64.0},
    "4070 super": {"min_unlocked": 88.0, "threshold": 76.0, "stock_typical": 60.0},
    "4070": {"min_unlocked": 78.0, "threshold": 68.0, "stock_typical": 52.0},
    "4060 ti": {"min_unlocked": 55.0, "threshold": 48.0, "stock_typical": 36.0},
    "4060": {"min_unlocked": 40.0, "threshold": 34.0, "stock_typical": 26.0},
    "3060 laptop": {"min_unlocked": 38.0, "threshold": 30.0, "stock_typical": 22.0},
    "3090 ti": {"min_unlocked": 125.0, "threshold": 105.0, "stock_typical": 78.0},
    "3090": {"min_unlocked": 115.0, "threshold": 95.0, "stock_typical": 72.0},
    "3080 ti": {"min_unlocked": 110.0, "threshold": 92.0, "stock_typical": 70.0},
    "3080": {"min_unlocked": 95.0, "threshold": 80.0, "stock_typical": 62.0},
    "3070 ti": {"min_unlocked": 75.0, "threshold": 62.0, "stock_typical": 48.0},
    "3070": {"min_unlocked": 65.0, "threshold": 54.0, "stock_typical": 40.0},
    "3060 ti": {"min_unlocked": 55.0, "threshold": 46.0, "stock_typical": 34.0},
    "3060": {"min_unlocked": 40.0, "threshold": 32.0, "stock_typical": 24.0},
    "3050": {"min_unlocked": 28.0, "threshold": 22.0, "stock_typical": 15.0},
    "2050": {"min_unlocked": 22.0, "threshold": 18.0, "stock_typical": 13.0},
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
        res = _run_hidden(cmd).strip()
        return res or None
    except Exception:
        return None


def get_gpu_utilization(gpu_index=0, threshold=EXTERNAL_LOAD_THRESHOLD):
    """Query pre-benchmark GPU/memory utilization via nvidia-smi to detect external load."""
    result = {"gpu_util": None, "mem_util": None, "under_load": False}
    exe = find_nvidia_smi()
    if not exe:
        return result

    try:
        out = _run_hidden([
            exe, "-i", str(gpu_index),
            "--query-gpu=utilization.gpu,utilization.memory",
            "--format=csv,noheader,nounits",
        ])
        parts = [p.strip() for p in out.strip().splitlines()[0].split(",")]
        if len(parts) >= 1:
            result["gpu_util"] = _parse_float(parts[0])
        if len(parts) >= 2:
            result["mem_util"] = _parse_float(parts[1])
    except Exception:
        pass

    if result["gpu_util"] is None:
        try:
            q_out = _run_hidden([exe, "-i", str(gpu_index), "-q", "-d", "UTILIZATION"])
            m_gpu = re.search(r"Gpu\s*:\s*([0-9.]+)\s*%", q_out)
            m_mem = re.search(r"Memory\s*:\s*([0-9.]+)\s*%", q_out)
            if m_gpu:
                result["gpu_util"] = _parse_float(m_gpu.group(1))
            if m_mem:
                result["mem_util"] = _parse_float(m_mem.group(1))
        except Exception:
            pass

    if result["gpu_util"] is not None:
        result["under_load"] = result["gpu_util"] > float(threshold)
    return result


def resolve_card_profile(gpu_name, major, minor, sm_count):
    """Return expected unlocked/stock TFLOPS thresholds for a CUDA GPU."""
    gpu_lower = str(gpu_name).lower()
    for key, prof in CARD_PROFILES.items():
        if key in gpu_lower:
            return dict(prof)

    if major == 8 and minor == 9:
        threshold = sm_count * 1.35
    elif major == 8 and minor in (0, 6):
        threshold = sm_count * 1.15
    elif major == 7 and minor == 5:
        threshold = sm_count * 0.65
    else:
        threshold = sm_count * 1.0
    return {
        "min_unlocked": threshold * 1.15,
        "threshold": threshold,
        "stock_typical": threshold * 0.70,
    }


def is_gtx16_no_tensor(gpu_name):
    """Return True for Turing TU116/TU117 GTX 16-series cards (no Tensor Cores)."""
    low = str(gpu_name or "").lower()
    return bool(re.search(r"\bgtx\s*16\d{2}\b|\b16(?:30|50|60)\b", low))


def benchmark_gemm(torch_mod, device, dtype, size=4096, warmup=5, runs=25):
    """Run synchronized square CUDA GEMM on the target device stream and return (avg_ms, tflops)."""
    if hasattr(torch_mod.backends, "cudnn") and hasattr(torch_mod.backends.cudnn, "benchmark"):
        torch_mod.backends.cudnn.benchmark = True
    flops = 2.0 * (size ** 3)
    cuda_mod = getattr(torch_mod, "cuda", None)
    dev_ctx = cuda_mod.device(device) if (cuda_mod and hasattr(cuda_mod, "device")) else nullcontext()
    with dev_ctx:
        a = torch_mod.randn((size, size), dtype=dtype, device=device)
        b = torch_mod.randn((size, size), dtype=dtype, device=device)

        for _ in range(warmup):
            c = torch_mod.matmul(a, b)
        if cuda_mod and hasattr(cuda_mod, "synchronize"):
            cuda_mod.synchronize(device)

        stream = (
            cuda_mod.current_stream(device)
            if (cuda_mod and hasattr(cuda_mod, "current_stream"))
            else None
        )
        start_ev = cuda_mod.Event(enable_timing=True)
        end_ev = cuda_mod.Event(enable_timing=True)
        if stream is not None:
            start_ev.record(stream)
        else:
            start_ev.record()
        for _ in range(runs):
            c = torch_mod.matmul(a, b)
        if stream is not None:
            end_ev.record(stream)
        else:
            end_ev.record()
        if cuda_mod and hasattr(cuda_mod, "synchronize"):
            cuda_mod.synchronize(device)

        avg_ms = start_ev.elapsed_time(end_ev) / runs
        tflops = (flops / (avg_ms * 1e-3)) / 1e12 if avg_ms > 0 else 0.0
        del a, b, c
        return avg_ms, tflops


def measure_fmla_ratio(torch_mod, device, size=4096, tflops_fmla16=None):
    """Compare FP16 FMLA16 (reduced=True) vs FMLA32 (reduced=False) and restore setting."""
    matmul = getattr(getattr(torch_mod.backends, "cuda", None), "matmul", None)
    if matmul is None or not hasattr(matmul, "allow_fp16_reduced_precision_reduction"):
        return {
            "supported": False,
            "fmla16_tflops": tflops_fmla16,
            "fmla32_tflops": None,
            "fmla32_ms": None,
            "ratio": None,
        }

    orig_val = matmul.allow_fp16_reduced_precision_reduction
    try:
        if tflops_fmla16 is None:
            matmul.allow_fp16_reduced_precision_reduction = True
            _, tflops_fmla16 = benchmark_gemm(torch_mod, device, torch_mod.float16, size=size, warmup=6, runs=20)

        matmul.allow_fp16_reduced_precision_reduction = False
        ms_fmla32, tflops_fmla32 = benchmark_gemm(torch_mod, device, torch_mod.float16, size=size, warmup=4, runs=15)
    finally:
        matmul.allow_fp16_reduced_precision_reduction = orig_val

    ratio = (tflops_fmla32 / tflops_fmla16) if (tflops_fmla16 and tflops_fmla16 > 0) else None
    return {
        "supported": True,
        "fmla16_tflops": tflops_fmla16,
        "fmla32_tflops": tflops_fmla32,
        "fmla32_ms": ms_fmla32,
        "ratio": ratio,
    }


def classify_unlock_status(tflops_fp16, profile, util_info, fmla_info, major, minor, gpu_name=None):
    """Determine unlock verdict without misclassifying external GPU load as stock clamping.

    On Ampere (sm_80/sm_86), FMLA32/FMLA16 stays near ~1.00x when unlocked vs ~0.50x
    when stock rate-limited, independent of absolute TFLOPS drops under load.
    On Ada (sm_89) and Turing (sm_75), PyTorch FMLA32 natively runs at ~0.50-0.63x of
    FMLA16 even when unlocked, so those architectures avoid treating ~0.55x as clamped.
    """
    if gpu_name and is_gtx16_no_tensor(gpu_name):
        gpu_util = util_info.get("gpu_util") if util_info else None
        fmla_ratio = fmla_info.get("ratio") if (fmla_info and fmla_info.get("supported")) else None
        return {
            "code": "NO_TENSOR_CORES",
            "is_unlocked": False,
            "supported_for_unlock": False,
            "inconclusive": False,
            "under_load": bool(util_info and util_info.get("under_load")),
            "gpu_util": gpu_util,
            "fmla_ratio": fmla_ratio,
            "supports_fmla_verdict": False,
        }

    tflops_ok = tflops_fp16 >= profile["threshold"]
    under_load = bool(util_info and util_info.get("under_load"))
    gpu_util = util_info.get("gpu_util") if util_info else None
    fmla_ratio = fmla_info.get("ratio") if (fmla_info and fmla_info.get("supported")) else None
    supports_fmla_verdict = (major == 8 and minor != 9)
    fmla_unlocked = supports_fmla_verdict and (fmla_ratio is not None) and (fmla_ratio >= 0.80)

    if tflops_ok:
        return {
            "code": "UNLOCKED",
            "is_unlocked": True,
            "inconclusive": False,
            "under_load": under_load,
            "gpu_util": gpu_util,
            "fmla_ratio": fmla_ratio,
            "supports_fmla_verdict": supports_fmla_verdict,
        }

    if under_load:
        if fmla_unlocked:
            return {
                "code": "UNLOCKED_UNDER_LOAD",
                "is_unlocked": True,
                "inconclusive": True,
                "under_load": True,
                "gpu_util": gpu_util,
                "fmla_ratio": fmla_ratio,
                "supports_fmla_verdict": supports_fmla_verdict,
            }
        return {
            "code": "EXTERNAL_LOAD",
            "is_unlocked": False,
            "inconclusive": True,
            "under_load": True,
            "gpu_util": gpu_util,
            "fmla_ratio": fmla_ratio,
            "supports_fmla_verdict": supports_fmla_verdict,
        }

    if fmla_unlocked and tflops_fp16 >= 5.0:
        return {
            "code": "UNLOCKED_LOW_TFLOPS",
            "is_unlocked": True,
            "inconclusive": True,
            "under_load": False,
            "gpu_util": gpu_util,
            "fmla_ratio": fmla_ratio,
            "supports_fmla_verdict": supports_fmla_verdict,
        }

    return {
        "code": "STOCK",
        "is_unlocked": False,
        "inconclusive": False,
        "under_load": False,
        "gpu_util": gpu_util,
        "fmla_ratio": fmla_ratio,
        "supports_fmla_verdict": supports_fmla_verdict,
    }
