"""Trilingual (EN / Simplified Chinese / DE) UI strings and config persistence for gpu_dashboard.py."""

import ctypes
import locale
import os

SUPPORTED_LANGUAGES = ("en", "zh", "de")
LANGUAGE_LABELS = {
    "en": "EN",
    "zh": "简体中文",
    "de": "DE",
}


def normalize_language(lang):
    """Normalize a language code or locale string into 'en', 'zh', or 'de'."""
    val = str(lang or "").strip().lower()
    if val.startswith("zh") or "chinese" in val or "简体" in val:
        return "zh"
    if val.startswith("de") or "german" in val or "deutsch" in val:
        return "de"
    if val in SUPPORTED_LANGUAGES:
        return val
    return "en"


def detect_system_language():
    """Detect the default Windows/OS UI language (falling back to English)."""
    if os.name == "nt":
        try:
            lang_id = ctypes.windll.kernel32.GetUserDefaultUILanguage()
            primary = lang_id & 0x3FF
            if primary == 0x04:  # LANG_CHINESE
                return "zh"
            if primary == 0x07:  # LANG_GERMAN
                return "de"
        except Exception:
            pass
    try:
        loc, _ = locale.getlocale()
        if loc:
            return normalize_language(loc)
    except Exception:
        pass
    return "en"


def _import_gsi():
    try:
        import gu_system_info as gsi
        return gsi
    except ImportError:
        from tools import gu_system_info as gsi
        return gsi


def load_saved_language():
    """Load persisted UI language from config.json, or detect from OS on first run."""
    try:
        gsi = _import_gsi()
        cfg = gsi.load_user_config()
        if isinstance(cfg, dict):
            ui_cfg = cfg.get("ui")
            if isinstance(ui_cfg, dict):
                raw = str(ui_cfg.get("language", "")).strip()
                if raw:
                    norm = normalize_language(raw)
                    if norm in SUPPORTED_LANGUAGES:
                        return norm
    except Exception:
        pass
    return detect_system_language()


def save_saved_language(lang):
    """Persist selected UI language into config.json under ui.language."""
    norm = normalize_language(lang)
    try:
        gsi = _import_gsi()
        cfg = gsi.load_user_config()
        if not isinstance(cfg, dict):
            cfg = {}
        ui_cfg = cfg.get("ui")
        if not isinstance(ui_cfg, dict):
            ui_cfg = {}
        ui_cfg["language"] = norm
        cfg["ui"] = ui_cfg
        gsi.save_user_config(cfg)
        sync_fn = getattr(gsi, "sync_boot_lang_cfg", None)
        if callable(sync_fn):
            sync_fn(norm)
    except Exception:
        pass
    return norm


def ui_font(lang, size, weight=None):
    """Return a clean Tkinter font tuple suitable for the active language."""
    family = "Microsoft YaHei UI" if (lang == "zh" and os.name == "nt") else "Segoe UI"
    if weight:
        return (family, size, weight)
    return (family, size)


STRINGS = {
    "en": {
        "window_title": "Universal GPU Unlock - Status & Diagnostics",
        "header_title": "UNIVERSAL GPU UNLOCK DASHBOARD",
        "header_subtitle": "Tensor Core Status, S3 Sleep Telemetry, Power Limit & SM Clock Control",
        "active_device": "Active Device:",
        "card_hw_title": "GPU HARDWARE",
        "hw_arch_vram": "Arch: {arch} ({sm} SMs) | VRAM: {vram}",
        "gpu_not_detected": "NVIDIA GPU Not Detected",
        "card_s3_title": "BIOS FIRMWARE & S3 ARMED",
        "fw_legacy_warn": "Legacy CSM (Warning)",
        "fw_csm_off": "CSM: Disabled",
        "fw_csm_on": "CSM: Active",
        "wake_label": "Wake: {wake}",
        "s3_label_armed": "UEFI + S3 Armed",
        "s3_label_s0ix": "UEFI (S0ix Standby!)",
        "s3_label_unverified": "UEFI (Unverified)",
        "s3_label_not_armed": "UEFI (S3 Not Armed)",
        "s3_detail_armed_log": "Confirmed via {log_name} (FACS: {facs})",
        "s3_detail_armed_mem": "Shadow FACS active @ {facs}",
        "s3_detail_s0ix": "Modern Standby active - run S3 Sleep Check",
        "s3_detail_unverified": "No S3-Resume log yet (check Wake)",
        "s3_detail_not_armed": "Boot USB -> Resume S3 Sleep -> Enable",
        "card_rebar_title": "RESIZABLE BAR (ReBAR)",
        "rebar_enabled": "Enabled (Full VRAM)",
        "rebar_disabled": "Disabled (256 MB)",
        "rebar_undetected": "Undetected",
        "rebar_bar1_bios": "BAR1: {bar1} MiB (Enable in BIOS)",
        "rebar_bar1_short": "BAR1: {bar1} MiB",
        "rebar_unavailable": "Query unavailable",
        "bench_title": "COMPUTE THROUGHPUT STATUS",
        "badge_ready": "READY TO BENCHMARK",
        "badge_running": "BENCHMARK RUNNING",
        "bench_tflops_default_sub": "FP16 Tensor Core Peak Performance (GEMM 4096x4096)",
        "bench_tflops_subtitle": "FP16 Tensor Core Peak Performance (GEMM 4096x4096{fmla_sub}{load_sub})",
        "bench_fmla_sub": " | FMLA32/16: {fmla:.2f}x",
        "bench_pre_load": " | Pre-Load: {util:.0f}%",
        "metric_fp16": "FP16 Tensor",
        "metric_bf16": "BF16 Tensor",
        "metric_tf32": "TF32 Tensor",
        "metric_fp32": "FP32 CUDA",
        "metric_tflops_ms": "{val:.1f} TFLOPS ({ms:.2f}ms)",
        "metric_tflops": "{val:.1f} TFLOPS",
        "na_value": "N/A",
        "baseline_none": "Baseline: Not saved yet (run benchmark and click 'Save Baseline')",
        "baseline_compared": "vs. Baseline ({base16:.1f} TF saved {ts}): {sign}{delta:.1f}%",
        "baseline_saved_only": "Saved Baseline: {base16:.1f} TFLOPS FP16 ({ts})",
        "badge_status_unlocked": "STATUS: UNLOCKED",
        "badge_status_unlocked_load": "STATUS: UNLOCKED (EXTERNAL LOAD)",
        "badge_status_external_load": "STATUS: EXTERNAL LOAD ({util:.0f}%)",
        "badge_status_low_tflops": "STATUS: CHECK POWER / CLOCKS",
        "badge_status_rate_limited": "STATUS: STOCK RATE-LIMITED",
        "badge_status_no_tensor": "STATUS: NO TENSOR CORES (GTX 16xx)",
        "badge_status_error": "BENCHMARK ERROR",
        "bench_verdict_unlocked": "Unlocked ({ratio:.2f}x CUDA baseline). cuBLAS autotune active.",
        "bench_verdict_unlocked_ext": " (Note: {util:.0f}% external load during bench)",
        "bench_verdict_under_load": "GPU under external load ({util:.0f}%) - pause games/renders/upscalers for peak TFLOPS (FMLA32/FMLA16 {fmla:.2f}x indicates unlock active).",
        "bench_verdict_external_load": "GPU is under external load ({util:.0f}%) - pause games/renders/upscalers for an accurate status check.",
        "bench_verdict_low_tflops": "FMLA32/FMLA16 ({fmla:.2f}x) looks unlocked, but TFLOPS ({fp16:.1f}) is below {thresh:.0f}. Check PL/thermals.",
        "bench_verdict_rate_limited": "Rate-limiter ACTIVE ({fp16:.1f} TFLOPS). Expected unlocked performance: > {thresh:.0f} TFLOPS.",
        "bench_verdict_no_tensor": "GTX 16-series cards lack Tensor Cores and are intentionally bypassed at boot.",
        "bench_error_msg": "Error: {err}",
        "log_bench_init": "Initializing CUDA tensors and warming up kernels...",
        "log_cuda_unavail": "CUDA is not available on this system.",
        "bench_step_fp16": "Measuring FP16 Tensor Cores on GPU {idx}...",
        "bench_step_fmla": "Measuring FMLA32/FMLA16 ratio on GPU {idx}...",
        "bench_step_bf16": "Measuring BF16 Tensor Cores on GPU {idx}...",
        "bench_step_tf32": "Measuring TF32 Tensor Cores on GPU {idx}...",
        "bench_step_fp32": "Measuring FP32 CUDA Control on GPU {idx}...",
        "pl_title": "POWER LIMIT & EFFICIENCY (Eco / Stock / Max)",
        "pl_querying": "Querying nvidia-smi...",
        "pl_unavailable": "unavailable",
        "pl_locked_err": "Power limit read-only / locked ({err})",
        "pl_preview_locked": "Locked",
        "pl_readout": "Current: {cur_w} W | Draw: {draw_txt} | Range: {min_w}-{max_w} W (Default: {stock_w} W)",
        "pl_target": "{w} W ({pct}%)",
        "btn_apply_pl": "Apply PL",
        "btn_eco": "Eco (~78%)",
        "btn_eco_w": "Eco ({w} W)",
        "btn_stock": "Stock (100%)",
        "btn_stock_w": "Stock ({w} W)",
        "btn_max": "Max (VBIOS)",
        "btn_max_w": "Max ({w} W)",
        "chk_wake_reapply": "Re-apply Tuning on S3 Wake",
        "btn_ai_eff": "AI Efficiency",
        "btn_gaming_stock": "Gaming / Stock",
        "pl_applying": "Applying {w} W power limit (accept UAC if prompted)...",
        "pl_preset_applying": "Applying {preset} profile (accept UAC if prompted)...",
        "clk_title": "SM CLOCK CAP (-lgc) — Ada / AI Efficiency",
        "clk_querying": "Querying SM clocks...",
        "clk_unavailable": "SM clock cap unavailable ({err})",
        "clk_unsupported": "Unsupported",
        "clk_cap_locked": "Cap: {min_mhz}-{max_mhz} MHz (-lgc)",
        "clk_cap_unlocked": "Cap: Unlocked (-rgc)",
        "clk_sw_power_cap": " | SW Power Cap: ACTIVE",
        "clk_live_readout": "Live SM: {cur_txt} | {cap_txt} | Range: {min_c}-{max_c} MHz{sw_cap_txt}",
        "clk_preview_max_eff": "{mhz} MHz (Max Eff)",
        "clk_preview_sweetspot": "{mhz} MHz (Sweetspot)",
        "clk_preview_custom": "{mhz} MHz (Custom)",
        "btn_clk_reset": "Reset (-rgc)",
        "btn_clk_apply": "Apply Cap",
        "btn_clk_max_eff": "Max Eff (~2300)",
        "btn_clk_max_eff_val": "Max Eff ({mhz} MHz)",
        "btn_clk_sweetspot": "Sweetspot (~2450)",
        "btn_clk_sweetspot_val": "Sweetspot ({mhz} MHz)",
        "clk_tip": "Tip: Open PL + -lgc reduces SW Power Cap oscillation (pair with +600 MHz VRAM; reset before gaming).",
        "clk_applying": "Locking SM clock ceiling to {mhz} MHz (-lgc, accept UAC if prompted)...",
        "clk_resetting": "Clearing SM clock lock (-rgc, accept UAC if prompted)...",
        "log_initial": "Press 'Run TFLOPS Benchmark' to measure live tensor throughput.",
        "btn_run_bench": "RUN TFLOPS BENCHMARK",
        "btn_running_bench": "Running Benchmark...",
        "btn_save_baseline": "Save Baseline",
        "btn_refresh": "Refresh Info",
        "btn_guard_active": "Idle Guard: ACTIVE",
        "btn_guard_default": "Idle Guard: DEFAULT",
        "btn_devmgmt": "Device Manager",
        "btn_msinfo": "msinfo32",
        "utils_title": " Quick Utilities & Advanced Setup ",
        "btn_turing_gsp": "Turing GSP Mode",
        "btn_sleep_shortcut": "Sleep Shortcut",
        "btn_fast_resume": "Fast-Resume Setup",
        "btn_adv_cc": "Control Center (Audits & Dumps)",
        "btn_export_log": "Export Boot Log",
        "btn_exporting_log": "Exporting...",
        "btn_usb_setup": "Setup / Update USB",
        "log_baseline_saved": "Saved current benchmark metrics as local baseline.",
        "log_not_found": "Not found: {rel_path}",
        "log_export_searching": "Searching USB, hidden GPU_UNLOCK, and ESP for RUN*.TXT boot logs...",
        "log_guard_applying": "Applying Idle Relock Guard configuration...",
        "log_guard_failed": "Failed to toggle guard: {err}",
        "log_refreshed": "Hardware configuration refreshed.",
        "log_wake_auto_refresh": "S3 Sleep Wake detected. Hardware & tuning status refreshed.",
        "log_lang_switched": "UI language set to {label}.",
    },
    "zh": {
        "window_title": "Universal GPU Unlock 控制台 - 状态与诊断",
        "header_title": "通用 GPU 解锁控制台",
        "header_subtitle": "Tensor Core 状态、S3 休眠遥测、功耗上限与 SM 核心频率控制",
        "active_device": "当前设备:",
        "card_hw_title": "GPU 硬件",
        "hw_arch_vram": "架构: {arch} ({sm} SM) | 显存: {vram}",
        "gpu_not_detected": "未检测到 NVIDIA GPU",
        "card_s3_title": "BIOS 固件与 S3 状态",
        "fw_legacy_warn": "Legacy CSM (警告)",
        "fw_csm_off": "CSM: 已关闭",
        "fw_csm_on": "CSM: 已开启",
        "wake_label": "唤醒: {wake}",
        "s3_label_armed": "UEFI + S3 已就绪",
        "s3_label_s0ix": "UEFI (仅 S0ix 待机!)",
        "s3_label_unverified": "UEFI (未验证)",
        "s3_label_not_armed": "UEFI (S3 未就绪)",
        "s3_detail_armed_log": "已通过 {log_name} 确认 (FACS: {facs})",
        "s3_detail_armed_mem": "Shadow FACS 已激活 @ {facs}",
        "s3_detail_s0ix": "现代待机 (S0ix) 激活中 - 请运行 S3 休眠检测",
        "s3_detail_unverified": "暂无 S3 恢复日志 (请确认唤醒设置)",
        "s3_detail_not_armed": "从 USB 引导 -> S3 休眠唤醒 -> 激活",
        "card_rebar_title": "RESIZABLE BAR (显存直连)",
        "rebar_enabled": "已开启 (完整显存)",
        "rebar_disabled": "未开启 (256 MB)",
        "rebar_undetected": "未检测到",
        "rebar_bar1_bios": "BAR1: {bar1} MiB (请在 BIOS 中开启)",
        "rebar_bar1_short": "BAR1: {bar1} MiB",
        "rebar_unavailable": "无法读取 BAR1",
        "bench_title": "算力吞吐测试状态",
        "badge_ready": "准备就绪",
        "badge_running": "测试运行中",
        "bench_tflops_default_sub": "FP16 Tensor Core 峰值性能 (GEMM 4096x4096)",
        "bench_tflops_subtitle": "FP16 Tensor Core 峰值性能 (GEMM 4096x4096{fmla_sub}{load_sub})",
        "bench_fmla_sub": " | FMLA32/16: {fmla:.2f}x",
        "bench_pre_load": " | 初始负载: {util:.0f}%",
        "metric_fp16": "FP16 Tensor",
        "metric_bf16": "BF16 Tensor",
        "metric_tf32": "TF32 Tensor",
        "metric_fp32": "FP32 CUDA",
        "metric_tflops_ms": "{val:.1f} TFLOPS ({ms:.2f}ms)",
        "metric_tflops": "{val:.1f} TFLOPS",
        "na_value": "不可用",
        "baseline_none": "基准记录: 尚未保存 (跑一次测试后可点击“保存基准”)",
        "baseline_compared": "对比基准 ({ts} 存 {base16:.1f} TF): {sign}{delta:.1f}%",
        "baseline_saved_only": "已存基准: {base16:.1f} TFLOPS FP16 ({ts})",
        "badge_status_unlocked": "状态: 已解锁",
        "badge_status_unlocked_load": "状态: 已解锁 (存在外部负载)",
        "badge_status_external_load": "状态: 存在外部负载 ({util:.0f}%)",
        "badge_status_low_tflops": "状态: 请检查功耗 / 频率",
        "badge_status_rate_limited": "状态: 默认限速状态 (Stock)",
        "badge_status_no_tensor": "状态: 无 Tensor Core (GTX 16xx)",
        "badge_status_error": "基准测试出错",
        "bench_verdict_unlocked": "已解锁 (达到 CUDA 基准的 {ratio:.2f} 倍)，cuBLAS 自动调优已生效。",
        "bench_verdict_unlocked_ext": " (注: 测试期间检测到 {util:.0f}% 外部负载)",
        "bench_verdict_under_load": "GPU 存在 {util:.0f}% 外部负载，请暂停游戏/渲染/超分以获取峰值成绩 (FMLA32/FMLA16 {fmla:.2f}x 显示解锁已激活)。",
        "bench_verdict_external_load": "GPU 存在 {util:.0f}% 外部负载，请暂停其他 GPU 程序以获得准确检测结果。",
        "bench_verdict_low_tflops": "FMLA32/FMLA16 ({fmla:.2f}x) 显示已解锁，但算力 ({fp16:.1f} TFLOPS) 低于 {thresh:.0f}，请检查功耗或温度。",
        "bench_verdict_rate_limited": "算力限制仍处于激活状态 ({fp16:.1f} TFLOPS)，解锁后预期应大于 {thresh:.0f} TFLOPS。",
        "bench_verdict_no_tensor": "GTX 16 系列显卡不包含硬件 Tensor Core，预引导阶段已按设计安全跳过。",
        "bench_error_msg": "错误: {err}",
        "log_bench_init": "正在初始化 CUDA 张量并预热矩阵计算核心...",
        "log_cuda_unavail": "当前系统不可用 CUDA。",
        "bench_step_fp16": "正在测试 GPU {idx} 的 FP16 Tensor Core...",
        "bench_step_fmla": "正在测试 GPU {idx} 的 FMLA32/FMLA16 比值...",
        "bench_step_bf16": "正在测试 GPU {idx} 的 BF16 Tensor Core...",
        "bench_step_tf32": "正在测试 GPU {idx} 的 TF32 Tensor Core...",
        "bench_step_fp32": "正在测试 GPU {idx} 的 FP32 CUDA 对照性能...",
        "pl_title": "功耗限制与能效预设 (节能 / 默认 / 满血)",
        "pl_querying": "正在查询 nvidia-smi...",
        "pl_unavailable": "不可用",
        "pl_locked_err": "功耗上限仅只读 / 已锁定 ({err})",
        "pl_preview_locked": "已锁定",
        "pl_readout": "当前: {cur_w} W | 实时功耗: {draw_txt} | 范围: {min_w}-{max_w} W (默认: {stock_w} W)",
        "pl_target": "{w} W ({pct}%)",
        "btn_apply_pl": "应用功耗",
        "btn_eco": "节能 (~78%)",
        "btn_eco_w": "节能 ({w} W)",
        "btn_stock": "默认 (100%)",
        "btn_stock_w": "默认 ({w} W)",
        "btn_max": "极限 (VBIOS)",
        "btn_max_w": "极限 ({w} W)",
        "chk_wake_reapply": "S3 唤醒后自动恢复调优",
        "btn_ai_eff": "AI 能效预设",
        "btn_gaming_stock": "游戏 / 默认全速",
        "pl_applying": "正在应用 {w} W 功耗限制 (若弹出 UAC 请确认)...",
        "pl_preset_applying": "正在应用 {preset} 配置 (若弹出 UAC 请确认)...",
        "clk_title": "SM 频率上限 (-lgc) — Ada / AI 能效调优",
        "clk_querying": "正在查询 SM 频率...",
        "clk_unavailable": "SM 频率控制不可用 ({err})",
        "clk_unsupported": "不支持",
        "clk_cap_locked": "限制: {min_mhz}-{max_mhz} MHz (-lgc)",
        "clk_cap_unlocked": "限制: 未限制 (-rgc)",
        "clk_sw_power_cap": " | SW Power Cap: 触发中",
        "clk_live_readout": "实时 SM: {cur_txt} | {cap_txt} | 范围: {min_c}-{max_c} MHz{sw_cap_txt}",
        "clk_preview_max_eff": "{mhz} MHz (极限能效)",
        "clk_preview_sweetspot": "{mhz} MHz (甜点区间)",
        "clk_preview_custom": "{mhz} MHz (自定义)",
        "btn_clk_reset": "恢复默认 (-rgc)",
        "btn_clk_apply": "应用频率上限",
        "btn_clk_max_eff": "极限能效 (~2300)",
        "btn_clk_max_eff_val": "极限能效 ({mhz} MHz)",
        "btn_clk_sweetspot": "甜点能效 (~2450)",
        "btn_clk_sweetspot_val": "甜点能效 ({mhz} MHz)",
        "clk_tip": "提示: 放开功耗 + -lgc 可减少 SW Power Cap 抖动 (可搭配 +600 MHz 显存；游戏前点重置)。",
        "clk_applying": "正在将 SM 最高频率锁定至 {mhz} MHz (-lgc，若弹出 UAC 请确认)...",
        "clk_resetting": "正在解除 SM 频率锁定 (-rgc，若弹出 UAC 请确认)...",
        "log_initial": "点击“运行 TFLOPS 基准测试”以检测实时 Tensor 算力。",
        "btn_run_bench": "运行 TFLOPS 基准测试",
        "btn_running_bench": "基准测试运行中...",
        "btn_save_baseline": "保存基准",
        "btn_refresh": "刷新信息",
        "btn_guard_active": "防休眠锁频: 已开启",
        "btn_guard_default": "防休眠锁频: 默认",
        "btn_devmgmt": "设备管理器",
        "btn_msinfo": "系统信息",
        "utils_title": " 快捷工具与高级设置 ",
        "btn_turing_gsp": "Turing GSP 模式",
        "btn_sleep_shortcut": "休眠快捷方式",
        "btn_fast_resume": "快速唤醒配置",
        "btn_adv_cc": "控制中心 (诊断与导出)",
        "btn_export_log": "导出引导日志",
        "btn_exporting_log": "正在导出...",
        "btn_usb_setup": "制作 / 更新 U 盘",
        "log_baseline_saved": "已将当前基准成绩保存为本地参考。",
        "log_not_found": "未找到文件: {rel_path}",
        "log_export_searching": "正在搜索 U 盘、隐藏 GPU_UNLOCK 分区及 ESP 中的 RUN*.TXT 日志...",
        "log_guard_applying": "正在应用防休眠降频保护配置...",
        "log_guard_failed": "切换防休眠保护失败: {err}",
        "log_refreshed": "硬件配置与遥测状态已刷新。",
        "log_wake_auto_refresh": "检测到系统从 S3 休眠唤醒，硬件与调优状态已刷新。",
        "log_lang_switched": "界面语言已切换为 {label}。",
    },
    "de": {
        "window_title": "Universal GPU Unlock - Status & Diagnose",
        "header_title": "UNIVERSAL GPU UNLOCK DASHBOARD",
        "header_subtitle": "Tensor-Core-Status, S3-Sleep-Telemetrie, Power Limit & SM-Takt-Steuerung",
        "active_device": "Aktives Gerät:",
        "card_hw_title": "GPU-HARDWARE",
        "hw_arch_vram": "Arch: {arch} ({sm} SMs) | VRAM: {vram}",
        "gpu_not_detected": "Keine NVIDIA-GPU erkannt",
        "card_s3_title": "BIOS-FIRMWARE & S3-STATUS",
        "fw_legacy_warn": "Legacy CSM (Warnung)",
        "fw_csm_off": "CSM: Deaktiviert",
        "fw_csm_on": "CSM: Aktiv",
        "wake_label": "Wake: {wake}",
        "s3_label_armed": "UEFI + S3 Aktiv",
        "s3_label_s0ix": "UEFI (nur S0ix-Standby!)",
        "s3_label_unverified": "UEFI (Ungeprüft)",
        "s3_label_not_armed": "UEFI (S3 nicht aktiv)",
        "s3_detail_armed_log": "Bestätigt per {log_name} (FACS: {facs})",
        "s3_detail_armed_mem": "Shadow FACS aktiv @ {facs}",
        "s3_detail_s0ix": "Modern Standby aktiv - S3-Check ausführen",
        "s3_detail_unverified": "Noch kein S3-Log (Wake prüfen)",
        "s3_detail_not_armed": "USB booten -> S3 Standby -> Aktivieren",
        "card_rebar_title": "RESIZABLE BAR (ReBAR)",
        "rebar_enabled": "Aktiviert (Voller VRAM)",
        "rebar_disabled": "Deaktiviert (256 MB)",
        "rebar_undetected": "Nicht erkannt",
        "rebar_bar1_bios": "BAR1: {bar1} MiB (im BIOS aktivieren)",
        "rebar_bar1_short": "BAR1: {bar1} MiB",
        "rebar_unavailable": "Abfrage nicht verfügbar",
        "bench_title": "RECHENLEISTUNGS-STATUS",
        "badge_ready": "BEREIT FÜR BENCHMARK",
        "badge_running": "BENCHMARK LÄUFT",
        "bench_tflops_default_sub": "FP16 Tensor Core Spitzenleistung (GEMM 4096x4096)",
        "bench_tflops_subtitle": "FP16 Tensor Core Spitzenleistung (GEMM 4096x4096{fmla_sub}{load_sub})",
        "bench_fmla_sub": " | FMLA32/16: {fmla:.2f}x",
        "bench_pre_load": " | Vorlast: {util:.0f}%",
        "metric_fp16": "FP16 Tensor",
        "metric_bf16": "BF16 Tensor",
        "metric_tf32": "TF32 Tensor",
        "metric_fp32": "FP32 CUDA",
        "metric_tflops_ms": "{val:.1f} TFLOPS ({ms:.2f}ms)",
        "metric_tflops": "{val:.1f} TFLOPS",
        "na_value": "N/V",
        "baseline_none": "Referenz: Keine gespeichert (Benchmark starten & 'Referenz speichern')",
        "baseline_compared": "vs. Referenz ({base16:.1f} TF vom {ts}): {sign}{delta:.1f}%",
        "baseline_saved_only": "Gespeicherte Referenz: {base16:.1f} TFLOPS FP16 ({ts})",
        "badge_status_unlocked": "STATUS: ENTSPERRT",
        "badge_status_unlocked_load": "STATUS: ENTSPERRT (EXTERNE LAST)",
        "badge_status_external_load": "STATUS: EXTERNE LAST ({util:.0f}%)",
        "badge_status_low_tflops": "STATUS: POWER / TAKT PRÜFEN",
        "badge_status_rate_limited": "STATUS: STOCK-GEDROSSELT",
        "badge_status_no_tensor": "STATUS: OHNE TENSOR CORES (GTX 16xx)",
        "badge_status_error": "BENCHMARK-FEHLER",
        "bench_verdict_unlocked": "Entsperrt ({ratio:.2f}x CUDA-Basis). cuBLAS Autotune aktiv.",
        "bench_verdict_unlocked_ext": " (Hinweis: {util:.0f}% externe Last während Messung)",
        "bench_verdict_under_load": "GPU unter externer Last ({util:.0f}%) - Spiele/Renderer/Upscaler pausieren (FMLA32/FMLA16 {fmla:.2f}x zeigt aktiven Unlock).",
        "bench_verdict_external_load": "GPU hat {util:.0f}% externe Last - Spiele/Renderer/Upscaler für saubere Prüfung pausieren.",
        "bench_verdict_low_tflops": "FMLA32/FMLA16 ({fmla:.2f}x) wirkt entsperrt, aber {fp16:.1f} TFLOPS liegt unter {thresh:.0f}. PL/Temperaturen prüfen.",
        "bench_verdict_rate_limited": "Rate-Limiter AKTIV ({fp16:.1f} TFLOPS). Erwartete entsperrte Leistung: > {thresh:.0f} TFLOPS.",
        "bench_verdict_no_tensor": "GTX-16-Karten besitzen keine Tensor Cores und werden beim Booten absichtlich übersprungen.",
        "bench_error_msg": "Fehler: {err}",
        "log_bench_init": "Initialisiere CUDA-Tensoren und starte Kernel-Warmup...",
        "log_cuda_unavail": "CUDA ist auf diesem System nicht verfügbar.",
        "bench_step_fp16": "Messe FP16 Tensor Cores auf GPU {idx}...",
        "bench_step_fmla": "Messe FMLA32/FMLA16-Verhältnis auf GPU {idx}...",
        "bench_step_bf16": "Messe BF16 Tensor Cores auf GPU {idx}...",
        "bench_step_tf32": "Messe TF32 Tensor Cores auf GPU {idx}...",
        "bench_step_fp32": "Messe FP32-CUDA-Referenz auf GPU {idx}...",
        "pl_title": "POWER LIMIT & EFFIZIENZ (Eco / Stock / Max)",
        "pl_querying": "Frage nvidia-smi ab...",
        "pl_unavailable": "nicht verfügbar",
        "pl_locked_err": "Power Limit schreibgeschützt / gesperrt ({err})",
        "pl_preview_locked": "Gesperrt",
        "pl_readout": "Aktuell: {cur_w} W | Verbrauch: {draw_txt} | Bereich: {min_w}-{max_w} W (Standard: {stock_w} W)",
        "pl_target": "{w} W ({pct}%)",
        "btn_apply_pl": "PL setzen",
        "btn_eco": "Eco (~78%)",
        "btn_eco_w": "Eco ({w} W)",
        "btn_stock": "Stock (100%)",
        "btn_stock_w": "Stock ({w} W)",
        "btn_max": "Max (VBIOS)",
        "btn_max_w": "Max ({w} W)",
        "chk_wake_reapply": "Tuning nach S3-Wake anwenden",
        "btn_ai_eff": "KI-Effizienz",
        "btn_gaming_stock": "Gaming / Stock",
        "pl_applying": "Setze {w} W Power Limit (ggf. UAC bestätigen)...",
        "pl_preset_applying": "Wende Profil '{preset}' an (ggf. UAC bestätigen)...",
        "clk_title": "SM-TAKTLIMIT (-lgc) — Ada / KI-Effizienz",
        "clk_querying": "Frage SM-Takt ab...",
        "clk_unavailable": "SM-Taktlimit nicht verfügbar ({err})",
        "clk_unsupported": "Nicht unterstützt",
        "clk_cap_locked": "Limit: {min_mhz}-{max_mhz} MHz (-lgc)",
        "clk_cap_unlocked": "Limit: Frei (-rgc)",
        "clk_sw_power_cap": " | SW Power Cap: AKTIV",
        "clk_live_readout": "Live SM: {cur_txt} | {cap_txt} | Bereich: {min_c}-{max_c} MHz{sw_cap_txt}",
        "clk_preview_max_eff": "{mhz} MHz (Max. Eff)",
        "clk_preview_sweetspot": "{mhz} MHz (Sweetspot)",
        "clk_preview_custom": "{mhz} MHz (Manuell)",
        "btn_clk_reset": "Reset (-rgc)",
        "btn_clk_apply": "Limit setzen",
        "btn_clk_max_eff": "Max. Eff (~2300)",
        "btn_clk_max_eff_val": "Max. Eff ({mhz} MHz)",
        "btn_clk_sweetspot": "Sweetspot (~2450)",
        "btn_clk_sweetspot_val": "Sweetspot ({mhz} MHz)",
        "clk_tip": "Tipp: Offenes PL + -lgc vermeidet SW Power Cap (+600 MHz VRAM; vor Gaming zurücksetzen).",
        "clk_applying": "Setze SM-Taktlimit auf {mhz} MHz (-lgc, ggf. UAC bestätigen)...",
        "clk_resetting": "Hebe SM-Taktlimit auf (-rgc, ggf. UAC bestätigen)...",
        "log_initial": "Auf 'TFLOPS BENCHMARK' klicken, um die Tensor-Leistung zu messen.",
        "btn_run_bench": "TFLOPS BENCHMARK",
        "btn_running_bench": "Benchmark läuft...",
        "btn_save_baseline": "Referenz speichern",
        "btn_refresh": "Aktualisieren",
        "btn_guard_active": "Idle Guard: AKTIV",
        "btn_guard_default": "Idle Guard: STANDARD",
        "btn_devmgmt": "Geräte-Manager",
        "btn_msinfo": "msinfo32",
        "utils_title": " Schnellwerkzeuge & Erweitertes Setup ",
        "btn_turing_gsp": "Turing GSP-Modus",
        "btn_sleep_shortcut": "Standby-Verknüpfung",
        "btn_fast_resume": "Fast-Resume Setup",
        "btn_adv_cc": "Control Center (Audits)",
        "btn_export_log": "Boot-Log exportieren",
        "btn_exporting_log": "Exportiere...",
        "btn_usb_setup": "USB Setup / Update",
        "log_baseline_saved": "Aktuelle Benchmark-Werte als lokale Referenz gespeichert.",
        "log_not_found": "Nicht gefunden: {rel_path}",
        "log_export_searching": "Durchsuche USB, versteckte GPU_UNLOCK-Partition und ESP nach RUN*.TXT...",
        "log_guard_applying": "Wende Idle-Relock-Guard-Konfiguration an...",
        "log_guard_failed": "Umschalten von Idle Guard fehlgeschlagen: {err}",
        "log_refreshed": "Hardware-Konfiguration aktualisiert.",
        "log_wake_auto_refresh": "S3-Wake erkannt. Hardware- & Tuning-Status aktualisiert.",
        "log_lang_switched": "Oberflächensprache auf {label} umgestellt.",
    },
}


def t(lang, key, **kwargs):
    """Look up a localized format string, falling back safely to English."""
    norm = normalize_language(lang)
    table = STRINGS.get(norm, STRINGS["en"])
    text = table.get(key) or STRINGS["en"].get(key, key)
    if kwargs:
        try:
            return text.format(**kwargs)
        except Exception:
            return text
    return text


def translate_s3_status(lang, s3_status, fw_is_uefi):
    """Return localized (headline_text, detail_text) for Card 2."""
    if fw_is_uefi is False:
        return t(lang, "fw_legacy_warn"), ""
    s3_status = s3_status or {}
    code = str(s3_status.get("code", "UNVERIFIED")).upper()
    if code == "ARMED":
        headline = t(lang, "s3_label_armed")
        log_name = s3_status.get("log_name")
        facs = s3_status.get("shadow_facs") or s3_status.get("firmware_ctrl") or "OK"
        if log_name:
            detail = t(lang, "s3_detail_armed_log", log_name=log_name, facs=facs)
        else:
            detail = t(lang, "s3_detail_armed_mem", facs=facs)
        return headline, detail
    if code == "S0IX_WARN":
        return t(lang, "s3_label_s0ix"), t(lang, "s3_detail_s0ix")
    if code == "NOT_ARMED":
        return t(lang, "s3_label_not_armed"), t(lang, "s3_detail_not_armed")
    return t(lang, "s3_label_unverified"), t(lang, "s3_detail_unverified")
