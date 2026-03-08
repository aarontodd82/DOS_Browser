"""Page complexity detection for RetroSurf native rendering mode.

Scores a page's visual complexity to decide whether it can be rendered
natively on the DOS client (simple text/links/images) or needs the
full screenshot pipeline (complex layouts, SPAs, heavy CSS).

Score <= 40 → recommend native rendering.
"""

# Always use native rendering for these domains (simple/retro sites)
NATIVE_WHITELIST = {
    'wiby.me',
    'neocities.org',
    'geocities.ws',
    'theoldnet.com',
    '68k.news',
}

# Always use screenshot rendering for these domains (complex sites)
SCREENSHOT_BLACKLIST = {
    'google.com', 'www.google.com',
    'youtube.com', 'www.youtube.com',
    'reddit.com', 'www.reddit.com', 'old.reddit.com',
    'twitter.com', 'x.com',
    'facebook.com', 'www.facebook.com',
    'instagram.com', 'www.instagram.com',
    'amazon.com', 'www.amazon.com',
    'github.com',
    'docs.google.com',
    'mail.google.com',
    'outlook.com',
}

# JavaScript executed in the browser to score page complexity
_COMPLEXITY_JS = '''() => {
    const result = { score: 0, reasons: [] };

    // SPA framework detection
    if (window.__NEXT_DATA__ || window.__NUXT__ || window.__remixContext) {
        result.score += 100;
        result.reasons.push('SPA framework detected');
    }
    if (document.querySelector('[data-reactroot], [id="__next"], #app[data-v-]')) {
        result.score += 100;
        result.reasons.push('React/Vue/Nuxt root detected');
    }
    if (window.angular || document.querySelector('[ng-app], [data-ng-app]')) {
        result.score += 100;
        result.reasons.push('Angular detected');
    }

    // Canvas/WebGL
    const canvases = document.querySelectorAll('canvas');
    if (canvases.length > 0) {
        result.score += 80;
        result.reasons.push('Canvas elements: ' + canvases.length);
    }

    // Element count
    const allElements = document.querySelectorAll('*').length;
    if (allElements > 2000) {
        result.score += 25;
        result.reasons.push('Very high element count: ' + allElements);
    } else if (allElements > 500) {
        result.score += 15;
        result.reasons.push('High element count: ' + allElements);
    }

    // Flexbox/grid containers
    const allEls = document.querySelectorAll('*');
    let flexGridCount = 0;
    let absCount = 0;
    let transformCount = 0;
    const checkLimit = Math.min(allEls.length, 200);
    for (let i = 0; i < checkLimit; i++) {
        const style = window.getComputedStyle(allEls[i]);
        const display = style.display;
        if (display === 'flex' || display === 'inline-flex' ||
            display === 'grid' || display === 'inline-grid') {
            flexGridCount++;
        }
        if (style.position === 'absolute' || style.position === 'fixed') {
            absCount++;
        }
        if (style.transform !== 'none') {
            transformCount++;
        }
    }
    if (flexGridCount > 5) {
        result.score += 15;
        result.reasons.push('Flexbox/grid containers: ' + flexGridCount);
    }
    if (absCount > 10) {
        result.score += 15;
        result.reasons.push('Absolute/fixed positioned: ' + absCount);
    }
    if (transformCount > 3) {
        result.score += 10;
        result.reasons.push('CSS transforms: ' + transformCount);
    }

    // Multi-column layout
    for (let i = 0; i < checkLimit; i++) {
        const cc = window.getComputedStyle(allEls[i]).columnCount;
        if (cc !== 'auto' && parseInt(cc) > 1) {
            result.score += 20;
            result.reasons.push('Multi-column layout');
            break;
        }
    }

    // SVG elements
    const svgs = document.querySelectorAll('svg');
    if (svgs.length > 3) {
        result.score += 10;
        result.reasons.push('SVG elements: ' + svgs.length);
    }

    // Iframes
    const iframes = document.querySelectorAll('iframe');
    if (iframes.length > 2) {
        result.score += 15;
        result.reasons.push('Iframes: ' + iframes.length);
    }

    // Shadow DOM / web components
    let shadowCount = 0;
    for (let i = 0; i < checkLimit; i++) {
        if (allEls[i].shadowRoot) shadowCount++;
    }
    if (shadowCount > 0) {
        result.score += 20;
        result.reasons.push('Shadow DOM elements: ' + shadowCount);
    }

    return result;
}'''


async def detect_complexity(page):
    """Score a page's complexity to decide rendering mode.

    Args:
        page: Playwright page object

    Returns:
        dict with keys:
            score (int): complexity score (lower = simpler)
            recommend_native (bool): True if native rendering recommended
            reasons (list[str]): human-readable reasons for the score
    """
    # Check domain whitelist/blacklist first
    try:
        url = page.url
    except Exception:
        return {'score': 100, 'recommend_native': False,
                'reasons': ['Could not get URL']}

    from urllib.parse import urlparse
    parsed = urlparse(url)
    hostname = parsed.hostname or ''

    # Check whitelist (match domain or parent domain)
    for domain in NATIVE_WHITELIST:
        if hostname == domain or hostname.endswith('.' + domain):
            return {'score': 0, 'recommend_native': True,
                    'reasons': [f'Whitelisted domain: {domain}']}

    # Check blacklist
    for domain in SCREENSHOT_BLACKLIST:
        if hostname == domain or hostname.endswith('.' + domain):
            return {'score': 100, 'recommend_native': False,
                    'reasons': [f'Blacklisted domain: {domain}']}

    # Run JS complexity scoring
    try:
        result = await page.evaluate(_COMPLEXITY_JS)
    except Exception as e:
        return {'score': 100, 'recommend_native': False,
                'reasons': [f'JS evaluation failed: {e}']}

    score = result.get('score', 100)
    reasons = result.get('reasons', [])

    return {
        'score': score,
        'recommend_native': score <= 40,
        'reasons': reasons,
    }
