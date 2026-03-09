/*
 * RetroSurf Sound Blaster DMA Driver
 *
 * 8-bit mono PCM playback using auto-init DMA double-buffering.
 *
 * Architecture:
 *   - ISR fires when DMA finishes each half-buffer (~186ms at 11025 Hz)
 *   - ISR only acknowledges the SB interrupt and increments a counter
 *   - sb_pump() (called from main loop) copies data from the ring buffer
 *     to the consumed DMA half-buffer
 *   - sb_feed() fills the ring buffer from network audio data
 *
 * DJGPP specifics:
 *   - DMA buffer allocated in conventional memory (below 1MB)
 *   - ISR code and data locked to prevent CWSDPMI page faults
 *   - dosmemput() used to copy from extended to conventional memory
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pc.h>
#include <dpmi.h>
#include <go32.h>
#include <dos.h>

#include "sbdma.h"

/* ---- DSP I/O port offsets ---- */
#define DSP_RESET       0x06
#define DSP_READ_DATA   0x0A
#define DSP_WRITE_DATA  0x0C  /* also Write Buffer Status */
#define DSP_WRITE_STAT  0x0C
#define DSP_READ_STAT   0x0E  /* also used to acknowledge IRQ */

/* ---- Module state (locked for ISR access) ---- */
static struct {
    /* Hardware config */
    uint16_t base;
    uint8_t  irq;
    uint8_t  dma;
    uint16_t dsp_ver;      /* major<<8 | minor */

    /* DMA buffer in conventional memory */
    uint32_t dma_phys;     /* Physical address of usable region */
    int      dma_seg;      /* Real-mode segment from DPMI alloc */
    int      dma_sel;      /* DPMI selector for free */

    /* Ring buffer (extended memory) */
    uint8_t  ring[SB_RING_SIZE];
    volatile uint16_t ring_rd;
    volatile uint16_t ring_wr;

    /* ISR synchronization */
    volatile uint32_t isr_count;
    uint32_t pump_count;

    /* ISR save/restore */
    _go32_dpmi_seginfo old_isr;
    _go32_dpmi_seginfo new_isr;
    uint8_t  old_pic_mask;

    /* Playback state */
    uint16_t sample_rate;
    int      detected;
    int      playing;
} sb;

/* ---- DSP low-level I/O ---- */

static int dsp_reset(uint16_t base)
{
    int i;

    outportb(base + DSP_RESET, 1);
    /* Wait at least 3 microseconds */
    for (i = 0; i < 200; i++)
        inportb(base + DSP_RESET);   /* ~1us per I/O read */
    outportb(base + DSP_RESET, 0);

    /* Wait for DSP ready: bit 7 of Read Buffer Status, then read 0xAA */
    for (i = 0; i < 1000; i++) {
        if (inportb(base + DSP_READ_STAT) & 0x80) {
            if (inportb(base + DSP_READ_DATA) == 0xAA)
                return 1;
        }
    }
    return 0;
}

static void dsp_write(uint16_t base, uint8_t val)
{
    /* Wait for Write Buffer Status bit 7 to be clear */
    while (inportb(base + DSP_WRITE_STAT) & 0x80)
        ;
    outportb(base + DSP_WRITE_DATA, val);
}

static uint8_t dsp_read(uint16_t base)
{
    /* Wait for Read Buffer Status bit 7 to be set */
    while (!(inportb(base + DSP_READ_STAT) & 0x80))
        ;
    return inportb(base + DSP_READ_DATA);
}

/* ---- Ring buffer helpers ---- */

static uint16_t ring_available(void)
{
    int diff = (int)sb.ring_wr - (int)sb.ring_rd;
    if (diff < 0) diff += SB_RING_SIZE;
    return (uint16_t)diff;
}

static uint16_t ring_free(void)
{
    return SB_RING_SIZE - 1 - ring_available();
}

/* Copy 'len' bytes from ring buffer to conventional memory at 'addr'. */
static void ring_copy_to_dma(uint32_t addr, uint16_t len)
{
    uint16_t first = SB_RING_SIZE - sb.ring_rd;

    if (first >= len) {
        dosmemput(sb.ring + sb.ring_rd, len, addr);
    } else {
        dosmemput(sb.ring + sb.ring_rd, first, addr);
        dosmemput(sb.ring, len - first, addr + first);
    }
    sb.ring_rd = (sb.ring_rd + len) % SB_RING_SIZE;
}

/* Fill conventional memory with silence (0x80 = unsigned 8-bit midpoint). */
static void dma_fill_silence(uint32_t addr, uint16_t len)
{
    uint8_t buf[SB_DMA_HALF_SIZE];
    memset(buf, 0x80, len);
    dosmemput(buf, len, addr);
}

/* ---- ISR ---- */

static void sb_isr(void)
{
    /* Acknowledge Sound Blaster interrupt (read DSP Read Status) */
    inportb(sb.base + DSP_READ_STAT);

    sb.isr_count++;

    /* Send EOI to PIC(s) */
    if (sb.irq >= 8)
        outportb(0xA0, 0x20);
    outportb(0x20, 0x20);
}

/* Marker for _go32_dpmi_lock_code size calculation */
static void sb_isr_end(void) { }

/* ---- ISR installation ---- */

static void install_isr(void)
{
    int vec = (sb.irq < 8) ? sb.irq + 8 : sb.irq - 8 + 0x70;

    /* Lock ISR code and data */
    _go32_dpmi_lock_code((void *)sb_isr,
        (unsigned long)sb_isr_end - (unsigned long)sb_isr);
    _go32_dpmi_lock_data(&sb, sizeof(sb));

    /* Save old handler */
    _go32_dpmi_get_protected_mode_interrupt_vector(vec, &sb.old_isr);

    /* Set up new handler */
    sb.new_isr.pm_offset = (unsigned long)sb_isr;
    sb.new_isr.pm_selector = _go32_my_cs();
    _go32_dpmi_allocate_iret_wrapper(&sb.new_isr);
    _go32_dpmi_set_protected_mode_interrupt_vector(vec, &sb.new_isr);

    /* Save and unmask IRQ in PIC */
    if (sb.irq < 8) {
        sb.old_pic_mask = inportb(0x21) & (1 << sb.irq);
        outportb(0x21, inportb(0x21) & ~(1 << sb.irq));
    } else {
        sb.old_pic_mask = inportb(0xA1) & (1 << (sb.irq - 8));
        outportb(0xA1, inportb(0xA1) & ~(1 << (sb.irq - 8)));
        /* Ensure cascade IRQ 2 is unmasked */
        outportb(0x21, inportb(0x21) & ~(1 << 2));
    }
}

static void uninstall_isr(void)
{
    int vec = (sb.irq < 8) ? sb.irq + 8 : sb.irq - 8 + 0x70;

    /* Restore PIC mask */
    if (sb.irq < 8) {
        outportb(0x21, (inportb(0x21) & ~(1 << sb.irq)) | sb.old_pic_mask);
    } else {
        outportb(0xA1, (inportb(0xA1) & ~(1 << (sb.irq - 8)))
                 | sb.old_pic_mask);
    }

    /* Restore old handler */
    _go32_dpmi_set_protected_mode_interrupt_vector(vec, &sb.old_isr);
    _go32_dpmi_free_iret_wrapper(&sb.new_isr);
}

/* ---- DMA programming ---- */

/* DMA port addresses for channels 0-3 (8-bit DMA) */
static const uint8_t dma_addr_port[] = { 0x00, 0x02, 0x04, 0x06 };
static const uint8_t dma_count_port[] = { 0x01, 0x03, 0x05, 0x07 };
static const uint8_t dma_page_port[] = { 0x87, 0x83, 0x81, 0x82 };

static void program_dma(void)
{
    uint8_t ch = sb.dma;
    uint32_t addr = sb.dma_phys;
    uint16_t count = SB_DMA_BUF_SIZE - 1;

    /* Mask channel */
    outportb(0x0A, 0x04 | ch);

    /* Reset flip-flop */
    outportb(0x0C, 0x00);

    /* Mode: single, read (mem->device), auto-init, increment */
    outportb(0x0B, 0x58 | ch);

    /* Set address (low byte, high byte, page) */
    outportb(dma_addr_port[ch], addr & 0xFF);
    outportb(dma_addr_port[ch], (addr >> 8) & 0xFF);
    outportb(dma_page_port[ch], (addr >> 16) & 0xFF);

    /* Set count (low byte, high byte) */
    outportb(dma_count_port[ch], count & 0xFF);
    outportb(dma_count_port[ch], (count >> 8) & 0xFF);

    /* Unmask channel */
    outportb(0x0A, ch);
}

/* ---- DMA buffer allocation ---- */

static int alloc_dma_buffer(void)
{
    int paras, selector, segment;
    uint32_t phys, page_end;

    /* Allocate 2x to guarantee a non-boundary-crossing region */
    paras = (SB_DMA_BUF_SIZE * 2 + 15) / 16;
    segment = __dpmi_allocate_dos_memory(paras, &selector);
    if (segment == -1) {
        printf("ERROR: Cannot allocate DMA buffer\n");
        return -1;
    }

    sb.dma_seg = segment;
    sb.dma_sel = selector;

    phys = (uint32_t)segment * 16;

    /* Check if first SB_DMA_BUF_SIZE bytes cross a 64KB boundary */
    if ((phys & 0xFFFF0000UL) ==
        ((phys + SB_DMA_BUF_SIZE - 1) & 0xFFFF0000UL)) {
        sb.dma_phys = phys;
    } else {
        /* Align to next 64KB boundary */
        page_end = (phys + 0xFFFF) & 0xFFFF0000UL;
        sb.dma_phys = page_end;
    }

    return 0;
}

static void free_dma_buffer(void)
{
    if (sb.dma_sel) {
        __dpmi_free_dos_memory(sb.dma_sel);
        sb.dma_sel = 0;
        sb.dma_seg = 0;
    }
}

/* ---- Public API ---- */

int sb_detect(uint16_t cfg_base, uint8_t cfg_irq, uint8_t cfg_dma)
{
    const char *blaster;
    const char *p;
    uint16_t base;
    uint8_t irq, dma;
    uint8_t major, minor;

    memset(&sb, 0, sizeof(sb));

    /* Start with config values */
    base = cfg_base;
    irq = cfg_irq;
    dma = cfg_dma;

    /* BLASTER environment variable overrides config */
    blaster = getenv("BLASTER");
    if (blaster) {
        p = blaster;
        while (*p) {
            switch (*p) {
            case 'A': case 'a':
                base = (uint16_t)strtol(p + 1, NULL, 16);
                break;
            case 'I': case 'i':
                irq = (uint8_t)atoi(p + 1);
                break;
            case 'D': case 'd':
                dma = (uint8_t)atoi(p + 1);
                break;
            }
            /* Skip to next space */
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }
    }

    /* Probe DSP reset */
    if (!dsp_reset(base)) {
        printf("Sound Blaster not detected at %03Xh\n", base);
        sb.detected = 0;
        return -1;
    }

    /* Get DSP version */
    dsp_write(base, 0xE1);
    major = dsp_read(base);
    minor = dsp_read(base);

    sb.base = base;
    sb.irq = irq;
    sb.dma = dma;
    sb.dsp_ver = (major << 8) | minor;
    sb.detected = 1;

    printf("Sound Blaster DSP v%d.%02d at %03Xh IRQ %d DMA %d\n",
           major, minor, base, irq, dma);

    return 0;
}

int sb_start(uint16_t sample_rate)
{
    uint16_t block_size;

    if (!sb.detected)
        return -1;

    sb.sample_rate = sample_rate;
    sb.isr_count = 0;
    sb.pump_count = 0;

    /* Allocate DMA buffer in conventional memory */
    if (alloc_dma_buffer() != 0)
        return -1;

    /* Pre-fill DMA buffer from ring buffer (or silence) */
    {
        uint16_t avail = ring_available();
        uint16_t to_copy = (avail < SB_DMA_BUF_SIZE)
                           ? avail : SB_DMA_BUF_SIZE;

        if (to_copy > 0)
            ring_copy_to_dma(sb.dma_phys, to_copy);
        if (to_copy < SB_DMA_BUF_SIZE)
            dma_fill_silence(sb.dma_phys + to_copy,
                             SB_DMA_BUF_SIZE - to_copy);
    }

    /* Install ISR */
    install_isr();

    /* Program DMA controller */
    program_dma();

    /* Turn speaker ON (required — SB won't output without this) */
    dsp_write(sb.base, 0xD1);

    /* Set sample rate */
    if (sb.dsp_ver >= 0x0400) {
        /* SB16: exact sample rate command */
        dsp_write(sb.base, 0x41);
        dsp_write(sb.base, sample_rate >> 8);
        dsp_write(sb.base, sample_rate & 0xFF);
    } else {
        /* SB 2.0+: time constant */
        uint8_t tc = (uint8_t)(256 - 1000000 / sample_rate);
        dsp_write(sb.base, 0x40);
        dsp_write(sb.base, tc);
    }

    /* Start auto-init 8-bit DMA playback.
     * Block size = half buffer (ISR fires at each half). */
    block_size = SB_DMA_HALF_SIZE - 1;

    if (sb.dsp_ver >= 0x0400) {
        /* SB16: 8-bit auto-init with FIFO */
        dsp_write(sb.base, 0xC6);
        dsp_write(sb.base, 0x00);   /* mono, unsigned */
        dsp_write(sb.base, block_size & 0xFF);
        dsp_write(sb.base, block_size >> 8);
    } else {
        /* SB 2.0+: set block size, then auto-init */
        dsp_write(sb.base, 0x48);
        dsp_write(sb.base, block_size & 0xFF);
        dsp_write(sb.base, block_size >> 8);
        dsp_write(sb.base, 0x1C);   /* 8-bit auto-init DMA */
    }

    sb.playing = 1;
    printf("SB playback started at %u Hz\n", sample_rate);
    return 0;
}

int sb_feed(const uint8_t *data, uint16_t len)
{
    uint16_t space, first;

    space = ring_free();
    if (len > space) len = space;
    if (len == 0) return 0;

    /* Copy with wrap handling */
    first = SB_RING_SIZE - sb.ring_wr;
    if (first >= len) {
        memcpy(sb.ring + sb.ring_wr, data, len);
    } else {
        memcpy(sb.ring + sb.ring_wr, data, first);
        memcpy(sb.ring, data + first, len - first);
    }
    sb.ring_wr = (sb.ring_wr + len) % SB_RING_SIZE;

    return len;
}

uint16_t sb_buffer_ms(void)
{
    uint16_t samples = ring_available();
    if (sb.sample_rate == 0) return 0;
    return (uint16_t)((uint32_t)samples * 1000 / sb.sample_rate);
}

void sb_pump(void)
{
    if (!sb.playing) return;

    while (sb.pump_count < sb.isr_count) {
        uint16_t half = sb.pump_count & 1;
        uint32_t dma_off = sb.dma_phys + (uint32_t)half * SB_DMA_HALF_SIZE;
        uint16_t avail = ring_available();

        if (avail >= SB_DMA_HALF_SIZE) {
            /* Full half-buffer of data available */
            ring_copy_to_dma(dma_off, SB_DMA_HALF_SIZE);
        } else if (avail > 0) {
            /* Partial data + silence */
            ring_copy_to_dma(dma_off, avail);
            dma_fill_silence(dma_off + avail, SB_DMA_HALF_SIZE - avail);
        } else {
            /* Underrun: fill with silence */
            dma_fill_silence(dma_off, SB_DMA_HALF_SIZE);
        }

        sb.pump_count++;
    }
}

void sb_stop(void)
{
    if (!sb.playing) return;

    /* Halt DMA playback */
    dsp_write(sb.base, 0xD0);   /* Pause 8-bit DMA */
    dsp_write(sb.base, 0xDA);   /* Exit auto-init 8-bit */
    dsp_write(sb.base, 0xD3);   /* Turn speaker OFF */

    /* Mask DMA channel */
    outportb(0x0A, 0x04 | sb.dma);

    /* Restore ISR */
    uninstall_isr();

    /* Free DMA buffer */
    free_dma_buffer();

    sb.playing = 0;

    /* Reset ring buffer */
    sb.ring_rd = 0;
    sb.ring_wr = 0;

    printf("SB playback stopped\n");
}

int sb_is_available(void)
{
    return sb.detected;
}
