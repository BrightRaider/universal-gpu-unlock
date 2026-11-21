/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include "gpu_discovery.h"

/* Options for GuUnlockAllGpus. */
#define GU_UNLOCK_REGISTER_S3    0x1U /* register the S3 boot script after each unlock */
#define GU_UNLOCK_VERIFY_WINDOWS 0x2U /* run GuUnlockReadyForWindows after each unlock */

/* Runs the unlock on every GPU in devices, one after the other.
 *
 * Before the call, devices[0] must be bound, its assets loaded and GuPrepare done.
 * A secondary GPU whose asset load or GuPrepare fails is skipped and stays stock;
 * both only read from the GPU. A failed unlock on any GPU stops the run.
 *
 * On success, *unlocked is the number of unlocked GPUs and Gu is clean.
 * On failure, Gu stays bound to the failed GPU, *failed is its index and *stage
 * names the step, so the caller can run its normal failure path. */
EFI_STATUS GuUnlockAllGpus(EFI_SYSTEM_TABLE *t, EFI_HANDLE image,
                           const GU_DISCOVERED_DEVICE *devices, UINTN count,
                           UINT32 options, const char *log_prefix,
                           const char **stage, UINTN *unlocked, UINTN *failed);
