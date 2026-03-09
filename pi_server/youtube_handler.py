"""YouTube video extraction and transcoding for RetroSurf.

Uses yt-dlp to extract stream URLs and ffmpeg to decode/scale video
frames to raw RGB24 for the DOS client.
"""

import asyncio
import json
import os
import re
import shutil
import sys

YOUTUBE_RE = re.compile(
    r'(?:https?://)?(?:www\.|m\.)?'
    r'(?:youtube\.com/(?:watch|shorts|embed)|youtu\.be/)',
    re.IGNORECASE
)


def is_youtube_url(url):
    """Check if a URL is a YouTube video URL."""
    return bool(YOUTUBE_RE.search(url))


class YouTubeHandler:
    """Manages yt-dlp extraction and ffmpeg transcoding pipeline."""

    def __init__(self, config):
        self.config = config
        self.yt_dlp_path = self._find_tool(
            config.get('yt_dlp_path', 'yt-dlp'), 'yt-dlp')
        self.ffmpeg_path = self._find_tool(
            config.get('ffmpeg_path', 'ffmpeg'), 'ffmpeg')
        self.width = config.get('youtube_width', 320)
        self.height = config.get('youtube_height', 200)
        self.fps = config.get('youtube_fps', 10)

        self.stream_url = None
        self.title = 'Unknown'
        self.duration = 0

        self._video_proc = None
        self._frame_size = self.width * self.height * 3

    @staticmethod
    def _find_tool(configured_path, tool_name):
        """Find tool executable, checking venv Scripts dir first."""
        # Check configured path
        found = shutil.which(configured_path)
        if found:
            return found

        # Check venv Scripts directory
        venv_dir = os.path.dirname(os.path.dirname(sys.executable))
        for scripts_dir in ['Scripts', 'bin']:
            candidate = os.path.join(venv_dir, scripts_dir, tool_name)
            for ext in ['', '.exe', '.cmd']:
                if os.path.isfile(candidate + ext):
                    return candidate + ext

        # Fall back to configured path (will fail at runtime with clear error)
        return configured_path

    async def extract_info(self, url):
        """Use yt-dlp to get stream URL, title, and duration.

        Raises RuntimeError on failure.
        """
        proc = await asyncio.create_subprocess_exec(
            self.yt_dlp_path, '-j', '--no-warnings',
            '-f', 'best[height<=480]/best',
            url,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await proc.communicate()

        if proc.returncode != 0:
            err = stderr.decode(errors='replace')[:200]
            raise RuntimeError(f"yt-dlp failed (exit {proc.returncode}): {err}")

        info = json.loads(stdout.decode())
        self.stream_url = info.get('url', '')
        self.title = (info.get('title', 'Unknown') or 'Unknown')[:79]
        self.duration = int(info.get('duration') or 0)

        if not self.stream_url:
            raise RuntimeError("yt-dlp returned no stream URL")

        print(f"[YouTube] Title: {self.title}")
        print(f"[YouTube] Duration: {self.duration}s")
        print(f"[YouTube] Stream URL obtained ({len(self.stream_url)} chars)")

    async def start_video(self):
        """Start ffmpeg to decode video and output raw RGB24 frames."""
        w, h, fps = self.width, self.height, self.fps

        vf = (f'scale={w}:{h}:'
              f'force_original_aspect_ratio=decrease,'
              f'pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=black')

        self._video_proc = await asyncio.create_subprocess_exec(
            self.ffmpeg_path,
            '-i', self.stream_url,
            '-vf', vf,
            '-r', str(fps),
            '-pix_fmt', 'rgb24',
            '-f', 'rawvideo',
            '-loglevel', 'error',
            '-',
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        print(f"[YouTube] ffmpeg started ({w}x{h} @ {fps} FPS)")

    async def read_video_frame(self):
        """Read one raw RGB24 frame from ffmpeg stdout.

        Returns bytes of width*height*3 RGB data, or None on EOF.
        """
        if self._video_proc is None:
            return None

        try:
            data = await self._video_proc.stdout.readexactly(self._frame_size)
            return data
        except asyncio.IncompleteReadError:
            return None

    def is_running(self):
        """Check if ffmpeg is still running."""
        return (self._video_proc is not None and
                self._video_proc.returncode is None)

    async def stop(self):
        """Kill ffmpeg and clean up."""
        if self._video_proc and self._video_proc.returncode is None:
            try:
                self._video_proc.kill()
                # Timeout: process.wait() can hang on Windows
                await asyncio.wait_for(self._video_proc.wait(), timeout=2.0)
            except (ProcessLookupError, asyncio.TimeoutError):
                pass
            print("[YouTube] ffmpeg stopped")
        self._video_proc = None
