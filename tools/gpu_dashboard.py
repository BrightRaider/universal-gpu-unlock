"""
Universal GPU Rate-Limiter Dashboard & Benchmark GUI
SPDX-License-Identifier: GPL-2.0-only
"""
import sys
import os
import re
import time
import subprocess
import ctypes
import threading
import tkinter as tk
from tkinter import ttk, messagebox

# --- Hardware & System Inspection Helpers ---

# Shared helpers live next to this script (tools/gu_system_info.py).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gu_system_info import CARD_PROFILES, get_firmware_mode, get_rebar_info  # noqa: E402

def get_gpu_details():
    try:
        import torch
        if torch.cuda.is_available():
            name = torch.cuda.get_device_name(0)
            props = torch.cuda.get_device_properties(0)
            vram_gb = props.total_memory / (1024**3)
            arch = f"sm_{props.major}{props.minor}"
            sm = props.multi_processor_count
            return {
                "name": name,
                "arch": arch,
                "sm": sm,
                "vram": f"{vram_gb:.1f} GB",
                "props": props,
                "torch_ok": True
            }
    except Exception:
        pass
    
    # Fallback to nvidia-smi if torch not imported
    try:
        out = subprocess.check_output(["nvidia-smi", "--query-gpu=name,memory.total", "--format=csv,noheader"], text=True)
        parts = [p.strip() for p in out.strip().split(",")]
        return {
            "name": parts[0] if parts else "NVIDIA GPU",
            "arch": "NVIDIA",
            "sm": 0,
            "vram": parts[1] if len(parts) > 1 else "N/A",
            "props": None,
            "torch_ok": False
        }
    except Exception:
        return {
            "name": "NVIDIA GPU Not Detected",
            "arch": "Unknown",
            "sm": 0,
            "vram": "N/A",
            "props": None,
            "torch_ok": False
        }

class GpuUnlockApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Universal GPU Unlock - Status & Diagnostics")
        self.geometry("820x680")
        self.minsize(780, 640)
        self.configure(bg="#121212")

        # Color Palette
        self.c_bg = "#121212"
        self.c_card = "#1E1E1E"
        self.c_card_border = "#2D2D2D"
        self.c_text_bright = "#FFFFFF"
        self.c_text_dim = "#A0A0A0"
        self.c_green = "#00E676"
        self.c_yellow = "#FFD600"
        self.c_red = "#FF5252"
        self.c_blue = "#2979FF"

        self.gpu_info = get_gpu_details()
        self.fw_mode, self.fw_is_uefi = get_firmware_mode()
        self.bar1, self.fb, self.rebar_active = get_rebar_info()

        self.benchmark_running = False
        self.build_ui()

    def build_ui(self):
        # Header Frame
        header = tk.Frame(self, bg=self.c_bg, padx=20, pady=16)
        header.pack(fill="x")

        title_lbl = tk.Label(
            header,
            text="UNIVERSAL GPU UNLOCK DASHBOARD",
            font=("Segoe UI", 16, "bold"),
            fg=self.c_text_bright,
            bg=self.c_bg
        )
        title_lbl.pack(anchor="w")

        sub_lbl = tk.Label(
            header,
            text="Hardware Rate-Limiter Status, System Firmware & Live TFLOPS Benchmark",
            font=("Segoe UI", 10),
            fg=self.c_text_dim,
            bg=self.c_bg
        )
        sub_lbl.pack(anchor="w")

        # Main Content Scrollable / Container
        container = tk.Frame(self, bg=self.c_bg, padx=20)
        container.pack(fill="both", expand=True)

        # Status Cards Frame (Grid)
        cards_frame = tk.Frame(container, bg=self.c_bg)
        cards_frame.pack(fill="x", pady=6)
        cards_frame.columnconfigure((0, 1, 2), weight=1, uniform="card")

        # Card 1: GPU Identity
        self.card_gpu = self.create_card(cards_frame, 0, "GPU HARDWARE", self.gpu_info["name"])
        lbl_arch = tk.Label(self.card_gpu, text=f"Arch: {self.gpu_info['arch']} ({self.gpu_info['sm']} SMs) | VRAM: {self.gpu_info['vram']}",
                            font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card)
        lbl_arch.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 2: Firmware & CSM
        fw_color = self.c_green if self.fw_is_uefi else (self.c_red if self.fw_is_uefi is False else self.c_yellow)
        fw_status = "Pure UEFI (OK)" if self.fw_is_uefi else "Legacy CSM (Warning!)"
        self.card_fw = self.create_card(cards_frame, 1, "BIOS FIRMWARE MODE", fw_status, val_color=fw_color)
        lbl_fw_detail = tk.Label(self.card_fw, text="CSM: Disabled" if self.fw_is_uefi else "CSM: Active (Disable in BIOS!)",
                                 font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card)
        lbl_fw_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 3: Resizable BAR
        rebar_color = self.c_green if self.rebar_active else (self.c_red if self.rebar_active is False else self.c_text_dim)
        rebar_txt = "ENABLED (Full VRAM)" if self.rebar_active else ("DISABLED (256 MB)!" if self.rebar_active is False else "Undetected")
        self.card_rebar = self.create_card(cards_frame, 2, "RESIZABLE BAR (ReBAR)", rebar_txt, val_color=rebar_color)
        bar_sub = f"BAR1: {self.bar1} MiB (Enable in BIOS!)" if (self.rebar_active is False and self.bar1) else (f"BAR1: {self.bar1} MiB" if self.bar1 else "Query unavailable")
        lbl_rebar_detail = tk.Label(self.card_rebar, text=bar_sub,
                                    font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card)
        lbl_rebar_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Main Benchmark Display Box
        bench_box = tk.Frame(container, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        bench_box.pack(fill="x", pady=12)

        bench_header = tk.Frame(bench_box, bg=self.c_card, padx=16, pady=12)
        bench_header.pack(fill="x")

        tk.Label(bench_header, text="COMPUTE THROUGHPUT STATUS", font=("Segoe UI", 11, "bold"),
                 fg=self.c_text_bright, bg=self.c_card).pack(side="left")

        self.lbl_status_badge = tk.Label(
            bench_header, text="READY TO BENCHMARK", font=("Segoe UI", 10, "bold"),
            fg=self.c_text_bright, bg="#333333", padx=10, pady=4
        )
        self.lbl_status_badge.pack(side="right")

        # TFLOPS Display Center
        tflops_center = tk.Frame(bench_box, bg=self.c_card, pady=10)
        tflops_center.pack(fill="x")

        self.lbl_tflops_big = tk.Label(
            tflops_center, text="--.- TFLOPS", font=("Segoe UI", 36, "bold"),
            fg=self.c_text_bright, bg=self.c_card
        )
        self.lbl_tflops_big.pack()

        self.lbl_tflops_subtitle = tk.Label(
            tflops_center, text="FP16 Tensor Core Peak Performance (GEMM 4096x4096)",
            font=("Segoe UI", 10), fg=self.c_text_dim, bg=self.c_card
        )
        self.lbl_tflops_subtitle.pack()

        # Detailed Metrics Table
        metrics_frame = tk.Frame(bench_box, bg=self.c_card, padx=20, pady=12)
        metrics_frame.pack(fill="x")
        metrics_frame.columnconfigure((0, 1, 2, 3), weight=1)

        self.lbl_m_fp16 = self.create_metric_col(metrics_frame, 0, "FP16 Tensor", "--")
        self.lbl_m_bf16 = self.create_metric_col(metrics_frame, 1, "BF16 Tensor", "--")
        self.lbl_m_tf32 = self.create_metric_col(metrics_frame, 2, "TF32 Tensor", "--")
        self.lbl_m_fp32 = self.create_metric_col(metrics_frame, 3, "FP32 CUDA", "--")

        # Progress / Status Log Line
        self.lbl_log = tk.Label(
            container, text="Press 'Run TFLOPS Benchmark' to measure live tensor throughput.",
            font=("Segoe UI", 9, "italic"), fg=self.c_text_dim, bg=self.c_bg
        )
        self.lbl_log.pack(anchor="w", pady=(4, 8))

        # Action Buttons Frame
        btn_frame = tk.Frame(container, bg=self.c_bg)
        btn_frame.pack(fill="x", pady=6)

        self.btn_run = tk.Button(
            btn_frame, text="RUN TFLOPS BENCHMARK", font=("Segoe UI", 11, "bold"),
            bg="#00C853", fg="#FFFFFF", activebackground="#00E676", activeforeground="#FFFFFF",
            relief="flat", padx=20, pady=10, cursor="hand2", command=self.start_benchmark_thread
        )
        self.btn_run.pack(side="left", padx=(0, 10))

        btn_refresh = tk.Button(
            btn_frame, text="Refresh Info", font=("Segoe UI", 10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=14, pady=10, cursor="hand2", command=self.refresh_all
        )
        btn_refresh.pack(side="left", padx=5)

        self.guard_active = self.check_idle_guard()
        guard_txt = "Idle Guard: ACTIVE" if self.guard_active else "Idle Guard: DEFAULT"
        guard_col = "#00C853" if self.guard_active else "#2A2A2A"
        self.btn_guard = tk.Button(
            btn_frame, text=guard_txt, font=("Segoe UI", 10),
            bg=guard_col, fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=14, pady=10, cursor="hand2", command=self.toggle_idle_guard
        )
        self.btn_guard.pack(side="left", padx=5)

        btn_msinfo = tk.Button(
            btn_frame, text="Hardware (msinfo32)", font=("Segoe UI", 10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=14, pady=10, cursor="hand2", command=lambda: subprocess.Popen(["msinfo32.exe"])
        )
        btn_msinfo.pack(side="right")

        btn_devmgr = tk.Button(
            btn_frame, text="Device Manager", font=("Segoe UI", 10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=14, pady=10, cursor="hand2", command=lambda: subprocess.Popen(["devmgmt.msc"], shell=True)
        )
        btn_devmgr.pack(side="right", padx=5)

    def create_card(self, parent, col, title, val_text, val_color=None):
        if val_color is None:
            val_color = self.c_text_bright
        card = tk.Frame(parent, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        card.grid(row=0, column=col, padx=4, sticky="nsew")

        tk.Label(card, text=title, font=("Segoe UI", 8, "bold"), fg=self.c_text_dim, bg=self.c_card).pack(anchor="w", padx=12, pady=(10, 2))
        tk.Label(card, text=val_text, font=("Segoe UI", 11, "bold"), fg=val_color, bg=self.c_card).pack(anchor="w", padx=12, pady=(0, 4))
        return card

    def create_metric_col(self, parent, col, title, initial_val):
        frame = tk.Frame(parent, bg=self.c_card)
        frame.grid(row=0, column=col, sticky="nsew")
        tk.Label(frame, text=title, font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card).pack()
        lbl_val = tk.Label(frame, text=initial_val, font=("Segoe UI", 12, "bold"), fg=self.c_text_bright, bg=self.c_card)
        lbl_val.pack()
        return lbl_val

    def check_idle_guard(self):
        try:
            import winreg
            base_key = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base_key) as root:
                for i in range(16):
                    try:
                        sub = winreg.EnumKey(root, i)
                        with winreg.OpenKey(root, sub) as k:
                            try:
                                desc, _ = winreg.QueryValueEx(k, "DriverDesc")
                                if "NVIDIA" in str(desc).upper():
                                    val, _ = winreg.QueryValueEx(k, "RMDisableRtd3")
                                    if val == 1:
                                        return True
                            except OSError:
                                pass
                    except OSError:
                        break
        except Exception:
            pass
        return False

    def toggle_idle_guard(self):
        repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        fast_resume = os.path.join(repo_dir, "Setup-FastResume.bat")
        if not os.path.exists(fast_resume):
            fast_resume = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Setup-FastResume.bat")

        target_arg = "--revert" if self.guard_active else "--yes"
        cmd = f'powershell -NoProfile -Command "Start-Process cmd.exe -ArgumentList \'/c \"\"{fast_resume}\"\" {target_arg} --no-pause\' -Verb RunAs -Wait"'
        try:
            self.lbl_log.config(text="Applying Idle Relock Guard configuration...", fg=self.c_yellow)
            subprocess.Popen(cmd, shell=True)
            self.after(2500, self.refresh_all)
        except Exception as e:
            self.lbl_log.config(text=f"Failed to toggle guard: {e}", fg=self.c_red)

    def refresh_all(self):
        self.gpu_info = get_gpu_details()
        self.fw_mode, self.fw_is_uefi = get_firmware_mode()
        self.bar1, self.fb, self.rebar_active = get_rebar_info()
        self.guard_active = self.check_idle_guard()
        if hasattr(self, "btn_guard"):
            self.btn_guard.config(
                text="Idle Guard: ACTIVE" if self.guard_active else "Idle Guard: DEFAULT",
                bg="#00C853" if self.guard_active else "#2A2A2A"
            )
        self.lbl_log.config(text="Hardware configuration refreshed.", fg=self.c_green)

    def start_benchmark_thread(self):
        if self.benchmark_running:
            return
        self.benchmark_running = True
        self.btn_run.config(state="disabled", text="Running Benchmark...", bg="#555555")
        self.lbl_log.config(text="Initializing CUDA tensors and warming up kernels...", fg=self.c_yellow)
        threading.Thread(target=self.run_benchmark_worker, daemon=True).start()

    def run_benchmark_worker(self):
        try:
            import torch
            if not torch.cuda.is_available():
                self.after(0, lambda: self.finish_benchmark_error("CUDA is not available on this system."))
                return

            device = torch.device("cuda:0")
            gpu_name = torch.cuda.get_device_name(0)
            props = torch.cuda.get_device_properties(0)
            size = 4096

            if hasattr(torch.backends, "cudnn") and hasattr(torch.backends.cudnn, "benchmark"):
                torch.backends.cudnn.benchmark = True

            # Lookup Profile
            profile = None
            gpu_lower = gpu_name.lower()
            for key, prof in CARD_PROFILES.items():
                if key in gpu_lower:
                    profile = prof
                    break
            if profile is None:
                threshold = props.multi_processor_count * 1.35 if (props.major==8 and props.minor==9) else props.multi_processor_count * 1.15
                profile = {"min_unlocked": threshold * 1.15, "threshold": threshold, "stock": threshold * 0.75}

            def bench(dtype, runs=20, warmup=5):
                a = torch.randn((size, size), dtype=dtype, device=device)
                b = torch.randn((size, size), dtype=dtype, device=device)
                for _ in range(warmup):
                    torch.matmul(a, b)
                torch.cuda.synchronize(device)
                start_ev = torch.cuda.Event(enable_timing=True)
                end_ev = torch.cuda.Event(enable_timing=True)
                start_ev.record()
                for _ in range(runs):
                    torch.matmul(a, b)
                end_ev.record()
                torch.cuda.synchronize(device)
                ms = start_ev.elapsed_time(end_ev) / runs
                tflops = (2.0 * (size**3) / (ms * 1e-3)) / 1e12
                del a, b
                return ms, tflops

            self.after(0, lambda: self.lbl_log.config(text="Measuring FP16 Tensor Cores (GEMM)..."))
            ms_fp16, tflops_fp16 = bench(torch.float16, runs=25, warmup=8)

            has_bf16 = hasattr(torch.cuda, "is_bf16_supported") and torch.cuda.is_bf16_supported()
            if has_bf16:
                self.after(0, lambda: self.lbl_log.config(text="Measuring BF16 Tensor Cores..."))
                ms_bf16, tflops_bf16 = bench(torch.bfloat16, runs=15, warmup=4)
            else:
                ms_bf16, tflops_bf16 = 0.0, 0.0

            has_tf32 = props.major >= 8 and hasattr(torch.backends.cuda.matmul, "allow_tf32")
            if has_tf32:
                torch.backends.cuda.matmul.allow_tf32 = True
                self.after(0, lambda: self.lbl_log.config(text="Measuring TF32 Tensor Cores..."))
                ms_tf32, tflops_tf32 = bench(torch.float32, runs=15, warmup=4)
            else:
                ms_tf32, tflops_tf32 = 0.0, 0.0

            if hasattr(torch.backends.cuda.matmul, "allow_tf32"):
                torch.backends.cuda.matmul.allow_tf32 = False
            self.after(0, lambda: self.lbl_log.config(text="Measuring FP32 CUDA Control..."))
            ms_fp32, tflops_fp32 = bench(torch.float32, runs=15, warmup=4)

            torch.cuda.empty_cache()

            is_unlocked = tflops_fp16 >= profile["threshold"]
            ratio = tflops_fp16 / tflops_fp32 if tflops_fp32 > 0 else 1.0

            self.after(0, lambda: self.display_benchmark_results(
                tflops_fp16, ms_fp16, tflops_bf16, ms_bf16, tflops_tf32, ms_tf32, tflops_fp32, ms_fp32,
                is_unlocked, ratio, profile
            ))

        except Exception as e:
            self.after(0, lambda: self.finish_benchmark_error(str(e)))

    def display_benchmark_results(self, fp16, ms16, bf16, msbf, tf32, mstf, fp32, ms32, is_unlocked, ratio, profile):
        self.lbl_tflops_big.config(text=f"{fp16:.2f} TFLOPS")
        self.lbl_m_fp16.config(text=f"{fp16:.1f} TFLOPS ({ms16:.2f}ms)")
        self.lbl_m_bf16.config(text=f"{bf16:.1f} TFLOPS" if bf16 > 0 else "N/A")
        self.lbl_m_tf32.config(text=f"{tf32:.1f} TFLOPS" if tf32 > 0 else "N/A")
        self.lbl_m_fp32.config(text=f"{fp32:.1f} TFLOPS ({ms32:.2f}ms)")

        if is_unlocked:
            self.lbl_tflops_big.config(fg=self.c_green)
            self.lbl_status_badge.config(text="STATUS: FULLY UNLOCKED!", bg="#00C853", fg="#FFFFFF")
            log_text = f"Rate-limiter BYPASS CONFIRMED! ({ratio:.2f}x CUDA baseline). cuBLAS autotune active."
            self.lbl_log.config(text=log_text, fg=self.c_green)
        else:
            self.lbl_tflops_big.config(fg=self.c_red)
            self.lbl_status_badge.config(text="STATUS: STOCK RATE-LIMITED", bg="#D50000", fg="#FFFFFF")
            log_text = f"Rate-limiter ACTIVE ({fp16:.1f} TFLOPS). Expected unlocked performance: > {profile['threshold']:.0f} TFLOPS."
            self.lbl_log.config(text=log_text, fg=self.c_red)

        self.btn_run.config(state="normal", text="RUN TFLOPS BENCHMARK", bg="#00C853")
        self.benchmark_running = False

    def finish_benchmark_error(self, err_msg):
        self.lbl_status_badge.config(text="BENCHMARK ERROR", bg="#D50000")
        self.lbl_log.config(text=f"Error: {err_msg}", fg=self.c_red)
        self.btn_run.config(state="normal", text="RUN TFLOPS BENCHMARK", bg="#00C853")
        self.benchmark_running = False

if __name__ == "__main__":
    app = GpuUnlockApp()
    app.mainloop()
