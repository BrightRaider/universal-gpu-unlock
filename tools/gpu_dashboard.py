"""
Universal GPU Rate-Limiter Dashboard & Benchmark GUI
SPDX-License-Identifier: GPL-2.0-only
"""
import os
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import messagebox, ttk

# Shared helpers live next to this script (tools/gu_system_info.py).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gu_system_info import (  # noqa: E402
    CARD_PROFILES,
    apply_saved_power_limits,
    configure_wake_reapply_task,
    find_nvidia_smi,
    get_firmware_mode,
    get_last_wake_time,
    get_power_limit_info,
    get_rebar_info,
    get_s3_armed_status,
    load_benchmark_baseline,
    load_user_config,
    save_benchmark_baseline,
    set_power_limit,
)


def get_gpu_list():
    gpus = []
    try:
        import torch
        if torch.cuda.is_available():
            count = torch.cuda.device_count()
            for i in range(count):
                name = torch.cuda.get_device_name(i)
                props = torch.cuda.get_device_properties(i)
                vram_gb = props.total_memory / (1024**3)
                arch = f"sm_{props.major}{props.minor}"
                sm = props.multi_processor_count
                gpus.append({
                    "index": i,
                    "name": name,
                    "arch": arch,
                    "sm": sm,
                    "vram": f"{vram_gb:.1f} GB",
                    "props": props,
                    "torch_ok": True,
                })
            if gpus:
                return gpus
    except Exception:
        pass

    # Fallback to nvidia-smi if torch not imported
    try:
        smi_exe = find_nvidia_smi() or "nvidia-smi"
        kwargs = {"text": True}
        if os.name == "nt":
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        out = subprocess.check_output(
            [smi_exe, "--query-gpu=index,name,memory.total", "--format=csv,noheader"],
            **kwargs,
        )
        for line in out.strip().splitlines():
            parts = [p.strip() for p in line.split(",")]
            if len(parts) >= 3:
                gpus.append({
                    "index": int(parts[0]),
                    "name": parts[1],
                    "arch": "NVIDIA",
                    "sm": 0,
                    "vram": parts[2],
                    "props": None,
                    "torch_ok": False,
                })
        if gpus:
            return gpus
    except Exception:
        pass

    return [{
        "index": 0,
        "name": "NVIDIA GPU Not Detected",
        "arch": "Unknown",
        "sm": 0,
        "vram": "N/A",
        "props": None,
        "torch_ok": False,
    }]


class GpuUnlockApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Universal GPU Unlock - Status & Diagnostics")
        self.geometry("920x860")
        self.minsize(840, 800)
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

        self.gpus = get_gpu_list()
        self.selected_gpu_idx = 0
        self.gpu_info = self.gpus[0]
        self.fw_mode, self.fw_is_uefi = get_firmware_mode()
        self.s3_wake = get_last_wake_time()
        self.s3_status = get_s3_armed_status()
        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        self.pl_info = get_power_limit_info(self.gpu_info["index"])
        self.last_metrics = None
        self.benchmark_running = False
        self._slider_syncing = False

        self.build_ui()
        self.sync_power_limit_ui()
        self.after(15000, self._poll_wake_event)

    def get_repo_dir(self):
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def get_repo_path(self, rel_path):
        repo_dir = self.get_repo_dir()
        p = os.path.join(repo_dir, rel_path)
        if os.path.exists(p):
            return p
        p_tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.path.basename(rel_path))
        if os.path.exists(p_tools):
            return p_tools
        p_root = os.path.join(repo_dir, os.path.basename(rel_path))
        if os.path.exists(p_root):
            return p_root
        return p

    def _s3_card_colors(self):
        code = self.s3_status.get("code")
        if code == "ARMED":
            return self.c_green
        if code in ("S0IX_WARN",):
            return self.c_yellow
        if code == "UNVERIFIED":
            return "#40C4FF" if self.fw_is_uefi else self.c_red
        return self.c_red

    def _s3_card_headline(self):
        if self.fw_is_uefi is False:
            return "Legacy CSM (Warning!)"
        return self.s3_status.get("label", "Unknown")

    def build_ui(self):
        header = tk.Frame(self, bg=self.c_bg, padx=20, pady=12)
        header.pack(fill="x")

        title_frame = tk.Frame(header, bg=self.c_bg)
        title_frame.pack(side="left", fill="both")

        tk.Label(
            title_frame,
            text="UNIVERSAL GPU UNLOCK DASHBOARD",
            font=("Segoe UI", 16, "bold"),
            fg=self.c_text_bright,
            bg=self.c_bg,
        ).pack(anchor="w")

        tk.Label(
            title_frame,
            text="Hardware Rate-Limiter Bypass, S3 Armed Telemetry & Stepless Power-Limit Control",
            font=("Segoe UI", 10),
            fg=self.c_text_dim,
            bg=self.c_bg,
        ).pack(anchor="w")

        if len(self.gpus) > 1:
            sel_frame = tk.Frame(header, bg=self.c_bg)
            sel_frame.pack(side="right", padx=10)
            tk.Label(
                sel_frame, text="Active Device:", font=("Segoe UI", 9, "bold"),
                fg=self.c_text_dim, bg=self.c_bg,
            ).pack(anchor="e")
            self.gpu_combo = ttk.Combobox(
                sel_frame,
                values=[f"GPU {g['index']}: {g['name']}" for g in self.gpus],
                state="readonly",
                width=28,
            )
            self.gpu_combo.current(0)
            self.gpu_combo.bind("<<ComboboxSelected>>", self.on_gpu_selected)
            self.gpu_combo.pack(anchor="e")

        container = tk.Frame(self, bg=self.c_bg, padx=20)
        container.pack(fill="both", expand=True)

        # Status Cards Frame
        cards_frame = tk.Frame(container, bg=self.c_bg)
        cards_frame.pack(fill="x", pady=4)
        cards_frame.columnconfigure((0, 1, 2), weight=1, uniform="card")

        # Card 1: GPU Identity
        self.card_gpu = self.create_card(cards_frame, 0, "GPU HARDWARE", self.gpu_info["name"])
        self.lbl_arch = tk.Label(
            self.card_gpu,
            text=f"Arch: {self.gpu_info['arch']} ({self.gpu_info['sm']} SMs) | VRAM: {self.gpu_info['vram']}",
            font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_arch.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 2: Firmware & S3 Persistence Live Traffic Light
        self.card_fw = self.create_card(
            cards_frame, 1, "BIOS FIRMWARE & S3 ARMED", self._s3_card_headline(), val_color=self._s3_card_colors()
        )
        self.lbl_card_val_s3 = self._last_card_val
        self.lbl_fw_detail = tk.Label(
            self.card_fw, text=self.firmware_detail_text(),
            font=("Segoe UI", 8), fg=self.c_text_dim, bg=self.c_card, wraplength=255, justify="left",
        )
        self.lbl_fw_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 3: Resizable BAR
        rebar_color = self.c_green if self.rebar_active else (self.c_red if self.rebar_active is False else self.c_text_dim)
        rebar_txt = "ENABLED (Full VRAM)" if self.rebar_active else ("DISABLED (256 MB)!" if self.rebar_active is False else "Undetected")
        self.card_rebar = self.create_card(cards_frame, 2, "RESIZABLE BAR (ReBAR)", rebar_txt, val_color=rebar_color)
        self.lbl_card_val_rebar = self._last_card_val
        bar_sub = f"BAR1: {self.bar1} MiB (Enable in BIOS!)" if (self.rebar_active is False and self.bar1) else (f"BAR1: {self.bar1} MiB" if self.bar1 else "Query unavailable")
        self.lbl_rebar_detail = tk.Label(
            self.card_rebar, text=bar_sub,
            font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_rebar_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Benchmark Display Box
        bench_box = tk.Frame(container, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        bench_box.pack(fill="x", pady=8)

        bench_header = tk.Frame(bench_box, bg=self.c_card, padx=16, pady=8)
        bench_header.pack(fill="x")

        tk.Label(
            bench_header, text="COMPUTE THROUGHPUT STATUS", font=("Segoe UI", 11, "bold"),
            fg=self.c_text_bright, bg=self.c_card,
        ).pack(side="left")

        self.lbl_status_badge = tk.Label(
            bench_header, text="READY TO BENCHMARK", font=("Segoe UI", 10, "bold"),
            fg=self.c_text_bright, bg="#333333", padx=10, pady=4,
        )
        self.lbl_status_badge.pack(side="right")

        tflops_center = tk.Frame(bench_box, bg=self.c_card, pady=4)
        tflops_center.pack(fill="x")

        self.lbl_tflops_big = tk.Label(
            tflops_center, text="--.- TFLOPS", font=("Segoe UI", 34, "bold"),
            fg=self.c_text_bright, bg=self.c_card,
        )
        self.lbl_tflops_big.pack()

        self.lbl_tflops_subtitle = tk.Label(
            tflops_center, text="FP16 Tensor Core Peak Performance (GEMM 4096x4096)",
            font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_tflops_subtitle.pack()

        self.lbl_baseline_delta = tk.Label(
            tflops_center, text=self.baseline_summary_text(),
            font=("Segoe UI", 9, "bold"), fg="#82B1FF", bg=self.c_card,
        )
        self.lbl_baseline_delta.pack(pady=(2, 0))

        metrics_frame = tk.Frame(bench_box, bg=self.c_card, padx=20, pady=8)
        metrics_frame.pack(fill="x")
        metrics_frame.columnconfigure((0, 1, 2, 3), weight=1)

        self.lbl_m_fp16 = self.create_metric_col(metrics_frame, 0, "FP16 Tensor", "--")
        self.lbl_m_bf16 = self.create_metric_col(metrics_frame, 1, "BF16 Tensor", "--")
        self.lbl_m_tf32 = self.create_metric_col(metrics_frame, 2, "TF32 Tensor", "--")
        self.lbl_m_fp32 = self.create_metric_col(metrics_frame, 3, "FP32 CUDA", "--")

        # Power Limit Control Card (Stepless Slider + Eco / Stock / Max Markers + Wake Re-apply)
        pl_box = tk.Frame(container, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1, padx=14, pady=10)
        pl_box.pack(fill="x", pady=6)

        pl_top = tk.Frame(pl_box, bg=self.c_card)
        pl_top.pack(fill="x")

        tk.Label(
            pl_top, text="POWER LIMIT & EFFICIENCY (Eco / Stock / Max)", font=("Segoe UI", 10, "bold"),
            fg=self.c_text_bright, bg=self.c_card,
        ).pack(side="left")

        self.lbl_pl_live = tk.Label(
            pl_top, text="Querying nvidia-smi...", font=("Segoe UI", 9),
            fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_pl_live.pack(side="right")

        pl_slider_row = tk.Frame(pl_box, bg=self.c_card, pady=4)
        pl_slider_row.pack(fill="x")

        self.pl_var = tk.IntVar(value=0)
        self.pl_scale = tk.Scale(
            pl_slider_row,
            from_=100,
            to=500,
            resolution=1,
            orient="horizontal",
            variable=self.pl_var,
            showvalue=False,
            bg=self.c_card,
            fg=self.c_text_bright,
            troughcolor="#2E2E2E",
            activebackground="#2979FF",
            highlightthickness=0,
            bd=0,
            command=self.on_pl_slider_move,
        )
        self.pl_scale.pack(side="left", fill="x", expand=True, padx=(0, 12))

        self.lbl_pl_preview = tk.Label(
            pl_slider_row, text="-- W (--%)", font=("Segoe UI", 10, "bold"),
            fg="#FFD600", bg=self.c_card, width=16, anchor="e",
        )
        self.lbl_pl_preview.pack(side="left", padx=(0, 8))

        self.btn_pl_apply = tk.Button(
            pl_slider_row, text="Apply PL", font=("Segoe UI", 9, "bold"),
            bg="#1565C0", fg="#FFFFFF", activebackground="#1E88E5", activeforeground="#FFFFFF",
            relief="flat", padx=12, pady=4, cursor="hand2", command=self.apply_slider_power_limit,
        )
        self.btn_pl_apply.pack(side="right")

        pl_markers_row = tk.Frame(pl_box, bg=self.c_card)
        pl_markers_row.pack(fill="x", pady=(2, 0))

        self.btn_pl_eco = tk.Button(
            pl_markers_row, text="Eco (~78%)", font=("Segoe UI", 8, "bold"),
            bg="#263238", fg="#80CBC4", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_power_marker("eco"),
        )
        self.btn_pl_eco.pack(side="left", padx=(0, 4))

        self.btn_pl_stock = tk.Button(
            pl_markers_row, text="Stock (100%)", font=("Segoe UI", 8, "bold"),
            bg="#263238", fg="#FFFFFF", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_power_marker("stock"),
        )
        self.btn_pl_stock.pack(side="left", padx=4)

        self.btn_pl_max = tk.Button(
            pl_markers_row, text="Max (VBIOS)", font=("Segoe UI", 8, "bold"),
            bg="#263238", fg="#FFAB91", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_power_marker("max"),
        )
        self.btn_pl_max.pack(side="left", padx=4)

        self.wake_reapply_var = tk.BooleanVar(
            value=bool(load_user_config().get("power_limit", {}).get("reapply_on_wake", False))
        )
        self.chk_wake_reapply = tk.Checkbutton(
            pl_markers_row,
            text="Re-apply PL on S3 Wake",
            variable=self.wake_reapply_var,
            command=self.on_wake_reapply_toggled,
            bg=self.c_card,
            fg=self.c_text_bright,
            selectcolor="#121212",
            activebackground=self.c_card,
            activeforeground=self.c_text_bright,
            font=("Segoe UI", 8),
        )
        self.chk_wake_reapply.pack(side="left", padx=12)

        self.btn_prof_ai = tk.Button(
            pl_markers_row, text="AI / Compute", font=("Segoe UI", 8, "bold"),
            bg="#00695C", fg="#FFFFFF", activebackground="#00897B", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.apply_quick_profile("ai"),
        )
        self.btn_prof_ai.pack(side="right", padx=(4, 0))

        self.btn_prof_gaming = tk.Button(
            pl_markers_row, text="Gaming / Quiet", font=("Segoe UI", 8, "bold"),
            bg="#2E2E2E", fg="#FFFFFF", activebackground="#424242", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.apply_quick_profile("gaming"),
        )
        self.btn_prof_gaming.pack(side="right", padx=4)

        # Progress / Status Log Line
        self.lbl_log = tk.Label(
            container, text="Press 'Run TFLOPS Benchmark' to measure live tensor throughput.",
            font=("Segoe UI", 9, "italic"), fg=self.c_text_dim, bg=self.c_bg,
        )
        self.lbl_log.pack(anchor="w", pady=(2, 4))

        # Action Buttons Frame (Row 1: Primary Controls)
        btn_frame1 = tk.Frame(container, bg=self.c_bg)
        btn_frame1.pack(fill="x", pady=4)

        self.btn_run = tk.Button(
            btn_frame1, text="RUN TFLOPS BENCHMARK", font=("Segoe UI", 11, "bold"),
            bg="#00C853", fg="#FFFFFF", activebackground="#00E676", activeforeground="#FFFFFF",
            relief="flat", padx=16, pady=8, cursor="hand2", command=self.start_benchmark_thread,
        )
        self.btn_run.pack(side="left", padx=(0, 6))

        self.btn_save_baseline = tk.Button(
            btn_frame1, text="Save Baseline", font=("Segoe UI", 9, "bold"),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=self.save_current_baseline,
            state="disabled",
        )
        self.btn_save_baseline.pack(side="left", padx=4)

        tk.Button(
            btn_frame1, text="Refresh Info", font=("Segoe UI", 10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=self.refresh_all,
        ).pack(side="left", padx=4)

        self.guard_active = self.check_idle_guard()
        guard_txt = "Idle Guard: ACTIVE" if self.guard_active else "Idle Guard: DEFAULT"
        guard_col = "#00C853" if self.guard_active else "#2A2A2A"
        self.btn_guard = tk.Button(
            btn_frame1, text=guard_txt, font=("Segoe UI", 9),
            bg=guard_col, fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=self.toggle_idle_guard,
        )
        self.btn_guard.pack(side="left", padx=4)

        tk.Button(
            btn_frame1, text="Device Manager", font=("Segoe UI", 9),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=lambda: subprocess.Popen(["devmgmt.msc"], shell=True),
        ).pack(side="right")

        tk.Button(
            btn_frame1, text="msinfo32", font=("Segoe UI", 9),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=lambda: subprocess.Popen(["msinfo32.exe"]),
        ).pack(side="right", padx=4)

        # Quick Tools & Integrations Frame (Row 2)
        tools_box = tk.LabelFrame(
            container, text=" Quick Utilities & Advanced Setup ",
            font=("Segoe UI", 9, "bold"), fg=self.c_text_dim, bg=self.c_bg, padx=10, pady=6,
        )
        tools_box.pack(fill="x", pady=(6, 6))

        tk.Button(
            tools_box, text="Turing GSP Mode", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=5,
            command=lambda: self.launch_script("tools/Toggle-Turing-GSP.bat"),
        ).pack(side="left", padx=3)

        tk.Button(
            tools_box, text="Sleep Shortcut", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=5,
            command=lambda: self.launch_script("tools/Toggle-Sleep-Button.bat"),
        ).pack(side="left", padx=3)

        tk.Button(
            tools_box, text="Fast-Resume Setup", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=5,
            command=lambda: self.launch_script("tools/Setup-FastResume.bat"),
        ).pack(side="left", padx=3)

        tk.Button(
            tools_box, text="Advanced Audits & Dumps (Control Center)", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=5,
            command=lambda: self.launch_script("tools/Universal-Control-Center.cmd"),
        ).pack(side="left", padx=3)

        tk.Button(
            tools_box, text="Setup / Update USB", font=("Segoe UI", 9, "bold"),
            bg="#1565C0", fg="#FFFFFF", activebackground="#1E88E5", relief="flat", padx=12, pady=5,
            command=lambda: self.launch_script("Setup.bat"),
        ).pack(side="right", padx=3)

    def sync_power_limit_ui(self):
        info = self.pl_info
        adjustable = bool(info and info.get("adjustable"))
        if not adjustable:
            err = info.get("error") if info else "unavailable"
            self.lbl_pl_live.config(text=f"Power limit read-only / locked ({err or 'VBIOS'})")
            self.lbl_pl_preview.config(text="Locked", fg=self.c_text_dim)
            for w in (self.pl_scale, self.btn_pl_apply, self.btn_pl_eco, self.btn_pl_stock,
                      self.btn_pl_max, self.chk_wake_reapply, self.btn_prof_ai, self.btn_prof_gaming):
                w.config(state="disabled")
            return

        min_w = int(round(info["min_w"]))
        max_w = int(round(info["max_w"]))
        cur_w = int(round(info["current_w"] if info["current_w"] is not None else info["stock_w"]))
        draw_txt = f"{info['draw_w']:.1f} W" if info.get("draw_w") is not None else "N/A"
        self.lbl_pl_live.config(
            text=f"Current: {cur_w} W | Draw: {draw_txt} | Range: {min_w}-{max_w} W (Default: {info['stock_w']} W)"
        )
        self.btn_pl_eco.config(text=f"Eco ({info['eco_w']} W)")
        self.btn_pl_stock.config(text=f"Stock ({info['stock_w']} W)")
        self.btn_pl_max.config(text=f"Max ({max_w} W)")

        for w in (self.pl_scale, self.btn_pl_apply, self.btn_pl_eco, self.btn_pl_stock,
                  self.btn_pl_max, self.chk_wake_reapply, self.btn_prof_ai, self.btn_prof_gaming):
            w.config(state="normal")

        self._slider_syncing = True
        self.pl_scale.config(from_=min_w, to=max_w)
        self.pl_var.set(cur_w)
        self._slider_syncing = False
        self.update_pl_preview(cur_w)

    def update_pl_preview(self, watts):
        stock_w = (self.pl_info or {}).get("stock_w") or 1
        pct = (float(watts) / float(stock_w)) * 100.0
        self.lbl_pl_preview.config(text=f"{int(watts)} W ({pct:.0f}%)", fg="#FFD600")

    def on_pl_slider_move(self, val):
        if self._slider_syncing:
            return
        try:
            self.update_pl_preview(int(float(val)))
        except Exception:
            pass

    def snap_power_marker(self, marker):
        if not self.pl_info or not self.pl_info.get("adjustable"):
            return
        if marker == "eco":
            target = self.pl_info["eco_w"]
        elif marker == "stock":
            target = self.pl_info["stock_w"]
        else:
            target = int(round(self.pl_info["max_w"]))
        self.pl_var.set(target)
        self.update_pl_preview(target)
        self.apply_power_limit_value(target)

    def apply_slider_power_limit(self):
        self.apply_power_limit_value(self.pl_var.get())

    def _set_pl_controls_state(self, state):
        for w in (self.pl_scale, self.btn_pl_apply, self.btn_pl_eco, self.btn_pl_stock,
                  self.btn_pl_max, self.chk_wake_reapply, self.btn_prof_ai, self.btn_prof_gaming):
            w.config(state=state)

    def apply_power_limit_value(self, target_w, enable_wake_after=None):
        if getattr(self, "_pl_busy", False):
            return
        self._pl_busy = True
        self._set_pl_controls_state("disabled")
        self.lbl_log.config(text=f"Applying {int(target_w)} W power limit (accept UAC if prompted)...", fg=self.c_yellow)
        idx = self.selected_gpu_idx
        repo_dir = self.get_repo_dir()

        def worker():
            ok = False
            readback = self.pl_info
            msg = ""
            wake_msg = None
            try:
                ok, readback, msg = set_power_limit(idx, target_w, allow_uac=True)
                if ok:
                    want_wake = enable_wake_after if enable_wake_after is not None else self.wake_reapply_var.get()
                    if want_wake:
                        _, wake_msg = configure_wake_reapply_task(True, repo_dir, idx, target_w)
                    elif enable_wake_after is False:
                        _, wake_msg = configure_wake_reapply_task(False, repo_dir, idx, target_w)
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_apply_power_limit(ok, readback, msg, wake_msg))

        threading.Thread(target=worker, daemon=True).start()

    def _finish_apply_power_limit(self, ok, readback, msg, wake_msg):
        self._pl_busy = False
        self.pl_info = readback
        self.sync_power_limit_ui()
        if ok:
            full_msg = f"{msg} | {wake_msg}" if wake_msg else msg
            self.lbl_log.config(text=full_msg, fg=self.c_green)
        else:
            self.lbl_log.config(text=msg, fg=self.c_red)

    def on_wake_reapply_toggled(self):
        if getattr(self, "_pl_busy", False):
            return
        self._pl_busy = True
        self._set_pl_controls_state("disabled")
        enabled = bool(self.wake_reapply_var.get())
        target = self.pl_var.get() if (self.pl_info and self.pl_info.get("adjustable")) else None
        idx = self.selected_gpu_idx
        repo_dir = self.get_repo_dir()

        def worker():
            ok = False
            msg = ""
            try:
                ok, msg = configure_wake_reapply_task(enabled, repo_dir, idx, target)
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_wake_toggle(ok, msg))

        threading.Thread(target=worker, daemon=True).start()

    def _finish_wake_toggle(self, ok, msg):
        self._pl_busy = False
        self.sync_power_limit_ui()
        self.lbl_log.config(text=msg, fg=self.c_green if ok else self.c_yellow)

    def apply_quick_profile(self, mode):
        if getattr(self, "_pl_busy", False):
            return
        if not self.pl_info or not self.pl_info.get("adjustable"):
            return
        if mode == "ai":
            target = int(round(self.pl_info["max_w"]))
            self.wake_reapply_var.set(True)
            wake_after = True
        else:
            target = int(round(self.pl_info["stock_w"]))
            self.wake_reapply_var.set(False)
            wake_after = False
        self.pl_var.set(target)
        self.update_pl_preview(target)
        self.apply_power_limit_value(target, enable_wake_after=wake_after)

    def _poll_wake_event(self):
        try:
            latest_wake = get_last_wake_time()
            if latest_wake and latest_wake != self.s3_wake:
                self.s3_wake = latest_wake
                if self.wake_reapply_var.get():
                    threading.Thread(target=apply_saved_power_limits, daemon=True).start()
                self.refresh_all()
        except Exception:
            pass
        self.after(15000, self._poll_wake_event)

    def baseline_summary_text(self):
        base = load_benchmark_baseline(self.selected_gpu_idx)
        if not base or not base.get("fp16"):
            return "Baseline: Not saved yet (run benchmark and click 'Save Baseline')"
        if self.last_metrics and self.last_metrics.get("fp16", 0) > 0:
            delta_pct = ((self.last_metrics["fp16"] - base["fp16"]) / base["fp16"]) * 100.0
            sign = "+" if delta_pct >= 0 else ""
            return f"vs. Baseline ({base['fp16']:.1f} TF saved {base['saved_at']}): {sign}{delta_pct:.1f}%"
        return f"Saved Baseline: {base['fp16']:.1f} TFLOPS FP16 ({base['saved_at']})"

    def save_current_baseline(self):
        if not self.last_metrics:
            return
        save_benchmark_baseline(self.selected_gpu_idx, self.gpu_info["name"], self.last_metrics)
        self.lbl_baseline_delta.config(text=self.baseline_summary_text(), fg="#00E676")
        self.lbl_log.config(text="Saved current benchmark metrics as local baseline.", fg=self.c_green)

    def on_gpu_selected(self, event=None):
        self.selected_gpu_idx = self.gpu_combo.current()
        self.gpu_info = self.gpus[self.selected_gpu_idx]
        self.lbl_card_val_gpu.config(text=self.gpu_info["name"])
        self.lbl_arch.config(text=f"Arch: {self.gpu_info['arch']} ({self.gpu_info['sm']} SMs) | VRAM: {self.gpu_info['vram']}")
        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        bar_sub = f"BAR1: {self.bar1} MiB" if self.bar1 else "Query unavailable"
        self.lbl_rebar_detail.config(text=bar_sub)
        self.pl_info = get_power_limit_info(self.gpu_info["index"])
        self.sync_power_limit_ui()
        self.lbl_baseline_delta.config(text=self.baseline_summary_text())

    def firmware_detail_text(self):
        csm = "CSM: Disabled" if self.fw_is_uefi else "CSM: Active!"
        wake = f" | Wake: {self.s3_wake}" if self.s3_wake else ""
        return f"{csm}{wake}\n{self.s3_status.get('detail', '')}"

    def launch_script(self, rel_path):
        """Opens a .bat/.cmd/.ps1 from the repository in its own console window."""
        script = self.get_repo_path(rel_path)
        if not os.path.exists(script):
            self.lbl_log.config(text=f"Not found: {rel_path}", fg=self.c_red)
            return
        subprocess.Popen(f'start "" cmd /c "{script}"', shell=True)

    def create_card(self, parent, col, title, val_text, val_color=None):
        if val_color is None:
            val_color = self.c_text_bright
        card = tk.Frame(parent, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        card.grid(row=0, column=col, padx=4, sticky="nsew")

        tk.Label(card, text=title, font=("Segoe UI", 8, "bold"), fg=self.c_text_dim, bg=self.c_card).pack(anchor="w", padx=12, pady=(10, 2))
        lbl_val = tk.Label(card, text=val_text, font=("Segoe UI", 10, "bold"), fg=val_color, bg=self.c_card)
        lbl_val.pack(anchor="w", padx=12, pady=(0, 4))
        self._last_card_val = lbl_val
        if col == 0:
            self.lbl_card_val_gpu = lbl_val
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
        fast_resume = self.get_repo_path("tools/Setup-FastResume.bat")
        target_arg = "--revert" if self.guard_active else "--yes"
        cmd = f'powershell -NoProfile -Command "Start-Process cmd.exe -ArgumentList \'/c \"\"{fast_resume}\"\" {target_arg} --no-pause\' -Verb RunAs -Wait"'
        try:
            self.lbl_log.config(text="Applying Idle Relock Guard configuration...", fg=self.c_yellow)
            subprocess.Popen(cmd, shell=True)
            self.after(2500, self.refresh_all)
        except Exception as e:
            self.lbl_log.config(text=f"Failed to toggle guard: {e}", fg=self.c_red)

    def refresh_all(self):
        self.gpus = get_gpu_list()
        if self.selected_gpu_idx >= len(self.gpus):
            self.selected_gpu_idx = 0
        self.gpu_info = self.gpus[self.selected_gpu_idx]
        self.lbl_card_val_gpu.config(text=self.gpu_info["name"])
        self.lbl_arch.config(text=f"Arch: {self.gpu_info['arch']} ({self.gpu_info['sm']} SMs) | VRAM: {self.gpu_info['vram']}")

        self.fw_mode, self.fw_is_uefi = get_firmware_mode()
        self.s3_wake = get_last_wake_time()
        self.s3_status = get_s3_armed_status()
        self.lbl_card_val_s3.config(text=self._s3_card_headline(), fg=self._s3_card_colors())
        self.lbl_fw_detail.config(text=self.firmware_detail_text())

        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        rebar_color = self.c_green if self.rebar_active else (self.c_red if self.rebar_active is False else self.c_text_dim)
        rebar_txt = "ENABLED (Full VRAM)" if self.rebar_active else ("DISABLED (256 MB)!" if self.rebar_active is False else "Undetected")
        self.lbl_card_val_rebar.config(text=rebar_txt, fg=rebar_color)
        bar_sub = f"BAR1: {self.bar1} MiB" if self.bar1 else "Query unavailable"
        self.lbl_rebar_detail.config(text=bar_sub)

        self.pl_info = get_power_limit_info(self.gpu_info["index"])
        self.sync_power_limit_ui()
        self.lbl_baseline_delta.config(text=self.baseline_summary_text())

        self.guard_active = self.check_idle_guard()
        if hasattr(self, "btn_guard"):
            self.btn_guard.config(
                text="Idle Guard: ACTIVE" if self.guard_active else "Idle Guard: DEFAULT",
                bg="#00C853" if self.guard_active else "#2A2A2A",
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

            dev_idx = self.selected_gpu_idx
            device = torch.device(f"cuda:{dev_idx}")
            gpu_name = torch.cuda.get_device_name(dev_idx)
            props = torch.cuda.get_device_properties(dev_idx)
            size = 4096

            if hasattr(torch.backends, "cudnn") and hasattr(torch.backends.cudnn, "benchmark"):
                torch.backends.cudnn.benchmark = True

            profile = None
            gpu_lower = gpu_name.lower()
            for key, prof in CARD_PROFILES.items():
                if key in gpu_lower:
                    profile = prof
                    break
            if profile is None:
                if props.major == 8 and props.minor == 9:
                    threshold = props.multi_processor_count * 1.35
                elif props.major == 7 and props.minor == 5:
                    threshold = props.multi_processor_count * 0.65
                else:
                    threshold = props.multi_processor_count * 1.15
                profile = {"min_unlocked": threshold * 1.15, "threshold": threshold, "stock_typical": threshold * 0.70}

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

            self.after(0, lambda: self.lbl_log.config(text=f"Measuring FP16 Tensor Cores on GPU {dev_idx}..."))
            ms_fp16, tflops_fp16 = bench(torch.float16, runs=25, warmup=8)

            has_bf16 = hasattr(torch.cuda, "is_bf16_supported") and torch.cuda.is_bf16_supported()
            if has_bf16:
                self.after(0, lambda: self.lbl_log.config(text=f"Measuring BF16 Tensor Cores on GPU {dev_idx}..."))
                ms_bf16, tflops_bf16 = bench(torch.bfloat16, runs=15, warmup=4)
            else:
                ms_bf16, tflops_bf16 = 0.0, 0.0

            has_tf32 = props.major >= 8 and hasattr(torch.backends.cuda.matmul, "allow_tf32")
            if has_tf32:
                torch.backends.cuda.matmul.allow_tf32 = True
                self.after(0, lambda: self.lbl_log.config(text=f"Measuring TF32 Tensor Cores on GPU {dev_idx}..."))
                ms_tf32, tflops_tf32 = bench(torch.float32, runs=15, warmup=4)
            else:
                ms_tf32, tflops_tf32 = 0.0, 0.0

            if hasattr(torch.backends.cuda.matmul, "allow_tf32"):
                torch.backends.cuda.matmul.allow_tf32 = False
            self.after(0, lambda: self.lbl_log.config(text=f"Measuring FP32 CUDA Control on GPU {dev_idx}..."))
            ms_fp32, tflops_fp32 = bench(torch.float32, runs=15, warmup=4)

            torch.cuda.empty_cache()

            is_unlocked = tflops_fp16 >= profile["threshold"]
            ratio = tflops_fp16 / tflops_fp32 if tflops_fp32 > 0 else 1.0

            self.after(0, lambda: self.display_benchmark_results(
                tflops_fp16, ms_fp16, tflops_bf16, ms_bf16, tflops_tf32, ms_tf32, tflops_fp32, ms_fp32,
                is_unlocked, ratio, profile,
            ))

        except Exception as e:
            self.after(0, lambda: self.finish_benchmark_error(str(e)))

    def display_benchmark_results(self, fp16, ms16, bf16, msbf, tf32, mstf, fp32, ms32, is_unlocked, ratio, profile):
        self.last_metrics = {"fp16": fp16, "bf16": bf16, "tf32": tf32, "fp32": fp32}
        self.btn_save_baseline.config(state="normal")

        self.lbl_tflops_big.config(text=f"{fp16:.2f} TFLOPS")
        self.lbl_m_fp16.config(text=f"{fp16:.1f} TFLOPS ({ms16:.2f}ms)")
        self.lbl_m_bf16.config(text=f"{bf16:.1f} TFLOPS" if bf16 > 0 else "N/A")
        self.lbl_m_tf32.config(text=f"{tf32:.1f} TFLOPS" if tf32 > 0 else "N/A")
        self.lbl_m_fp32.config(text=f"{fp32:.1f} TFLOPS ({ms32:.2f}ms)")
        self.lbl_baseline_delta.config(text=self.baseline_summary_text(), fg="#82B1FF")

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
