/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure read-only PCI/GOP topology assessment: never authorizes actuation. */
#pragma once
#include "gpu_inventory.h"

#define GU_TOPO_MAX_PCI 128U
#define GU_TOPO_MAX_GOPS 32U
#define GU_TOPO_MAX_CONSOLES 8U
#define GU_TOPO_PATH_BYTES 192U

typedef struct {
  uint8_t bytes[GU_TOPO_PATH_BYTES];
  size_t length; /* Includes the four-byte END_ENTIRE node. */
} GU_TOPO_PATH;

typedef struct {
  GU_GPU_RECORD identity;
  GU_TOPO_PATH path;
  uint8_t root_port;  /* PCIe capability Port Type == 4; not inferred from name. */
  uint8_t bridge;     /* Class 06:04 */
  uint8_t secondary, subordinate; /* PCI bridge bus-number window. */
  unsigned by_driver; /* OpenProtocolInformation BY_DRIVER observers, if available. */
  uint8_t openers_known;
} GU_TOPO_PCI;

typedef struct {
  GU_TOPO_PATH path;
} GU_TOPO_GOP;

typedef struct {
  size_t pci_count, gop_count, console_count;
  GU_TOPO_PCI pci[GU_TOPO_MAX_PCI];
  GU_TOPO_GOP gop[GU_TOPO_MAX_GOPS];
  GU_TOPO_PATH console[GU_TOPO_MAX_CONSOLES];
  uint8_t console_available;
} GU_TOPO_CAPTURE;

typedef struct {
  size_t gpu_index[2], parent_index[2], root_index[2];
  unsigned depth[2], gop_count[2], nvidia_console_count;
  unsigned amd_gop_count, amd_console_count, unmatched_gops;
  uint8_t pair_exact, ancestry_verified[2], root_port_verified[2];
  uint8_t distinct_roots, amd_console_independent, gop_relationships_observed;
  int status;
} GU_TOPO_REPORT;

enum { GU_TOPO_COMPLETE=0, GU_TOPO_INCOMPLETE=1, GU_TOPO_CONFLICT=2, GU_TOPO_INVALID=3 };

/* Reject malformed/overlong or multi-instance paths. PCI paths must be
 * unique; any unknown/ambiguous fact is explicitly incomplete. */
int GuTopoPathValid(const GU_TOPO_PATH *p);
int GuTopoAssess(const GU_TOPO_CAPTURE *capture, GU_TOPO_REPORT *report);
