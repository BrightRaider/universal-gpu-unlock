/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include "uefi_lang.h"
EFI_STATUS GuBootOpen(EFI_HANDLE,EFI_SYSTEM_TABLE *);
EFI_STATUS GuBootLoadAssets(VOID);
EFI_STATUS GuBootLoadWindows(VOID);
EFI_STATUS GuBootChoose(CHAR16 *);
EFI_STATUS GuBootWindows(VOID);
EFI_STATUS GuBootSyncLog(VOID);
EFI_STATUS GuBootClose(VOID);
VOID GuBootSay(const CHAR16 *);
VOID GuBootEnableLargeConsole(VOID);
VOID GuBootSilence(VOID);
VOID GuBootUnsilence(VOID);
VOID GuBootHalt(VOID);
VOID GuBootAppendLog(const char *Format, ...);
/* v0.5.4: diagnostics log only while the USB log is open (GuLog faults otherwise). */
BOOLEAN GuBootLogOpen(VOID);
EFI_HANDLE GuBootGetTargetEspVolume(VOID);
const CHAR16 *GuBootGetLogPath(VOID);
/* MindAllik v1.3.3 / v1.3.4 Boot-Loop Safeguard: ATTEMPT.FLG */
BOOLEAN GuCheckSafeguardFlag(VOID);
EFI_STATUS GuSetSafeguardFlag(VOID);
EFI_STATUS GuClearSafeguardFlag(VOID);
VOID GuBootSetCsmStatus(UINT32 status);
UINT32 GuBootGetLanguage(VOID);
VOID GuBootSetLanguage(UINT32 lang);
const CHAR16 *GuBootUiStr(UINT32 str_id);
