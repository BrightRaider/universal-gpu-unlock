"""
Universal GPU Rate-Limiter Dashboard Launcher (.pyw = No Console Window)
SPDX-License-Identifier: GPL-2.0-only
"""
import sys
import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
TOOLS_DIR = REPO_ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import gpu_dashboard

if __name__ == "__main__":
    app = gpu_dashboard.GpuUnlockApp()
    app.mainloop()
