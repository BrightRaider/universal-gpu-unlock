/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact captured MSI RTX 4090 Suprim X (AD102, CAP0001 2026-10-25). A matched profile is not hardware qualification. */
#include "profile.h"
#include "checks.h"
#include "sha256.h"
#include "compat/compat.h"
#include "port.h"

static const struct {
  const char *name,*hash;
  UINTN bytes;
} PinsAd102[5]={
 {"gpu-unlock/610.43.03/booter_load_ad102.bin","6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49",57672},
 {"gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin","44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb",64000},
 {"gpu-unlock/610.43.03/gsp_ga10x.bin","572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a",84277400},
 {"gpu-unlock/610.43.03/gsp_boot_ad102.bin","45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0",36864},
 {"gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin","83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42",84}
};

static const struct {
  const char *name,*hash;
  UINTN bytes;
} PinsGa102[5]={
 {"gpu-unlock/610.43.03/booter_load_ga102.bin","056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4",61256},
 {"gpu-unlock/610.43.03/v67-ga102-610.43.03.bin","2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197",64000},
 {"gpu-unlock/610.43.03/gsp_ga10x.bin","572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a",84277400},
 {"gpu-unlock/610.43.03/gsp_boot_ga102.bin","3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768",24576},
 {"gpu-unlock/610.43.03/gsp_boot_desc_ga102.bin","cc5a3fc1f4c47f2fe107b1b68f0176d8a448e6dec84b575e5560ccef5262bf15",84}
};

BOOLEAN GuAd104AssetsValid(VOID) {
  if (Gu.is_turing) return FALSE; /* No pinned TU102 assets exist yet: fail closed. */
  for(UINTN i=0;i<5;i++) {
    const char *name = Gu.is_ampere ? PinsGa102[i].name : PinsAd102[i].name;
    const char *hash = Gu.is_ampere ? PinsGa102[i].hash : PinsAd102[i].hash;
    UINTN bytes = Gu.is_ampere ? PinsGa102[i].bytes : PinsAd102[i].bytes;
    if(!GuAssets[i].name || strcmp(GuAssets[i].name,name) ||
       GuAssets[i].firmware.size!=bytes ||
       !GuHashMatches(GuAssets[i].firmware.data,bytes,hash))return FALSE;
  }
  return TRUE;
}
/* Plausibility only: a universal build cannot pin a per-card VBIOS hash. The GPU
 * itself authenticates the signed FWSEC image; this only catches a bad readout.
 * A PCIR device ID that differs from the live card is logged, not refused, since
 * some boards legitimately report another ID in the image. */
BOOLEAN GuAd104PromPlausible(const VOID *Data,UINTN Bytes) {
  const UINT8 *rom=Data;
  if(!rom||Bytes!=1048576)return FALSE;
  UINTN pcir=rom[0x18]+((UINTN)rom[0x19]<<8);
  if(pcir+8>Bytes)return FALSE;
  if(rom[pcir]=='P'&&rom[pcir+1]=='C'&&rom[pcir+2]=='I'&&rom[pcir+3]=='R') {
    UINT16 vendor=(UINT16)(rom[pcir+4]+(rom[pcir+5]<<8)),device=(UINT16)(rom[pcir+6]+(rom[pcir+7]<<8));
    if(vendor!=0x10de) {
      GuLog("PROM refused: PCIR vendor=%04x is not NVIDIA\n",(unsigned)vendor);
      return FALSE;
    }
    if(device!=Gu.device_id)GuLog("PROM note: PCIR device=%04x differs from live device=%04x\n",(unsigned)device,(unsigned)Gu.device_id);
  }
  UINT8 hash[32];
  GuSha256(Data,Bytes,hash);
  CHAR8 s[65];
  static const char hex[]="0123456789abcdef";
  for(int i=0;i<32;i++){s[i*2]=hex[(hash[i]>>4)&0xf];s[i*2+1]=hex[hash[i]&0xf];}
  s[64]=0;
  GuLog("PROM actual SHA256: %s\n", s);
  return TRUE;
}
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *f) {
  if (!f) return FALSE;
  return f->ucode_id != 0 && f->sig_count >= 1 && f->sig_bytes == (UINT32)f->sig_count * 384;
}

static BOOLEAN Config(VOID) {
  UINT32 c[64];UINTN s,b,d,f;GU_CAPS caps;
  if(!Gu.pci || !Gu.pci->GetLocation || !Gu.pci->Pci.Read ||
     !Gu.bar_base || Gu.bar_size!=0x1000000 ||
     EFI_ERROR(Gu.pci->GetLocation(Gu.pci,&s,&b,&d,&f))) {
    GuLog("Config: invalid pci handle or location\n");
    return FALSE;
  }
  for(UINTN i=0;i<64;i++) {
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)i*4,&c[i]))) {
      GuLog("Config: read failed at word %u\n", (unsigned)i);
      return FALSE;
    }
  }
  if(!GuCheckConfig(c,s,b,d,f,&caps)) {
    GuLog("Config: GuCheckConfig failed (seg=%u bus=%u dev=%u fun=%u)\n", (unsigned)s, (unsigned)b, (unsigned)d, (unsigned)f);
    return FALSE;
  }
  if(!GuIsSupportedPciId(c[0]) || ((c[2]>>16)!=0x0300U && (c[2]>>16)!=0x0302U) || (c[4]&~0xfU)!=Gu.bar_base) {
    GuLog("Config: mismatch c0=0x%x c2=0x%x c4=0x%x bar_base=0x%llx\n", c[0], c[2], c[4], (unsigned long long)Gu.bar_base);
    return FALSE;
  }
  if(!(caps.pm==Gu.pm_cap && caps.pcie==Gu.pcie_cap && caps.pcie_version==Gu.pcie_version && caps.msi==Gu.msi_cap && caps.msix==Gu.msix_cap)) {
    GuLog("Config: caps mismatch: pm=%u/%u pcie=%u/%u pcie_ver=%u/%u msi=%u/%u msix=%u/%u\n",
          caps.pm, Gu.pm_cap, caps.pcie, Gu.pcie_cap, caps.pcie_version, Gu.pcie_version,
          caps.msi, Gu.msi_cap, caps.msix, Gu.msix_cap);
    return FALSE;
  }
  GuLog("Config: OK\n");
  return TRUE;
}
static BOOLEAN Registers(UINT32 *Ss0,UINT32 *Ss1) {
  UINT32 pmc = GuRead32(0);
  if (Gu.fault || pmc == 0xffffffff || (pmc & 0xffff0000) == 0xbadf0000) {
    GuLog("Registers: BAR0 PMC read failed (0x%x)\n", pmc);
    return FALSE;
  }
  UINT32 wpr2_hi = GuRead32(0x1fa828);
  if (Gu.fault || (wpr2_hi >> 4) != 0) {
    GuLog("Registers: WPR2 already active (hi=0x%x), cold reboot required\n", wpr2_hi);
    return FALSE;
  }
  *Ss0=GuRead32(0x82381c);*Ss1=GuRead32(0x823820);
  GuLog("Selectors: SS0=0x%x, SS1=0x%x (fault=%u)\n", *Ss0, *Ss1, (unsigned)Gu.fault);
  if(*Ss0==0xffffffff || (*Ss0&0xffff0000)==0xbadf0000 || *Ss0==0x88888888 || (*Ss1 > 7)) {
    GuLog("Selectors invalid: SS0=0x%x, SS1=0x%x (expected SS1 <= 7)\n", *Ss0, *Ss1);
    return FALSE;
  }
  GuLog("Registers: cold state verified (PMC=0x%x, SS0=0x%x, SS1=%u)\n", pmc, *Ss0, *Ss1);
  return !Gu.fault;
}
BOOLEAN GuAd104ColdEntry(VOID) {
  UINT32 ss0,ss1;
  return !Gu.started && !Gu.fault && !Gu.retained && Config() &&
    Registers(&ss0,&ss1) && GuEntrySelectorsMatch(ss0,ss1);
}
EFI_STATUS GuAd104Prepare(VOID) {
  UINT32 ss0,ss1,after0,after1;VOID *prom=NULL;struct gpu_unlock_fwsec fw;
  GuLog("GuAd104Prepare: start\n");
  if(Gu.started || Gu.fault || Gu.retained || Gu.allocations || Gu.profile_verified || Gu.prom_checked) {
    GuLog("GuAd104Prepare: state invalid (started=%u fault=%u retained=%u alloc=%p prof=%u prom=%u)\n",
          Gu.started, Gu.fault, Gu.retained, (void *)Gu.allocations, Gu.profile_verified, Gu.prom_checked);
    return EFI_NOT_READY;
  }
  if(!Config()) {
    GuLog("GuAd104Prepare: Config() returned FALSE\n");
    return EFI_NOT_READY;
  }
  if(!Registers(&ss0,&ss1)) {
    GuLog("GuAd104Prepare: Registers() returned FALSE\n");
    return EFI_NOT_READY;
  }
  if(!GuAd104AssetsValid()) {
    GuLog("GuAd104Prepare: Assets invalid\n");
    return EFI_CRC_ERROR;
  }
  prom=GuAlloc(1048576);
  if(!prom) {
    GuLog("GuAd104Prepare: PROM alloc failed\n");
    return EFI_OUT_OF_RESOURCES;
  }
  GuCopyFromIo(prom,(VOID *)(UINTN)(Gu.bar_base+0x300000),1048576);
  BOOLEAN prom_ok = GuAd104PromPlausible(prom,1048576);
  int fwsec_res = gpu_unlock_vbios_fwsec(prom,1048576,&fw);
  BOOLEAN fwsec_match = (fwsec_res == 0) && GuAd104FwsecMatches(&fw);
  BOOLEAN cfg2 = Config();
  BOOLEAN reg2 = Registers(&after0,&after1);
  BOOLEAN valid = !Gu.fault && prom_ok && fwsec_match && cfg2 && reg2 && ss0==after0 && ss1==after1;
  GuLog("GuAd104Prepare: prom_ok=%u fwsec_res=%d fwsec_match=%u cfg2=%u reg2=%u ss0_stable=%u\n",
        prom_ok, fwsec_res, fwsec_match, cfg2, reg2, (ss0==after0 && ss1==after1));
  GuFree(prom);
  if(!valid) return EFI_COMPROMISED_DATA;
  Gu.prom_checked=TRUE;
  if(!GuLatchEntrySelectors(ss0,ss1)) {
    Gu.prom_checked=FALSE;
    GuLog("GuAd104Prepare: GuLatchEntrySelectors failed\n");
    return EFI_NOT_READY;
  }
  Gu.profile_verified=TRUE;
  GuLog("GuAd104Prepare: SUCCESS\n");
  return EFI_SUCCESS;
}
