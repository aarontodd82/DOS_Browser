"""Browser session management for RetroSurf.

Each connected DOS client gets a BrowserSession wrapping a Playwright
browser context and page. Sessions persist across client disconnects
for a configurable timeout.
"""

import asyncio
import time

from image_pipeline import ImagePipeline
from interaction_detector import detect_interactive_elements


# CSS injected into every page to make scrolling instant (no smooth scroll)
_PAGE_CSS = '''
*, *::before, *::after {
    scroll-behavior: auto !important;
}
'''

# JavaScript injected to track DOM changes via MutationObserver
_MUTATION_OBSERVER_JS = '''
window.__retrosurf_dirty = true;
window.__retrosurf_scroll_dirty = false;
window.__retrosurf_has_animation = false;
window.__retrosurf_input_focused = false;

const observer = new MutationObserver(() => {
    window.__retrosurf_dirty = true;
});

const startObserving = () => {
    const target = document.body || document.documentElement;
    if (target) {
        observer.observe(target, {
            childList: true, attributes: true,
            characterData: true, subtree: true
        });
    } else {
        document.addEventListener('DOMContentLoaded', () => {
            const t = document.body || document.documentElement;
            if (t) observer.observe(t, {
                childList: true, attributes: true,
                characterData: true, subtree: true
            });
        });
    }
};
startObserving();

// Re-attach observer after navigation (SPA pushState)
const origPushState = history.pushState;
history.pushState = function() {
    origPushState.apply(this, arguments);
    window.__retrosurf_dirty = true;
    window.__retrosurf_has_animation = false;
    window.__retrosurf_input_focused = false;
    setTimeout(startObserving, 100);
};

window.addEventListener('scroll', () => {
    window.__retrosurf_dirty = true;
    window.__retrosurf_scroll_dirty = true;
}, {passive: true, capture: true});

window.addEventListener('resize', () => {
    window.__retrosurf_dirty = true;
});

// Track focus changes (input field focus triggers visual changes)
document.addEventListener('focusin', (e) => {
    window.__retrosurf_dirty = true;
    const tag = e.target.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA' || e.target.isContentEditable) {
        window.__retrosurf_input_focused = true;
    }
}, true);
document.addEventListener('focusout', () => {
    window.__retrosurf_dirty = true;
    window.__retrosurf_input_focused = false;
}, true);

// --- Animation detection ---
// Hook requestAnimationFrame to detect JS-driven animations.
// If rAF is called repeatedly (animation loop pattern), mark dirty.
(function() {
    const origRAF = window.requestAnimationFrame;
    let rafCount = 0;
    let rafWindowStart = Date.now();

    window.requestAnimationFrame = function(callback) {
        rafCount++;
        const now = Date.now();

        // If we see 3+ rAF calls within 1 second, animation is active
        if (now - rafWindowStart > 1000) {
            if (rafCount >= 3) {
                window.__retrosurf_has_animation = true;
            } else {
                window.__retrosurf_has_animation = false;
            }
            rafCount = 0;
            rafWindowStart = now;
        }

        return origRAF.call(window, callback);
    };
})();

// Detect canvas elements (likely animated if present)
// and animated images - check periodically
(function() {
    let checkInterval = null;

    function checkAnimatedContent() {
        // Canvas elements that are being drawn to indicate animation
        const canvases = document.querySelectorAll('canvas');
        if (canvases.length > 0) {
            window.__retrosurf_has_animation = true;
            return;
        }

        // Animated images (GIF, APNG, animated WebP)
        const imgs = document.querySelectorAll('img');
        for (let i = 0; i < imgs.length; i++) {
            const src = (imgs[i].src || '').toLowerCase();
            if (src.endsWith('.gif') || src.includes('.gif?')) {
                window.__retrosurf_has_animation = true;
                return;
            }
        }
    }

    // Check once at load, then every 2 seconds
    if (document.readyState === 'complete' || document.readyState === 'interactive') {
        checkAnimatedContent();
    } else {
        window.addEventListener('DOMContentLoaded', checkAnimatedContent);
    }
    checkInterval = setInterval(checkAnimatedContent, 2000);
})();
'''


# Map DOS scancodes / ASCII to Playwright key names
_KEY_MAP = {
    # Special keys (scancode-based, ascii=0)
    0x48: 'ArrowUp',
    0x50: 'ArrowDown',
    0x4B: 'ArrowLeft',
    0x4D: 'ArrowRight',
    0x47: 'Home',
    0x4F: 'End',
    0x49: 'PageUp',
    0x51: 'PageDown',
    0x52: 'Insert',
    0x53: 'Delete',
    0x3B: 'F1',
    0x3C: 'F2',
    0x3D: 'F3',
    0x3E: 'F4',
    0x3F: 'F5',
    0x40: 'F6',
    0x41: 'F7',
    0x42: 'F8',
    0x43: 'F9',
    0x44: 'F10',
    0x57: 'F11',
    0x58: 'F12',
    0x0F: 'Tab',
}


def dos_scancode_to_key(scancode, ascii_val, modifiers):
    """Map a DOS scancode + ASCII value to a Playwright key name.

    Args:
        scancode: DOS keyboard scancode
        ascii_val: ASCII value (0 for special keys)
        modifiers: bit 0=shift, 1=ctrl, 2=alt

    Returns:
        string key name for Playwright keyboard API
    """
    # Special keys (ascii == 0 or 0xE0)
    if ascii_val == 0:
        key = _KEY_MAP.get(scancode, '')
        if not key:
            return None
        return key

    # Standard ASCII keys
    if ascii_val == 8:
        return 'Backspace'
    if ascii_val == 9:
        return 'Tab'
    if ascii_val == 13:
        return 'Enter'
    if ascii_val == 27:
        return 'Escape'

    # Ctrl+key combos
    if modifiers & 0x02:  # ctrl
        if 1 <= ascii_val <= 26:
            return f'Control+{chr(ascii_val + 96)}'

    # Printable ASCII
    if 32 <= ascii_val <= 126:
        return chr(ascii_val)

    return None


class BrowserSession:
    """Manages a single browser session for a DOS client."""

    def __init__(self, browser, session_id, config):
        self.browser = browser
        self.session_id = session_id
        self.config = config
        self.context = None
        self.page = None
        self.pipeline = None
        self.viewport_width = 640
        self.viewport_height = 456
        self.color_depth = 8
        self.tile_size = 16
        self.created_at = time.time()
        self.last_activity = time.time()
        self.current_url = ''
        self.page_title = ''
        self.is_loading = False

        # Change tracking
        self._dirty = True
        self._interaction_dirty = True
        self._last_scroll_y = 0
        self._nav_burst_until = 0

    async def configure_viewport(self, width, height, color_depth, tile_size):
        """Set up the browser context and page at the requested viewport size."""
        self.viewport_width = width
        self.viewport_height = height
        self.color_depth = color_depth
        self.tile_size = tile_size

        self.context = await self.browser.new_context(
            viewport={'width': width, 'height': height},
            device_scale_factor=1,
        )
        self.page = await self.context.new_page()

        # Inject page CSS (disable smooth scrolling)
        await self.page.add_init_script(f'''
            const style = document.createElement('style');
            style.textContent = `{_PAGE_CSS}`;
            if (document.head) document.head.appendChild(style);
            else document.addEventListener('DOMContentLoaded', () => {{
                document.head.appendChild(style);
            }});
        ''')

        # Inject MutationObserver
        await self.page.add_init_script(_MUTATION_OBSERVER_JS)

        # Listen for page events
        self.page.on('load', self._on_load)
        self.page.on('domcontentloaded', self._on_domcontentloaded)

        # Initialize image pipeline
        self.pipeline = ImagePipeline(width, height, color_depth, tile_size)

    def _on_load(self, _=None):
        self._dirty = True
        self._interaction_dirty = True
        self.is_loading = False

    def _on_domcontentloaded(self, _=None):
        self._dirty = True
        self._interaction_dirty = True

    async def navigate(self, url):
        """Navigate to a URL."""
        self.is_loading = True
        self._dirty = True
        self._interaction_dirty = True
        self._last_scroll_y = 0
        self._nav_burst_until = time.time() + 3.0
        self.last_activity = time.time()

        # Add https:// if no scheme provided
        if not url.startswith(('http://', 'https://', 'file://')):
            url = 'https://' + url

        try:
            await self.page.goto(url, wait_until='domcontentloaded', timeout=30000)
        except Exception as e:
            print(f"[Session {self.session_id}] Navigation error: {e}")
            self.is_loading = False

        self.current_url = self.page.url
        try:
            self.page_title = await self.page.title()
        except Exception:
            self.page_title = ''

    async def go_back(self):
        """Navigate back."""
        self.last_activity = time.time()
        self._dirty = True
        self._interaction_dirty = True
        self._last_scroll_y = 0
        self._nav_burst_until = time.time() + 3.0
        try:
            await self.page.go_back(wait_until='domcontentloaded', timeout=15000)
            self.current_url = self.page.url
        except Exception:
            pass

    async def go_forward(self):
        """Navigate forward."""
        self.last_activity = time.time()
        self._dirty = True
        self._interaction_dirty = True
        self._last_scroll_y = 0
        self._nav_burst_until = time.time() + 3.0
        try:
            await self.page.go_forward(wait_until='domcontentloaded', timeout=15000)
            self.current_url = self.page.url
        except Exception:
            pass

    async def reload(self):
        """Reload the current page."""
        self.last_activity = time.time()
        self.is_loading = True
        self._dirty = True
        self._interaction_dirty = True
        self._last_scroll_y = 0
        self._nav_burst_until = time.time() + 3.0
        try:
            await self.page.reload(wait_until='domcontentloaded', timeout=30000)
        except Exception:
            pass
        self.is_loading = False

    async def stop_loading(self):
        """Stop page loading."""
        self.last_activity = time.time()
        try:
            # Playwright doesn't have a direct stop() - navigate to about:blank
            # and then back, or just let it timeout. For now, evaluate stop.
            await self.page.evaluate('window.stop()')
        except Exception:
            pass
        self.is_loading = False

    async def check_dirty(self):
        """Check if the page has visually changed since last check.

        Returns True if we need to capture a new screenshot.
        Checks three sources of visual changes:
          1. Internal dirty flag (set by clicks, navigation, etc.)
          2. MutationObserver (DOM attribute/child/text changes)
          3. Running animations (CSS @keyframes, Web Animations API,
             playing videos) which don't trigger MutationObserver
        """
        if self._dirty:
            return True

        if time.time() < self._nav_burst_until:
            return True

        try:
            is_dirty = await self.page.evaluate('''() => {
                // Check MutationObserver flag
                const domDirty = window.__retrosurf_dirty;
                window.__retrosurf_dirty = false;
                if (domDirty) return true;

                // Check scroll dirty flag (set by scroll event listener)
                const scrollDirty = window.__retrosurf_scroll_dirty;
                window.__retrosurf_scroll_dirty = false;
                if (scrollDirty) return true;

                // Check our rAF hook / canvas / GIF detection flag
                if (window.__retrosurf_has_animation) return true;

                // Check for running CSS/Web animations
                if (document.getAnimations) {
                    const anims = document.getAnimations();
                    for (let i = 0; i < anims.length; i++) {
                        if (anims[i].playState === 'running') return true;
                    }
                }

                // Check for playing video elements
                const videos = document.querySelectorAll('video');
                for (let i = 0; i < videos.length; i++) {
                    if (!videos[i].paused && !videos[i].ended) return true;
                }

                // Text input focused - cursor blink needs periodic updates
                if (window.__retrosurf_input_focused) return true;

                return false;
            }''')
            if is_dirty:
                self._dirty = True
            return is_dirty
        except Exception:
            # Page might be navigating, treat as dirty
            return True

    async def capture_frame(self):
        """Capture a screenshot and process it through the pipeline.

        Returns:
            tuple of (tiles, scroll_dy) where tiles is a list of
            (tile_index, compressed_bytes) for changed tiles, and
            scroll_dy is the vertical scroll delta in pixels.
            Returns ([], 0) if capture failed.
        """
        self._dirty = False
        self.last_activity = time.time()

        # Track scroll position for shift-based delta optimization
        scroll_dy = 0
        try:
            current_sy = await self.page.evaluate('window.scrollY')
            scroll_dy = int(current_sy - self._last_scroll_y)
            self._last_scroll_y = current_sy
        except Exception:
            pass

        try:
            screenshot_bytes = await self.page.screenshot(
                type='jpeg',
                quality=self.config.get('screenshot_quality', 70),
                caret='initial',
            )
        except Exception as e:
            print(f"[Session {self.session_id}] Screenshot failed: {e}")
            return ([], 0)

        # Update URL and title
        try:
            self.current_url = self.page.url
            self.page_title = await self.page.title()
        except Exception:
            pass

        # Process through image pipeline (shift-aware delta)
        tiles = self.pipeline.process_frame(screenshot_bytes, scroll_dy=scroll_dy)
        return (tiles, scroll_dy)

    async def get_interaction_map(self):
        """Detect interactive elements on the current page.

        Returns:
            tuple of (elements, scroll_y, scroll_height)
        """
        self._interaction_dirty = False
        self.last_activity = time.time()

        try:
            elements = await detect_interactive_elements(self.page)
            scroll_info = await self.page.evaluate('''() => ({
                y: Math.round(window.scrollY),
                h: Math.round(document.documentElement.scrollHeight)
            })''')
            scroll_y = scroll_info['y']
            scroll_height = scroll_info['h']
        except Exception:
            elements = []
            scroll_y = 0
            scroll_height = 0

        return elements, scroll_y, scroll_height

    async def inject_mouse_click(self, x, y, button='left'):
        """Click at screen coordinates."""
        self.last_activity = time.time()
        self._dirty = True
        self._interaction_dirty = True
        try:
            await self.page.mouse.click(x, y, button=button)
        except Exception as e:
            print(f"[Session {self.session_id}] Click failed: {e}")

    async def inject_mouse_dblclick(self, x, y):
        """Double-click at screen coordinates."""
        self.last_activity = time.time()
        self._dirty = True
        self._interaction_dirty = True
        try:
            await self.page.mouse.dblclick(x, y)
        except Exception as e:
            print(f"[Session {self.session_id}] Double-click failed: {e}")

    async def inject_mouse_move(self, x, y):
        """Move mouse to screen coordinates (for hover effects)."""
        self.last_activity = time.time()
        try:
            await self.page.mouse.move(x, y)
            # CSS :hover effects don't trigger MutationObserver (no DOM change),
            # so we must mark dirty to re-capture the frame.
            self._dirty = True
        except Exception:
            pass

    async def inject_mouse_down(self, x, y, button='left'):
        """Mouse button down at coordinates."""
        self.last_activity = time.time()
        self._dirty = True
        try:
            await self.page.mouse.move(x, y)
            await self.page.mouse.down(button=button)
        except Exception:
            pass

    async def inject_mouse_up(self, button='left'):
        """Mouse button up."""
        self.last_activity = time.time()
        self._dirty = True
        try:
            await self.page.mouse.up(button=button)
        except Exception:
            pass

    async def inject_key(self, scancode, ascii_val, modifiers, event_type):
        """Inject a keyboard event."""
        self.last_activity = time.time()
        self._dirty = True

        key = dos_scancode_to_key(scancode, ascii_val, modifiers)
        if not key:
            return

        try:
            if event_type == 0:  # press
                if len(key) == 1 and not (modifiers & 0x02):
                    # Single printable character - use type() for proper input
                    await self.page.keyboard.type(key)
                else:
                    await self.page.keyboard.press(key)
            elif event_type == 1:  # release
                # Playwright doesn't have great key-up support for most keys
                # key.up() only works for modifier keys
                pass
        except Exception as e:
            print(f"[Session {self.session_id}] Key inject failed: {e}")

    async def inject_text_input(self, element_id, text):
        """Set text value on a form element by its retrosurf ID."""
        self.last_activity = time.time()
        self._dirty = True

        try:
            await self.page.evaluate('''([id, text]) => {
                const el = document.querySelector(
                    `[data-retrosurf-id="${id}"]`
                );
                if (!el) return;

                if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
                    // Set value and fire events so frameworks detect the change
                    const nativeInputValueSetter = Object.getOwnPropertyDescriptor(
                        window.HTMLInputElement.prototype, 'value'
                    )?.set || Object.getOwnPropertyDescriptor(
                        window.HTMLTextAreaElement.prototype, 'value'
                    )?.set;

                    if (nativeInputValueSetter) {
                        nativeInputValueSetter.call(el, text);
                    } else {
                        el.value = text;
                    }

                    el.dispatchEvent(new Event('input', {bubbles: true}));
                    el.dispatchEvent(new Event('change', {bubbles: true}));
                }
            }''', [str(element_id), text])
        except Exception as e:
            print(f"[Session {self.session_id}] Text input failed: {e}")

    async def inject_scroll(self, direction, amount):
        """Scroll the page.

        Args:
            direction: 0=down, 1=up
            amount: number of "lines" to scroll
        """
        self.last_activity = time.time()

        pixels = amount * 48  # Tile-aligned: 48px = 3 × 16px tiles
        if direction == 1:
            pixels = -pixels

        try:
            await self.page.mouse.wheel(0, pixels)
        except Exception:
            try:
                await self.page.evaluate(f'window.scrollBy(0, {pixels})')
            except Exception:
                pass

        # Set dirty AFTER scroll completes to prevent push loop from
        # capturing a pre-scroll frame during the await above
        self._dirty = True
        self._interaction_dirty = True

    async def get_status_text(self):
        """Build status bar text."""
        parts = []
        if self.is_loading:
            parts.append('Loading...')
        if self.page_title:
            parts.append(self.page_title[:60])
        return ' | '.join(parts) if parts else self.current_url[:80]

    async def close(self):
        """Clean up browser context."""
        try:
            if self.context:
                await self.context.close()
        except Exception:
            pass
        self.context = None
        self.page = None
