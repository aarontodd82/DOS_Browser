/*
 * RetroSurf Configuration - RETROSURF.CFG parser
 */

#ifndef RETROSURF_CONFIG_H
#define RETROSURF_CONFIG_H

#include <stdint.h>

/* Video mode preferences */
#define VIDMODE_AUTO    0   /* Auto-detect best mode */
#define VIDMODE_800     1   /* Force 800x600x256 */
#define VIDMODE_640     2   /* Force 640x480x256 */
#define VIDMODE_VGA16   3   /* Force 640x480x16 (VGA fallback) */

typedef struct {
    char     server_ip[64];
    uint16_t server_port;
    uint8_t  video_mode;        /* VIDMODE_xxx */
    char     home_url[256];
} Config;

/* Load config from file. Uses defaults if file not found.
 * Returns 0 on success, -1 if file not found (defaults used). */
int config_load(Config *cfg, const char *filename);

#endif /* RETROSURF_CONFIG_H */
