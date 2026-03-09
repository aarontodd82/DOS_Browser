/*
 * RetroSurf Configuration - RETROSURF.CFG parser
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

static void config_defaults(Config *cfg)
{
    strcpy(cfg->server_ip, "10.0.2.2");
    cfg->server_port = 8086;
    cfg->video_mode = VIDMODE_AUTO;
    strcpy(cfg->home_url, "https://www.google.com");
    cfg->sb_base = 0x220;
    cfg->sb_irq = 7;
    cfg->sb_dma = 1;
}

static void trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
}

int config_load(Config *cfg, const char *filename)
{
    FILE *f;
    char line[512], key[64], val[256];

    config_defaults(cfg);

    f = fopen(filename, "r");
    if (!f) return -1;

    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and blank lines */
        trim(line);
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0')
            continue;

        /* Parse "key = value" */
        if (sscanf(line, "%63[^=]=%255[^\n]", key, val) != 2)
            continue;
        trim(key);
        trim(val);

        if (strcmp(key, "server_ip") == 0) {
            strncpy(cfg->server_ip, val, sizeof(cfg->server_ip) - 1);
        } else if (strcmp(key, "server_port") == 0) {
            cfg->server_port = (uint16_t)atoi(val);
        } else if (strcmp(key, "video_mode") == 0) {
            if (strcmp(val, "800") == 0) cfg->video_mode = VIDMODE_800;
            else if (strcmp(val, "640") == 0) cfg->video_mode = VIDMODE_640;
            else if (strcmp(val, "vga16") == 0) cfg->video_mode = VIDMODE_VGA16;
            else cfg->video_mode = VIDMODE_AUTO;
        } else if (strcmp(key, "home_url") == 0) {
            strncpy(cfg->home_url, val, sizeof(cfg->home_url) - 1);
        } else if (strcmp(key, "sb_base") == 0) {
            cfg->sb_base = (uint16_t)strtol(val, NULL, 16);
        } else if (strcmp(key, "sb_irq") == 0) {
            cfg->sb_irq = (uint8_t)atoi(val);
        } else if (strcmp(key, "sb_dma") == 0) {
            cfg->sb_dma = (uint8_t)atoi(val);
        }
    }

    fclose(f);
    return 0;
}
