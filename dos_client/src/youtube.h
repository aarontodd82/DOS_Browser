/*
 * RetroSurf YouTube Mode - Video playback in Mode 13h
 *
 * When the server detects a YouTube URL, it streams dithered video
 * frames over TCP. The client switches to VGA Mode 13h (320x200x256)
 * for fast direct framebuffer writes, receives block-delta RLE frames,
 * and blits them to VGA.
 *
 * Phase 1: Silent video only (no audio).
 */

#ifndef RETROSURF_YOUTUBE_H
#define RETROSURF_YOUTUBE_H

#include <stdint.h>
#include "video.h"
#include "network.h"
#include "config.h"

/* YouTube player state */
#define YT_STATE_IDLE      0
#define YT_STATE_PLAYING   1
#define YT_STATE_PAUSED    2
#define YT_STATE_ENDED     3

/* Video constants for Mode 13h */
#define YT_VIDEO_W       320
#define YT_VIDEO_H       200
#define YT_BLOCK_SIZE      8
#define YT_BLOCKS_X       (YT_VIDEO_W / YT_BLOCK_SIZE)   /* 40 */
#define YT_BLOCKS_Y       (YT_VIDEO_H / YT_BLOCK_SIZE)   /* 25 */
#define YT_BLOCK_PIXELS   (YT_BLOCK_SIZE * YT_BLOCK_SIZE) /* 64 */
#define YT_FRAMEBUF_SIZE  (YT_VIDEO_W * YT_VIDEO_H)      /* 64000 */

/* YouTube control actions (matches server protocol) */
#define YT_ACTION_PAUSE     0
#define YT_ACTION_RESUME    1
#define YT_ACTION_SEEK_FWD  2
#define YT_ACTION_SEEK_BACK 3
#define YT_ACTION_STOP      4

/* Enter YouTube mode and run the video player loop.
 *
 * Called from main.c when MSG_MODE_SWITCH(2) is received.
 * Switches to Mode 13h, receives and displays video frames,
 * handles keyboard input (ESC to exit, Space to pause).
 * Restores VESA mode before returning.
 *
 * Args:
 *   cfg: client configuration
 *   vc: video config (VESA mode info, needed for restore)
 *   ctx: network context (for send/receive)
 *   recv_buf: shared receive buffer (MAX_PAYLOAD_SIZE bytes)
 *
 * Returns: 0 = normal exit, 1 = disconnected
 */
int run_youtube(Config *cfg, VideoConfig *vc, net_context_t *ctx,
                uint8_t *recv_buf);

#endif /* RETROSURF_YOUTUBE_H */
