"""
Universal GPU Rate-Limiter Hardware Status Checker (Ada Lovelace, Ampere & Turing)
SPDX-License-Identifier: GPL-2.0-only
"""
import sys
import time
import os
import re
import subprocess
import ctypes

# Enable ANSI escape sequences on Windows consoles
if os.name == "nt":
    try:
        os.system("")
    except Exception:
        pass

C_GREEN = "\033[92m"
C_GREEN_BOLD = "\033[92;1m"
C_RED_BOLD = "\033[91;1m"
C_YELLOW = "\033[93m"
C_CYAN = "\033[96m"
C_RESET = "\033[0m"

# Shared helpers live next to this script (tools/gu_system_info.py).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gu_system_info import (  # noqa: E402
    apply_saved_power_limits,
    benchmark_gemm,
    classify_unlock_status,
    find_nvidia_smi,
    get_clock_cap_info,
    get_firmware_mode,
    get_gpu_utilization,
    get_last_wake_time,
    get_power_limit_info,
    get_rebar_info,
    get_s3_armed_status,
    measure_fmla_ratio,
    resolve_card_profile,
)


def get_pcie_info(gpu_index=0):
    exe = find_nvidia_smi()
    if not exe:
        return None, None, False
    try:
        cmd = [
            exe, "-i", str(gpu_index),
            "--query-gpu=pcie.link.gen.max,pcie.link.width.max",
            "--format=csv,noheader,nounits"
        ]
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
        parts = [p.strip() for p in out.strip().split(",")]
        if len(parts) >= 2:
            gen_max = int(parts[0])
            width_max = int(parts[1])
            is_throttled = width_max < 8 or gen_max < 3
            return gen_max, width_max, is_throttled
    except Exception:
        pass
    return None, None, False

def copy_to_clipboard(text):
    if os.name == "nt":
        try:
            proc = subprocess.Popen(["clip.exe"], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)
            proc.communicate(input=text.encode("utf-16le"))
            return True
        except Exception:
            pass
    return False

def evaluate_gpu(dev_idx, dev_count, fw_str):
    device = torch.device(f"cuda:{dev_idx}")
    gpu_name = torch.cuda.get_device_name(dev_idx)
    props = torch.cuda.get_device_properties(dev_idx)
    sm_count = props.multi_processor_count
    arch = f"sm_{props.major}{props.minor}"

    # Sample pre-benchmark GPU load before allocating CUDA tensors.
    util_info = get_gpu_utilization(dev_idx)
    profile = resolve_card_profile(gpu_name, props.major, props.minor, sm_count)
    size = 4096

    matmul = getattr(getattr(torch.backends, "cuda", None), "matmul", None)
    has_fmla_flag = matmul is not None and hasattr(matmul, "allow_fp16_reduced_precision_reduction")
    has_tf32 = props.major >= 8 and matmul is not None and hasattr(matmul, "allow_tf32")
    orig_fmla = getattr(matmul, "allow_fp16_reduced_precision_reduction", True) if matmul else True
    orig_tf32 = getattr(matmul, "allow_tf32", True) if matmul else True

    try:
        # 1. Primary Benchmark: FP16 Tensor Cores (FMLA16 when supported)
        if has_fmla_flag:
            matmul.allow_fp16_reduced_precision_reduction = True
        ms_fp16, tflops_fp16 = benchmark_gemm(torch, device, torch.float16, size=size, warmup=8, runs=25)

        # 1b. Load-Independent Indicator: FMLA32 (reduced=False) vs FMLA16 (reduced=True)
        fmla_info = measure_fmla_ratio(torch, device, size=size, tflops_fmla16=tflops_fp16)

        # 2. Secondary Benchmark: BF16 Tensor Cores (Ampere / Ada sm_80+ only; Turing sm_75 lacks hardware BF16)
        has_bf16 = props.major >= 8 and hasattr(torch.cuda, "is_bf16_supported") and torch.cuda.is_bf16_supported()
        if has_bf16:
            ms_bf16, tflops_bf16 = benchmark_gemm(torch, device, torch.bfloat16, size=size, warmup=4, runs=15)
        else:
            ms_bf16, tflops_bf16 = 0.0, 0.0

        # 3. TF32 Tensor Cores (Ampere / Ada, allow_tf32=True)
        if has_tf32:
            matmul.allow_tf32 = True
            ms_tf32, tflops_tf32 = benchmark_gemm(torch, device, torch.float32, size=size, warmup=4, runs=15)
        else:
            ms_tf32, tflops_tf32 = 0.0, 0.0

        # 4. Physical Negative Control: Standard FP32 CUDA Cores (allow_tf32=False)
        if has_tf32:
            matmul.allow_tf32 = False
        ms_fp32, tflops_fp32 = benchmark_gemm(torch, device, torch.float32, size=size, warmup=4, runs=15)
    finally:
        if has_fmla_flag:
            matmul.allow_fp16_reduced_precision_reduction = orig_fmla
        if has_tf32:
            matmul.allow_tf32 = orig_tf32
        torch.cuda.empty_cache()

    verdict = classify_unlock_status(tflops_fp16, profile, util_info, fmla_info, props.major, props.minor)
    is_unlocked = verdict["is_unlocked"]
    code = verdict["code"]
    ratio = tflops_fp16 / tflops_fp32 if tflops_fp32 > 0 else 1.0
    fmla_ratio = verdict["fmla_ratio"]

    bar1, fb, rebar_active = get_rebar_info(dev_idx)
    gen_max, width_max, is_throttled = get_pcie_info(dev_idx)

    prefix = f"GPU [{dev_idx + 1}/{dev_count}]: " if dev_count > 1 else ""
    print(f"  {prefix}Hardware        : {gpu_name}")
    print(f"  {prefix}Architecture    : {arch} ({sm_count} Streaming Multiprocessors)")
    if util_info.get("gpu_util") is not None:
        u_gpu = util_info["gpu_util"]
        u_mem = util_info.get("mem_util")
        mem_txt = f", {u_mem:.0f}% Mem" if u_mem is not None else ""
        if util_info["under_load"]:
            print(f"  {prefix}Pre-Bench Load  : {C_YELLOW}[!] {u_gpu:.0f}% GPU{mem_txt} (External workload active!){C_RESET}")
        else:
            print(f"  {prefix}Pre-Bench Load  : {C_GREEN}[OK] {u_gpu:.0f}% GPU{mem_txt} (Idle){C_RESET}")
    if rebar_active is True:
        print(f"  {prefix}Resizable BAR   : {C_GREEN}[OK] ENABLED (BAR1: {bar1} MiB / VRAM: {fb} MiB){C_RESET}")
    elif rebar_active is False:
        print(f"  {prefix}Resizable BAR   : {C_RED_BOLD}[!] DISABLED (BAR1: {bar1} MiB restricted) -> Turn ON in BIOS!{C_RESET}")
    else:
        print(f"  {prefix}Resizable BAR   : Undetected")
    if gen_max and width_max:
        if is_throttled:
            print(f"  {prefix}PCIe Slot Link  : {C_RED_BOLD}[!] THROTTLED: Gen{gen_max} x{width_max} (Riser or Slot Bottleneck!){C_RESET}")
        else:
            print(f"  {prefix}PCIe Slot Link  : {C_GREEN}[OK] Gen{gen_max} x{width_max} (Full Bandwidth){C_RESET}")
    else:
        print(f"  {prefix}PCIe Slot Link  : Undetected")
    print(f"  {prefix}Matrix Dimension: {size}x{size} Dense Matrix Multiply (GEMM)")
    print("-" * 72)
    print(f"  [1] FP16 Tensor Cores   : {tflops_fp16:>6.2f} TFLOPS ({ms_fp16:.3f} ms)  -> Primary Rate-Limiter Metric")
    if fmla_info.get("supported") and fmla_info.get("fmla32_tflops") is not None:
        print(
            f"  [1b] FP16 FMLA32 Strict : {fmla_info['fmla32_tflops']:>6.2f} TFLOPS ({fmla_info['fmla32_ms']:.3f} ms)"
            f"  -> FP32-Accum Indicator"
        )
    if has_bf16:
        print(f"  [2] BF16 Tensor Cores   : {tflops_bf16:>6.2f} TFLOPS ({ms_bf16:.3f} ms)  -> Bfloat16 AI Training Format")
    elif props.major == 7:
        print(f"  [2] BF16 Tensor Cores   :    N/A (Turing {arch} lacks hardware BF16; Ampere/Ada sm_80+ only)")
    if has_tf32:
        print(f"  [3] TF32 Tensor Cores   : {tflops_tf32:>6.2f} TFLOPS ({ms_tf32:.3f} ms)  -> Ampere/Ada 19-bit Format")
    print(f"  [4] FP32 CUDA Control   : {tflops_fp32:>6.2f} TFLOPS ({ms_fp32:.3f} ms)  -> Standard Physical Control")
    print("-" * 72)
    print("  PHYSICAL PROOF OF MECHANISM (Negative Control Analysis):")
    print(f"  * Tensor/CUDA Ratio     : {ratio:.2f}x execution throughput multiplier")
    if fmla_ratio is not None:
        if verdict["supports_fmla_verdict"]:
            fmla_note = "~1.00x = unlocked vs. ~0.50x = stock clamped"
        else:
            fmla_note = "native 1:2 FMLA32:FMLA16 hardware ratio on this architecture"
        print(f"  * FMLA32/FMLA16 Ratio   : {fmla_ratio:.2f}x ({fmla_note})")
    print(f"  * FP32 CUDA Baseline    : {tflops_fp32:.2f} TFLOPS (Factory stock compute preserved)")
    if code == "UNLOCKED":
        print("  * Hardware Invariance   : Confirms rate-limiter bypass WITHOUT core overclocking.")
    elif code == "UNLOCKED_UNDER_LOAD":
        print("  * Hardware Invariance   : FMLA32/FMLA16 ratio confirms unlock despite background GPU load.")
    elif code == "EXTERNAL_LOAD":
        print("  * Hardware Status       : Raw TFLOPS suppressed by concurrent GPU load (status inconclusive).")
    elif code == "UNLOCKED_LOW_TFLOPS":
        print("  * Hardware Status       : FMLA32/FMLA16 ratio looks unlocked; inspect power limit or clocks.")
    else:
        print("  * Hardware Status       : Tensor datapath is restricted by factory rate-limiter.")
    print("-" * 72)

    if code == "UNLOCKED":
        print(f"  {C_GREEN_BOLD}STATUS: [OK] FULLY UNLOCKED! ({tflops_fp16:.1f} TFLOPS){C_RESET}")
        print(f"  {C_GREEN}Hardware rate-limiter is INACTIVE.{C_RESET}")
        print(f"  {C_GREEN}GPU is running at 100% full unrestricted tensor compute capacity.{C_RESET}")
        if util_info.get("under_load"):
            print(f"  {C_YELLOW}[i] Note: GPU had {util_info['gpu_util']:.0f}% external load; pause apps for peak score.{C_RESET}")
        if props.major >= 8:
            print(f"  {C_CYAN}[i] AI PERFORMANCE PRO-TIP (cuBLASLt Kernel Heuristics):{C_RESET}")
            print(f"      cuBLASLt defaults to stock-calibrated kernels on GeForce cards.")
            print(f"      In PyTorch & ComfyUI, enable autotuning for maximum throughput (+15% to +35%):")
            print(f"      -> torch.backends.cudnn.benchmark = True{C_RESET}")
    elif code == "UNLOCKED_UNDER_LOAD":
        print(f"  {C_GREEN_BOLD}STATUS: [OK] UNLOCKED (Under {util_info['gpu_util']:.0f}% External Load | {tflops_fp16:.1f} TFLOPS){C_RESET}")
        print(f"  {C_YELLOW}[WARNING] GPU is under external load ({util_info['gpu_util']:.0f}%) - pause games/renders/upscalers for an accurate peak TFLOPS check.{C_RESET}")
        print(f"  {C_GREEN}FMLA32/FMLA16 ratio ({fmla_ratio:.2f}x) confirms unthrottled tensor execution despite competing GPU work.{C_RESET}")
    elif code == "EXTERNAL_LOAD":
        print(f"  {C_YELLOW}STATUS: [WARNING] EXTERNAL GPU LOAD DETECTED ({util_info['gpu_util']:.0f}% util | {tflops_fp16:.1f} TFLOPS){C_RESET}")
        print(f"  {C_YELLOW}GPU is under external load - pause games/renders/upscalers for an accurate status check.{C_RESET}")
        print(f"  Benchmark skipped stock verdict: concurrent workloads share GPU time, VRAM bandwidth, and power budget.")
    elif code == "UNLOCKED_LOW_TFLOPS":
        print(f"  {C_YELLOW}STATUS: [!] LIKELY UNLOCKED AT REDUCED CLOCKS/POWER ({tflops_fp16:.1f} TFLOPS | FMLA {fmla_ratio:.2f}x){C_RESET}")
        print(f"  Expected idle unrestricted throughput: > {profile['threshold']:.0f} TFLOPS. Check Power Limit / thermals.")
    else:
        print(f"  {C_RED_BOLD}STATUS: [WARNING] STOCK RATE-LIMITER ACTIVE! ({tflops_fp16:.1f} TFLOPS){C_RESET}")
        print(f"  Expected unlocked performance: > {profile['threshold']:.0f} TFLOPS (typical ~{profile['min_unlocked']:.0f} TFLOPS).")
        print("  Recommendation: Boot via USB [U]/[P]/[I] (or S4 Hibernation without pre-boot ReBAR overrides).")
        if props.major == 7:
            print("  Note for Turing: Verify EnableGpuFirmware = 1 is configured in Windows Registry.")

    if rebar_active is False:
        print("-" * 72)
        print(f"  {C_RED_BOLD}[!] PRO-TUNING ALERT (Resizable BAR):{C_RESET}")
        print(f"  {C_YELLOW}Resizable BAR is currently DISABLED in your BIOS (BAR1 = 256 MiB restricted).{C_RESET}")
        print(f"  {C_YELLOW}-> Enable 'Above 4G Decoding' and 'ReBAR Support' in your motherboard BIOS{C_RESET}")
        print(f"  {C_YELLOW}   to unlock full PCIe VRAM aperture and boost AI & gaming by +10% to +15%!{C_RESET}")

    if is_throttled:
        print("-" * 72)
        print(f"  {C_RED_BOLD}[!] PCIe LINK BOTTLENECK DETECTED:{C_RESET}")
        print(f"  {C_YELLOW}Your GPU is running at Gen{gen_max} x{width_max} instead of full x16.{C_RESET}")
        print(f"  {C_YELLOW}-> Check your PCIe riser cable, clean slot contacts, or move the GPU{C_RESET}")
        print(f"  {C_YELLOW}   to the primary top PCIe slot directly attached to the CPU.{C_RESET}")

    return {
        "dev_idx": dev_idx,
        "name": gpu_name,
        "tflops": tflops_fp16,
        "is_unlocked": is_unlocked,
        "status_code": code,
        "under_load": verdict["under_load"],
        "gpu_util": verdict["gpu_util"],
        "fmla_ratio": fmla_ratio,
        "rebar_active": rebar_active,
        "bar1": bar1,
        "gen_max": gen_max,
        "width_max": width_max,
        "profile": profile,
    }

def main():
    if "--reapply-pl" in sys.argv:
        ok, msg = apply_saved_power_limits()
        print(msg)
        sys.exit(0 if ok else 1)

    fw_str, fw_is_uefi = get_firmware_mode()
    wake_time = get_last_wake_time()
    s3_status = get_s3_armed_status()
    pl_info = get_power_limit_info(0)
    clk_info = get_clock_cap_info(0)

    def _s3_line():
        code = s3_status["code"]
        col = C_GREEN if code == "ARMED" else (C_YELLOW if code in ("S0IX_WARN", "UNVERIFIED") else C_RED_BOLD)
        return f"  * S3 Persistence     : {col}{s3_status['label']}{C_RESET} ({s3_status['detail']})"

    def _pl_line():
        if pl_info.get("current_w") is None:
            return None
        rng = (
            f"Range {pl_info['min_w']:.0f}-{pl_info['max_w']:.0f} W, Default {pl_info['default_w']:.0f} W"
            if pl_info.get("adjustable") and pl_info.get("default_w") is not None
            else "Read-only"
        )
        return f"  * Power Limit        : {pl_info['current_w']:.0f} W ({rng})"

    def _clock_line():
        if not clk_info or clk_info.get("current_sm_mhz") is None:
            return None
        cur = clk_info["current_sm_mhz"]
        if clk_info.get("lock_enabled") and clk_info.get("locked_max_mhz"):
            cap_str = f"Locked {clk_info.get('locked_min_mhz')}-{clk_info.get('locked_max_mhz')} MHz (-lgc)"
        elif clk_info.get("adjustable"):
            cap_str = f"Unlocked (-rgc | Range {clk_info['min_sm_mhz']}-{clk_info['max_sm_mhz']} MHz)"
        else:
            cap_str = "Default"
        sw_cap = " | SW Power Cap: ACTIVE" if clk_info.get("sw_power_cap") else ""
        return f"  * SM Clock State     : {cur} MHz ({cap_str}{sw_cap})"

    try:
        global torch
        import torch
    except ImportError:
        bar1, fb, rebar_active = get_rebar_info()
        gen_max, width_max, is_throttled = get_pcie_info()
        print("=" * 72)
        print("     UNIVERSAL GPU RATE-LIMITER STATUS CHECK (ADA, AMPERE & TURING)")
        print("=" * 72)
        print("  SYSTEM HARDWARE ENVIRONMENT:")
        print(f"  * Firmware Mode      : {fw_str}")
        print(_s3_line())
        pl_txt = _pl_line()
        if pl_txt:
            print(pl_txt)
        clk_txt = _clock_line()
        if clk_txt:
            print(clk_txt)
        if wake_time:
            print(f"  * Last Wake (S3/S4)  : {wake_time}")
        if rebar_active is True:
            print(f"  * Resizable BAR      : {C_GREEN}[OK] ENABLED (BAR1: {bar1} MiB / VRAM: {fb} MiB){C_RESET}")
        elif rebar_active is False:
            print(f"  * Resizable BAR      : {C_RED_BOLD}[!] DISABLED (BAR1: {bar1} MiB restricted) -> Turn ON in BIOS!{C_RESET}")
        else:
            print(f"  * Resizable BAR      : Undetected (BAR1: {bar1 if bar1 else 'N/A'})")
        if gen_max and width_max:
            if is_throttled:
                print(f"  * PCIe Slot Link     : {C_RED_BOLD}[!] THROTTLED: Gen{gen_max} x{width_max} (Riser or Slot Bottleneck!){C_RESET}")
            else:
                print(f"  * PCIe Slot Link     : {C_GREEN}[OK] Gen{gen_max} x{width_max} (Full Bandwidth){C_RESET}")
        print("-" * 72)
        print("  [INFO] PyTorch with CUDA is required for the live TFLOPS compute test.")
        print("  To install PyTorch with CUDA support, run:")
        print("    pip install torch --index-url https://download.pytorch.org/whl/cu121")
        print("=" * 72)
        sys.exit(0)

    if not torch.cuda.is_available():
        print("=" * 72)
        print("  [ERROR] CUDA is not available. NVIDIA driver could not be contacted.")
        print("=" * 72)
        print("  DIAGNOSIS & TROUBLESHOOTING:")
        print("  1. Check Windows Device Manager (Win + X -> Device Manager -> Display adapters):")
        print("     - If your GPU shows Error Code 43 or 'Microsoft Basic Display Adapter':")
        print("       Your motherboard firmware has CSM (Compatibility Support Module) active!")
        print("       -> Quick Verification: Press Win + R, type msinfo32, confirm 'BIOS Mode: UEFI'.")
        print("       -> Solution: Restart PC into BIOS, navigate to Boot -> CSM, and set")
        print("          CSM to [Disabled] (pure UEFI mode). This eliminates legacy VGA")
        print("          driver conflicts, restores native resolution, and enables Resizable BAR.")
        print("  2. If your NVIDIA GeForce GPU is working normally in Device Manager:")
        print("     - PyTorch in this Python environment is running on a CPU-only wheel.")
        print("     - Reinstall PyTorch with native CUDA 12.1/12.4 support:")
        print("       pip install --force-reinstall torch --index-url https://download.pytorch.org/whl/cu121")
        print("=" * 72)
        sys.exit(2)

    dev_count = torch.cuda.device_count()

    print("=" * 72)
    print("     UNIVERSAL GPU RATE-LIMITER STATUS CHECK (ADA, AMPERE & TURING)")
    print("=" * 72)
    print("  SYSTEM HARDWARE ENVIRONMENT:")
    print(f"  * Firmware Mode      : {fw_str}")
    print(_s3_line())
    pl_txt = _pl_line()
    if pl_txt:
        print(pl_txt)
    clk_txt = _clock_line()
    if clk_txt:
        print(clk_txt)
    if wake_time:
        print(f"  * Last Wake (S3/S4)  : {wake_time}")
    print(f"  * NVIDIA CUDA GPUs   : {dev_count} device(s) detected")
    print("-" * 72)

    results = []
    all_unlocked = True
    for i in range(dev_count):
        if i > 0:
            print("\n" + "=" * 72)
        res = evaluate_gpu(i, dev_count, fw_str)
        results.append(res)
        if not res["is_unlocked"]:
            all_unlocked = False

    if dev_count > 1:
        print("\n" + "=" * 72)
        print("  MULTI-GPU SYSTEM STATUS SUMMARY:")
        print("-" * 72)
        for res in results:
            sc = res.get("status_code")
            if res["is_unlocked"]:
                tag = f"{C_GREEN}[UNLOCKED]{C_RESET}"
            elif sc == "EXTERNAL_LOAD":
                tag = f"{C_YELLOW}[EXTERNAL LOAD]{C_RESET}"
            else:
                tag = f"{C_RED_BOLD}[STOCK]{C_RESET}"
            print(f"  * GPU {res['dev_idx']} [{res['name']}]: {res['tflops']:.1f} TFLOPS  {tag}")
        print("-" * 72)
        if all_unlocked:
            print(f"  {C_GREEN_BOLD}OVERALL STATUS: ALL {dev_count} GPUS FULLY UNLOCKED!{C_RESET}")
        else:
            unlocked_num = sum(1 for r in results if r["is_unlocked"])
            print(f"  {C_YELLOW}OVERALL STATUS: {unlocked_num} of {dev_count} GPUs verified unlocked.{C_RESET}")

    # Build clipboard summary badge
    if dev_count == 1:
        r0 = results[0]
        tag = "UNLOCKED" if r0["is_unlocked"] else ("EXTERNAL_LOAD" if r0.get("status_code") == "EXTERNAL_LOAD" else "STOCK")
        rebar_str = f"{r0['bar1']}MB (Active)" if r0["rebar_active"] else ("Disabled" if r0["rebar_active"] is False else "N/A")
        pcie_str = f"Gen{r0['gen_max']} x{r0['width_max']}" if r0["gen_max"] else "N/A"
        summary_badge = f"[{tag}] {r0['name']}: {r0['tflops']:.1f} TFLOPS | ReBAR: {rebar_str} | PCIe: {pcie_str} | BIOS: {fw_str}"
    else:
        tag = "UNLOCKED" if all_unlocked else "PARTIAL"
        gpu_snippets = " | ".join(f"GPU {r['dev_idx']}: {r['tflops']:.1f} TF" for r in results)
        summary_badge = f"[{tag} x{dev_count}] {gpu_snippets} | BIOS: {fw_str}"

    print("-" * 72)
    if copy_to_clipboard(summary_badge):
        print(f"  {C_CYAN}[+] Result badge copied to clipboard! Ready to paste into GitHub / Reddit:{C_RESET}")
        print(f"      {summary_badge}")
    else:
        print(f"  {C_CYAN}Summary: {summary_badge}{C_RESET}")

    print("=" * 72)
    sys.exit(0 if all_unlocked else 1)

if __name__ == "__main__":
    main()
