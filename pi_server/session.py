"""Browser session management for RetroSurf.

Each connected DOS client gets a BrowserSession wrapping a Playwright
browser context and page. Sessions persist across client disconnects
for a configurable timeout.
"""

import asyncio
import time

from image_pipeline import ImagePipeline
from interaction_detector import detect_interactive_elements
from complexity_detector import detect_complexity
from content_extractor import extract_content
from native_encoder import (build_native_payload, process_native_image,
                            generate_alt_placeholder,
                            render_glyph_cache, process_bg_tile)


# CSS injected into every page to optimize for low-bandwidth tile streaming.
# Goals: eliminate animations/transitions (instant appearance), disable
# blur/transparency effects, hide unplayable media, reduce visual churn.
_PAGE_CSS = '''
*, *::before, *::after {
    scroll-behavior: auto !important;
    animation-duration: 0s !important;
    animation-delay: 0s !important;
    transition-duration: 0s !important;
    transition-delay: 0s !important;
    backdrop-filter: none !important;
    -webkit-backdrop-filter: none !important;
}
/* Hide video/audio - can't play on DOS */
video, audio,
iframe[src*="youtube"], iframe[src*="youtu.be"],
iframe[src*="vimeo"], iframe[src*="dailymotion"] {
    visibility: hidden !important;
    height: 0 !important;
    min-height: 0 !important;
    overflow: hidden !important;
}
/* Disable heavy visual effects that create lots of dirty tiles */
*::-webkit-scrollbar { display: none !important; }
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
        self._url_changed = False

        # Native rendering mode
        self.render_mode = 'screenshot'  # 'screenshot' or 'native'
        self.native_link_table = []  # [(link_id, href_url), ...]
        self._native_variant_map = {}  # (size, family, bold) -> variant_id
        self._pending_bg_tiles = []  # bg_image nodes for lazy loading

    async def configure_viewport(self, width, height, color_depth, tile_size):
        """Set up the browser context and page at the requested viewport size."""
        self.viewport_width = width
        self.viewport_height = height
        self.color_depth = color_depth
        self.tile_size = tile_size

        self.context = await self.browser.new_context(
            viewport={'width': width, 'height': height},
            device_scale_factor=1,
            reduced_motion='reduce',
            color_scheme='light',
            user_agent=(
                'Mozilla/5.0 (Windows NT 10.0; Win64; x64) '
                'AppleWebKit/537.36 (KHTML, like Gecko) '
                'Chrome/131.0.0.0 Safari/537.36'
            ),
        )
        self.page = await self.context.new_page()

        # Stealth: remove automation indicators before any page JS runs
        await self.page.add_init_script('''
            // Remove webdriver flag (the #1 bot detection signal)
            Object.defineProperty(navigator, 'webdriver', {
                get: () => undefined
            });

            // Fake plugins array (real Chrome always has plugins)
            Object.defineProperty(navigator, 'plugins', {
                get: () => [
                    { name: 'Chrome PDF Plugin', filename: 'internal-pdf-viewer' },
                    { name: 'Chrome PDF Viewer', filename: 'mhjfbmdgcfjbbpaeojofohoefgiehjai' },
                    { name: 'Native Client', filename: 'internal-nacl-plugin' },
                ]
            });

            // Fake languages (headless often has empty array)
            Object.defineProperty(navigator, 'languages', {
                get: () => ['en-US', 'en']
            });

            // Add window.chrome object (missing in headless)
            if (!window.chrome) {
                window.chrome = { runtime: {} };
            }

            // Fix permissions query (headless returns different results)
            const origQuery = window.navigator.permissions?.query;
            if (origQuery) {
                window.navigator.permissions.query = (params) =>
                    params.name === 'notifications'
                        ? Promise.resolve({ state: Notification.permission })
                        : origQuery.call(window.navigator.permissions, params);
            }
        ''')

        # Inject page CSS (kill animations, transitions, effects)
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
        # Track URL changes for YouTube detection in push_loop
        try:
            new_url = self.page.url if self.page else ''
            if new_url and new_url != self.current_url:
                self.current_url = new_url
                self._url_changed = True
        except Exception:
            pass

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
        if self.pipeline:
            self.pipeline.prev_indexed = None

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
        if self.pipeline:
            self.pipeline.prev_indexed = None
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
        if self.pipeline:
            self.pipeline.prev_indexed = None
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
        if self.pipeline:
            self.pipeline.prev_indexed = None
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

    async def capture_and_prepare(self):
        """Capture screenshot + dither/quantize + detect changed tiles.

        Separated from tile compression so the server can compress and
        send tiles in progressive batches, interleaving CPU work with
        network transfer. The DOS client sees tiles start arriving
        ~110ms after capture instead of ~300ms.

        Returns:
            (indexed, changed_indices, scroll_dy) or None if failed.
            - indexed: (H, W) uint8 palette index image
            - changed_indices: list of tile indices that changed
            - scroll_dy: vertical scroll delta in pixels
        """
        self._dirty = False
        self.last_activity = time.time()

        t0 = time.perf_counter()

        # Track scroll position
        scroll_dy = 0
        try:
            current_sy = await self.page.evaluate('window.scrollY')
            scroll_dy = int(current_sy - self._last_scroll_y)
            self._last_scroll_y = current_sy
        except Exception:
            pass

        t1 = time.perf_counter()

        # Capture screenshot
        try:
            screenshot_bytes = await self.page.screenshot(
                type='jpeg',
                quality=self.config.get('screenshot_quality', 70),
                caret='initial',
            )
        except Exception as e:
            print(f"[Session {self.session_id}] Screenshot failed: {e}")
            return None

        t2 = time.perf_counter()

        # Prepare indexed image and find changed tiles (no compression yet)
        indexed, changed_indices = self.pipeline.prepare_indexed(
            screenshot_bytes, scroll_dy=scroll_dy
        )

        t3 = time.perf_counter()

        if changed_indices:
            print(f"[Timing] scrollY={(t1-t0)*1000:.1f}ms  screenshot={(t2-t1)*1000:.1f}ms  "
                  f"dither+lut+detect={(t3-t2)*1000:.1f}ms  "
                  f"total={(t3-t0)*1000:.1f}ms  changed={len(changed_indices)} tiles")

        return indexed, changed_indices, scroll_dy

    async def update_page_info(self):
        """Update URL and title from the page. Call after tiles are sent.

        This can block 300-400ms during page loads (page.title() waits
        for the page to be ready), so it must NOT be on the critical
        path before tile streaming.
        """
        try:
            self.current_url = self.page.url
            self.page_title = await self.page.title()
        except Exception:
            pass

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
        """Scroll the page by a tile-aligned amount.

        Uses JavaScript scrollTo() with position snapped to tile boundary
        instead of mouse.wheel(), which can produce non-tile-aligned
        scroll positions due to Chrome's scroll processing.

        Args:
            direction: 0=down, 1=up
            amount: number of "lines" to scroll (each line = 48px = 3 tiles)
        """
        self.last_activity = time.time()

        pixels = amount * 48  # 48px = 3 × 16px tiles
        if direction == 1:
            pixels = -pixels

        await self.inject_scroll_pixels(pixels)

    async def inject_scroll_pixels(self, pixels):
        """Scroll by an exact pixel amount, snapping to tile boundary.

        Args:
            pixels: signed pixel delta (positive = scroll down)
        """
        self.last_activity = time.time()

        try:
            await self.page.evaluate(f'''(() => {{
                const target = window.scrollY + ({pixels});
                const snapped = Math.round(target / 16) * 16;
                const maxScroll = document.documentElement.scrollHeight - window.innerHeight;
                window.scrollTo(0, Math.max(0, Math.min(snapped, maxScroll)));
            }})()''')
        except Exception:
            try:
                await self.page.evaluate(f'window.scrollBy(0, {pixels})')
            except Exception:
                pass

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

    async def check_complexity(self):
        """Check page complexity and decide rendering mode.

        Returns:
            dict with score, recommend_native, reasons
        """
        result = await detect_complexity(self.page)
        print(f"[Session {self.session_id}] Complexity: score={result['score']}, "
              f"native={result['recommend_native']}, "
              f"reasons={result['reasons']}")
        return result

    async def generate_glyph_cache(self, font_variants):
        """Generate glyph cache for the given font variants.

        Args:
            font_variants: set of (size_px, family, bold, italic) tuples

        Returns:
            glyph_cache_payload bytes for MSG_GLYPH_CACHE
        """
        import time
        t0 = time.perf_counter()

        variants_list, variant_map = render_glyph_cache(font_variants)
        self._native_variant_map = variant_map

        from protocol import encode_glyph_cache
        payload = encode_glyph_cache(variants_list)

        t1 = time.perf_counter()
        print(f"[Session {self.session_id}] Glyph cache: "
              f"{len(variants_list)} variants, {len(payload)} bytes, "
              f"{(t1-t0)*1000:.1f}ms")

        return payload

    async def extract_native_content(self):
        """Extract content structure and build command stream payload.

        Background tiles are processed inline (small images, fast download).
        Regular images are deferred for lazy loading.

        Returns:
            tuple of (glyph_payload, content_payload)
        """
        # Reset image pixel budget for new page
        self._image_pixels_used = 0

        content = await extract_content(self.page, self.viewport_width)

        # Store page bg color as RGB for transparent image compositing
        from native_encoder import palette_index_to_rgb
        self._native_bg_rgb = palette_index_to_rgb(
            content.get('bg_color', 215))

        font_variants = content.get('font_variants', set())

        # Generate glyph cache first (needed for variant_map)
        glyph_payload = await self.generate_glyph_cache(font_variants)

        # Process background tiles inline — they're small and critical
        # for visual appearance.  Convert bg_image nodes to bg_tile nodes
        # with embedded tile data.
        bg_tiles_processed = 0
        bg_images_found = 0
        nodes = content.get('nodes', [])
        for i, node in enumerate(nodes):
            if node.get('type') != 'bg_image':
                continue
            bg_images_found += 1
            if bg_tiles_processed >= 8:  # cap
                break
            try:
                repeat = node.get('repeat', 'repeat')
                bg_size = node.get('bg_size', 'auto')
                src = node.get('src', '')
                area_w = node.get('w', 0)
                area_h = node.get('h', 0)
                print(f"[Session {self.session_id}] BG image: "
                      f"repeat={repeat}, size={bg_size}, "
                      f"area={area_w}x{area_h}, src={src[:60]}...")

                # Determine target dimensions based on background-size
                target_w = 0  # 0 = use natural size
                target_h = 0
                if bg_size in ('cover', 'contain', '100%',
                               '100% 100%', '100% auto', 'auto 100%'):
                    # Image should fill the element — render once
                    target_w = min(area_w, 640)
                    target_h = min(area_h, 800)
                    repeat = 'no-repeat'  # override — don't tile
                elif bg_size != 'auto' and bg_size != 'auto auto':
                    # Explicit size like "200px 100px" or "50% auto"
                    parts = bg_size.replace(',', ' ').split()
                    try:
                        pw = parts[0] if parts else 'auto'
                        ph = parts[1] if len(parts) > 1 else 'auto'
                        if pw.endswith('px'):
                            target_w = int(float(pw[:-2]))
                        elif pw.endswith('%'):
                            target_w = int(area_w * float(pw[:-1]) / 100)
                        if ph.endswith('px'):
                            target_h = int(float(ph[:-2]))
                        elif ph.endswith('%'):
                            target_h = int(area_h * float(ph[:-1]) / 100)
                    except (ValueError, IndexError):
                        pass

                # Choose max_tile based on whether this tiles or not
                if repeat == 'no-repeat':
                    # Single image: allow up to element size
                    max_t = max(target_w, target_h, 300)
                    max_t = min(max_t, 600)
                else:
                    # True tiling pattern: use natural size, cap at 128
                    max_t = 128

                result = await process_bg_tile(
                    self.page, src, max_tile=max_t,
                    bg_rgb=self._native_bg_rgb)
                if result:
                    tile_w, tile_h, rle_data = result
                    # Replace bg_image with bg_tile (has embedded data)
                    # Use possibly-overridden repeat (cover/contain → no-repeat)
                    nodes[i] = {
                        'type': 'bg_tile',
                        'x': node.get('x', 0),
                        'y': node.get('y', 0),
                        'w': node.get('w', 0),
                        'h': node.get('h', 0),
                        'tile_w': tile_w,
                        'tile_h': tile_h,
                        'repeat': repeat,
                        'rle_data': rle_data,
                    }
                    bg_tiles_processed += 1
            except Exception as e:
                print(f"[Session {self.session_id}] BG tile failed: {e}")

        # Build command stream payload with variant map
        payload, link_table = build_native_payload(
            content, self._native_variant_map, self.viewport_width
        )
        self.native_link_table = link_table

        # Store image nodes for lazy loading
        self._pending_native_images = [
            node for node in nodes
            if node.get('type') == 'image'
        ]

        print(f"[Session {self.session_id}] Native content: "
              f"{len(payload)} bytes, {len(link_table)} links, "
              f"{len(self._pending_native_images)} images, "
              f"{bg_tiles_processed} bg tiles")

        return glyph_payload, payload

    # Image pool budget — must match NATIVE_IMAGE_POOL in native.h
    _IMAGE_POOL_BUDGET = 512 * 1024  # 512KB

    async def process_next_native_image(self):
        """Process and return the next pending native image.

        Tracks cumulative pixel usage to avoid overflowing the DOS
        image pool. Reduces image dimensions as budget gets consumed.

        Returns:
            (image_id, payload_bytes) or None if no more images.
        """
        if not hasattr(self, '_image_pixels_used'):
            self._image_pixels_used = 0

        while self._pending_native_images:
            node = self._pending_native_images.pop(0)

            remaining = self._IMAGE_POOL_BUDGET - self._image_pixels_used
            if remaining < 1000:
                # Pool nearly full, skip remaining images
                print(f"[Session {self.session_id}] "
                      f"Image pool full, skipping remaining images")
                self._pending_native_images.clear()
                return None

            # Scale max_pixels based on remaining budget
            # Reserve at least 20% for future images
            pending = len(self._pending_native_images) + 1
            per_image_budget = min(120000, remaining // max(1, pending))
            per_image_budget = max(4000, per_image_budget)  # min 4K pixels

            try:
                result = await process_native_image(
                    self.page,
                    node['src'],
                    node['image_id'],
                    display_width=node.get('width', 0),
                    display_height=node.get('height', 0),
                    max_width=self.viewport_width - 16,
                    max_height=800,
                    max_pixels=per_image_budget,
                    bg_rgb=getattr(self, '_native_bg_rgb',
                                   (255, 255, 255)),
                )
                if result:
                    img_id, w, h, rle_data = result
                    self._image_pixels_used += w * h
                    from protocol import encode_native_image
                    img_payload = encode_native_image(
                        img_id, w, h, rle_data)
                    return (img_id, img_payload)
                else:
                    # Image download failed — generate placeholder
                    alt = node.get('alt', '')
                    result = generate_alt_placeholder(
                        alt,
                        node.get('width', 100),
                        node.get('height', 20),
                        node['image_id'])
                    if result:
                        img_id, w, h, rle_data = result
                        self._image_pixels_used += w * h
                        from protocol import encode_native_image
                        img_payload = encode_native_image(
                            img_id, w, h, rle_data)
                        return (img_id, img_payload)
            except Exception as e:
                print(f"[Session {self.session_id}] "
                      f"Image {node.get('image_id')} failed: {e}")
        return None

    def has_pending_native_images(self):
        """Check if there are still images to process."""
        return bool(getattr(self, '_pending_native_images', None))

    async def process_next_bg_tile(self):
        """Process and return the next pending background tile.

        Returns:
            (bg_tile_cmd_bytes,) or None if no more tiles.
            The cmd_bytes is a complete CMD_BG_TILE command to insert
            into the content stream (or send as a supplementary message).
        """
        while getattr(self, '_pending_bg_tiles', None):
            node = self._pending_bg_tiles.pop(0)
            try:
                result = await process_bg_tile(
                    self.page,
                    node['src'],
                    max_tile=64,
                    bg_rgb=getattr(self, '_native_bg_rgb',
                                   (255, 255, 255)),
                )
                if result:
                    tile_w, tile_h, rle_data = result
                    import struct
                    # Build CMD_BG_TILE command
                    cmd = bytearray()
                    cmd.append(0x06)  # CMD_BG_TILE
                    cmd.extend(struct.pack('<HHHHHH',
                        node.get('x', 0),
                        node.get('y', 0),
                        node.get('w', 0),
                        node.get('h', 0),
                        tile_w, tile_h))
                    cmd.extend(struct.pack('<H', len(rle_data)))
                    cmd.extend(rle_data)
                    return bytes(cmd)
            except Exception as e:
                print(f"[Session {self.session_id}] "
                      f"BG tile failed: {e}")
        return None

    def has_pending_bg_tiles(self):
        """Check if there are still bg tiles to process."""
        return bool(getattr(self, '_pending_bg_tiles', None))

    async def close(self):
        """Clean up browser context."""
        try:
            if self.context:
                await self.context.close()
        except Exception:
            pass
        self.context = None
        self.page = None
