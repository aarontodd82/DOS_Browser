"""Detect interactive elements on a web page via Playwright.

Evaluates JavaScript in the browser context to find all clickable,
editable, and otherwise interactive elements, returning their bounding
boxes, types, and current values.
"""

from protocol import (
    ELEM_LINK, ELEM_BUTTON, ELEM_TEXT_INPUT, ELEM_TEXT_AREA,
    ELEM_PASSWORD, ELEM_CHECKBOX, ELEM_RADIO, ELEM_SELECT,
    ELEM_CONTENTEDITABLE, ELEM_CUSTOM_WIDGET,
)

# JavaScript executed inside the browser to enumerate interactive elements.
# Returns a list of element descriptors with bounding boxes.
_DETECT_JS = '''() => {
    const results = [];
    let nextId = 1;

    const selectors = [
        'a[href]',
        'button',
        'input:not([type="hidden"])',
        'select',
        'textarea',
        '[contenteditable="true"]',
        '[role="button"]',
        '[role="link"]',
        '[role="checkbox"]',
        '[role="radio"]',
        '[role="textbox"]',
        '[role="combobox"]',
        '[role="tab"]',
        '[tabindex]:not([tabindex="-1"])',
    ];

    const allElements = document.querySelectorAll(selectors.join(','));
    const seen = new Set();

    for (const el of allElements) {
        if (seen.has(el)) continue;
        seen.add(el);

        const rect = el.getBoundingClientRect();
        if (rect.width < 2 || rect.height < 2) continue;

        const style = window.getComputedStyle(el);
        if (style.display === 'none' || style.visibility === 'hidden') continue;
        if (parseFloat(style.opacity) < 0.1) continue;

        // Determine element type
        let elemType = 0x09; // CUSTOM_WIDGET
        const tag = el.tagName.toLowerCase();
        const type = (el.type || '').toLowerCase();

        if (tag === 'a') elemType = 0x00;
        else if (tag === 'button' || type === 'button' || type === 'submit')
            elemType = 0x01;
        else if (tag === 'input' && ['text','search','email','url','tel','number',''].includes(type))
            elemType = 0x02;
        else if (tag === 'textarea')
            elemType = 0x03;
        else if (tag === 'input' && type === 'password')
            elemType = 0x04;
        else if (tag === 'input' && type === 'checkbox')
            elemType = 0x05;
        else if (tag === 'input' && type === 'radio')
            elemType = 0x06;
        else if (tag === 'select')
            elemType = 0x07;
        else if (el.isContentEditable)
            elemType = 0x08;

        // Font size bucket
        const fontSize = parseFloat(style.fontSize);
        let fontBucket = 1;
        if (fontSize <= 10) fontBucket = 0;
        else if (fontSize >= 18) fontBucket = 2;

        // Flags
        let flags = 0;
        if (document.activeElement === el) flags |= 0x01;  // focused
        if (el.disabled) flags |= 0x02;                     // disabled
        if (el.checked) flags |= 0x04;                      // checked
        if (type === 'password') flags |= 0x08;              // password

        // Assign tracking attribute
        const id = nextId++;
        el.setAttribute('data-retrosurf-id', id);

        // Get value (truncated)
        let value = '';
        if (el.value !== undefined && el.value !== null) {
            value = String(el.value);
        } else if (el.innerText) {
            value = el.innerText;
        }
        value = value.substring(0, 500);

        // Parse color to approximate palette index
        // (simplified - just pass the raw CSS color string for now,
        //  the server will map it to palette indices)
        const color = style.color;
        const bgColor = style.backgroundColor;

        results.push({
            id: id,
            x: Math.round(rect.x),
            y: Math.round(rect.y),
            w: Math.round(rect.width),
            h: Math.round(rect.height),
            type: elemType,
            flags: flags,
            fontBucket: fontBucket,
            value: value,
            color: color,
            bgColor: bgColor,
        });
    }
    return results;
}'''


def _css_color_to_palette_index(css_color, default=248):
    """Rough mapping of CSS color string to nearest palette index.

    This is a simplified mapping. For production, we'd parse the rgb()
    values and look up the nearest palette entry via the LUT. For now,
    map common cases and default to dark text.
    """
    if not css_color:
        return default

    s = css_color.strip().lower()

    # Quick map for very common values
    if 'rgba(0, 0, 0' in s or s == 'rgb(0, 0, 0)':
        return 0  # black (palette index 0 in 6x6x6 cube)
    if 'rgb(255, 255, 255)' in s:
        return 215  # white (5,5,5 in 6x6x6 = index 5*36 + 5*6 + 5 = 215)
    if 'rgba(0, 0, 0, 0)' in s or s == 'transparent':
        return 249  # white/transparent background

    # Try to parse rgb(r, g, b)
    try:
        if s.startswith('rgb'):
            nums = s.replace('rgba(', '').replace('rgb(', '').replace(')', '')
            parts = [int(float(x.strip())) for x in nums.split(',')[:3]]
            r, g, b = parts[0], parts[1], parts[2]
            # Map to 6x6x6 cube index
            ri = min(r * 6 // 256, 5)
            gi = min(g * 6 // 256, 5)
            bi = min(b * 6 // 256, 5)
            return ri * 36 + gi * 6 + bi
    except (ValueError, IndexError):
        pass

    return default


async def detect_interactive_elements(page):
    """Detect all interactive elements on the current page.

    Args:
        page: Playwright page object

    Returns:
        list of element dicts ready for protocol encoding, with keys:
            id, x, y, w, h, type, flags, font_size, text_color, bg_color, value
    """
    try:
        raw_elements = await page.evaluate(_DETECT_JS)
    except Exception:
        return []

    elements = []
    for raw in raw_elements:
        elements.append({
            'id': raw['id'],
            'x': max(0, raw['x']),
            'y': max(0, raw['y']),
            'w': raw['w'],
            'h': raw['h'],
            'type': raw['type'],
            'flags': raw.get('flags', 0),
            'font_size': raw.get('fontBucket', 1),
            'text_color': _css_color_to_palette_index(raw.get('color'), 248),
            'bg_color': _css_color_to_palette_index(raw.get('bgColor'), 249),
            'value': raw.get('value', ''),
        })

    return elements


def detect_interactive_elements_sync(page):
    """Synchronous version for testing."""
    try:
        raw_elements = page.evaluate(_DETECT_JS)
    except Exception:
        return []

    elements = []
    for raw in raw_elements:
        elements.append({
            'id': raw['id'],
            'x': max(0, raw['x']),
            'y': max(0, raw['y']),
            'w': raw['w'],
            'h': raw['h'],
            'type': raw['type'],
            'flags': raw.get('flags', 0),
            'font_size': raw.get('fontBucket', 1),
            'text_color': _css_color_to_palette_index(raw.get('color'), 248),
            'bg_color': _css_color_to_palette_index(raw.get('bgColor'), 249),
            'value': raw.get('value', ''),
        })

    return elements
