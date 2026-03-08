"""DOM content extraction for RetroSurf native rendering mode.

Walks the DOM tree depth-first and extracts pre-positioned draw commands
using the Range API for exact per-line text coordinates. All coordinates
are in document space (viewport + scrollY). The DOS client renders these
as a flat display list — no word-wrap or layout needed.
"""

# JavaScript that walks the DOM and extracts positioned commands
_EXTRACT_JS = '''() => {
    const commands = [];
    let linkId = 0;
    let imageId = 0;

    // Get page background color
    const bodyStyle = window.getComputedStyle(document.body);
    const htmlStyle = window.getComputedStyle(document.documentElement);
    const bgColor = bodyStyle.backgroundColor !== 'rgba(0, 0, 0, 0)'
        ? bodyStyle.backgroundColor
        : (htmlStyle.backgroundColor !== 'rgba(0, 0, 0, 0)'
            ? htmlStyle.backgroundColor : 'rgb(255, 255, 255)');

    const scrollY = window.scrollY;

    // Tags to skip entirely
    const SKIP_TAGS = new Set([
        'SCRIPT', 'STYLE', 'NOSCRIPT', 'SVG', 'CANVAS',
        'VIDEO', 'AUDIO', 'IFRAME', 'TEMPLATE', 'HEAD',
        'META', 'LINK', 'TITLE', 'BASE'
    ]);

    function isVisible(el) {
        const style = window.getComputedStyle(el);
        return style.display !== 'none' &&
               style.visibility !== 'hidden' &&
               style.opacity !== '0';
    }

    function getTextStyle(el) {
        const style = window.getComputedStyle(el);
        return {
            color: style.color,
            fontSize: parseFloat(style.fontSize),
            bold: parseInt(style.fontWeight) >= 700 ||
                  style.fontWeight === 'bold',
            italic: style.fontStyle === 'italic' ||
                    style.fontStyle === 'oblique',
            underline: style.textDecorationLine.includes('underline'),
        };
    }

    function emitLine(lineStart, lineEnd, textNode, text, style) {
        const lineText = text.substring(lineStart, lineEnd)
                             .replace(/\\s+/g, ' ').trim();
        if (!lineText) return;

        const range = document.createRange();
        range.setStart(textNode, lineStart);
        range.setEnd(textNode, lineEnd);
        const rect = range.getBoundingClientRect();
        if (rect.width === 0 && rect.height === 0) return;

        commands.push({
            type: 'text',
            x: Math.round(rect.left),
            y: Math.round(rect.top + scrollY),
            text: lineText,
            color: style.color,
            fontSize: style.fontSize,
            bold: style.bold,
            italic: style.italic,
            underline: style.underline,
        });
    }

    function extractTextLines(textNode) {
        const fullText = textNode.textContent;
        if (!fullText || !fullText.trim()) return;

        // Performance guard: 5000 char limit
        const len = Math.min(fullText.length, 5000);

        const parent = textNode.parentElement;
        if (!parent) return;

        const style = getTextStyle(parent);
        const range = document.createRange();

        // Fast path: check if single line
        range.setStart(textNode, 0);
        range.setEnd(textNode, len);
        const rects = range.getClientRects();

        if (rects.length <= 1) {
            const trimmed = fullText.substring(0, len)
                                    .replace(/\\s+/g, ' ').trim();
            if (trimmed && rects.length === 1) {
                commands.push({
                    type: 'text',
                    x: Math.round(rects[0].left),
                    y: Math.round(rects[0].top + scrollY),
                    text: trimmed,
                    color: style.color,
                    fontSize: style.fontSize,
                    bold: style.bold,
                    italic: style.italic,
                    underline: style.underline,
                });
            }
            return;
        }

        // Multi-line: iterate characters to find line breaks
        let lineStart = 0;
        let prevTop = null;

        for (let i = 0; i < len; i++) {
            range.setStart(textNode, i);
            range.setEnd(textNode, i + 1);
            const r = range.getBoundingClientRect();

            if (r.height === 0 || r.width === 0) continue;

            if (prevTop === null) {
                prevTop = r.top;
                lineStart = i;
                continue;
            }

            if (Math.abs(r.top - prevTop) > 2) {
                // Line break detected — emit previous line
                emitLine(lineStart, i, textNode, fullText, style);
                lineStart = i;
                prevTop = r.top;
            }
        }

        // Emit last line
        emitLine(lineStart, len, textNode, fullText, style);
    }

    function walkNode(node) {
        if (node.nodeType === Node.TEXT_NODE) {
            extractTextLines(node);
            return;
        }

        if (node.nodeType !== Node.ELEMENT_NODE) return;

        const tag = node.tagName;
        if (SKIP_TAGS.has(tag)) return;
        if (!isVisible(node)) return;

        if (tag === 'IMG') {
            const src = node.src;
            if (!src) return;
            const rect = node.getBoundingClientRect();
            const w = Math.round(rect.width);
            const h = Math.round(rect.height);
            if (w > 0 && h > 0) {
                commands.push({
                    type: 'image',
                    x: Math.round(rect.left),
                    y: Math.round(rect.top + scrollY),
                    width: w,
                    height: h,
                    src: src,
                    image_id: imageId++,
                });
            }
            return;
        }

        if (tag === 'HR') {
            const rect = node.getBoundingClientRect();
            const hrStyle = window.getComputedStyle(node);
            if (rect.width > 0) {
                commands.push({
                    type: 'rect',
                    x: Math.round(rect.left),
                    y: Math.round(rect.top + scrollY),
                    width: Math.round(rect.width),
                    height: Math.max(1, Math.round(rect.height)),
                    color: hrStyle.borderTopColor || hrStyle.color ||
                           'rgb(128,128,128)',
                });
            }
            return;
        }

        if (tag === 'A' && node.href) {
            // Emit link_rect commands for the clickable area
            const id = linkId++;
            const rects = node.getClientRects();
            for (let i = 0; i < rects.length; i++) {
                const r = rects[i];
                if (r.width > 0 && r.height > 0) {
                    commands.push({
                        type: 'link_rect',
                        link_id: id,
                        x: Math.round(r.left),
                        y: Math.round(r.top + scrollY),
                        width: Math.round(r.width),
                        height: Math.round(r.height),
                        href: node.href,
                    });
                }
            }
            // Recurse into link children for text/image extraction
            for (const child of node.childNodes) {
                walkNode(child);
            }
            return;
        }

        // Recurse into children
        for (const child of node.childNodes) {
            walkNode(child);
        }
    }

    if (document.body) {
        walkNode(document.body);
    }

    return {
        bg_color: bgColor,
        commands: commands,
        link_count: linkId,
        image_count: imageId,
        doc_height: Math.round(
            document.documentElement.scrollHeight),
    };
}'''


def _parse_css_color(color_str):
    """Parse a CSS color string like 'rgb(255, 0, 0)' to (r, g, b).

    Returns (0, 0, 0) if parsing fails.
    """
    if not color_str:
        return (0, 0, 0)

    color_str = color_str.strip()

    # Handle rgb(r, g, b) and rgba(r, g, b, a)
    if color_str.startswith('rgb'):
        try:
            # Extract numbers from rgb(...) or rgba(...)
            inner = color_str.split('(')[1].rstrip(')')
            parts = [p.strip() for p in inner.split(',')]
            r = int(float(parts[0]))
            g = int(float(parts[1]))
            b = int(float(parts[2]))
            return (max(0, min(255, r)), max(0, min(255, g)),
                    max(0, min(255, b)))
        except (IndexError, ValueError):
            return (0, 0, 0)

    # Handle hex colors
    if color_str.startswith('#'):
        hex_str = color_str[1:]
        if len(hex_str) == 3:
            hex_str = hex_str[0]*2 + hex_str[1]*2 + hex_str[2]*2
        if len(hex_str) >= 6:
            try:
                r = int(hex_str[0:2], 16)
                g = int(hex_str[2:4], 16)
                b = int(hex_str[4:6], 16)
                return (r, g, b)
            except ValueError:
                pass

    return (0, 0, 0)


def _rgb_to_palette_index(r, g, b):
    """Convert RGB color to 6x6x6 palette index.

    Uses the same mapping as the server palette:
    index = R*36 + G*6 + B where R,G,B are 0-5
    """
    ri = min(5, (r + 25) // 51)
    gi = min(5, (g + 25) // 51)
    bi = min(5, (b + 25) // 51)
    return ri * 36 + gi * 6 + bi


def _css_color_to_palette(color_str):
    """Convert a CSS color string to a palette index."""
    r, g, b = _parse_css_color(color_str)
    return _rgb_to_palette_index(r, g, b)


def _font_size_to_id(px):
    """Map CSS font size in pixels to font ID.

    FONT_SMALL(0) = 8x8, FONT_MEDIUM(1) = 8x14, FONT_LARGE(2) = 8x16.
    """
    if px <= 10:
        return 0  # FONT_SMALL
    elif px <= 17:
        return 1  # FONT_MEDIUM
    else:
        return 2  # FONT_LARGE


async def extract_content(page, viewport_width=640):
    """Extract page content as pre-positioned draw commands.

    Args:
        page: Playwright page object
        viewport_width: viewport width in pixels (for image clamping)

    Returns:
        dict with keys:
            bg_color (int): palette index for page background
            nodes (list): positioned draw commands
            link_count (int): number of links
            image_count (int): number of images
            doc_height (int): total document height in pixels
    """
    try:
        raw = await page.evaluate(_EXTRACT_JS)
    except Exception as e:
        print(f"[ContentExtractor] JS extraction failed: {e}")
        return {
            'bg_color': 215,  # white
            'nodes': [],
            'link_count': 0,
            'image_count': 0,
            'doc_height': 0,
        }

    bg_color = _css_color_to_palette(raw.get('bg_color', 'rgb(255,255,255)'))
    max_w = viewport_width - 16

    processed = []
    for cmd in raw.get('commands', []):
        ctype = cmd.get('type')

        if ctype == 'text':
            text = cmd.get('text', '').strip()
            if not text:
                continue
            flags = 0
            if cmd.get('bold'):
                flags |= 0x01
            if cmd.get('italic'):
                flags |= 0x02
            if cmd.get('underline'):
                flags |= 0x04
            processed.append({
                'type': 'text',
                'x': min(65535, max(0, cmd.get('x', 0))),
                'y': min(65535, max(0, cmd.get('y', 0))),
                'text': text,
                'color': _css_color_to_palette(
                    cmd.get('color', 'rgb(0,0,0)')),
                'font': _font_size_to_id(cmd.get('fontSize', 14)),
                'flags': flags,
            })

        elif ctype == 'link_rect':
            processed.append({
                'type': 'link_rect',
                'link_id': cmd.get('link_id', 0),
                'x': min(65535, max(0, cmd.get('x', 0))),
                'y': min(65535, max(0, cmd.get('y', 0))),
                'w': min(65535, cmd.get('width', 0)),
                'h': min(65535, cmd.get('height', 0)),
                'href': cmd.get('href', ''),
            })

        elif ctype == 'image':
            w = cmd.get('width', 0)
            h = cmd.get('height', 0)
            if w > max_w and w > 0:
                h = int(h * max_w / w)
                w = max_w
            processed.append({
                'type': 'image',
                'image_id': cmd.get('image_id', 0),
                'x': min(65535, max(0, cmd.get('x', 0))),
                'y': min(65535, max(0, cmd.get('y', 0))),
                'width': w,
                'height': h,
                'src': cmd.get('src', ''),
            })

        elif ctype == 'rect':
            processed.append({
                'type': 'rect',
                'x': min(65535, max(0, cmd.get('x', 0))),
                'y': min(65535, max(0, cmd.get('y', 0))),
                'w': min(65535, cmd.get('width', 0)),
                'h': min(65535, max(1, cmd.get('height', 1))),
                'color': _css_color_to_palette(
                    cmd.get('color', 'rgb(128,128,128)')),
            })

    return {
        'bg_color': bg_color,
        'nodes': processed,  # Keep 'nodes' key for session.py compat
        'link_count': raw.get('link_count', 0),
        'image_count': raw.get('image_count', 0),
        'doc_height': min(65535, raw.get('doc_height', 0)),
    }
