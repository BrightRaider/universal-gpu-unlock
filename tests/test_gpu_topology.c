/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only negative evidence: no UEFI services or physical GPU access. */
#include "../src/gpu_topology.h"
#include <stdio.h>
#include <string.h>

static GU_TOPO_CAPTURE snapshot;
static GU_TOPO_REPORT report;
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x);return 1;}}while(0)
static const uint8_t acpi[]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0};

static GU_TOPO_PATH Path(int bridge_device,int bridge_function,int endpoint_device,
                         int endpoint_function) {
  GU_TOPO_PATH p;
  size_t at=0;
  memset(&p,0,sizeof(p));
  memcpy(p.bytes,acpi,sizeof(acpi));at+=sizeof(acpi);
  if(bridge_device>=0) {
    uint8_t hop[]={1,1,6,0,(uint8_t)bridge_function,(uint8_t)bridge_device};
    memcpy(p.bytes+at,hop,sizeof(hop));at+=sizeof(hop);
  }
  if(endpoint_device>=0) {
    uint8_t hop[]={1,1,6,0,(uint8_t)endpoint_function,(uint8_t)endpoint_device};
    memcpy(p.bytes+at,hop,sizeof(hop));at+=sizeof(hop);
  }
  {const uint8_t end[]={0x7f,0xff,4,0};memcpy(p.bytes+at,end,sizeof(end));at+=sizeof(end);}
  p.length=at;return p;
}
static void Pci(size_t i,GU_GPU_IDENTITY ident,GU_TOPO_PATH path,uint16_t cls,
                int root,uint8_t secondary,uint8_t subordinate) {
  GU_TOPO_PCI *p=&snapshot.pci[i];
  p->identity.pci_id=ident.pci_id;p->identity.subsystem_id=ident.subsystem_id;
  p->identity.segment=ident.segment;p->identity.bus=ident.bus;
  p->identity.device=ident.device;p->identity.function=ident.function;
  p->identity.class_code=cls;
  p->identity.handle=(void *)(uintptr_t)(i*2+1);
  p->identity.pci=(void *)(uintptr_t)(i*2+2);
  p->path=path;p->bridge=(cls==0x0604U);p->root_port=(uint8_t)root;
  p->secondary=secondary;p->subordinate=subordinate;
}
static void Baseline(void) {
  GU_GPU_IDENTITY ra={0x12341022U,0,0,0,1,0};
  GU_GPU_IDENTITY rg={0x12341022U,0,0,0,3,1};
  GU_GPU_IDENTITY amd={0x164e1002U,0,0,0,8,0};
  memset(&snapshot,0,sizeof(snapshot));
  snapshot.pci_count=5;
  Pci(0,ra,Path(-1,0,1,0),0x0604,1,1,1);
  Pci(1,GuPinnedAd102,Path(1,0,0,0),0x0300,0,0,0);
  Pci(2,rg,Path(-1,0,3,1),0x0604,1,4,4);
  Pci(3,GuPinnedGa102,Path(3,1,0,0),0x0300,0,0,0);
  Pci(4,amd,Path(-1,0,8,0),0x0300,0,0,0);
  snapshot.gop_count=3;
  snapshot.gop[0].path=snapshot.pci[1].path;
  snapshot.gop[1].path=snapshot.pci[3].path;
  snapshot.gop[2].path=snapshot.pci[4].path;
  snapshot.console_available=1;snapshot.console_count=1;
  snapshot.console[0]=snapshot.pci[4].path;
}
int main(void) {
  GU_TOPO_PCI swap;
  Baseline();
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.pair_exact && report.ancestry_verified[0] && report.ancestry_verified[1]);
  CHECK(report.root_index[0]==0 && report.root_index[1]==2 && report.distinct_roots);
  CHECK(report.gop_count[0]==1 && report.gop_count[1]==1 && report.amd_gop_count==1);
  CHECK(report.amd_console_independent && !report.nvidia_console_count);
  CHECK(report.parent_index[0]==0 && report.parent_index[1]==2);

  Baseline();swap=snapshot.pci[1];snapshot.pci[1]=snapshot.pci[3];snapshot.pci[3]=swap;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.gpu_index[0]==3 && report.gpu_index[1]==1);

  Baseline();snapshot.pci[3].identity.subsystem_id ^=1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE && !report.pair_exact);
  Baseline();snapshot.pci[3].identity.bus=5;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE && !report.pair_exact);
  Baseline();snapshot.pci[3].identity.pci_id ^=0x10000;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE && !report.pair_exact);
  Baseline();snapshot.pci[3].identity.segment=1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[3].identity.pci_id=0x220a1002U;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);

  Baseline();snapshot.pci[3].identity.bus=snapshot.pci[1].identity.bus;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci[3].identity.handle=snapshot.pci[1].identity.handle;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci[2].identity.pci=snapshot.pci[0].identity.pci;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);

  Baseline();snapshot.pci[0].root_port=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[0].secondary=2;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  Baseline();snapshot.pci[2].subordinate=3;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  Baseline();snapshot.pci[0].bridge=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.pci[0].path=Path(-1,0,5,0);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);

  Baseline();snapshot.pci[2].identity.device=1;
  snapshot.pci[2].path=snapshot.pci[0].path;
  snapshot.pci[2].secondary=1;snapshot.pci[2].subordinate=4;
  snapshot.pci[3].path=Path(1,0,0,0);
  /* Duplicate PCI path is ambiguous and must never select a shared owner. */
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);

  Baseline();snapshot.console[0]=snapshot.pci[1].path;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_CONFLICT);
  CHECK(report.nvidia_console_count==1 && !report.amd_console_independent);
  Baseline();snapshot.console_count=0;snapshot.console_available=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.gop_count=2;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.console[0]=Path(-1,0,15,0);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INCOMPLETE);
  Baseline();snapshot.gop[0].path=Path(-1,0,15,0);
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_COMPLETE);
  CHECK(report.unmatched_gops==1 && report.gop_count[0]==0);

  Baseline();snapshot.pci[1].path.bytes[14]=1; /* six-byte PCI node now too long */
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci[1].path.length=GU_TOPO_PATH_BYTES+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.console[0].bytes[snapshot.console[0].length-3]=1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.gop[0].path.length=0;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.pci_count=GU_TOPO_MAX_PCI+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.gop_count=GU_TOPO_MAX_GOPS+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  Baseline();snapshot.console_count=GU_TOPO_MAX_CONSOLES+1;
  CHECK(GuTopoAssess(&snapshot,&report)==GU_TOPO_INVALID);
  CHECK(GuTopoAssess(NULL,&report)==GU_TOPO_INVALID);
  CHECK(GuTopoAssess(&snapshot,NULL)==GU_TOPO_INVALID);
  printf("PASS read-only topology negatives: %u checks; no firmware calls\n",checks);
  return 0;
}
