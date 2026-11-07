/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_topology.h"
#include <string.h>

static size_t Body(const GU_TOPO_PATH *p) { return p->length - 4U; }

int GuTopoPathValid(const GU_TOPO_PATH *p) {
  size_t at = 0;
  if (!p || p->length < 4 || p->length > GU_TOPO_PATH_BYTES) return 0;
  while (at + 4 <= p->length) {
    size_t length = (size_t)p->bytes[at+2] | ((size_t)p->bytes[at+3] << 8);
    if (length < 4 || length > p->length-at) return 0;
    if (p->bytes[at] == 0x7f) {
      return p->bytes[at+1] == 0xff && length == 4 &&
             at+length == p->length;
    }
    at += length;
  }
  return 0;
}

/* The last PCI node must identify the same function as GetLocation().
 * Otherwise a fabricated/changed path could assign the wrong bridge chain. */
static int EndpointMatches(const GU_TOPO_PCI *node) {
  const GU_TOPO_PATH *p=&node->path;
  size_t body=Body(p),at=0,last=0;
  if(body<6)return 0;
  while(at<body) {
    size_t n=(size_t)p->bytes[at+2] | ((size_t)p->bytes[at+3]<<8);
    if(n<4 || n>body-at)return 0;
    last=at;at+=n;
  }
  return p->bytes[last]==1U && p->bytes[last+1]==1U &&
    body-last==6U && p->bytes[last+4]==node->identity.function &&
    p->bytes[last+5]==node->identity.device;
}

static int Prefix(const GU_TOPO_PATH *prefix, const GU_TOPO_PATH *path) {
  size_t length;
  if (!GuTopoPathValid(prefix) || !GuTopoPathValid(path)) return 0;
  length = Body(prefix);
  return length <= Body(path) && !memcmp(prefix->bytes,path->bytes,length);
}

static int SameBdf(const GU_GPU_RECORD *a, const GU_GPU_RECORD *b) {
  return a->segment == b->segment && a->bus == b->bus &&
    a->device == b->device && a->function == b->function;
}

static int Record(void *opaque,size_t index,GU_GPU_RECORD *out) {
  const GU_TOPO_CAPTURE *capture=(const GU_TOPO_CAPTURE *)opaque;
  *out=capture->pci[index].identity;
  return 0;
}

/* Only a complete PCI device path prefix identifies an ancestor. A nearest
 * parent must be exactly one PCI node (six bytes) earlier; missing bridges
 * never get silently skipped. */
static int Parent(const GU_TOPO_CAPTURE *s, size_t child, size_t *parent) {
  size_t i, found=GU_TOPO_MAX_PCI, depth=0;
  const GU_TOPO_PATH *path=&s->pci[child].path;
  if (Body(path) < 6) return 0;
  for(i=0;i<s->pci_count;i++) {
    const GU_TOPO_PCI *p=&s->pci[i];
    if(i==child || p->identity.segment!=s->pci[child].identity.segment ||
       !p->bridge || Body(&p->path)+6 != Body(path) ||
       !Prefix(&p->path,path)) continue;
    if(found != GU_TOPO_MAX_PCI) return -1;
    found=i;depth++;
  }
  if(depth!=1) return 0;
  *parent=found;
  return 1;
}

static int Root(const GU_TOPO_CAPTURE *s, size_t gpu, size_t *parent,
                size_t *root, unsigned *depth) {
  size_t current=gpu, closest=GU_TOPO_MAX_PCI, seen=0;
  *depth=0;
  *parent=*root=GU_TOPO_MAX_PCI;
  /* A child bus must fall within each ancestor bridge window. */
  while (*depth < 16) {
    size_t index;
    int found=Parent(s,current,&index);
    if (found<0) return -1;
    if (!found) break;
    if (!*depth) closest=index;
    if (s->pci[index].identity.segment != s->pci[gpu].identity.segment ||
        s->pci[index].secondary > s->pci[index].subordinate ||
        s->pci[gpu].identity.bus < s->pci[index].secondary ||
        s->pci[gpu].identity.bus > s->pci[index].subordinate) return -1;
    if (s->pci[index].root_port) {
      if (seen++) return -1; /* multiple root ports on the same chain */
      *root=index;
    }
    current=index;(*depth)++;
  }
  if (*depth==16) return -1;
  *parent=closest;
  return closest!=GU_TOPO_MAX_PCI && *root!=GU_TOPO_MAX_PCI ? 1:0;
}

/* Longest complete PCI prefix identifies a GOP/ConOut path's exact endpoint.
 * Root prefixes alone are not accepted as display ownership. */
static int DisplayOwner(const GU_TOPO_CAPTURE *s, const GU_TOPO_PATH *path) {
  size_t i, longest=0;
  int result=-1;
  for(i=0;i<s->pci_count;i++) {
    const GU_GPU_RECORD *r=&s->pci[i].identity;
    size_t length=Body(&s->pci[i].path);
    if(r->class_code != 0x0300U && r->class_code != 0x0302U) continue;
    if(length < 6 || length < longest || !Prefix(&s->pci[i].path,path)) continue;
    if(length == longest) { result=-2; continue; }
    longest=length;result=(int)i;
  }
  return result;
}

int GuTopoAssess(const GU_TOPO_CAPTURE *s, GU_TOPO_REPORT *r) {
  GU_GPU_INVENTORY inventory;
  size_t i,j, ad,ga;
  int result, bad=0, incomplete=0;
  if (!r) return GU_TOPO_INVALID;
  memset(r,0,sizeof(*r));r->status=GU_TOPO_INVALID;
  for(i=0;i<2;i++)r->gpu_index[i]=r->parent_index[i]=r->root_index[i]=GU_TOPO_MAX_PCI;
  if(!s || s->pci_count==0 || s->pci_count>GU_TOPO_MAX_PCI ||
     s->gop_count>GU_TOPO_MAX_GOPS || s->console_count>GU_TOPO_MAX_CONSOLES ||
     (s->console_count && !s->console_available))return r->status;
  for(i=0;i<s->pci_count;i++) {
    const GU_GPU_RECORD *id=&s->pci[i].identity;
    if(!id->handle || !id->pci || id->segment>65535 || id->bus>255 ||
       id->device>31 || id->function>7 || !GuTopoPathValid(&s->pci[i].path) ||
       !EndpointMatches(&s->pci[i]))
       return r->status;
    for(j=0;j<i;j++)
      if(SameBdf(id,&s->pci[j].identity) || id->handle==s->pci[j].identity.handle ||
         id->pci==s->pci[j].identity.pci) return r->status;
  }
  for(i=0;i<s->gop_count;i++)if(!GuTopoPathValid(&s->gop[i].path))return r->status;
  for(i=0;i<s->console_count;i++)if(!GuTopoPathValid(&s->console[i]))return r->status;
  result=GuCollectGpuInventory(s->pci_count,Record,(void *)s,&inventory);
  if(result!=GU_INVENTORY_OK || GuFindPinnedGpuPair(&inventory,&ad,&ga)!=GU_INVENTORY_OK) {
    r->status=GU_TOPO_INCOMPLETE;return r->status;
  }
  r->pair_exact=1;
  /* Lookup by handles returned from the same shared inventory collector. */
  for(i=0;i<s->pci_count;i++) {
    if(s->pci[i].identity.handle==inventory.nvidia[ad].handle)r->gpu_index[0]=i;
    if(s->pci[i].identity.handle==inventory.nvidia[ga].handle)r->gpu_index[1]=i;
  }
  if(r->gpu_index[0]==GU_TOPO_MAX_PCI || r->gpu_index[1]==GU_TOPO_MAX_PCI)
    return r->status;
  for(i=0;i<2;i++) {
    int found=Root(s,r->gpu_index[i],&r->parent_index[i],&r->root_index[i],&r->depth[i]);
    if(found<0)bad=1;
    if(found>0)r->ancestry_verified[i]=r->root_port_verified[i]=1;
    else incomplete=1;
  }
  if(r->root_port_verified[0] && r->root_port_verified[1]) {
    r->distinct_roots=r->root_index[0]!=r->root_index[1];
    if(!r->distinct_roots)bad=1;
  }
  for(i=0;i<s->gop_count;i++) {
    int owner=DisplayOwner(s,&s->gop[i].path);
    if(owner==-2)bad=1;
    if(owner<0) {r->unmatched_gops++;continue;}
    if((size_t)owner==r->gpu_index[0])r->gop_count[0]++;
    else if((size_t)owner==r->gpu_index[1])r->gop_count[1]++;
    else if((s->pci[owner].identity.pci_id&0xffffU)==0x1002U)r->amd_gop_count++;
  }
  r->gop_relationships_observed=(r->gop_count[0] && r->gop_count[1]);
  /* Each GPU needs an actual associated GOP path. Absence is not evidence that
   * the proprietary NVIDIA firmware driver is detached or safe to Stop. */
  if(!r->gop_relationships_observed)incomplete=1;
  /* Lack of an OpenProtocolInformation result on either target makes UEFI
   * protocol ownership unknown, even if its PCI ancestry is established. */
  for(i=0;i<2;i++) {
    const GU_TOPO_PCI *node=&s->pci[r->gpu_index[i]];
    if(!node->openers_known || !node->by_driver)incomplete=1;
    if(node->by_driver>1)bad=1;
  }
  /* NVIDIA GOP absence is recorded, never interpreted as unbound/independent. */
  for(i=0;i<s->console_count;i++) {
    int owner=DisplayOwner(s,&s->console[i]);
    if(owner==-2)bad=1;
    if(owner<0)continue;
    if((size_t)owner==r->gpu_index[0] || (size_t)owner==r->gpu_index[1])
      r->nvidia_console_count++;
    else if((s->pci[owner].identity.pci_id&0xffffU)==0x1002U)
      r->amd_console_count++;
  }
  if(r->nvidia_console_count)bad=1;
  r->amd_console_independent=(s->console_available && r->amd_console_count &&
     r->amd_gop_count && !r->nvidia_console_count);
  if(!r->amd_console_independent)incomplete=1;
  r->status=bad?GU_TOPO_CONFLICT:incomplete?GU_TOPO_INCOMPLETE:GU_TOPO_COMPLETE;
  return r->status;
}
