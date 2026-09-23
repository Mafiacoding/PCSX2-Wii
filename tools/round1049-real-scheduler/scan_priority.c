/* Round 1049: scan all of IOP RAM for the classic priority-bitmap
 * scheduler pattern (srl/sra by 5 to get word index into a 32-bit-wide
 * priority bitmap, paired with andi 0x1f for the bit index, or a
 * direct 4-byte-stride table index by priority) - the standard O(1)
 * real-time-kernel ready-queue technique (matches real PS2/IOP
 * THREADMAN semantics per ps2sdk citations already in this project's
 * docs). This targets the REAL scheduler dispatch code, distinct from
 * the SIF-RPC service-list walk Round 1047/1048 found and ruled out.
 */
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
 for (uint32_t a=0; a+16<=iop->ram_size && a<0x00200000u; a+=4) {
   uint32_t w0=rd(iop,a), w1=rd(iop,a+4);
   /* srl $rt,$rs,5  (op=0,funct=2,sh=5) followed within a couple insns by andi $x,$y,0x1f */
   int is_srl5 = ((w0>>26)==0 && (w0&0x3F)==0x02 && ((w0>>6)&0x1F)==5);
   int is_sra5 = ((w0>>26)==0 && (w0&0x3F)==0x03 && ((w0>>6)&0x1F)==5);
   if (is_srl5 || is_sra5) {
     /* look ahead up to 4 instrs for andi ...,0x1f (op=0x0C, imm=0x1f) */
     for (uint32_t b=a+4; b<=a+20; b+=4) {
       uint32_t w=rd(iop,b);
       if ((w>>26)==0x0C && (w&0xFFFF)==0x1F) {
         printf("[R1049-PRIOSCAN] priority-bitmap-index pattern at 0x%08x (srl/sra,5) .. andi,0x1f at 0x%08x\n", a, b);
         hits++;
         break;
       }
     }
   }
 }
 printf("[R1049-PRIOSCAN] total hits: %d\n", hits);
 return 0;
}
