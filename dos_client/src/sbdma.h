/*
 * RetroSurf Sound Blaster DMA Driver
 *
 * 8-bit mono PCM playback via DMA double-buffering.
 * Supports SB 2.0+ / SBPro / SB16 (auto-init DMA mode).
 * ISR acknowledges SB interrupt; main-loop sb_pump() fills DMA buffer
 * from a 16KB ring buffer fed by network audio data.
 */

#ifndef RETROSURF_SBDMA_H
#define RETROSURF_SBDMA_H

#include <stdint.h>

/* DMA buffer: 4KB in conventional memory, split into 2x2KB halves.
 * At 11025 Hz, each half = ~186ms. */
#define SB_DMA_BUF_SIZE   4096
#define SB_DMA_HALF_SIZE  2048

/* Ring buffer: 16KB in extended memory for network-to-DMA staging.
 * At 11025 Hz = ~1.5s of buffered audio. */
#define SB_RING_SIZE      16384

/* Detect Sound Blaster hardware.
 * Checks BLASTER environment variable, then probes DSP reset.
 * cfg_base/irq/dma are fallback values from RETRO.CFG.
 * Returns 0 on success, -1 if not found. */
int sb_detect(uint16_t cfg_base, uint8_t cfg_irq, uint8_t cfg_dma);

/* Start audio playback at given sample rate.
 * Pre-fills DMA buffer from ring buffer, installs ISR, programs DMA.
 * Returns 0 on success, -1 on failure. */
int sb_start(uint16_t sample_rate);

/* Feed audio samples into the ring buffer.
 * Returns number of bytes actually accepted (may be less than len
 * if ring buffer is nearly full). */
int sb_feed(const uint8_t *data, uint16_t len);

/* Get milliseconds of audio currently in the ring buffer. */
uint16_t sb_buffer_ms(void);

/* Pump audio from ring buffer to DMA buffer.
 * Call frequently from main loop (after net_poll, after frame render).
 * Checks ISR counter and refills consumed DMA halves. */
void sb_pump(void);

/* Stop playback, restore IRQ handler, free DMA buffer. */
void sb_stop(void);

/* Returns 1 if Sound Blaster was detected, 0 otherwise. */
int sb_is_available(void);

#endif /* RETROSURF_SBDMA_H */
