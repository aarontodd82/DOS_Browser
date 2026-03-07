"""Server configuration for RetroSurf."""

import json
import os

DEFAULT_CONFIG = {
    'port': 8086,
    'default_url': 'https://www.google.com',
    'session_timeout_seconds': 300,
    'max_sessions': 2,
    'screenshot_quality': 70,
    'frame_check_interval_ms': 50,
    'interaction_scan_interval_ms': 500,
    'status_update_interval_ms': 2000,
    'chromium_args': [
        '--disable-gpu',
        '--no-sandbox',
        '--disable-dev-shm-usage',
        '--disable-extensions',
        '--disable-background-networking',
        '--disable-sync',
        '--disable-translate',
        '--disable-smooth-scrolling',
        '--disable-default-apps',
        '--disable-component-update',
        '--force-color-profile=srgb',
        '--autoplay-policy=user-gesture-required',
        # Anti-bot-detection: remove automation indicators
        '--disable-blink-features=AutomationControlled',
        # Force 1:1 pixel scaling (ignore Windows DPI scaling)
        '--force-device-scale-factor=1',
        '--high-dpi-support=0',
        # Don't throttle when minimized/background
        '--disable-renderer-backgrounding',
        '--disable-backgrounding-occluded-windows',
        '--disable-hang-monitor',
    ],
}


def load_config(path=None):
    """Load configuration from a JSON file, with defaults.

    Args:
        path: path to config.json, or None to use defaults

    Returns:
        dict with all config keys
    """
    config = dict(DEFAULT_CONFIG)

    if path is None:
        path = os.path.join(os.path.dirname(__file__), 'config.json')

    if os.path.exists(path):
        try:
            with open(path, 'r') as f:
                user_config = json.load(f)
            config.update(user_config)
            print(f"Loaded config from {path}")
        except Exception as e:
            print(f"Warning: Could not load {path}: {e}")
            print("Using default configuration.")
    else:
        print("No config.json found, using defaults.")

    return config
