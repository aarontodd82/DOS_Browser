"""DOM content extraction for RetroSurf native rendering mode v3.

Walks the DOM tree depth-first and extracts pre-positioned draw commands
using the Range API for exact per-line text coordinates. Also extracts
per-element backgrounds, table borders, list markers, and background images.
All coordinates are in document space (viewport + scrollY).

Commands are sorted by visual layer (painter's algorithm):
  1. bg_rect  - element background colors
  2. bg_image - tiled background images
  3. border   - table cell borders
  4. rect     - HR elements
  5. image    - inline images
  6. text / list_marker - text content
  7. link_rect - invisible hit-test regions
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

    // Tags to check for backgrounds (block + inline with common bg use)
    const BG_TAGS = new Set([
        'DIV', 'TD', 'TH', 'TABLE', 'TR', 'SECTION', 'ARTICLE',
        'HEADER', 'FOOTER', 'NAV', 'ASIDE', 'BLOCKQUOTE', 'PRE',
        'LI', 'P', 'DD', 'DT', 'FIGCAPTION', 'FIGURE', 'MAIN',
        'FORM', 'FIELDSET', 'DETAILS', 'SUMMARY', 'BODY',
        'H1', 'H2', 'H3', 'H4', 'H5', 'H6',
        'SPAN', 'CODE', 'KBD', 'SAMP', 'MARK', 'BUTTON',
        'INPUT', 'SELECT', 'TEXTAREA'
    ]);

    // Tags to check for borders
    const BORDER_TAGS = new Set([
        'TD', 'TH', 'TABLE',
        'DIV', 'FIELDSET', 'BLOCKQUOTE', 'PRE', 'P', 'FORM',
        'BUTTON', 'INPUT', 'SELECT', 'TEXTAREA', 'SPAN'
    ]);

    function isVisible(el) {
        const style = window.getComputedStyle(el);
        return style.display !== 'none' &&
               style.visibility !== 'hidden' &&
               style.opacity !== '0';
    }

    function getFontFamily(style) {
        const ff = style.fontFamily.toLowerCase();
        if (/\\b(courier|mono|consolas|menlo|source.code|lucida.console)\\b/.test(ff))
            return 'mono';
        if (/\\b(times|georgia|palatino|garamond|baskerville|book)\\b/.test(ff) &&
            !/\\bsans/.test(ff))
            return 'serif';
        return 'sans';
    }

    function getTextStyle(el) {
        const style = window.getComputedStyle(el);
        const ws = style.whiteSpace;
        return {
            color: style.color,
            fontSize: parseFloat(style.fontSize),
            fontFamily: getFontFamily(style),
            bold: parseInt(style.fontWeight) >= 700 ||
                  style.fontWeight === 'bold',
            italic: style.fontStyle === 'italic' ||
                    style.fontStyle === 'oblique',
            underline: style.textDecorationLine.includes('underline'),
            strikethrough: style.textDecorationLine.includes('line-through'),
            preserveWS: ws === 'pre' || ws === 'pre-wrap' || ws === 'pre-line',
        };
    }

    function emitLine(lineStart, lineEnd, textNode, text, style) {
        let lineText;
        if (style.preserveWS) {
            // Preserve whitespace for <pre> etc — only strip newlines
            lineText = text.substring(lineStart, lineEnd)
                           .replace(/\\r/g, '').replace(/\\n/g, '')
                           .replace(/\\t/g, '    ');
            // Allow leading spaces but trim trailing
            lineText = lineText.replace(/\\s+$/, '');
        } else {
            lineText = text.substring(lineStart, lineEnd)
                               .replace(/\\s+/g, ' ').trim();
        }
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
            textWidth: Math.round(rect.width),
            color: style.color,
            fontSize: style.fontSize,
            fontFamily: style.fontFamily,
            bold: style.bold,
            italic: style.italic,
            underline: style.underline,
            strikethrough: style.strikethrough,
            preserveWS: style.preserveWS,
        });
    }

    function extractTextLines(textNode) {
        const fullText = textNode.textContent;
        if (!fullText) return;

        // For non-pre content, skip whitespace-only nodes
        const parent = textNode.parentElement;
        if (!parent) return;
        const pStyle = window.getComputedStyle(parent);
        const isPre = pStyle.whiteSpace === 'pre' ||
                      pStyle.whiteSpace === 'pre-wrap' ||
                      pStyle.whiteSpace === 'pre-line';
        if (!isPre && !fullText.trim()) return;

        // Performance guard: 5000 char limit
        const len = Math.min(fullText.length, 5000);

        const style = getTextStyle(parent);
        const range = document.createRange();

        // Fast path: check if single line
        range.setStart(textNode, 0);
        range.setEnd(textNode, len);
        const rects = range.getClientRects();

        if (rects.length <= 1) {
            let trimmed;
            if (style.preserveWS) {
                trimmed = fullText.substring(0, len)
                    .replace(/\\r/g, '').replace(/\\n/g, '')
                    .replace(/\\t/g, '    ').replace(/\\s+$/, '');
            } else {
                trimmed = fullText.substring(0, len)
                    .replace(/\\s+/g, ' ').trim();
            }
            if (trimmed && rects.length === 1) {
                commands.push({
                    type: 'text',
                    x: Math.round(rects[0].left),
                    y: Math.round(rects[0].top + scrollY),
                    text: trimmed,
                    textWidth: Math.round(rects[0].width),
                    color: style.color,
                    fontSize: style.fontSize,
                    fontFamily: style.fontFamily,
                    bold: style.bold,
                    italic: style.italic,
                    underline: style.underline,
                    strikethrough: style.strikethrough,
                    preserveWS: style.preserveWS,
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
                // Line break detected
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

        const style = window.getComputedStyle(node);

        // --- Background color extraction ---
        if (BG_TAGS.has(tag)) {
            const bg = style.backgroundColor;
            if (bg && bg !== 'rgba(0, 0, 0, 0)') {
                const rect = node.getBoundingClientRect();
                if (rect.width > 0 && rect.height > 0) {
                    commands.push({
                        type: 'bg_rect',
                        x: Math.round(rect.left),
                        y: Math.round(rect.top + scrollY),
                        width: Math.round(rect.width),
                        height: Math.round(rect.height),
                        color: bg,
                    });
                }
            }
        }

        // --- Background image extraction ---
        const bgImage = style.backgroundImage;
        if (bgImage && bgImage !== 'none') {
            const match = bgImage.match(/url\\(["']?([^"')]+)["']?\\)/);
            if (match) {
                const rect = node.getBoundingClientRect();
                if (rect.width > 0 && rect.height > 0) {
                    // Normalize repeat (Chrome may return two-value syntax)
                    let bgRepeat = (style.backgroundRepeat || 'repeat').trim();
                    // "repeat repeat" → "repeat", "no-repeat no-repeat" → "no-repeat"
                    if (bgRepeat === 'repeat repeat') bgRepeat = 'repeat';
                    else if (bgRepeat === 'no-repeat no-repeat') bgRepeat = 'no-repeat';
                    else if (bgRepeat === 'repeat no-repeat') bgRepeat = 'repeat-x';
                    else if (bgRepeat === 'no-repeat repeat') bgRepeat = 'repeat-y';

                    // Extract background-size for proper scaling
                    const bgSize = style.backgroundSize || 'auto';

                    commands.push({
                        type: 'bg_image',
                        x: Math.round(rect.left),
                        y: Math.round(rect.top + scrollY),
                        width: Math.round(rect.width),
                        height: Math.round(rect.height),
                        src: match[1],
                        repeat: bgRepeat,
                        bgSize: bgSize,
                    });
                }
            }
        }

        // --- Border extraction ---
        if (BORDER_TAGS.has(tag)) {
            const rect = node.getBoundingClientRect();
            if (rect.width > 0 && rect.height > 0) {
                // For TD/TH, inherit bordercolor from parent TABLE
                // if Chrome didn't propagate it (common with HTML4 attrs)
                let inheritColor = null;
                if ((tag === 'TD' || tag === 'TH') && node.closest) {
                    const tbl = node.closest('table');
                    if (tbl) {
                        const bc = tbl.getAttribute('bordercolor');
                        if (bc) inheritColor = bc;
                    }
                }
                const sides = ['Top', 'Right', 'Bottom', 'Left'];
                for (const side of sides) {
                    const bw = parseFloat(style['border' + side + 'Width']);
                    const bs = style['border' + side + 'Style'];
                    if (bw >= 1 && bs !== 'none' && bs !== 'hidden') {
                        let bcolor = style['border' + side + 'Color'];
                        // Use inherited table bordercolor if cell border
                        // is default gray/black
                        if (inheritColor && (bcolor === 'rgb(0, 0, 0)' ||
                            bcolor === 'rgb(128, 128, 128)' ||
                            bcolor === 'rgb(169, 169, 169)')) {
                            bcolor = inheritColor;
                        }
                        commands.push({
                            type: 'border',
                            side: side.toLowerCase(),
                            rect_x: Math.round(rect.left),
                            rect_y: Math.round(rect.top + scrollY),
                            rect_w: Math.round(rect.width),
                            rect_h: Math.round(rect.height),
                            color: bcolor,
                            bwidth: Math.min(3, Math.round(bw)),
                        });
                    }
                }
            }
        }

        // --- Specific tag handling ---
        if (tag === 'IMG') {
            const src = node.src;
            if (!src) return;
            const rect = node.getBoundingClientRect();
            const w = Math.round(rect.width);
            const h = Math.round(rect.height);
            // Skip spacer GIFs (1x1 or 2x2 invisible layout images)
            if (w <= 2 && h <= 2) return;
            if (w > 0 && h > 0) {
                const imgId = imageId++;
                const alt = node.alt || '';

                // Linked image border (colored border like Netscape)
                const parentA = node.closest('a');
                if (parentA && parentA.href) {
                    const borderW = parseInt(node.getAttribute('border'));
                    const bw = (borderW >= 0 && !isNaN(borderW)) ? borderW : 2;
                    if (bw > 0) {
                        // Use computed link color (blue unvisited, purple visited)
                        const bcolor = window.getComputedStyle(parentA).color;
                        const ix = Math.round(rect.left);
                        const iy = Math.round(rect.top + scrollY);
                        commands.push({type:'border', side:'top',
                            rect_x:ix-bw, rect_y:iy-bw, rect_w:w+bw*2, rect_h:h+bw*2,
                            color:bcolor, bwidth:bw});
                        commands.push({type:'border', side:'bottom',
                            rect_x:ix-bw, rect_y:iy-bw, rect_w:w+bw*2, rect_h:h+bw*2,
                            color:bcolor, bwidth:bw});
                        commands.push({type:'border', side:'left',
                            rect_x:ix-bw, rect_y:iy-bw, rect_w:w+bw*2, rect_h:h+bw*2,
                            color:bcolor, bwidth:bw});
                        commands.push({type:'border', side:'right',
                            rect_x:ix-bw, rect_y:iy-bw, rect_w:w+bw*2, rect_h:h+bw*2,
                            color:bcolor, bwidth:bw});
                    }
                }

                commands.push({
                    type: 'image',
                    x: Math.round(rect.left),
                    y: Math.round(rect.top + scrollY),
                    width: w,
                    height: h,
                    src: src,
                    image_id: imgId,
                    alt: alt,
                });

                // Client-side image map support
                const usemap = node.getAttribute('usemap');
                if (usemap) {
                    const mapName = usemap.replace('#', '');
                    const mapEl = document.querySelector(
                        'map[name="' + mapName + '"]');
                    if (mapEl) {
                        const areas = mapEl.querySelectorAll('area');
                        const imgX = Math.round(rect.left);
                        const imgY = Math.round(rect.top + scrollY);
                        for (const area of areas) {
                            if (!area.href) continue;
                            const shape = (area.getAttribute('shape') || 'rect')
                                              .toLowerCase();
                            const coords = (area.getAttribute('coords') || '')
                                .split(',').map(c => parseInt(c.trim()));
                            let ax, ay, aw, ah;
                            if (shape === 'rect' && coords.length >= 4) {
                                ax = imgX + coords[0];
                                ay = imgY + coords[1];
                                aw = coords[2] - coords[0];
                                ah = coords[3] - coords[1];
                            } else if (shape === 'circle' && coords.length >= 3) {
                                // Approximate circle with bounding rect
                                ax = imgX + coords[0] - coords[2];
                                ay = imgY + coords[1] - coords[2];
                                aw = coords[2] * 2;
                                ah = coords[2] * 2;
                            } else if (shape === 'poly' && coords.length >= 6) {
                                // Bounding rect of polygon
                                let minX = coords[0], maxX = coords[0];
                                let minY = coords[1], maxY = coords[1];
                                for (let ci = 2; ci < coords.length - 1; ci += 2) {
                                    if (coords[ci] < minX) minX = coords[ci];
                                    if (coords[ci] > maxX) maxX = coords[ci];
                                    if (coords[ci+1] < minY) minY = coords[ci+1];
                                    if (coords[ci+1] > maxY) maxY = coords[ci+1];
                                }
                                ax = imgX + minX;
                                ay = imgY + minY;
                                aw = maxX - minX;
                                ah = maxY - minY;
                            } else if (shape === 'default') {
                                ax = imgX; ay = imgY; aw = w; ah = h;
                            } else {
                                continue;
                            }
                            if (aw > 0 && ah > 0) {
                                const id = linkId++;
                                commands.push({
                                    type: 'link_rect',
                                    link_id: id,
                                    x: ax, y: ay,
                                    width: aw, height: ah,
                                    href: area.href,
                                });
                            }
                        }
                    }
                }
            }
            return;
        }

        if (tag === 'HR') {
            const rect = node.getBoundingClientRect();
            if (rect.width > 0) {
                const noshade = node.hasAttribute('noshade');
                const hrColor = node.getAttribute('color');
                const hrX = Math.round(rect.left);
                const hrY = Math.round(rect.top + scrollY);
                const hrW = Math.round(rect.width);

                if (noshade || hrColor) {
                    // Flat solid rule (noshade or colored)
                    const c = hrColor
                        ? hrColor
                        : style.color || 'rgb(128,128,128)';
                    const hrH = Math.max(2, Math.round(rect.height));
                    commands.push({
                        type: 'rect',
                        x: hrX, y: hrY, width: hrW, height: hrH,
                        color: c,
                    });
                } else {
                    // 3D embossed look: dark top line, light bottom line
                    commands.push({
                        type: 'rect',
                        x: hrX, y: hrY, width: hrW, height: 1,
                        color: 'rgb(128,128,128)',
                    });
                    commands.push({
                        type: 'rect',
                        x: hrX, y: hrY + 1, width: hrW, height: 1,
                        color: 'rgb(255,255,255)',
                    });
                }
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

        // --- Form element extraction ---
        if (tag === 'INPUT') {
            const inputType = (node.getAttribute('type') || 'text').toLowerCase();
            if (inputType === 'hidden') return;
            const rect = node.getBoundingClientRect();
            if (rect.width <= 0 || rect.height <= 0) return;
            const ix = Math.round(rect.left);
            const iy = Math.round(rect.top + scrollY);
            const iw = Math.round(rect.width);
            const ih = Math.round(rect.height);
            const isButton = (inputType === 'submit' || inputType === 'button' || inputType === 'reset');
            // Background
            const ibg = style.backgroundColor !== 'rgba(0, 0, 0, 0)'
                ? style.backgroundColor
                : (isButton ? 'rgb(192,192,192)' : 'rgb(255,255,255)');
            commands.push({type:'bg_rect', x:ix, y:iy, width:iw, height:ih, color:ibg});
            // 3D borders: sunken for text inputs, raised for buttons
            if (isButton) {
                commands.push({type:'border', side:'top', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(255,255,255)', bwidth:1});
                commands.push({type:'border', side:'left', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(255,255,255)', bwidth:1});
                commands.push({type:'border', side:'bottom', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(128,128,128)', bwidth:1});
                commands.push({type:'border', side:'right', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(128,128,128)', bwidth:1});
            } else {
                commands.push({type:'border', side:'top', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(128,128,128)', bwidth:1});
                commands.push({type:'border', side:'left', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(128,128,128)', bwidth:1});
                commands.push({type:'border', side:'bottom', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(255,255,255)', bwidth:1});
                commands.push({type:'border', side:'right', rect_x:ix, rect_y:iy, rect_w:iw, rect_h:ih, color:'rgb(255,255,255)', bwidth:1});
            }
            // Value text
            let valText = '';
            if (inputType === 'checkbox') valText = node.checked ? '[X]' : '[ ]';
            else if (inputType === 'radio') valText = node.checked ? '(*)' : '( )';
            else if (isButton) valText = node.value || (inputType === 'submit' ? 'Submit' : (inputType === 'reset' ? 'Reset' : 'Button'));
            else if (inputType !== 'image' && inputType !== 'file')
                valText = node.value || node.placeholder || '';
            if (valText) {
                const ts = getTextStyle(node);
                commands.push({
                    type:'text', x:ix+3, y:iy+2,
                    text: valText.substring(0, 200),
                    textWidth: Math.max(0, iw - 6),
                    color: (node.value || isButton) ? style.color : 'rgb(169,169,169)',
                    fontSize:ts.fontSize, fontFamily:ts.fontFamily,
                    bold:ts.bold, italic:ts.italic,
                    underline:false, strikethrough:false,
                });
            }
            return;
        }

        if (tag === 'TEXTAREA') {
            const rect = node.getBoundingClientRect();
            if (rect.width <= 0 || rect.height <= 0) return;
            const tx = Math.round(rect.left);
            const ty = Math.round(rect.top + scrollY);
            const tw = Math.round(rect.width);
            const th = Math.round(rect.height);
            const tbg = style.backgroundColor !== 'rgba(0, 0, 0, 0)'
                ? style.backgroundColor : 'rgb(255,255,255)';
            commands.push({type:'bg_rect', x:tx, y:ty, width:tw, height:th, color:tbg});
            // Sunken border
            commands.push({type:'border', side:'top', rect_x:tx, rect_y:ty, rect_w:tw, rect_h:th, color:'rgb(128,128,128)', bwidth:1});
            commands.push({type:'border', side:'left', rect_x:tx, rect_y:ty, rect_w:tw, rect_h:th, color:'rgb(128,128,128)', bwidth:1});
            commands.push({type:'border', side:'bottom', rect_x:tx, rect_y:ty, rect_w:tw, rect_h:th, color:'rgb(255,255,255)', bwidth:1});
            commands.push({type:'border', side:'right', rect_x:tx, rect_y:ty, rect_w:tw, rect_h:th, color:'rgb(255,255,255)', bwidth:1});
            const taVal = (node.value || node.placeholder || '').substring(0, 500);
            if (taVal) {
                const ts = getTextStyle(node);
                // Split into lines that fit
                const lines = taVal.split('\\n');
                let lineY = ty + 2;
                const lineH = Math.round(ts.fontSize * 1.2) || 14;
                for (let li = 0; li < lines.length && lineY < ty + th - 2; li++) {
                    if (lines[li]) {
                        commands.push({
                            type:'text', x:tx+3, y:lineY,
                            text: lines[li].substring(0, 200),
                            textWidth: Math.max(0, tw - 6),
                            color: node.value ? style.color : 'rgb(169,169,169)',
                            fontSize:ts.fontSize, fontFamily:ts.fontFamily,
                            bold:ts.bold, italic:ts.italic,
                            underline:false, strikethrough:false,
                        });
                    }
                    lineY += lineH;
                }
            }
            return;
        }

        if (tag === 'SELECT') {
            const rect = node.getBoundingClientRect();
            if (rect.width <= 0 || rect.height <= 0) return;
            const sx = Math.round(rect.left);
            const sy2 = Math.round(rect.top + scrollY);
            const sw = Math.round(rect.width);
            const sh = Math.round(rect.height);
            const sbg = style.backgroundColor !== 'rgba(0, 0, 0, 0)'
                ? style.backgroundColor : 'rgb(192,192,192)';
            commands.push({type:'bg_rect', x:sx, y:sy2, width:sw, height:sh, color:sbg});
            // Raised border
            commands.push({type:'border', side:'top', rect_x:sx, rect_y:sy2, rect_w:sw, rect_h:sh, color:'rgb(255,255,255)', bwidth:1});
            commands.push({type:'border', side:'left', rect_x:sx, rect_y:sy2, rect_w:sw, rect_h:sh, color:'rgb(255,255,255)', bwidth:1});
            commands.push({type:'border', side:'bottom', rect_x:sx, rect_y:sy2, rect_w:sw, rect_h:sh, color:'rgb(128,128,128)', bwidth:1});
            commands.push({type:'border', side:'right', rect_x:sx, rect_y:sy2, rect_w:sw, rect_h:sh, color:'rgb(128,128,128)', bwidth:1});
            // Selected option text + down arrow
            let selText = '';
            if (node.selectedIndex >= 0 && node.options && node.options[node.selectedIndex]) {
                selText = node.options[node.selectedIndex].text;
            }
            if (selText) {
                const ts = getTextStyle(node);
                commands.push({
                    type:'text', x:sx+3, y:sy2+2,
                    text: selText.substring(0, 100) + ' v',
                    textWidth: Math.max(0, sw - 6),
                    color: style.color,
                    fontSize:ts.fontSize, fontFamily:ts.fontFamily,
                    bold:ts.bold, italic:ts.italic,
                    underline:false, strikethrough:false,
                });
            }
            return;
        }

        if (tag === 'BUTTON') {
            const rect = node.getBoundingClientRect();
            if (rect.width > 0 && rect.height > 0) {
                const bx = Math.round(rect.left);
                const by = Math.round(rect.top + scrollY);
                const bw2 = Math.round(rect.width);
                const bh = Math.round(rect.height);
                const bbg = style.backgroundColor !== 'rgba(0, 0, 0, 0)'
                    ? style.backgroundColor : 'rgb(192,192,192)';
                commands.push({type:'bg_rect', x:bx, y:by, width:bw2, height:bh, color:bbg});
                // Raised border
                commands.push({type:'border', side:'top', rect_x:bx, rect_y:by, rect_w:bw2, rect_h:bh, color:'rgb(255,255,255)', bwidth:1});
                commands.push({type:'border', side:'left', rect_x:bx, rect_y:by, rect_w:bw2, rect_h:bh, color:'rgb(255,255,255)', bwidth:1});
                commands.push({type:'border', side:'bottom', rect_x:bx, rect_y:by, rect_w:bw2, rect_h:bh, color:'rgb(128,128,128)', bwidth:1});
                commands.push({type:'border', side:'right', rect_x:bx, rect_y:by, rect_w:bw2, rect_h:bh, color:'rgb(128,128,128)', bwidth:1});
            }
            // Recurse into button children for text extraction
            for (const child of node.childNodes) {
                walkNode(child);
            }
            return;
        }

        // --- List marker extraction ---
        if (tag === 'LI') {
            const parent = node.closest('ol, ul');
            if (parent) {
                const listType = style.listStyleType;
                const rect = node.getBoundingClientRect();
                let marker = '';
                if (parent.tagName === 'UL') {
                    marker = (listType === 'circle') ? 'o' :
                             (listType === 'square') ? '-' : '*';
                } else {
                    const start = parseInt(parent.getAttribute('start') || '1');
                    const siblings = Array.from(parent.children)
                        .filter(c => c.tagName === 'LI');
                    const idx = siblings.indexOf(node);
                    const num = start + idx;
                    if (listType === 'lower-alpha' || listType === 'lower-latin') {
                        marker = String.fromCharCode(96 + ((num - 1) % 26) + 1) + '.';
                    } else if (listType === 'upper-alpha' || listType === 'upper-latin') {
                        marker = String.fromCharCode(64 + ((num - 1) % 26) + 1) + '.';
                    } else if (listType === 'lower-roman') {
                        const vals = [1000,'m',900,'cm',500,'d',400,'cd',100,'c',
                            90,'xc',50,'l',40,'xl',10,'x',9,'ix',5,'v',4,'iv',1,'i'];
                        let r = '', n = num;
                        for (let vi = 0; vi < vals.length; vi += 2) {
                            while (n >= vals[vi]) { r += vals[vi+1]; n -= vals[vi]; }
                        }
                        marker = r + '.';
                    } else if (listType === 'upper-roman') {
                        const vals = [1000,'M',900,'CM',500,'D',400,'CD',100,'C',
                            90,'XC',50,'L',40,'XL',10,'X',9,'IX',5,'V',4,'IV',1,'I'];
                        let r = '', n = num;
                        for (let vi = 0; vi < vals.length; vi += 2) {
                            while (n >= vals[vi]) { r += vals[vi+1]; n -= vals[vi]; }
                        }
                        marker = r + '.';
                    } else {
                        marker = num + '.';
                    }
                }
                if (marker && rect.width > 0) {
                    const markerStyle = getTextStyle(node);
                    const mw = Math.round(markerStyle.fontSize * marker.length * 0.6);
                    commands.push({
                        type: 'list_marker',
                        x: Math.round(rect.left - Math.max(18, mw + 4)),
                        y: Math.round(rect.top + scrollY),
                        text: marker,
                        textWidth: mw,
                        color: markerStyle.color,
                        fontSize: markerStyle.fontSize,
                        fontFamily: markerStyle.fontFamily,
                        bold: false,
                        italic: false,
                        underline: false,
                    });
                }
            }
        }

        // Recurse into children
        for (const child of node.childNodes) {
            walkNode(child);
        }
    }

    if (document.body) {
        // Also check <html> element for background images
        // (some pages set background on html, not body)
        const htmlEl = document.documentElement;
        if (htmlEl) {
            const htmlStyle = window.getComputedStyle(htmlEl);
            const htmlBg = htmlStyle.backgroundImage;
            if (htmlBg && htmlBg !== 'none') {
                const match = htmlBg.match(/url\\(["']?([^"')]+)["']?\\)/);
                if (match) {
                    let hRepeat = (htmlStyle.backgroundRepeat || 'repeat').trim();
                    if (hRepeat === 'repeat repeat') hRepeat = 'repeat';
                    else if (hRepeat === 'no-repeat no-repeat') hRepeat = 'no-repeat';
                    else if (hRepeat === 'repeat no-repeat') hRepeat = 'repeat-x';
                    else if (hRepeat === 'no-repeat repeat') hRepeat = 'repeat-y';
                    commands.push({
                        type: 'bg_image',
                        x: 0,
                        y: 0,
                        width: Math.round(htmlEl.scrollWidth),
                        height: Math.round(htmlEl.scrollHeight),
                        src: match[1],
                        repeat: hRepeat,
                        bgSize: htmlStyle.backgroundSize || 'auto',
                    });
                }
            }
        }

        walkNode(document.body);
    }

    // Fragment anchor scroll position
    let initialScrollY = 0;
    if (window.location.hash) {
        const anchor = window.location.hash.substring(1);
        const target = document.getElementById(anchor) ||
                       document.querySelector('a[name="' + anchor + '"]');
        if (target) {
            const targetRect = target.getBoundingClientRect();
            initialScrollY = Math.round(targetRect.top + scrollY);
        }
    }

    return {
        bg_color: bgColor,
        commands: commands,
        link_count: linkId,
        image_count: imageId,
        doc_height: Math.round(
            document.documentElement.scrollHeight),
        initial_scroll_y: initialScrollY,
    };
}'''


_NAMED_COLORS = {
    # HTML4 standard 16 + common extras
    'black': (0, 0, 0), 'white': (255, 255, 255),
    'red': (255, 0, 0), 'green': (0, 128, 0), 'blue': (0, 0, 255),
    'yellow': (255, 255, 0), 'cyan': (0, 255, 255),
    'magenta': (255, 0, 255), 'gray': (128, 128, 128),
    'grey': (128, 128, 128), 'silver': (192, 192, 192),
    'maroon': (128, 0, 0), 'olive': (128, 128, 0),
    'lime': (0, 255, 0), 'aqua': (0, 255, 255),
    'teal': (0, 128, 128), 'navy': (0, 0, 128),
    'fuchsia': (255, 0, 255), 'purple': (128, 0, 128),
    'orange': (255, 165, 0), 'brown': (165, 42, 42),
    # Additional CSS2/common web colors
    'darkgray': (169, 169, 169), 'darkgrey': (169, 169, 169),
    'lightgray': (211, 211, 211), 'lightgrey': (211, 211, 211),
    'darkred': (139, 0, 0), 'darkgreen': (0, 100, 0),
    'darkblue': (0, 0, 139), 'lightblue': (173, 216, 230),
    'pink': (255, 192, 203), 'gold': (255, 215, 0),
    'wheat': (245, 222, 179), 'beige': (245, 245, 220),
    'ivory': (255, 255, 240), 'linen': (250, 240, 230),
    'khaki': (240, 230, 140), 'coral': (255, 127, 80),
    'salmon': (250, 128, 114), 'crimson': (220, 20, 60),
    'indigo': (75, 0, 130), 'violet': (238, 130, 238),
    'plum': (221, 160, 221), 'tan': (210, 180, 140),
    'sienna': (160, 82, 45), 'peru': (205, 133, 63),
    'chocolate': (210, 105, 30), 'tomato': (255, 99, 71),
}


def _parse_css_color(color_str):
    """Parse a CSS color string like 'rgb(255, 0, 0)' to (r, g, b).

    Handles rgb(), rgba(), #hex, and named HTML colors.
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

    # Handle named HTML colors (common in 90s markup)
    named = _NAMED_COLORS.get(color_str.lower())
    if named:
        return named

    # Handle bare hex without # (some 90s HTML: <font color="FF0000">)
    bare = color_str.strip()
    if len(bare) == 6:
        try:
            r = int(bare[0:2], 16)
            g = int(bare[2:4], 16)
            b = int(bare[4:6], 16)
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


# Font size quantization buckets
_SIZE_BUCKETS = [8, 10, 12, 14, 16, 18, 20, 24]


def _quantize_font_size(px):
    """Quantize a CSS font size in pixels to the nearest bucket."""
    best = _SIZE_BUCKETS[0]
    best_dist = abs(px - best)
    for s in _SIZE_BUCKETS[1:]:
        d = abs(px - s)
        if d < best_dist:
            best = s
            best_dist = d
    return best


# Layer ordering for painter's algorithm.
# bg_rect, bg_image, bg_tile all share layer 0 so stable sort preserves
# DOM walk order (parent before child).  Within one element, bg_rect is
# emitted before bg_image, matching CSS paint order (color under image).
_LAYER_ORDER = {
    'bg_rect': 0,
    'bg_image': 0,
    'bg_tile': 0,
    'border': 2,
    'rect': 3,
    'image': 4,
    'list_marker': 5,
    'text': 5,
    'link_rect': 6,
}


async def extract_content(page, viewport_width=640):
    """Extract page content as pre-positioned draw commands.

    Args:
        page: Playwright page object
        viewport_width: viewport width in pixels (for image clamping)

    Returns:
        dict with keys:
            bg_color (int): palette index for page background
            nodes (list): positioned draw commands, sorted by layer
            link_count (int): number of links
            image_count (int): number of images
            doc_height (int): total document height in pixels
            font_variants (set): unique (size, family, bold) tuples used
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
            'font_variants': set(),
        }

    bg_color = _css_color_to_palette(raw.get('bg_color', 'rgb(255,255,255)'))
    max_w = viewport_width - 16

    processed = []
    font_variants = set()

    for cmd in raw.get('commands', []):
        ctype = cmd.get('type')

        if ctype == 'text' or ctype == 'list_marker':
            text = cmd.get('text', '')
            # Only strip if not preserving whitespace
            if not cmd.get('preserveWS'):
                text = text.strip()
            if not text:
                continue
            flags = 0
            if cmd.get('bold'):
                flags |= 0x01
            if cmd.get('italic'):
                flags |= 0x02
            if cmd.get('underline'):
                flags |= 0x04
            if cmd.get('strikethrough'):
                flags |= 0x08

            size_px = cmd.get('fontSize', 14)
            q_size = _quantize_font_size(size_px)
            family = cmd.get('fontFamily', 'sans')
            bold = cmd.get('bold', False)
            italic = cmd.get('italic', False)

            # Track font variant for glyph cache (with italic axis)
            font_variants.add((q_size, family, bold, italic))

            processed.append({
                'type': 'text',
                'x': min(65535, max(0, cmd.get('x', 0))),
                'y': min(65535, max(0, cmd.get('y', 0))),
                'text': text,
                'text_width': min(65535, max(0, cmd.get('textWidth', 0))),
                'color': _css_color_to_palette(
                    cmd.get('color', 'rgb(0,0,0)')),
                'font_size': q_size,
                'font_family': family,
                'font_bold': bold,
                'font_italic': italic,
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
                'alt': cmd.get('alt', ''),
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

        elif ctype == 'bg_rect':
            processed.append({
                'type': 'bg_rect',
                'x': min(65535, max(0, cmd.get('x', 0))),
                'y': min(65535, max(0, cmd.get('y', 0))),
                'w': min(65535, cmd.get('width', 0)),
                'h': min(65535, cmd.get('height', 0)),
                'color': _css_color_to_palette(
                    cmd.get('color', 'rgb(255,255,255)')),
            })

        elif ctype == 'border':
            side = cmd.get('side', 'top')
            rx = cmd.get('rect_x', 0)
            ry = cmd.get('rect_y', 0)
            rw = cmd.get('rect_w', 0)
            rh = cmd.get('rect_h', 0)
            bw = cmd.get('bwidth', 1)
            color = _css_color_to_palette(
                cmd.get('color', 'rgb(0,0,0)'))

            # Convert border side to a thin rect
            if side == 'top':
                bx, by, bwd, bht = rx, ry, rw, bw
            elif side == 'bottom':
                bx, by, bwd, bht = rx, ry + rh - bw, rw, bw
            elif side == 'left':
                bx, by, bwd, bht = rx, ry, bw, rh
            elif side == 'right':
                bx, by, bwd, bht = rx + rw - bw, ry, bw, rh
            else:
                continue

            processed.append({
                'type': 'border',
                'x': min(65535, max(0, bx)),
                'y': min(65535, max(0, by)),
                'w': min(65535, max(1, bwd)),
                'h': min(65535, max(1, bht)),
                'color': color,
            })

        elif ctype == 'bg_image':
            processed.append({
                'type': 'bg_image',
                'x': min(65535, max(0, cmd.get('x', 0))),
                'y': min(65535, max(0, cmd.get('y', 0))),
                'w': min(65535, cmd.get('width', 0)),
                'h': min(65535, cmd.get('height', 0)),
                'src': cmd.get('src', ''),
                'repeat': cmd.get('repeat', 'repeat'),
                'bg_size': cmd.get('bgSize', 'auto'),
            })

    # Sort by visual layer (painter's algorithm)
    processed.sort(key=lambda c: _LAYER_ORDER.get(c['type'], 5))

    # Cap font variants at 8 (drop least common if needed)
    if len(font_variants) > 8:
        # Count usage per variant
        var_counts = {}
        for node in processed:
            if node['type'] == 'text':
                key = (node['font_size'], node['font_family'],
                       node['font_bold'], node.get('font_italic', False))
                var_counts[key] = var_counts.get(key, 0) + 1
        # Keep top 8
        sorted_vars = sorted(var_counts.keys(),
                             key=lambda k: var_counts[k], reverse=True)
        font_variants = set(sorted_vars[:8])

    return {
        'bg_color': bg_color,
        'nodes': processed,
        'link_count': raw.get('link_count', 0),
        'image_count': raw.get('image_count', 0),
        'doc_height': min(65535, raw.get('doc_height', 0)),
        'font_variants': font_variants,
        'initial_scroll_y': min(65535, raw.get('initial_scroll_y', 0)),
    }
