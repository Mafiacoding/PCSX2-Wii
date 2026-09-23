#include <stdio.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/iop/iop_core.h"
static uint32_t rd(iop_state_t *iop, uint32_t addr){addr&=0x1FFFFF;if(addr+4>iop->ram_size)return 0xFFFFFFFF;
 return (uint32_t)iop->ram[addr]|((uint32_t)iop->ram[addr+1]<<8)|((uint32_t)iop->ram[addr+2]<<16)|((uint32_t)iop->ram[addr+3]<<24);}
int main(int argc,char**argv){
 bios_image_t bios; bios_load(argv[1],&bios); checkpoint_load(argv[2],&bios,&bios,NULL);
 iop_state_t *iop = iop_core_get_state();
 uint32_t targets[]={0x0011590C,0x001158A8};
 const char*names[]={"Reschedule(?)","GetHighestReadyPriority"};
 for(int t=0;t<2;t++){
   int found=0;
   for(uint32_t a=0;a+4<=iop->ram_size && a<0x00200000u;a+=4){
     uint32_t w=rd(iop,a);
     if((w>>26)==0x03){ uint32_t jt=(a&0xF0000000u)|((w&0x03FFFFFFu)<<2);
       if(jt==targets[t]){printf("[XREF2] %s (0x%08x) called from 0x%08x\n",names[t],targets[t],a);found++;}
     }
   }
   printf("[XREF2] %s total callers: %d\n",names[t],found);
 }
 /* also print current live value of the "current TCB pointer" global 0x0011BA20 */
 printf("[XREF2] live *0x0011BA20 (current TCB ptr) = 0x%08x\n", rd(iop,0x0011BA20));
 printf("[XREF2] live *0x0011BA24 (shadow/prev?)     = 0x%08x\n", rd(iop,0x0011BA24));
 printf("[XREF2] live ready-bitmap words: %08x %08x %08x %08x\n",
   rd(iop,0x0011BA28), rd(iop,0x0011BA2C), rd(iop,0x0011BA30), rd(iop,0x0011BA34));
 return 0;
}
