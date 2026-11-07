/* SPDX-License-Identifier: GPL-2.0-only */
/* Standalone diagnostic UEFI application: NO unlock, disconnect, MMIO, DMA,
 * PCI config writes, FLR, controller manipulation, or NVRAM writes. */
#if !defined(GU_GPU_DIAGNOSTIC_ONLY) || defined(GU_AD104_TRANSACTION) || \
    defined(GU_ENTRY_LATCHED_SS1_FULL) || defined(GU_FULL_UNLOCK)
#error This file may only be built as a standalone read-only diagnostic
#endif
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#ifdef GU_TOPO_SAVE_LOG
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#endif
#include "gpu_topology.h"
#include "format.h"
#include <stdarg.h>
#include <string.h>

static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static GU_TOPO_CAPTURE Snapshot;
static GU_TOPO_REPORT Report;
static EFI_SYSTEM_TABLE *System;
#ifdef GU_TOPO_SAVE_LOG
static char LogBytes[131072];
static size_t LogUsed;
static int LogOverflow;
static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
#endif

static void Say(const char *format, ...) {
  char line[320];CHAR16 wide[321];size_t i,n;
  va_list ap;va_start(ap,format);n=GuFormatV(line,sizeof(line),format,ap);va_end(ap);
  if(n==SIZE_MAX || n>=sizeof(line))return;
#ifdef GU_TOPO_SAVE_LOG
  if(n>sizeof(LogBytes)-LogUsed)LogOverflow=1;
  else {memcpy(LogBytes+LogUsed,line,n);LogUsed+=n;}
#endif
  for(i=0;i<n;i++)wide[i]=(CHAR16)(unsigned char)line[i];wide[n]=0;
  if(System && System->ConOut && System->ConOut->OutputString)
    (VOID)System->ConOut->OutputString(System->ConOut,wide);
}

/* Emit bounded device-path evidence; lengths alone cannot verify ancestry. */
static void SayPath(const char *kind, size_t index, const GU_TOPO_PATH *path) {
  static const char hex_digits[]="0123456789abcdef";
  size_t at;
  for(at=0;at<path->length;at+=24) {
    char hex[49];
    size_t i,used=path->length-at;
    if(used>24)used=24;
    for(i=0;i<used;i++) {
      hex[2*i]=hex_digits[path->bytes[at+i]>>4];
      hex[2*i+1]=hex_digits[path->bytes[at+i]&15];
    }
    hex[2*used]=0;
    Say("%s[%zu] PATH_OFFSET=%zu PATH_BYTES=%s\r\n",kind,index,at,hex);
  }
}

static int PathCopy(const EFI_DEVICE_PATH_PROTOCOL *source, GU_TOPO_PATH *out) {
  const UINT8 *bytes=(const UINT8 *)source;
  size_t at=0;
  if(!source || !out)return 0;
  memset(out,0,sizeof(*out));
  while(at+4<=GU_TOPO_PATH_BYTES) {
    size_t n=(size_t)bytes[at+2] | ((size_t)bytes[at+3]<<8);
    if(n<4 || n>GU_TOPO_PATH_BYTES-at)return 0;
    if(bytes[at]==0x7f && bytes[at+1]!=0xff)return 0;
    memcpy(out->bytes+at,bytes+at,n);at+=n;
    if(bytes[at-n]==0x7f) {out->length=at;return GuTopoPathValid(out);}
  }
  return 0;
}

static int ConsoleRead(EFI_SYSTEM_TABLE *table) {
  static UINT8 data[GU_TOPO_PATH_BYTES*GU_TOPO_MAX_CONSOLES];
  UINTN bytes=sizeof(data),at=0,start=0;
  UINT32 attrs=0;EFI_STATUS status;
  if(!table->RuntimeServices || !table->RuntimeServices->GetVariable)return 0;
  status=table->RuntimeServices->GetVariable(L"ConOut",&GlobalGuid,&attrs,&bytes,data);
  if(EFI_ERROR(status) || !bytes || bytes>sizeof(data))return 0;
  while(at+4<=bytes) {
    size_t n=(size_t)data[at+2] | ((size_t)data[at+3]<<8);
    if(n<4 || n>bytes-at)return 0;
    if(data[at]==0x7f) {
      GU_TOPO_PATH *path;
      if(data[at+1]!=1 && data[at+1]!=0xff)return 0;
      if(Snapshot.console_count>=GU_TOPO_MAX_CONSOLES ||
         at+n-start>GU_TOPO_PATH_BYTES)return 0;
      path=&Snapshot.console[Snapshot.console_count++];
      memcpy(path->bytes,data+start,at+n-start);
      path->length=at+n-start;
      path->bytes[path->length-3]=0xff; /* normalize END_INSTANCE */
      if(!GuTopoPathValid(path))return 0;
      if(data[at+1]==0xff) {
        Snapshot.console_available=(at+n==bytes);
        return Snapshot.console_available;
      }
      start=at+n;
    }
    at+=n;
  }
  return 0;
}

static int PcieRootPort(const UINT32 config[64]) {
  unsigned offset=config[0x34/4]&0xffU,seen=0;
  while(offset>=0x40 && offset<=0xfc && !(offset&3) && seen++<48) {
    UINT32 header=config[offset/4];
    if((header&0xffU)==0x10U)return ((header>>20)&0xfU)==4U;
    offset=(header>>8)&0xffU;
  }
  return 0;
}

static int ReadPci(EFI_BOOT_SERVICES *bs, EFI_HANDLE h, GU_TOPO_PCI *node) {
  EFI_PCI_IO_PROTOCOL *io=NULL;
  EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;
  UINTN count=0,s=0,b=0,d=0,f=0,i;
  UINT32 cfg[64],verify[64];
  EFI_STATUS status;
  memset(node,0,sizeof(*node));
  status=bs->HandleProtocol(h,&PciGuid,(VOID **)&io);
  if(EFI_ERROR(status)||!io||!io->Pci.Read||!io->GetLocation)return 0;
  status=io->GetLocation(io,&s,&b,&d,&f);
  if(EFI_ERROR(status)||s>65535 || b>255 || d>31 || f>7)return 0;
  status=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,cfg);
  if(EFI_ERROR(status))return 0;
  status=bs->HandleProtocol(h,&PathGuid,(VOID **)&path);
  if(EFI_ERROR(status)||!PathCopy(path,&node->path))return 0;
  node->identity.handle=h;node->identity.pci=io;
  node->identity.segment=(UINT32)s;node->identity.bus=(UINT32)b;
  node->identity.device=(UINT32)d;node->identity.function=(UINT32)f;
  node->identity.pci_id=cfg[0];node->identity.subsystem_id=cfg[11];
  node->identity.class_code=(UINT16)(cfg[2]>>16);
  node->bridge=node->identity.class_code==0x0604;
  if(node->bridge) {
    node->secondary=(UINT8)(cfg[0x18/4]>>8);
    node->subordinate=(UINT8)(cfg[0x18/4]>>16);
    node->root_port=(UINT8)PcieRootPort(cfg);
  }
  /* Only inspect owner records; never Open/Close with BY_DRIVER. */
  if(bs->OpenProtocolInformation) {
    status=bs->OpenProtocolInformation(h,&PciGuid,&entries,&count);
    if(!EFI_ERROR(status)) {
      node->openers_known=1;
      if(count>1024 || (count && !entries)) {
        if(entries && bs->FreePool)(VOID)bs->FreePool(entries);
        return 0;
      }
      for(i=0;i<count;i++) {
        if(entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) {
          if(!node->by_driver)node->first_owner_agent=entries[i].AgentHandle;
          node->by_driver++;
        }
      }
    }
    if(entries && ( !bs->FreePool || EFI_ERROR(bs->FreePool(entries))))return 0;
  }
  /* Re-resolve and verify that the entire PCI snapshot and binding are stable. */
  {
    EFI_PCI_IO_PROTOCOL *again=NULL;
    EFI_DEVICE_PATH_PROTOCOL *again_path=NULL;
    GU_TOPO_PATH fresh;
    UINTN ss=~(UINTN)0,bb=~(UINTN)0,dd=~(UINTN)0,ff=~(UINTN)0;
    if(EFI_ERROR(bs->HandleProtocol(h,&PciGuid,(VOID **)&again)) || again!=io ||
       EFI_ERROR(bs->HandleProtocol(h,&PathGuid,(VOID **)&again_path)) ||
       !PathCopy(again_path,&fresh) || fresh.length!=node->path.length ||
       memcmp(fresh.bytes,node->path.bytes,fresh.length) ||
       EFI_ERROR(io->GetLocation(io,&ss,&bb,&dd,&ff)) ||
       ss!=s || bb!=b || dd!=d || ff!=f ||
       EFI_ERROR(io->Pci.Read(io,EfiPciIoWidthUint32,0,64,verify)) ||
       memcmp(cfg,verify,sizeof(cfg)))return 0;
  }
  return 1;
}

static int CapturePci(EFI_BOOT_SERVICES *bs) {
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i;
  EFI_STATUS status=bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
  int okay=1;
  if(EFI_ERROR(status)||count==0 || count>GU_TOPO_MAX_PCI || !handles)okay=0;
  if(okay)for(i=0;i<count;i++) {
    GU_TOPO_PCI *p=&Snapshot.pci[Snapshot.pci_count];
    if(!ReadPci(bs,handles[i],p)){okay=0;break;}
    Snapshot.pci_count++;
  }
  if(handles && (!bs->FreePool || EFI_ERROR(bs->FreePool(handles))))okay=0;
  return okay;
}

static int CaptureGop(EFI_BOOT_SERVICES *bs) {
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i;
  EFI_STATUS status=bs->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  int okay=1;
  if(EFI_ERROR(status)||count>GU_TOPO_MAX_GOPS || (count&&!handles))okay=0;
  if(okay)for(i=0;i<count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    if(EFI_ERROR(bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path)) ||
       !PathCopy(path,&Snapshot.gop[i].path)){okay=0;break;}
    Snapshot.gop_count++;
  }
  if(handles && (!bs->FreePool || EFI_ERROR(bs->FreePool(handles))))okay=0;
  return okay;
}

#ifdef GU_TOPO_SAVE_LOG
/* Opt-in diagnostic file on the app's own EFI volume. Never overwrite an
 * earlier capture. The ONLY intended persistent mutation is this new file. */
static EFI_STATUS SaveLog(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  EFI_LOADED_IMAGE_PROTOCOL *li=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  EFI_FILE_PROTOCOL *root=NULL,*file=NULL,*prior=NULL;
  EFI_STATUS s;
  UINTN bytes;
  if(LogOverflow || !LogUsed)return EFI_BAD_BUFFER_SIZE;
  s=table->BootServices->HandleProtocol(image,&LoadedGuid,(VOID **)&li);
  if(EFI_ERROR(s)||!li)return EFI_NOT_FOUND;
  s=table->BootServices->HandleProtocol(li->DeviceHandle,&FsGuid,(VOID **)&fs);
  if(EFI_ERROR(s)||!fs||!fs->OpenVolume)return EFI_UNSUPPORTED;
  s=fs->OpenVolume(fs,&root);
  if(EFI_ERROR(s)||!root||!root->Open||!root->Close)return EFI_UNSUPPORTED;
  s=root->Open(root,&prior,L"\\GPU-TOPOLOGY-DIAGNOSTIC.TXT",EFI_FILE_MODE_READ,0);
  if(s!=EFI_NOT_FOUND) {
    if(s==EFI_SUCCESS && prior && prior->Close)(VOID)prior->Close(prior);
    (VOID)root->Close(root);
    return s==EFI_SUCCESS?EFI_ALREADY_STARTED:s;
  }
  s=root->Open(root,&file,L"\\GPU-TOPOLOGY-DIAGNOSTIC.TXT",
               EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(EFI_ERROR(s)||!file||!file->Write||!file->Close) {
    if(file && file->Close)(VOID)file->Close(file);
    (VOID)root->Close(root);return EFI_DEVICE_ERROR;
  }
  bytes=(UINTN)LogUsed;
  s=file->Write(file,&bytes,LogBytes);
  if(!EFI_ERROR(s) && bytes!=(UINTN)LogUsed)s=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(s) && (!file->Flush || EFI_ERROR(file->Flush(file))))s=EFI_DEVICE_ERROR;
  if(EFI_ERROR(file->Close(file)))s=EFI_DEVICE_ERROR;
  if(EFI_ERROR(root->Close(root)))s=EFI_DEVICE_ERROR;
  return s;
}
#endif

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  EFI_BOOT_SERVICES *bs;
  int pci_ok,gop_ok,console_ok,result;
  size_t i;
#ifndef GU_TOPO_SAVE_LOG
  (void)image;
#endif
  if(!table || !table->BootServices || !table->ConOut ||
     !table->BootServices->LocateHandleBuffer ||
     !table->BootServices->HandleProtocol ||
     !table->BootServices->FreePool)return EFI_INVALID_PARAMETER;
  System=table;bs=table->BootServices;
  Say("GPU TOPOLOGY DIAGNOSTIC ONLY - no GPU hardware writes or unlock calls\r\n");
  Say("READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\r\n");
  pci_ok=CapturePci(bs);
  gop_ok=CaptureGop(bs);
  console_ok=ConsoleRead(table);
  result=(pci_ok && gop_ok)?GuTopoAssess(&Snapshot,&Report):GU_TOPO_INVALID;
  Say("PCI_CAPTURE=%u GOP_CAPTURE=%u CONOUT_CAPTURE=%u PCI_NODES=%zu GOP_NODES=%zu CONOUT_PATHS=%zu\r\n",
      (unsigned)pci_ok,(unsigned)gop_ok,(unsigned)console_ok,
      Snapshot.pci_count,Snapshot.gop_count,Snapshot.console_count);
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *p=&Snapshot.pci[i];
    Say("PCI[%zu] %04x:%02x:%02x.%u %04x:%04x SUB=%04x:%04x CLASS=%04x ROOTPORT=%u BUSWINDOW=%02x-%02x BY_DRIVER=%u KNOWN=%u AGENT=%p PATH=%zu\r\n",
      i,(unsigned)p->identity.segment,(unsigned)p->identity.bus,
      (unsigned)p->identity.device,(unsigned)p->identity.function,
      (unsigned)(p->identity.pci_id&65535U),(unsigned)(p->identity.pci_id>>16),
      (unsigned)(p->identity.subsystem_id&65535U),(unsigned)(p->identity.subsystem_id>>16),
      (unsigned)p->identity.class_code,(unsigned)p->root_port,
      (unsigned)p->secondary,(unsigned)p->subordinate,
      p->by_driver,(unsigned)p->openers_known,p->first_owner_agent,p->path.length);
    SayPath("PCI",i,&p->path);
  }
  for(i=0;i<Snapshot.gop_count;i++) {
    Say("GOP[%zu] PATH_LEN=%zu\r\n",i,Snapshot.gop[i].path.length);
    SayPath("GOP",i,&Snapshot.gop[i].path);
  }
  for(i=0;i<Snapshot.console_count;i++) {
    Say("CONOUT[%zu] PATH_LEN=%zu\r\n",i,Snapshot.console[i].length);
    SayPath("CONOUT",i,&Snapshot.console[i]);
  }
  if(pci_ok && gop_ok && Report.pair_exact) {
    for(i=0;i<2;i++) {
      Say("GPU[%zu] PCI_INDEX=%zu PARENT=%zu ROOT_PORT=%zu DEPTH=%u ANCESTRY=%u ROOT_VALID=%u GOP_COUNT=%u\r\n",
         i,Report.gpu_index[i],Report.parent_index[i],Report.root_index[i],
         Report.depth[i],(unsigned)Report.ancestry_verified[i],
         (unsigned)Report.root_port_verified[i],Report.gop_count[i]);
    }
    Say("DISTINCT_ROOTS=%u AMD_GOP_COUNT=%u AMD_CONOUT_MATCH=%u NVIDIA_CONOUT_MATCH=%u GOP_UNMATCHED=%u AMD_CONSOLE_ASSOCIATION=%u\r\n",
      (unsigned)Report.distinct_roots,Report.amd_gop_count,Report.amd_console_count,
      Report.nvidia_console_count,Report.unmatched_gops,
      (unsigned)Report.amd_console_independent);
  }
  Say("STATUS=%s TOPOLOGY=%d EXACT_PAIR=%u HARDWARE_TESTED=0 PHYSICAL_DISPLAY_OWNER=UNKNOWN\r\n",
      result==GU_TOPO_COMPLETE?"COMPLETE":result==GU_TOPO_INVALID?"ERROR":"PARTIAL",
      result,(unsigned)Report.pair_exact);
  Say("DIAGNOSTIC FINISHED; GPU hardware untouched.\r\n");
#ifdef GU_TOPO_SAVE_LOG
  {
    EFI_STATUS written=SaveLog(image,table);
    Say("SAVE_LOG_STATUS=%#llx FILE=\\GPU-TOPOLOGY-DIAGNOSTIC.TXT (new file only)\r\n",
        (unsigned long long)written);
    if(EFI_ERROR(written))return written;
  }
#endif
  return result==GU_TOPO_COMPLETE?EFI_SUCCESS:EFI_UNSUPPORTED;
}
