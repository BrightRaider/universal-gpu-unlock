/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GU_S3_RESUME_H
#define GU_S3_RESUME_H

#include <Uefi.h>
#include "s3_save.h"
#include "s3_facs.h"
#include "gpu_discovery.h"

/*
 * Prepares the persistent ACPI NVS memory buffer, stages booter and V67 microcode
 * payloads, and registers the S3 Boot Script Dispatch or installs the ACPI FACS
 * Shadow Trampoline to guarantee S3 persistence across AMI Aptio V SMM lockdown.
 */
EFI_STATUS GuS3PrepareAndRegister(EFI_SYSTEM_TABLE *t);

/*
 * Standalone S3 Resume Dispatch Handler called by motherboard BIOS on S3 resume.
 * Executes SEC2 Falcon microcode authentication, latches SS0/SS1, and issues FLR.
 */
VOID EFIAPI GuS3ResumeDispatch(IN CONST VOID *This, IN VOID *Context);

/*
 * Internal resume engine function (testable in host test suites).
 * Returns 0 on success, or a negative error code.
 */
INTN GuS3ExecuteResumeUnlock(GU_S3_RESUME_CONTEXT *ctx);

#endif /* GU_S3_RESUME_H */
