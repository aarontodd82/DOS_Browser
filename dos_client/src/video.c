/*
 * RetroSurf Video Subsystem - VESA/VGA implementation
 *
 * Handles VBE mode detection, mode setting, LFB/banked framebuffer
 * writes, palette management, and dirty rect tracking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/nearptr.h>
#include <pc.h>
#include "video.h"
#include "config.h"

#define TILE_SIZE 16

/* VBE Mode Info Block (256 bytes, from VBE spec) */
#pragma pack(push, 1)
typedef struct {
    uint16_t attributes;       /* bit 0=supported, bit 7=LFB available */
    uint8_t  window_a_attrs;
    uint8_t  window_b_attrs;
    uint16_t granularity;      /* Window granularity in KB */
    uint16_t window_size;      /* Window size in KB */
    uint16_t segment_a;
    uint16_t segment_b;
    uint32_t win_func_ptr;
    uint16_t bytes_per_line;
    /* VBE 1.2+ */
    uint16_t width;
    uint16_t height;
    uint8_t  char_width;
    uint8_t  char_height;
    uint8_t  planes;
    uint8_t  bpp;
    uint8_t  banks;
    uint8_t  memory_model;
    uint8_t  bank_size;
    uint8_t  image_pages;
    uint8_t  reserved1;
    /* Direct color fields */
    uint8_t  red_mask_size;
    uint8_t  red_field_pos;
    uint8_t  green_mask_size;
    uint8_t  green_field_pos;
    uint8_t  blue_mask_size;
    uint8_t  blue_field_pos;
    uint8_t  rsv_mask_size;
    uint8_t  rsv_field_pos;
    uint8_t  direct_color_info;
    /* VBE 2.0+ */
    uint32_t phys_base_ptr;    /* LFB physical address */
    uint32_t reserved2;
    uint16_t reserved3;
    /* Pad to 256 bytes */
    uint8_t  padding[206];
} VbeModeInfo;
#pragma pack(pop)

/* Saved LFB mapping info for cleanup */
static __dpmi_meminfo lfb_mapping;
static int lfb_mapped = 0;
static int nearptr_enabled = 0;

/* ---- VBE helper functions ---- */

/* Detect VBE presence and version.
 * Returns VBE version (e.g., 0x0200 for VBE 2.0), or 0 if not present. */
static uint16_t vbe_detect(void)
{
    int dos_seg, dos_sel;
    __dpmi_regs r;
    uint8_t buf[512];
    uint16_t version;

    dos_seg = __dpmi_allocate_dos_memory(32, &dos_sel); /* 512 bytes */
    if (dos_seg == -1) return 0;

    /* Write "VBE2" signature to request VBE 2.0+ info */
    memset(buf, 0, 512);
    memcpy(buf, "VBE2", 4);
    dosmemput(buf, 512, dos_seg * 16);

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F00;
    r.x.es = dos_seg;
    r.x.di = 0;
    __dpmi_int(0x10, &r);

    if (r.x.ax != 0x004F) {
        __dpmi_free_dos_memory(dos_sel);
        return 0;
    }

    dosmemget(dos_seg * 16, 512, buf);
    __dpmi_free_dos_memory(dos_sel);

    if (memcmp(buf, "VESA", 4) != 0) return 0;

    version = *(uint16_t *)(buf + 4);
    return version;
}

/* Get mode info for a specific VBE mode.
 * Returns 1 on success, 0 on failure. */
static int vbe_get_mode_info(uint16_t mode, VbeModeInfo *info)
{
    int dos_seg, dos_sel;
    __dpmi_regs r;

    dos_seg = __dpmi_allocate_dos_memory(16, &dos_sel); /* 256 bytes */
    if (dos_seg == -1) return 0;

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F01;
    r.x.cx = mode;
    r.x.es = dos_seg;
    r.x.di = 0;
    __dpmi_int(0x10, &r);

    if (r.x.ax != 0x004F) {
        __dpmi_free_dos_memory(dos_sel);
        return 0;
    }

    dosmemget(dos_seg * 16, 256, info);
    __dpmi_free_dos_memory(dos_sel);
    return 1;
}

/* Set a VBE mode. use_lfb: set bit 14 for LFB.
 * Returns 1 on success, 0 on failure. */
static int vbe_set_mode(uint16_t mode, int use_lfb)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F02;
    r.x.bx = mode;
    if (use_lfb) r.x.bx |= 0x4000;
    __dpmi_int(0x10, &r);
    return (r.x.ax == 0x004F);
}

/* Set VGA bank for banked mode writes */
static void vbe_set_bank(int bank)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F05;
    r.x.bx = 0;     /* Window A */
    r.x.dx = bank;
    __dpmi_int(0x10, &r);
}

/* Map the LFB physical address to a near pointer.
 * Returns pointer on success, NULL on failure. */
static uint8_t *map_lfb(uint32_t phys_addr, uint32_t size)
{
    lfb_mapping.address = phys_addr;
    lfb_mapping.size = size;

    if (__dpmi_physical_address_mapping(&lfb_mapping) != 0) {
        return NULL;
    }
    lfb_mapped = 1;

    if (__djgpp_nearptr_enable() == 0) {
        __dpmi_free_physical_address_mapping(&lfb_mapping);
        lfb_mapped = 0;
        return NULL;
    }
    nearptr_enabled = 1;

    return (uint8_t *)(lfb_mapping.address + __djgpp_conventional_base);
}

/* ---- Banked mode write helpers ---- */

/* Copy a row of pixels to VGA in banked mode */
static void banked_copy_row(VideoConfig *vc, int y, int x, int width)
{
    uint32_t offset = (uint32_t)y * vc->bytes_per_line + x;
    uint32_t gran = (uint32_t)vc->bank_granularity * 1024;
    uint8_t *src = vc->backbuffer + y * vc->width + x;
    int remaining = width;

    while (remaining > 0) {
        uint32_t bank = offset / gran;
        uint32_t bank_off = offset % gran;
        uint32_t avail = gran - bank_off;
        int chunk = remaining < (int)avail ? remaining : (int)avail;
        int i;

        vbe_set_bank(bank);
        for (i = 0; i < chunk; i++) {
            _farpokeb(_dos_ds, 0xA0000 + bank_off + i, src[i]);
        }

        src += chunk;
        offset += chunk;
        remaining -= chunk;
    }
}

/* ---- Public API ---- */

int video_init(VideoConfig *vc, uint8_t preferred_mode)
{
    uint16_t vbe_ver;
    VbeModeInfo mi;
    int found = 0;

    /* Candidate modes in priority order */
    struct {
        uint16_t mode_num;
        uint16_t width, height;
        uint8_t  bpp;
        uint8_t  pref;   /* Required VIDMODE_xxx, or AUTO */
    } candidates[] = {
        { 0x103, 800, 600, 8, VIDMODE_800 },
        { 0x101, 640, 480, 8, VIDMODE_640 },
    };
    int num_candidates = 2;
    int i;

    memset(vc, 0, sizeof(VideoConfig));

    /* If user forced VGA16, skip VBE detection */
    if (preferred_mode == VIDMODE_VGA16)
        goto vga_fallback;

    /* Detect VBE */
    vbe_ver = vbe_detect();
    printf("VBE version: %d.%d\n", vbe_ver >> 8, vbe_ver & 0xFF);

    if (vbe_ver < 0x0102) {
        printf("VBE 1.2+ required for VESA modes\n");
        if (preferred_mode == VIDMODE_800 || preferred_mode == VIDMODE_640) {
            printf("Cannot set requested mode without VBE.\n");
            goto vga_fallback;
        }
        goto vga_fallback;
    }

    /* Try each candidate mode */
    for (i = 0; i < num_candidates; i++) {
        /* Skip if user forced a specific mode that doesn't match */
        if (preferred_mode != VIDMODE_AUTO &&
            preferred_mode != candidates[i].pref)
            continue;

        if (!vbe_get_mode_info(candidates[i].mode_num, &mi))
            continue;

        /* Check if mode is supported */
        if (!(mi.attributes & 0x01))
            continue;

        /* Verify resolution and BPP */
        if (mi.width != candidates[i].width ||
            mi.height != candidates[i].height ||
            mi.bpp != candidates[i].bpp)
            continue;

        printf("Found VESA mode 0x%03X: %ux%ux%u",
               candidates[i].mode_num, mi.width, mi.height, mi.bpp);

        /* Try LFB first (requires VBE 2.0+ and mode support) */
        if (vbe_ver >= 0x0200 && (mi.attributes & 0x80) && mi.phys_base_ptr) {
            if (vbe_set_mode(candidates[i].mode_num, 1)) {
                uint32_t fb_size = (uint32_t)mi.width * mi.height;
                vc->lfb_ptr = map_lfb(mi.phys_base_ptr, fb_size);
                if (vc->lfb_ptr) {
                    vc->has_lfb = 1;
                    vc->lfb_phys_addr = mi.phys_base_ptr;
                    printf(" [LFB at 0x%08lX]\n",
                           (unsigned long)mi.phys_base_ptr);
                    found = 1;
                } else {
                    printf(" [LFB map failed, trying banked]\n");
                    /* Fall through to try banked */
                }
            }
        }

        /* Try banked mode if LFB failed */
        if (!found) {
            if (vbe_set_mode(candidates[i].mode_num, 0)) {
                vc->has_lfb = 0;
                printf(" [banked, gran=%uKB]\n", mi.granularity);
                found = 1;
            } else {
                printf(" [set failed]\n");
                continue;
            }
        }

        if (found) {
            vc->width = mi.width;
            vc->height = mi.height;
            vc->bpp = mi.bpp;
            vc->bytes_per_line = mi.bytes_per_line;
            vc->vesa_mode = candidates[i].mode_num;
            vc->bank_granularity = mi.granularity ? mi.granularity : 64;
            break;
        }
    }

    if (!found) {
        printf("No suitable VESA mode found.\n");
        goto vga_fallback;
    }

    goto setup_common;

vga_fallback:
    /* Set VGA mode 12h (640x480x16) */
    {
        __dpmi_regs r;
        memset(&r, 0, sizeof(r));
        r.x.ax = 0x0012;
        __dpmi_int(0x10, &r);
    }
    printf("Using VGA mode 12h (640x480x16)\n");
    vc->width = 640;
    vc->height = 480;
    vc->bpp = 4;
    vc->bytes_per_line = 80;  /* 640/8 for planar */
    vc->has_lfb = 0;
    vc->vesa_mode = 0;
    vc->bank_granularity = 64;

setup_common:
    /* Calculate content area and tile grid */
    vc->chrome_height = (vc->width >= 800) ? CHROME_HEIGHT_800 : CHROME_HEIGHT_640;
    vc->status_height = 12;
    vc->content_width = vc->width;
    vc->content_height = vc->height - vc->chrome_height;
    vc->tile_cols = vc->content_width / TILE_SIZE;
    vc->tile_rows = (vc->content_height + TILE_SIZE - 1) / TILE_SIZE;
    vc->tile_total = vc->tile_cols * vc->tile_rows;

    /* Allocate backbuffer in extended memory */
    vc->backbuffer = (uint8_t *)malloc((uint32_t)vc->width * vc->height);
    if (!vc->backbuffer) {
        printf("ERROR: Cannot allocate backbuffer (%u bytes)\n",
               vc->width * vc->height);
        video_shutdown(vc);
        return -1;
    }
    memset(vc->backbuffer, 0, (uint32_t)vc->width * vc->height);

    vc->dirty_count = 0;
    vc->full_flush = 0;

    printf("Video: %ux%u %ubpp, content=%ux%u, tiles=%ux%u=%u\n",
           vc->width, vc->height, vc->bpp,
           vc->content_width, vc->content_height,
           vc->tile_cols, vc->tile_rows, vc->tile_total);

    return 0;
}

void video_set_palette(const uint8_t *rgb, int count)
{
    int i;
    outportb(0x3C8, 0);  /* Start at palette index 0 */
    for (i = 0; i < count * 3; i++) {
        outportb(0x3C9, rgb[i] >> 2);  /* 8-bit -> 6-bit */
    }
}

void video_mark_dirty(VideoConfig *vc, uint16_t x, uint16_t y,
                      uint16_t w, uint16_t h)
{
    if (vc->dirty_count < MAX_DIRTY_RECTS) {
        DirtyRect *dr = &vc->dirty_rects[vc->dirty_count++];
        dr->x = x;
        dr->y = y;
        dr->w = w;
        dr->h = h;
    } else {
        /* Too many dirty rects, flush everything */
        vc->full_flush = 1;
    }
}

void video_flush_dirty(VideoConfig *vc)
{
    int i, y;

    if (vc->full_flush) {
        video_flush_full(vc);
        vc->full_flush = 0;
        vc->dirty_count = 0;
        return;
    }

    if (vc->dirty_count == 0) return;

    if (vc->bpp == 4) {
        /* 16-color planar mode - just do full flush for simplicity */
        video_flush_full(vc);
        vc->dirty_count = 0;
        return;
    }

    if (vc->has_lfb) {
        /* LFB: copy dirty rect rows directly */
        for (i = 0; i < vc->dirty_count; i++) {
            DirtyRect *dr = &vc->dirty_rects[i];
            for (y = dr->y; y < dr->y + dr->h && y < vc->height; y++) {
                uint8_t *src = vc->backbuffer + y * vc->width + dr->x;
                uint8_t *dst = vc->lfb_ptr + y * vc->bytes_per_line + dr->x;
                memcpy(dst, src, dr->w);
            }
        }
    } else {
        /* Banked: copy dirty rect rows with bank switching */
        for (i = 0; i < vc->dirty_count; i++) {
            DirtyRect *dr = &vc->dirty_rects[i];
            for (y = dr->y; y < dr->y + dr->h && y < vc->height; y++) {
                banked_copy_row(vc, y, dr->x, dr->w);
            }
        }
    }

    vc->dirty_count = 0;
}

void video_flush_full(VideoConfig *vc)
{
    if (vc->bpp == 4) {
        /* 16-color VGA mode 12h: planar write */
        int plane, y, x, bit;
        for (plane = 0; plane < 4; plane++) {
            outportb(0x3C4, 0x02);        /* Sequencer: Map Mask Register */
            outportb(0x3C5, 1 << plane);  /* Select plane */
            for (y = 0; y < vc->height; y++) {
                for (x = 0; x < vc->width; x += 8) {
                    uint8_t byte = 0;
                    for (bit = 0; bit < 8; bit++) {
                        uint8_t pixel = vc->backbuffer[y * vc->width + x + bit];
                        if (pixel & (1 << plane))
                            byte |= (0x80 >> bit);
                    }
                    _farpokeb(_dos_ds, 0xA0000 + y * (vc->width / 8) + x / 8, byte);
                }
            }
        }
        return;
    }

    if (vc->has_lfb) {
        /* LFB: single memcpy */
        uint32_t size = (uint32_t)vc->width * vc->height;
        if (vc->bytes_per_line == vc->width) {
            memcpy(vc->lfb_ptr, vc->backbuffer, size);
        } else {
            /* Bytes per line may differ from width (padding) */
            int y;
            for (y = 0; y < vc->height; y++) {
                memcpy(vc->lfb_ptr + y * vc->bytes_per_line,
                       vc->backbuffer + y * vc->width,
                       vc->width);
            }
        }
    } else {
        /* Banked mode: copy in chunks with bank switching */
        uint32_t gran = (uint32_t)vc->bank_granularity * 1024;
        int bank = 0;
        int y;

        /* Row-by-row copy with bank tracking */
        for (y = 0; y < vc->height; y++) {
            uint8_t *src = vc->backbuffer + y * vc->width;
            uint32_t row_offset = (uint32_t)y * vc->bytes_per_line;
            int remaining = vc->width;
            int col = 0;

            while (remaining > 0) {
                uint32_t cur_offset = row_offset + col;
                int new_bank = cur_offset / gran;
                uint32_t bank_off = cur_offset % gran;
                uint32_t avail = gran - bank_off;
                int chunk = remaining < (int)avail ? remaining : (int)avail;
                int i;

                if (new_bank != bank) {
                    vbe_set_bank(new_bank);
                    bank = new_bank;
                }

                for (i = 0; i < chunk; i++) {
                    _farpokeb(_dos_ds, 0xA0000 + bank_off + i, src[col + i]);
                }

                col += chunk;
                remaining -= chunk;
            }
        }
    }
}

void video_fill_rect(VideoConfig *vc, uint16_t x, uint16_t y,
                     uint16_t w, uint16_t h, uint8_t color)
{
    int row;
    for (row = y; row < y + h && row < vc->height; row++) {
        memset(vc->backbuffer + row * vc->width + x, color, w);
    }
}

void video_shutdown(VideoConfig *vc)
{
    __dpmi_regs r;

    /* Restore text mode 03h */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);

    /* Free LFB mapping */
    if (nearptr_enabled) {
        __djgpp_nearptr_disable();
        nearptr_enabled = 0;
    }
    if (lfb_mapped) {
        __dpmi_free_physical_address_mapping(&lfb_mapping);
        lfb_mapped = 0;
    }

    /* Free backbuffer */
    if (vc->backbuffer) {
        free(vc->backbuffer);
        vc->backbuffer = NULL;
    }
}
