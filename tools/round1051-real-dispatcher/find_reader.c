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
 int hits=0;
 for (uint32_t a=0; a+4<=iop->ram_size && a<0x00200000u; a+=4) {
   uint32_t w = rd(iop,a);
   if ((w>>26)==0x0F && (w&0xFFFF)==0x0012) {
     for (uint32_t b=a+4;b<=a+12;b+=4){
       uint32_t w2=rd(iop,b);
       if ((w2>>26)==0x09) { int16_t imm=(int16_t)(w2&0xFFFF); if (imm==-17880){
         if (a<0x001156CC || a>0x001157F4) { printf("[R1051-READER] lui/addiu ->0x0011BA28 at 0x%08x (addiu at 0x%08x)\n",a,b); hits++; }
       } }
     }
   }
 }
 printf("[R1051-READER] total (excluding known 3 fns): %d\n", hits);
 return 0;
}
