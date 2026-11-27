/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stddef.h>

/*
 * Compact decoders for FEAT PLM (e.g. 0x823804) and Speed-Selector nibbles
 * (SS0 0x82381c / SS1 0x823820). Compatible with both UEFI/EDK2 (CHAR8 == char)
 * and hosted MSVC C test builds without pulling in <stdio.h>.
 *
 * PLM low byte:
 *   bits[3:0] = L0..L3 read mask (L0=Host/Driver, L1=Light-FW, L2=Mid-FW, L3=Heavy-Secure)
 *   bits[7:4] = L0..L3 write mask
 *
 * Selector nibble (4 bits per field):
 *   bit3      = override-enable (0 = fuse default pass-through, 1 = active override)
 *   bits[2:0] = divider code (0=1x, 1=1/2, 2=1/4, 3=1/8, >=4 out-of-spec/rsvd)
 */

static inline size_t GuDiagAppend(char *buf, size_t cap, size_t pos, const char *s) {
  if (!buf || !cap || !s) return pos;
  while (*s && pos + 1U < cap) {
    buf[pos++] = *s++;
  }
  buf[pos] = '\0';
  return pos;
}

static inline size_t GuDiagAppendLevelMask(char *buf, size_t cap, size_t pos, unsigned int mask4) {
  static const char *const labels[4] = { "L0", "L1", "L2", "L3" };
  int first = 1;
  mask4 &= 0x0fU;
  if (mask4 == 0U) return GuDiagAppend(buf, cap, pos, "none");
  if (mask4 == 0x0fU) return GuDiagAppend(buf, cap, pos, "L0-3");
  for (unsigned int i = 0; i < 4U; i++) {
    if (mask4 & (1U << i)) {
      if (!first) pos = GuDiagAppend(buf, cap, pos, ",");
      pos = GuDiagAppend(buf, cap, pos, labels[i]);
      first = 0;
    }
  }
  return pos;
}

static inline const char *GuPlmDecode(unsigned int plm, char *buf, size_t cap) {
  if (!buf || cap == 0U) return "";
  buf[0] = '\0';
  if ((plm & 0xffff0000U) == 0xbadf0000U) {
    GuDiagAppend(buf, cap, 0U, "badf-pri");
    return buf;
  }

  unsigned int rmask = plm & 0x0fU;
  unsigned int wmask = (plm >> 4U) & 0x0fU;
  const char *tag = "CUSTOM";
  if (plm == 0xffffffffU) {
    tag = "OPEN";
  } else if (plm == 0xffffff8fU) {
    tag = "STOCK-LOCK";
  } else if (plm == 0xffffffefU) {
    tag = "RELOCK-L0";
  } else if (plm == 0xffffffcfU) {
    tag = "DEEP-LOCK";
  } else if (wmask == 0x0fU) {
    tag = "OPEN-W";
  } else if ((wmask & 0x01U) == 0U) {
    tag = "L0-LOCKED";
  }

  size_t pos = GuDiagAppend(buf, cap, 0U, tag);
  pos = GuDiagAppend(buf, cap, pos, "(R:");
  pos = GuDiagAppendLevelMask(buf, cap, pos, rmask);
  pos = GuDiagAppend(buf, cap, pos, " W:");
  pos = GuDiagAppendLevelMask(buf, cap, pos, wmask);
  (void)GuDiagAppend(buf, cap, pos, ")");
  return buf;
}

static inline const char *GuRateNibbleLabel(unsigned int nibble) {
  nibble &= 0x0fU;
  if ((nibble & 0x08U) == 0U) {
    return (nibble == 0U) ? "def" : "def*";
  }
  switch (nibble & 0x07U) {
    case 0U: return "1x";
    case 1U: return "1/2";
    case 2U: return "1/4";
    case 3U: return "1/8";
    default: return "oos";
  }
}

static inline const char *GuRateSelectorDecode(unsigned int val, int is_ss0, char *buf, size_t cap) {
  if (!buf || cap == 0U) return "";
  buf[0] = '\0';
  if (val == 0xffffffffU || (val & 0xffff0000U) == 0xbadf0000U) {
    GuDiagAppend(buf, cap, 0U, (val == 0xffffffffU) ? "unmapped-ff" : "badf-pri");
    return buf;
  }

  if (!is_ss0) {
    unsigned int nib = val & 0x0fU;
    size_t pos = GuDiagAppend(buf, cap, 0U, (nib & 0x08U) ? "ovr:" : "fuse:");
    (void)GuDiagAppend(buf, cap, pos, GuRateNibbleLabel(nib));
    return buf;
  }

  if (val == 0x88888888U) {
    GuDiagAppend(buf, cap, 0U, "8/8 ovr:1x");
    return buf;
  }
  if (val == 0x00000000U) {
    GuDiagAppend(buf, cap, 0U, "0/8 fuse-def");
    return buf;
  }

  size_t pos = 0U;
  for (int slot = 7; slot >= 0; slot--) {
    unsigned int nib = (val >> (unsigned int)(slot * 4)) & 0x0fU;
    if (slot < 7) pos = GuDiagAppend(buf, cap, pos, " ");
    pos = GuDiagAppend(buf, cap, pos, GuRateNibbleLabel(nib));
  }
  return buf;
}
