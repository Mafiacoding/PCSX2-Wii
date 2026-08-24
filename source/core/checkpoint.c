/*
 * checkpoint.c - see include/core/checkpoint.h for full format/scope
 * notes and the leak-prevention/known-limitation citations.
 */
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/iop/iop_core.h"
#include "core/hw/dma.h"
#include "core/hw/ee_intc.h"
#include "core/hw/ee_sio.h"
#include "core/hw/ee_timers.h"
#include "core/hw/gif.h"
#include "core/hw/gs.h"
#include "core/hw/gs_mem.h"
#include "core/hw/iop_dma.h"
#include "core/hw/iop_excb.h"
#include "core/hw/iop_hle_bios.h"
#include "core/hw/iop_hle_modules.h"
#include "core/hw/iop_hle_thread.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_timers.h"
#include "core/hw/iop_heap.h"
#include "core/hw/iop_cdrom_legacy.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/mch.h"
#include "core/hw/sif.h"
#include "core/hw/vif.h"
#include "core/hw/vu.h"
#include "core/system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors ee_core.c's/iop_core.c's own private EE_RAM_SIZE/IOP_RAM_SIZE
 * #defines (32MB/2MB) - not exposed via any header, so re-stated here
 * under a distinct name to avoid a collision if a future round DOES
 * expose them. Scratch-buffer sizing only; the real authoritative
 * sizes always come from ee->ram_size/iop->ram_size at save time. */
#define EE_RAM_SIZE_CKPT  (32 * 1024 * 1024 + 65536)
#define IOP_RAM_SIZE_CKPT (2 * 1024 * 1024 + 65536)

static int write_block(FILE *f, const char tag[4], const void *data, uint32_t size)
{
    if (fwrite(tag, 1, 4, f) != 4) return -1;
    if (fwrite(&size, sizeof(size), 1, f) != 1) return -1;
    if (size > 0 && fwrite(data, 1, size, f) != size) return -1;
    return 0;
}

int checkpoint_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    const char magic[4] = {'P', 'W', '2', 'K'};
    uint32_t version = 1;
    if (fwrite(magic, 1, 4, f) != 4) goto fail;
    if (fwrite(&version, sizeof(version), 1, f) != 1) goto fail;

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    /* Fixed-layout structs, exposed via existing typed _get_state()
     * accessors (see checkpoint.h's module comment) - one block per
     * module, tag chosen to be readable in a hex dump. */
    if (write_block(f, "EES1", ee, sizeof(*ee)) < 0) goto fail;
    if (write_block(f, "ERAM", ee->ram, ee->ram_size) < 0) goto fail;
    if (write_block(f, "IOPS", iop, sizeof(*iop)) < 0) goto fail;
    if (write_block(f, "IRAM", iop->ram, iop->ram_size) < 0) goto fail;
    if (write_block(f, "DMA0", dma_get_state(), sizeof(*dma_get_state())) < 0) goto fail;
    if (write_block(f, "EINT", ee_intc_get_state(), sizeof(*ee_intc_get_state())) < 0) goto fail;
    if (write_block(f, "ESIO", ee_sio_get_state(), sizeof(*ee_sio_get_state())) < 0) goto fail;
    if (write_block(f, "ETMR", ee_timers_get_state(), sizeof(*ee_timers_get_state())) < 0) goto fail;
    if (write_block(f, "GIF0", gif_get_state(), sizeof(*gif_get_state())) < 0) goto fail;
    if (write_block(f, "GS00", gs_get_state(), sizeof(*gs_get_state())) < 0) goto fail;
    /* Round 649: GS local memory (the actual pixel/texture backing
     * store, separate from GS00's small register struct) was never
     * captured by any prior round - a real, evidenced gap found
     * while investigating why a resumed checkpoint showed non-zero
     * gif_state_t draw counters (sprites_drawn/etc, correctly
     * restored via GIF0) but a completely zeroed framebuffer: every
     * checkpoint_load() silently reset the entire 4MB gs_mem.c
     * buffer to zero because nothing ever wrote/read it. Saving it
     * here fixes that - draw counters and actual pixel content now
     * stay consistent across a save/resume boundary. */
    if (write_block(f, "GSM0", gs_mem_get(), GS_MEM_SIZE) < 0) goto fail;
    if (write_block(f, "IDMA", iop_dma_get_state(), sizeof(*iop_dma_get_state())) < 0) goto fail;
    if (write_block(f, "IEXC", iop_excb_get_state(), sizeof(*iop_excb_get_state())) < 0) goto fail;
    if (write_block(f, "IBIO", iop_hle_bios_get_state(), sizeof(*iop_hle_bios_get_state())) < 0) goto fail;
    if (write_block(f, "IMOD", iop_hle_modules_get_state(), sizeof(*iop_hle_modules_get_state())) < 0) goto fail;
    if (write_block(f, "IINT", iop_intc_get_state(), sizeof(*iop_intc_get_state())) < 0) goto fail;
    if (write_block(f, "ITMR", iop_timers_get_state(), sizeof(*iop_timers_get_state())) < 0) goto fail;
    /* Round 659: IOP HLE thread-scheduler state (source/hw/iop_hle_thread.c)
     * - see iop_hle_thread.h's iop_hle_thread_get_checkpoint_blob() header
     * comment for why this block was missing entirely before this round
     * (every IOP thread was silently reset to "0 threads" on every
     * checkpoint resume). */
    {
        uint32_t ithr_size = 0;
        void *ithr_blob = iop_hle_thread_get_checkpoint_blob(&ithr_size);
        if (write_block(f, "ITHR", ithr_blob, ithr_size) < 0) goto fail;
    }
    if (write_block(f, "MCH0", mch_get_state(), sizeof(*mch_get_state())) < 0) goto fail;
    if (write_block(f, "SIF0", sif_get_state(), sizeof(*sif_get_state())) < 0) goto fail;
    if (write_block(f, "VIF0", vif0_get_state(), sizeof(*vif0_get_state())) < 0) goto fail;
    if (write_block(f, "VIF1", vif1_get_state(), sizeof(*vif1_get_state())) < 0) goto fail;
    if (write_block(f, "VU10", vu1_get_state(), sizeof(*vu1_get_state())) < 0) goto fail;

    /* Opaque blob (no typed accessor - see ee_hle_thread.h citation). */
    {
        void *blob; uint32_t blob_size;
        ee_hle_thread_get_checkpoint_blob(&blob, &blob_size);
        if (write_block(f, "EETH", blob, blob_size) < 0) goto fail;
    }

    /* IOP heap allocator: NOT a raw struct dump - g_alloclist is a
     * malloc()'d chain of nodes, so a raw byte copy would embed
     * this PROCESS's own heap pointers, which are meaningless (and
     * unsafe to dereference) in the resuming process. Use the
     * dedicated snapshot pair instead (see iop_heap.h's citation on
     * exactly why - this is the documented root cause of the
     * Round 307-447 "[R313-SIGSEGV]" resume failures this format is
     * designed to avoid repeating). */
    {
        uint32_t heap_size = iop_heap_snapshot_size();
        void *heap_buf = malloc(heap_size ? heap_size : 1);
        if (!heap_buf) goto fail;
        iop_heap_snapshot_save(heap_buf);
        int rc = write_block(f, "IHP1", heap_buf, heap_size);
        free(heap_buf);
        if (rc < 0) goto fail;
    }

    if (write_block(f, "END0", NULL, 0) < 0) goto fail;

    fclose(f);
    return 0;

fail:
    fclose(f);
    remove(path);
    return -1;
}

/* Reads one block's tag+size+payload. Returns 1 on success, 0 on
 * EOF/END0, -1 on I/O error or a size that doesn't fit in `cap`. */
static int read_block(FILE *f, char tag_out[4], void *buf, uint32_t cap, uint32_t *size_out)
{
    char tag[4];
    uint32_t size;
    if (fread(tag, 1, 4, f) != 4) return -1;
    if (fread(&size, sizeof(size), 1, f) != 1) return -1;
    memcpy(tag_out, tag, 4);
    *size_out = size;
    if (memcmp(tag, "END0", 4) == 0) return 0;
    if (size > cap) return -1; /* caller's buffer too small - corrupt/mismatched file */
    if (size > 0 && fread(buf, 1, size, f) != size) return -1;
    return 1;
}

int checkpoint_load(const char *path, const bios_image_t *ee_bios,
                     const bios_image_t *iop_bios, const char *iso_path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char magic[4];
    uint32_t version;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PW2K", 4) != 0) { fclose(f); return -1; }
    if (fread(&version, sizeof(version), 1, f) != 1 || version != 1) { fclose(f); return -1; }

    /* Validate the whole block sequence into scratch buffers first -
     * per checkpoint.h's contract, never partially apply a corrupt
     * file. EE/IOP RAM blocks are large (32MB/2MB), so allocate the
     * scratch buffers on the heap, not the stack. */
    ee_state_t *ee_scratch = malloc(sizeof(ee_state_t));
    iop_state_t *iop_scratch = malloc(sizeof(iop_state_t));
    uint8_t *era_scratch = malloc(EE_RAM_SIZE_CKPT);
    uint8_t *ira_scratch = malloc(IOP_RAM_SIZE_CKPT);
    uint8_t *gsm_scratch = malloc(GS_MEM_SIZE); /* Round 649: gs_mem.c's 4MB pixel/texture buffer - see checkpoint_save()'s citation */
    /* Peripheral scratch buffers - sized generously; read_block()
     * rejects any on-disk block that doesn't fit, so a mismatched
     * struct layout across builds fails safely instead of
     * overflowing. */
    uint8_t generic[65536];
    uint8_t eeth_blob[65536];
    uint8_t heap_blob[1 << 20];
    uint32_t era_size = 0, ira_size = 0, eeth_size = 0, heap_size = 0, gsm_size = 0;

    if (!ee_scratch || !iop_scratch || !era_scratch || !ira_scratch || !gsm_scratch) goto fail_alloc;

    char tag[4]; uint32_t size; int rc;

    #define EXPECT(TAGSTR, DEST, CAP, SIZEOUT) \
        do { \
            rc = read_block(f, tag, (DEST), (CAP), (SIZEOUT)); \
            if (rc <= 0 || memcmp(tag, (TAGSTR), 4) != 0) goto fail_close; \
        } while (0)

    uint32_t discard_size;
    EXPECT("EES1", ee_scratch, sizeof(ee_state_t), &discard_size);
    EXPECT("ERAM", era_scratch, EE_RAM_SIZE_CKPT, &era_size);
    EXPECT("IOPS", iop_scratch, sizeof(iop_state_t), &discard_size);
    EXPECT("IRAM", ira_scratch, IOP_RAM_SIZE_CKPT, &ira_size);
    EXPECT("DMA0", generic, sizeof(generic), &size); memcpy(dma_get_state(), generic, size);
    EXPECT("EINT", generic, sizeof(generic), &size); memcpy(ee_intc_get_state(), generic, size);
    EXPECT("ESIO", generic, sizeof(generic), &size); memcpy(ee_sio_get_state(), generic, size);
    EXPECT("ETMR", generic, sizeof(generic), &size); memcpy(ee_timers_get_state(), generic, size);
    EXPECT("GIF0", generic, sizeof(generic), &size); memcpy(gif_get_state(), generic, size);
    EXPECT("GS00", generic, sizeof(generic), &size); memcpy(gs_get_state(), generic, size);
    EXPECT("GSM0", gsm_scratch, GS_MEM_SIZE, &gsm_size);
    EXPECT("IDMA", generic, sizeof(generic), &size); memcpy(iop_dma_get_state(), generic, size);
    EXPECT("IEXC", generic, sizeof(generic), &size); memcpy(iop_excb_get_state(), generic, size);
    EXPECT("IBIO", generic, sizeof(generic), &size); memcpy(iop_hle_bios_get_state(), generic, size);
    EXPECT("IMOD", generic, sizeof(generic), &size); memcpy(iop_hle_modules_get_state(), generic, size);
    EXPECT("IINT", generic, sizeof(generic), &size); memcpy(iop_intc_get_state(), generic, size);
    EXPECT("ITMR", generic, sizeof(generic), &size); memcpy(iop_timers_get_state(), generic, size);
    {
        uint32_t ithr_cap = 0;
        void *ithr_dest = iop_hle_thread_get_checkpoint_blob(&ithr_cap);
        EXPECT("ITHR", generic, sizeof(generic), &size);
        if (size != ithr_cap) goto fail_close; /* struct-layout mismatch - fail safely, per this file's own documented contract */
        memcpy(ithr_dest, generic, size);
    }
    EXPECT("MCH0", generic, sizeof(generic), &size); memcpy(mch_get_state(), generic, size);
    EXPECT("SIF0", generic, sizeof(generic), &size); memcpy(sif_get_state(), generic, size);
    EXPECT("VIF0", generic, sizeof(generic), &size); memcpy(vif0_get_state(), generic, size);
    EXPECT("VIF1", generic, sizeof(generic), &size); memcpy(vif1_get_state(), generic, size);
    EXPECT("VU10", generic, sizeof(generic), &size); memcpy(vu1_get_state(), generic, size);
    EXPECT("EETH", eeth_blob, sizeof(eeth_blob), &eeth_size);
    EXPECT("IHP1", heap_blob, sizeof(heap_blob), &heap_size);

    /* Final terminator. */
    rc = read_block(f, tag, NULL, 0, &size);
    if (rc != 0 || memcmp(tag, "END0", 4) != 0) goto fail_close;

    fclose(f);

    /* All blocks read and size-validated - now safe to apply. Order
     * matters: raw struct copies first (this clobbers ee_scratch's/
     * iop_scratch's now-stale `ram`/`bios` pointer VALUES with
     * whatever they happened to be in the WRITING process - fine,
     * since we overwrite both fields explicitly right after), then
     * every _bind_*()/_rebind_*() call from include/core/system.h's
     * documented restore-time checklist. */
    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    memcpy(ee, ee_scratch, sizeof(*ee));
    memcpy(iop, iop_scratch, sizeof(*iop));
    free(ee_scratch);
    free(iop_scratch);

    ee->ram = era_scratch; /* ownership transferred - this becomes the live EE RAM buffer */
    ee->ram_size = era_size;
    ee->bios = ee_bios;
    iop->ram = ira_scratch; /* ownership transferred - live IOP RAM buffer */
    iop->ram_size = ira_size;
    iop->bios = iop_bios;

    dma_bind_ee_ram(ee->ram, ee->ram_size);
    dma_bind_scratchpad(ee->scratch, sizeof(ee->scratch));
    iop_dma_bind_iop_ram(iop->ram, iop->ram_size);
    ee_core_rebind_dma_sinks();
    system_rebind_iop_bridge();
    if (iso_path) {
        iop_cdrom_legacy_rebind_iso(iso_path);
        iop_cdvd_rebind_iso(iso_path);
    }

    {
        void *blob; uint32_t blob_cap;
        ee_hle_thread_get_checkpoint_blob(&blob, &blob_cap);
        if (blob_cap == eeth_size) memcpy(blob, eeth_blob, eeth_size);
    }
    iop_heap_snapshot_load(heap_blob, heap_size);
    if (gsm_size == GS_MEM_SIZE) memcpy(gs_mem_get(), gsm_scratch, GS_MEM_SIZE); /* Round 649 */
    free(gsm_scratch);

    #undef EXPECT
    return 0;

fail_close:
    fclose(f);
fail_alloc:
    free(ee_scratch);
    free(iop_scratch);
    free(era_scratch);
    free(ira_scratch);
    free(gsm_scratch);
    return -1;
}
