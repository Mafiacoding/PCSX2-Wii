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
 uint32_t targets[] = {0x001156CC,0x00115730,0x00115798,0x0011b0e0,0x0011b100,0x0011b118};
 const char *names[] = {"ready_set(?)","ready_set_v2(?)","ready_clear/dequeue(?)","list_remove","list_test","list_insert"};
 for (int t=0;t<6;t++){
   int found=0;
   for (uint32_t a=0; a+4<=iop->ram_size && a < 0x00200000u; a+=4) {
     uint32_t w = rd(iop,a);
     if ((w>>26)==0x03) { /* jal */
       uint32_t jt = (a&0xF0000000u)|((w&0x03FFFFFFu)<<2);
       if (jt == targets[t]) { printf("[XREF] %-24s (0x%08x) called via jal from 0x%08x\n", names[t], targets[t], a); found++; if(found>20)break; }
     }
   }
   if (!found) printf("[XREF] %-24s (0x%08x): NO jal callers found anywhere in IOP RAM\n", names[t], targets[t]);
 }
 return 0;
}
