#include "style.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <strings.h>
#include <sstream>
#include <unordered_map>

namespace browser {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Global zoom factor. Applied inside computeStyle (layout.cpp) so that
// tag-default font sizes zoom too.
static float g_zoom = 1.0f;
void setGlobalZoom(float z) { g_zoom = z; }
float getGlobalZoom() { return g_zoom; }

// Bumped by parseCSS so layout-side caches can key on it.
static uint64_t g_parseGeneration = 0;

// Viewport width used for @media evaluation (set by the layout engine).
static int g_mediaViewportW = 1024;
void setMediaViewportWidth(int w) { if (w > 0) g_mediaViewportW = w; }
int mediaViewportWidth() { return g_mediaViewportW; }

// CSS custom properties (--name: value) for var() resolution. Global
// document scope; parseCSS registers them in stylesheet order (last wins),
// which mirrors the common ":root { --x: ... }" pattern well enough.
static std::unordered_map<std::string, std::string> g_customProps;
void setCustomProperty(const std::string& name, const std::string& value) {
    g_customProps[name] = value;
}
std::string getCustomProperty(const std::string& name) {
    auto it = g_customProps.find(name);
    return it == g_customProps.end() ? "" : it->second;
}
void clearCustomProperties() { g_customProps.clear(); }

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------

static SDL_Color namedColor(const std::string& name) {
    // CSS named colors (the common set; unknown names fall back to a
    // sensible gray instead of silently not applying).
    struct { const char* n; Uint8 r, g, b; } tab[] = {
        {"black",0,0,0},{"white",255,255,255},{"red",255,0,0},{"green",0,128,0},
        {"lime",0,255,0},{"blue",0,0,255},{"yellow",255,255,0},{"purple",128,0,128},
        {"orange",255,165,0},{"pink",255,192,203},{"gray",128,128,128},
        {"grey",128,128,128},{"silver",192,192,192},{"maroon",128,0,0},
        {"olive",128,128,0},{"aqua",0,255,255},{"cyan",0,255,255},{"teal",0,128,128},
        {"navy",0,0,128},{"fuchsia",255,0,255},{"magenta",255,0,255},
        {"brown",165,42,42},{"gold",255,215,0},{"indigo",75,0,130},
        {"violet",238,130,238},{"darkgray",169,169,169},{"darkgrey",169,169,169},
        {"dimgray",105,105,105},{"dimgrey",105,105,105},
        {"lightgray",211,211,211},{"lightgrey",211,211,211},{"gainsboro",220,220,220},
        {"whitesmoke",245,245,245},{"aliceblue",240,248,255},{"antiquewhite",250,235,215},
        {"azure",240,255,255},{"beige",245,245,220},{"bisque",255,228,196},
        {"blanchedalmond",255,235,205},{"blueviolet",138,43,226},{"burlywood",222,184,135},
        {"cadetblue",95,158,160},{"chartreuse",127,255,0},{"chocolate",210,105,30},
        {"coral",255,127,80},{"cornflowerblue",100,149,237},{"cornsilk",255,248,220},
        {"crimson",220,20,60},{"darkblue",0,0,139},{"darkcyan",0,139,139},
        {"darkgoldenrod",184,134,11},{"darkgreen",0,100,0},{"darkkhaki",189,183,107},
        {"darkmagenta",139,0,139},{"darkolivegreen",85,107,47},{"darkorange",255,140,0},
        {"darkorchid",153,50,204},{"darkred",139,0,0},{"darksalmon",233,150,122},
        {"darkseagreen",143,188,143},{"darkslateblue",72,61,139},
        {"darkslategray",47,79,79},{"darkturquoise",0,206,209},{"darkviolet",148,0,211},
        {"deeppink",255,20,147},{"deepskyblue",0,191,255},{"dodgerblue",30,144,255},
        {"firebrick",178,34,34},{"floralwhite",255,250,240},{"forestgreen",34,139,34},
        {"ghostwhite",248,248,255},{"greenyellow",173,255,47},{"honeydew",240,255,240},
        {"hotpink",255,105,180},{"indianred",205,92,92},{"ivory",255,255,240},
        {"khaki",240,230,140},{"lavender",230,230,250},{"lavenderblush",255,240,245},
        {"lawngreen",124,252,0},{"lemonchiffon",255,250,205},{"lightblue",173,216,230},
        {"lightcoral",240,128,128},{"lightcyan",224,255,255},{"lightgoldenrodyellow",250,250,210},
        {"lightgreen",144,238,144},{"lightpink",255,182,193},{"lightsalmon",255,160,122},
        {"lightseagreen",32,178,170},{"lightskyblue",135,206,250},
        {"lightslategray",119,136,153},{"lightsteelblue",176,196,222},
        {"lightyellow",255,255,224},{"limegreen",50,205,50},{"linen",250,240,230},
        {"mediumaquamarine",102,205,170},{"mediumblue",0,0,205},
        {"mediumorchid",186,85,211},{"mediumpurple",147,112,219},
        {"mediumseagreen",60,179,113},{"mediumslateblue",123,104,238},
        {"mediumspringgreen",0,250,154},{"mediumturquoise",72,209,204},
        {"mediumvioletred",199,21,133},{"midnightblue",25,25,112},
        {"mintcream",245,255,250},{"mistyrose",255,228,225},{"moccasin",255,228,181},
        {"navajowhite",255,222,173},{"oldlace",253,245,230},{"orangered",255,69,0},
        {"orchid",218,112,214},{"palegoldenrod",238,232,170},{"palegreen",152,251,152},
        {"paleturquoise",175,238,238},{"palevioletred",219,112,147},
        {"papayawhip",255,239,213},{"peachpuff",255,218,185},{"peru",205,133,63},
        {"plum",221,160,221},{"powderblue",176,224,230},{"rosybrown",188,143,143},
        {"royalblue",65,105,225},{"saddlebrown",139,69,19},{"salmon",250,128,114},
        {"sandybrown",244,164,96},{"seagreen",46,139,87},{"seashell",255,245,238},
        {"sienna",160,82,45},{"skyblue",135,206,235},{"slateblue",106,90,205},
        {"slategray",112,128,144},{"snow",255,250,250},{"springgreen",0,255,127},
        {"steelblue",70,130,180},{"tan",210,180,140},{"thistle",216,191,216},
        {"tomato",255,99,71},{"turquoise",64,224,208},{"wheat",245,222,179},
        {"whitesmoke2",245,245,245},{"yellowgreen",154,205,50},
        {"rebeccapurple",102,51,153},{"mediumspringgreen2",0,250,154},
    };
    for (auto& c : tab) {
        if (name == c.n) return {c.r, c.g, c.b, 255};
    }
    if (name == "transparent") return {0, 0, 0, 0};
    return {0, 0, 0, 0};
}

// ---------------------------------------------------------------------------
// Length parsing (px / pt / em / rem / % / calc()) with var() substitution
// ---------------------------------------------------------------------------

// Substitute var(--name [, fallback]) recursively using the global custom
// property map. Runs up to 8 rounds so var() inside var() resolves.
static std::string substituteVars(const std::string& in) {
    std::string s = in;
    for (int round = 0; round < 8; ++round) {
        size_t p = s.find("var(");
        if (p == std::string::npos) break;
        // Find matching close paren.
        int depth = 0;
        size_t i = p + 3, close = std::string::npos;
        for (; i < s.size(); ++i) {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')') {
                --depth;
                if (depth == 0) { close = i; break; }
            }
        }
        if (close == std::string::npos) break;
        std::string inner = s.substr(p + 4, close - (p + 4));
        std::string name = trim(inner);
        std::string fallback;
        size_t comma = inner.find(',');
        if (comma != std::string::npos) {
            name = trim(inner.substr(0, comma));
            fallback = trim(inner.substr(comma + 1));
        }
        std::string val = getCustomProperty(name);
        if (val.empty()) val = fallback;
        s = s.substr(0, p) + val + s.substr(close + 1);
    }
    return s;
}

// Parse one length component (number + unit, possibly a bare number).
// Returns px via `outPx` and % via `outPct` (both can be set for calc).
static bool parseLengthParts(const std::string& val, int base,
                             float& outPx, float& outPct) {
    std::string s = trim(val);
    if (s.empty() || s == "auto") return false;
    size_t i = 0;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) ||
                            s[i] == '.' || s[i] == '-' || s[i] == '+')) ++i;
    std::string num = s.substr(0, i);
    std::string unit = trim(s.substr(i));
    if (num.empty()) return false;
    float n = 0;
    try { n = std::stof(num); } catch (...) { return false; }
    if (unit.empty() || unit == "px") { outPx = n; return true; }
    if (unit == "pt")  { outPx = n * 96.0f / 72.0f; return true; }
    if (unit == "em")  { outPx = n * base; return true; }
    if (unit == "rem") { outPx = n * 16.0f; return true; }
    if (unit == "vw")  { outPct = n * mediaViewportWidth() / 100.0f; return true; }
    if (unit == "%")   { outPct = n; return true; }
    return false;
}

// Evaluate calc(...) expression into px + pct parts.
static bool evalCalc(const std::string& body, int base,
                     float& outPx, float& outPct);

// Parse any length value (plain, %, or calc(...)). `raw` must already be
// var()-substituted.
static bool parseValueParts(const std::string& raw, int base,
                            float& outPx, float& outPct) {
    std::string s = trim(raw);
    // calc?
    if (s.size() > 5 && lower(s.substr(0, 5)) == "calc(" && s.back() == ')') {
        return evalCalc(s.substr(5, s.size() - 6), base, outPx, outPct);
    }
    return parseLengthParts(s, base, outPx, outPct);
}

// Split a calc() body into top-level +/- terms respecting nested parens.
static bool evalCalc(const std::string& body, int base,
                     float& outPx, float& outPct) {
    outPx = 0; outPct = 0;
    std::string cur;
    float sign = 1.0f;
    int depth = 0;
    auto flushTerm = [&](float sgn) {
        std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return true;
        float px = 0, pct = 0;
        if (!parseValueParts(t, base, px, pct)) return false;
        outPx  += sgn * px;
        outPct += sgn * pct;
        return true;
    };
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '(') { ++depth; cur += c; continue; }
        if (c == ')') { --depth; cur += c; continue; }
        if (depth == 0 && (c == '+' || c == '-')) {
            // Only a separator if not directly after '(', ',' or an operator
            // (e.g. "e" in numbers won't occur; "-2px" at start is part of term).
            if (!trim(cur).empty()) {
                if (!flushTerm(sign)) return false;
                sign = (c == '+') ? 1.0f : -1.0f;
                continue;
            }
        }
        cur += c;
    }
    return flushTerm(sign);
}

// Parse a length in px honoring common units. `base` is the font size
// that em/% resolve against. Returns false if not parseable.
static bool parseLengthPx(const std::string& val, int base, float& outPx) {
    float px = 0, pct = 0;
    if (!parseValueParts(substituteVars(val), base, px, pct)) return false;
    outPx = px + pct * base / 100.0f;
    return true;
}

// Parse "Npx"/"Npt"/"Nem"/"N%" into an int pixel value.
static int parseLengthInt(const std::string& val, int base) {
    float px = 0;
    if (!parseLengthPx(val, base, px)) return 0;
    return (int)std::lround(px);
}

// Strip a trailing "!important" (with optional spaces before it).
static std::string stripImportant(const std::string& v) {
    std::string s = trim(v);
    if (s.size() >= 10) {
        std::string tail = lower(s.substr(s.size() - 10));
        if (tail == "!important") s = trim(s.substr(0, s.size() - 10));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Edge list parsing with units / % / auto
// ---------------------------------------------------------------------------

// Parse "a b c d" style values. px parts go into `e` (via out px array
// [top,right,bottom,left]), % parts into `pe`, auto tokens set the
// autoL/autoR flags (margins only).
static int parseEdgeListEx(const std::string& val, int base,
                           int* px4, float* pct4,
                           bool* autoL = nullptr, bool* autoR = nullptr) {
    std::string s = stripImportant(substituteVars(val));
    std::istringstream ss(s);
    std::vector<std::string> toks;
    std::string tok;
    while (ss >> tok) toks.push_back(tok);
    if (toks.empty() || toks.size() > 4) return 0;
    // Expand shorthand 1/2/3/4 values into [t,r,b,l].
    std::string v[4];
    if (toks.size() == 1) { v[0]=v[1]=v[2]=v[3]=toks[0]; }
    else if (toks.size() == 2) { v[0]=v[2]=toks[0]; v[1]=v[3]=toks[1]; }
    else if (toks.size() == 3) { v[0]=toks[0]; v[1]=v[3]=toks[1]; v[2]=toks[2]; }
    else { v[0]=toks[0]; v[1]=toks[1]; v[2]=toks[2]; v[3]=toks[3]; }
    for (int k = 0; k < 4; ++k) {
        std::string t = lower(v[k]);
        if (t == "auto") {
            px4[k] = 0; pct4[k] = 0;
            if (k == 1 && autoR) *autoR = true;   // right (index 1)
            if (k == 3 && autoL) *autoL = true;   // left  (index 3)
            continue;
        }
        float px = 0, pct = 0;
        if (!parseValueParts(t, base, px, pct)) { px4[k] = 0; pct4[k] = 0; continue; }
        px4[k]  = (int)std::lround(px);
        pct4[k] = pct;
    }
    return (int)toks.size();
}

// ---------------------------------------------------------------------------
// applyStyle
// ---------------------------------------------------------------------------

// Compare two styles and return the bits of every property that differs.
// Used by applyStyle to maintain Style::declared automatically, so every
// property branch is covered without per-branch bookkeeping.
static uint64_t diffBits(const Style& a, const Style& b) {
    uint64_t m = 0;
    auto cdiff = [](const SDL_Color& x, const SDL_Color& y) {
        return x.r != y.r || x.g != y.g || x.b != y.b || x.a != y.a;
    };
    auto ediff = [](const Edges& x, const Edges& y) {
        return x.top != y.top || x.right != y.right ||
               x.bottom != y.bottom || x.left != y.left;
    };
    auto pdiff = [](const PctEdges& x, const PctEdges& y) {
        return x.top != y.top || x.right != y.right ||
               x.bottom != y.bottom || x.left != y.left;
    };
    if (cdiff(a.color, b.color))          m |= B_COLOR;
    if (cdiff(a.bg, b.bg))                m |= B_BG;
    if (a.fontSize != b.fontSize)         m |= B_FONT_SIZE;
    if (a.bold != b.bold)                 m |= B_BOLD;
    if (a.italic != b.italic)             m |= B_ITALIC;
    if (a.underline != b.underline)       m |= B_UNDERLINE;
    if (a.fontFamily != b.fontFamily)     m |= B_FONT_FAMILY;
    if (a.lineHeight != b.lineHeight)     m |= B_LINE_HEIGHT;
    if (a.textTransform != b.textTransform) m |= B_TEXT_TRANSFORM;
    if (a.pre != b.pre)                   m |= B_PRE;
    if (a.bgImageUrl != b.bgImageUrl)     m |= B_BG_IMAGE;
    if (a.bgSize != b.bgSize || a.bgSizeW != b.bgSizeW || a.bgSizeH != b.bgSizeH)
                                          m |= B_BG_SIZE;
    if (a.bgRepeat != b.bgRepeat)         m |= B_BG_REPEAT;
    if (a.bgPosX != b.bgPosX || a.bgPosY != b.bgPosY) m |= B_BG_POS;
    if (ediff(a.margin, b.margin) || pdiff(a.marginPct, b.marginPct) ||
        a.marginAutoLeft != b.marginAutoLeft || a.marginAutoRight != b.marginAutoRight)
                                          m |= B_MARGIN;
    if (ediff(a.padding, b.padding) || pdiff(a.paddingPct, b.paddingPct))
                                          m |= B_PADDING;
    if (ediff(a.border, b.border))        m |= B_BORDER;
    if (cdiff(a.borderColor, b.borderColor) ||
        cdiff(a.borderTopColor, b.borderTopColor) ||
        cdiff(a.borderRightColor, b.borderRightColor) ||
        cdiff(a.borderBottomColor, b.borderBottomColor) ||
        cdiff(a.borderLeftColor, b.borderLeftColor)) m |= B_BORDER_COLOR;
    if (a.textAlign != b.textAlign)       m |= B_TEXT_ALIGN;
    if (a.display != b.display)           m |= B_DISPLAY;
    if (a.width != b.width || a.widthPct != b.widthPct)          m |= B_WIDTH;
    if (a.height != b.height || a.heightPct != b.heightPct)      m |= B_HEIGHT;
    if (a.maxWidth != b.maxWidth || a.maxWidthPct != b.maxWidthPct) m |= B_MAX_WIDTH;
    if (a.minWidth != b.minWidth || a.minWidthPct != b.minWidthPct) m |= B_MIN_WIDTH;
    if (a.maxHeight != b.maxHeight)       m |= B_MAX_HEIGHT;
    if (a.minHeight != b.minHeight)       m |= B_MIN_HEIGHT;
    if (a.position != b.position)         m |= B_POSITION;
    if (a.offTop != b.offTop || a.offRight != b.offRight ||
        a.offBottom != b.offBottom || a.offLeft != b.offLeft ||
        a.offTopPct != b.offTopPct || a.offRightPct != b.offRightPct ||
        a.offBottomPct != b.offBottomPct || a.offLeftPct != b.offLeftPct)
                                          m |= B_OFFSETS;
    if (a.cssFloat != b.cssFloat)         m |= B_FLOAT;
    if (a.flexDirection != b.flexDirection) m |= B_FLEX_DIR;
    if (a.justifyContent != b.justifyContent) m |= B_JUSTIFY;
    if (a.alignItems != b.alignItems)     m |= B_ALIGN_ITEMS;
    if (a.flexGap != b.flexGap)           m |= B_GAP;
    if (a.boxSizing != b.boxSizing)       m |= B_BOX_SIZING;
    if (a.objectFit != b.objectFit)       m |= B_OBJECT_FIT;
    if (a.opacity != b.opacity)           m |= B_OPACITY;
    if (a.listStyleType != b.listStyleType) m |= B_LIST_STYLE;
    if (a.verticalAlign != b.verticalAlign) m |= B_VERTICAL_ALIGN;
    if (a.visibilityHidden != b.visibilityHidden) m |= B_VISIBILITY;
    if (a.direction != b.direction)       m |= B_DIRECTION;
    if (a.gridTemplateColumns != b.gridTemplateColumns ||
        a.gridTemplateRows != b.gridTemplateRows) m |= B_GRID_TPL;
    if (a.gridTemplateAreas != b.gridTemplateAreas) m |= B_GRID_AREAS;
    if (a.gridArea != b.gridArea)         m |= B_GRID_AREA;
    if (a.gridColStartLine != b.gridColStartLine ||
        a.gridColEndLine != b.gridColEndLine ||
        a.gridColSpan != b.gridColSpan ||
        a.gridRowStartLine != b.gridRowStartLine ||
        a.gridRowEndLine != b.gridRowEndLine ||
        a.gridRowSpan != b.gridRowSpan) m |= B_GRID_POS;
    if (a.overflowHidden != b.overflowHidden) m |= B_OVERFLOW;
    if (a.clippedAway != b.clippedAway)       m |= B_CLIP;
    return m;
}

void mergeDeclared(Style& dst, const Style& src) {
    uint64_t m = src.declared;
    auto has = [&](uint64_t bit) { return (m & bit) != 0; };
    if (has(B_COLOR))          { dst.color = src.color; dst.hasColor = true; }
    if (has(B_BG))             { dst.bg = src.bg; dst.hasBg = true; }
    if (has(B_FONT_SIZE))      { dst.fontSize = src.fontSize; dst.hasFontSize = true; }
    if (has(B_BOLD))           { dst.bold = src.bold; dst.hasBold = true; }
    if (has(B_ITALIC))         { dst.italic = src.italic; dst.hasItalic = true; }
    if (has(B_UNDERLINE))      { dst.underline = src.underline; dst.hasUnderline = true; }
    if (has(B_FONT_FAMILY))    { dst.fontFamily = src.fontFamily; dst.hasFontFamily = true; }
    if (has(B_LINE_HEIGHT))    { dst.lineHeight = src.lineHeight; dst.hasLineHeight = true; }
    if (has(B_TEXT_TRANSFORM)) { dst.textTransform = src.textTransform; dst.hasTextTransform = true; }
    if (has(B_PRE))            { dst.pre = src.pre; dst.hasPre = true; }
    if (has(B_BG_IMAGE))       { dst.bgImageUrl = src.bgImageUrl; dst.hasBgImage = true; }
    if (has(B_BG_SIZE))        { dst.bgSize = src.bgSize; dst.bgSizeW = src.bgSizeW; dst.bgSizeH = src.bgSizeH; }
    if (has(B_BG_REPEAT))      { dst.bgRepeat = src.bgRepeat; }
    if (has(B_BG_POS))         { dst.bgPosX = src.bgPosX; dst.bgPosY = src.bgPosY; }
    if (has(B_MARGIN)) {
        dst.margin = src.margin; dst.marginPct = src.marginPct;
        dst.marginAutoLeft = src.marginAutoLeft; dst.marginAutoRight = src.marginAutoRight;
        dst.hasMargin = true;
    }
    if (has(B_PADDING))        { dst.padding = src.padding; dst.paddingPct = src.paddingPct; dst.hasPadding = true; }
    if (has(B_BORDER))         { dst.border = src.border; dst.hasBorder = true; }
    if (has(B_BORDER_COLOR)) {
        dst.borderColor = src.borderColor;
        dst.borderTopColor = src.borderTopColor;
        dst.borderRightColor = src.borderRightColor;
        dst.borderBottomColor = src.borderBottomColor;
        dst.borderLeftColor = src.borderLeftColor;
        dst.hasBorderColor = true;
    }
    if (has(B_TEXT_ALIGN))     { dst.textAlign = src.textAlign; dst.hasTextAlign = true; }
    if (has(B_DISPLAY))        { dst.display = src.display; dst.hasDisplay = true; }
    if (has(B_WIDTH))          { dst.width = src.width; dst.widthPct = src.widthPct; dst.hasWidth = src.hasWidth; dst.hasWidthPct = src.hasWidthPct; }
    if (has(B_HEIGHT))         { dst.height = src.height; dst.heightPct = src.heightPct; dst.hasHeight = src.hasHeight; dst.hasHeightPct = src.hasHeightPct; }
    if (has(B_MAX_WIDTH))      { dst.maxWidth = src.maxWidth; dst.maxWidthPct = src.maxWidthPct; dst.hasMaxWidth = src.hasMaxWidth; dst.hasMaxWidthPct = src.hasMaxWidthPct; }
    if (has(B_MIN_WIDTH))      { dst.minWidth = src.minWidth; dst.minWidthPct = src.minWidthPct; dst.hasMinWidth = src.hasMinWidth; dst.hasMinWidthPct = src.hasMinWidthPct; }
    if (has(B_MAX_HEIGHT))     { dst.maxHeight = src.maxHeight; dst.hasMaxHeight = src.hasMaxHeight; }
    if (has(B_MIN_HEIGHT))     { dst.minHeight = src.minHeight; dst.hasMinHeight = src.hasMinHeight; }
    if (has(B_POSITION))       { dst.position = src.position; dst.hasPosition = true; }
    if (has(B_OFFSETS)) {
        dst.offTop = src.offTop; dst.offRight = src.offRight;
        dst.offBottom = src.offBottom; dst.offLeft = src.offLeft;
        dst.offTopPct = src.offTopPct; dst.offRightPct = src.offRightPct;
        dst.offBottomPct = src.offBottomPct; dst.offLeftPct = src.offLeftPct;
        dst.hasOffTop = src.hasOffTop; dst.hasOffRight = src.hasOffRight;
        dst.hasOffBottom = src.hasOffBottom; dst.hasOffLeft = src.hasOffLeft;
    }
    if (has(B_FLOAT))          { dst.cssFloat = src.cssFloat; dst.hasFloat = true; }
    if (has(B_FLEX_DIR))       { dst.flexDirection = src.flexDirection; }
    if (has(B_JUSTIFY))        { dst.justifyContent = src.justifyContent; }
    if (has(B_ALIGN_ITEMS))    { dst.alignItems = src.alignItems; }
    if (has(B_GAP))            { dst.flexGap = src.flexGap; dst.hasFlexGap = true; }
    if (has(B_BOX_SIZING))     { dst.boxSizing = src.boxSizing; dst.hasBoxSizing = true; }
    if (has(B_OBJECT_FIT))     { dst.objectFit = src.objectFit; dst.hasObjectFit = true; }
    if (has(B_OPACITY))        { dst.opacity = src.opacity; dst.hasOpacity = true; }
    if (has(B_LIST_STYLE))     { dst.listStyleType = src.listStyleType; dst.hasListStyle = true; }
    if (has(B_VERTICAL_ALIGN)) { dst.verticalAlign = src.verticalAlign; dst.hasVerticalAlign = true; }
    if (has(B_VISIBILITY))     { dst.visibilityHidden = src.visibilityHidden; dst.hasVisibility = true; }
    if (has(B_DIRECTION))      { dst.direction = src.direction; dst.hasDirection = true; }
    if (has(B_GRID_TPL)) {
        dst.gridTemplateColumns = src.gridTemplateColumns;
        dst.gridTemplateRows = src.gridTemplateRows;
        dst.hasGridTemplateColumns = src.hasGridTemplateColumns;
        dst.hasGridTemplateRows = src.hasGridTemplateRows;
    }
    if (has(B_GRID_AREAS))     { dst.gridTemplateAreas = src.gridTemplateAreas; dst.hasGridTemplateAreas = true; }
    if (has(B_GRID_AREA))      { dst.gridArea = src.gridArea; dst.hasGridArea = true; }
    if (has(B_GRID_POS)) {
        dst.gridColStartLine = src.gridColStartLine;
        dst.gridColEndLine   = src.gridColEndLine;
        dst.gridColSpan      = src.gridColSpan;
        dst.gridRowStartLine = src.gridRowStartLine;
        dst.gridRowEndLine   = src.gridRowEndLine;
        dst.gridRowSpan      = src.gridRowSpan;
        dst.hasGridColumn    = src.hasGridColumn;
        dst.hasGridRow       = src.hasGridRow;
    }
    if (has(B_OVERFLOW))       { dst.overflowHidden = src.overflowHidden; }
    if (has(B_CLIP))           { dst.clippedAway = src.clippedAway; }
    dst.declared |= m;
}

Style applyStyle(const Style& base, const std::string& css) {
    Style s = base;
    auto toLower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return v;
    };

    auto applyColor = [&](const std::string& val, SDL_Color& target, bool& hasFlag) {
        std::string v = toLower(trim(val));
        if (v.size() == 7 && v[0] == '#') {
            try {
                target.r = std::stoi(v.substr(1, 2), nullptr, 16);
                target.g = std::stoi(v.substr(3, 2), nullptr, 16);
                target.b = std::stoi(v.substr(5, 2), nullptr, 16);
                target.a = 255;
                hasFlag = true;
            } catch (...) {}
        } else if (v.size() == 4 && v[0] == '#') {
            // #rgb shorthand
            try {
                target.r = std::stoi(std::string(2, v[1]), nullptr, 16);
                target.g = std::stoi(std::string(2, v[2]), nullptr, 16);
                target.b = std::stoi(std::string(2, v[3]), nullptr, 16);
                target.a = 255;
                hasFlag = true;
            } catch (...) {}
        } else if (v.size() == 9 && v[0] == '#') {
            // #rrggbbaa
            try {
                target.r = std::stoi(v.substr(1, 2), nullptr, 16);
                target.g = std::stoi(v.substr(3, 2), nullptr, 16);
                target.b = std::stoi(v.substr(5, 2), nullptr, 16);
                target.a = std::stoi(v.substr(7, 2), nullptr, 16);
                hasFlag = true;
            } catch (...) {}
        } else if (v.size() >= 5 && v.compare(0, 5, "rgba(") == 0 && v.back() == ')') {
            std::string body = v.substr(5, v.size() - 6);
            // Support both "r,g,b,a" and modern "r g b / a" syntax.
            if (body.find(',') == std::string::npos) {
                size_t slash = body.find('/');
                std::string rgb = body.substr(0, slash == std::string::npos ? body.size() : slash);
                std::istringstream ss(rgb);
                std::string tok;
                int comps[3] = {0,0,0};
                int idx = 0;
                while (ss >> tok && idx < 3) {
                    try { comps[idx] = std::stoi(tok); } catch (...) { comps[idx] = 0; }
                    ++idx;
                }
                auto cl = [](int c) { return std::max(0, std::min(255, c)); };
                target.r = cl(comps[0]); target.g = cl(comps[1]); target.b = cl(comps[2]);
                target.a = 255;
                if (slash != std::string::npos) {
                    std::string av = trim(body.substr(slash + 1));
                    try {
                        float af = std::stof(av);
                        if (av.find('%') != std::string::npos) af *= 2.55f;
                        target.a = cl((int)std::lround(af * 255));
                    } catch (...) {}
                }
                hasFlag = true;
                return;
            }
            std::istringstream ss(body);
            std::string tok;
            int comps[4] = {0, 0, 0, 255};
            int idx = 0;
            while (std::getline(ss, tok, ',') && idx < 4) {
                tok = trim(tok);
                if (!tok.empty() && tok.back() == '%') {
                    float p = 0;
                    try { p = std::stof(tok); } catch (...) { p = 0; }
                    comps[idx] = (int)std::lround(p * 255 / 100);
                } else {
                    try { comps[idx] = std::stoi(tok); } catch (...) { comps[idx] = 0; }
                }
                ++idx;
            }
            auto cl = [](int c) { return std::max(0, std::min(255, c)); };
            target.r = cl(comps[0]);
            target.g = cl(comps[1]);
            target.b = cl(comps[2]);
            target.a = cl(comps[3]);
            hasFlag = true;
        } else if (v.size() >= 4 && v.compare(0, 4, "rgb(") == 0 && v.back() == ')') {
            std::string body = v.substr(4, v.size() - 5);
            std::istringstream ss(body);
            std::string tok;
            int comps[3] = {0, 0, 0};
            int idx = 0;
            while (std::getline(ss, tok, ',') && idx < 3) {
                tok = trim(tok);
                try { comps[idx] = std::stoi(tok); } catch (...) { comps[idx] = 0; }
                ++idx;
            }
            auto cl = [](int c) { return std::max(0, std::min(255, c)); };
            target.r = cl(comps[0]);
            target.g = cl(comps[1]);
            target.b = cl(comps[2]);
            target.a = 255;
            hasFlag = true;
        } else if (v.size() >= 4 && v.compare(0, 4, "hsl(") == 0 && v.back() == ')') {
            // hsl(h, s%, l%) → RGB
            std::string body = v.substr(4, v.size() - 5);
            std::istringstream ss(body);
            std::string tok;
            float h = 0, sat = 0, li = 0;
            if (std::getline(ss, tok, ',')) { try { h = std::stof(tok); } catch (...) {} }
            if (std::getline(ss, tok, ',')) { try { sat = std::stof(tok); } catch (...) {} }
            if (std::getline(ss, tok, ',')) { try { li = std::stof(tok); } catch (...) {} }
            float cN = (1 - std::fabs(2 * li / 100 - 1)) * sat / 100;
            float hp = h / 60;
            float xN = cN * (1 - std::fabs(std::fmod(hp, 2) - 1));
            float r1=0,g1=0,b1=0;
            if      (hp < 1) { r1=cN; g1=xN; }
            else if (hp < 2) { r1=xN; g1=cN; }
            else if (hp < 3) { g1=cN; b1=xN; }
            else if (hp < 4) { g1=xN; b1=cN; }
            else if (hp < 5) { r1=xN; b1=cN; }
            else             { r1=cN; b1=xN; }
            float m = li / 100 - cN / 2;
            auto cl = [](float c) { return std::max(0, std::min(255, (int)std::lround(c * 255))); };
            target.r = cl(r1+m); target.g = cl(g1+m); target.b = cl(b1+m);
            target.a = 255;
            hasFlag = true;
        } else {
            SDL_Color nc = namedColor(v);
            if (nc.a > 0 || v == "transparent") {
                target = nc;
                hasFlag = true;
            }
        }
    };

    size_t pos = 0;
    while (pos < css.size()) {
        size_t semi = css.find(';', pos);
        if (semi == std::string::npos) semi = css.size();
        std::string decl = css.substr(pos, semi - pos);
        pos = semi + 1;

        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        std::string prop = toLower(trim(decl.substr(0, colon)));
        std::string val  = stripImportant(decl.substr(colon + 1));
        val = substituteVars(val);
        if (prop.empty() || val.empty()) continue;

        // CSS custom properties are registered by parseCSSRange (which
        // knows the selector) — applyStyle may be called with arbitrary
        // declaration lists whose owning rule must not leak its vars.
        if (prop.size() >= 2 && prop[0] == '-' && prop[1] == '-') {
            continue;
        }

        // visibility: hidden/collapse hide the subtree; visible re-shows.
        if (prop == "visibility") {
            std::string vv = toLower(trim(val));
            bool before2 = s.visibilityHidden;
            s.visibilityHidden = (vv == "hidden" || vv == "collapse");
            s.hasVisibility = true;
            if (before2 != s.visibilityHidden) s.declared |= B_VISIBILITY;
            if (getenv("MB_CLIPDBG") && s.visibilityHidden)
                std::fprintf(stderr, "[vis] set hidden by decl "
                                     "'visibility:%s'\n", vv.c_str());
        }

        // overflow: hidden/clip clip painted content to the box.
        // auto/scroll degrade to hidden (no scrollbars yet).
        if (prop == "overflow" || prop == "overflow-x" || prop == "overflow-y") {
            std::string vv = toLower(trim(val));
            s.overflowHidden = (vv == "hidden" || vv == "clip" ||
                                vv == "auto" || vv == "scroll");
            s.declared |= B_OVERFLOW;   // before the diff snapshot
        }
        // clip-path / clip: the sr-only/visually-hidden idioms produce a
        // zero-area clip — treat the element as fully hidden.
        //   clip-path: inset(50%) | rect(0 0 0 0)
        //   clip: rect(1px, 1px, 1px, 1px)  (visible w = right-left = 0)
        if (prop == "clip-path" || prop == "-webkit-clip-path" || prop == "clip") {
            std::string vv = toLower(trim(val));
            if (getenv("MB_CLIPDBG"))
                std::fprintf(stderr, "[clip] prop='%s' val='%s'\n",
                             prop.c_str(), vv.c_str());
            bool before2 = s.clippedAway;
            if (vv.empty() || vv == "none" || vv == "auto") {
                s.clippedAway = false;
            } else if (vv.rfind("inset", 0) == 0) {
                // inset(a b c d) — hide when the first side is >= 50%
                // (any pair of opposite sides then covers the whole box).
                float v1 = -1;
                if (std::sscanf(vv.c_str(), "inset(%f", &v1) == 1 && v1 >= 50.f)
                    s.clippedAway = true;
            } else if (vv.rfind("rect", 0) == 0) {
                float t = 0, r = 0, b = 0, l = 0;
                if (std::sscanf(vv.c_str(), "rect(%f%*[^0-9-]%f%*[^0-9-]%f%*[^0-9-]%f",
                                &t, &r, &b, &l) == 4) {
                    if (r - l <= 0 || b - t <= 0) s.clippedAway = true;
                } else if (vv.find("0") != std::string::npos &&
                           vv.find_first_not_of("rect(),0 .%emx-") ==
                               std::string::npos) {
                    // rect(0 0 0 0) space-separated zeros — all-zero => hidden
                    s.clippedAway = true;
                }
            }
            if (before2 != s.clippedAway) s.declared |= B_CLIP;
        }

        Style before = s;   // snapshot for declaration-bit diffing

        if (prop == "color") {
            applyColor(val, s.color, s.hasColor);
        } else if (prop == "background" || prop == "background-color") {
            // A background declaration may carry a color AND a url(...)
            // image: "background: #333 url(bg.png) no-repeat".
            std::string lowerVal = toLower(val);
            size_t urlPos = lowerVal.find("url(");
            std::string colorPart = val;
            if (urlPos != std::string::npos) {
                size_t open = urlPos + 4;
                size_t close = val.find(')', open);
                if (close != std::string::npos) {
                    std::string u = trim(val.substr(open, close - open));
                    if (u.size() >= 2 &&
                        (u.front() == '"' || u.front() == '\'') &&
                        u.back() == u.front()) {
                        u = u.substr(1, u.size() - 2);
                    }
                    if (!u.empty()) {
                        s.bgImageUrl = u;
                        s.hasBgImage = true;
                    }
                }
                colorPart = trim(val.substr(0, urlPos) + " " +
                                 val.substr(std::min(close + 1, val.size())));
            }
            colorPart = trim(colorPart);
            if (!colorPart.empty()) {
                // The surrounding keywords ("no-repeat", "fixed", ...)
                // aren't colors; try each token and keep the first that
                // actually parses as one.
                std::istringstream cs(colorPart);
                std::string tok;
                bool got = false;
                while (cs >> tok) {
                    SDL_Color probe = s.bg;
                    bool probeHas = false;
                    applyColor(tok, probe, probeHas);
                    if (probeHas) { s.bg = probe; got = true; break; }
                }
                if (got) s.hasBg = true;
            }
            // background: url(...) center / cover no-repeat
            if (lowerVal.find("/cover") != std::string::npos) s.bgSize = "cover";
            if (lowerVal.find("/contain") != std::string::npos) s.bgSize = "contain";
            if (lowerVal.find("no-repeat") != std::string::npos) s.bgRepeat = "no-repeat";
        } else if (prop == "background-image") {
            std::string lowerVal = toLower(val);
            size_t urlPos = lowerVal.find("url(");
            if (urlPos != std::string::npos) {
                size_t open = urlPos + 4;
                size_t close = val.find(')', open);
                if (close != std::string::npos) {
                    std::string u = trim(val.substr(open, close - open));
                    if (u.size() >= 2 &&
                        (u.front() == '"' || u.front() == '\'') &&
                        u.back() == u.front()) {
                        u = u.substr(1, u.size() - 2);
                    }
                    if (!u.empty()) {
                        s.bgImageUrl = u;
                        s.hasBgImage = true;
                    }
                }
            }
        } else if (prop == "background-size") {
            std::string v = toLower(val);
            if (v == "cover" || v == "contain") { s.bgSize = v; }
            else {
                std::istringstream ss(v);
                std::string a, b;
                if (ss >> a) {
                    s.bgSizeW = parseLengthInt(a, s.fontSize);
                    if (ss >> b) s.bgSizeH = parseLengthInt(b, s.fontSize);
                    if (s.bgSizeW > 0 || s.bgSizeH > 0) s.bgSize = "explicit";
                }
            }
        } else if (prop == "background-repeat") {
            std::string v = toLower(val);
            if (v == "no-repeat" || v == "repeat-x" || v == "repeat-y" || v == "repeat")
                s.bgRepeat = v;
        } else if (prop == "background-position") {
            std::string v = toLower(val);
            float px = 50, py = 50;
            std::istringstream ss(v);
            std::string a, b;
            auto kw = [](const std::string& t, float& out, bool isX) {
                if (t == "left")  { out = 0;   return true; }
                if (t == "right") { out = 100; return true; }
                if (t == "top")   { out = 0;   return true; }
                if (t == "bottom"){ out = 100; return true; }
                if (t == "center"){ out = 50;  return true; }
                try {
                    size_t end = 0;
                    float f = std::stof(t, &end);
                    out = (t.find('%') != std::string::npos) ? f : f;  // px treated loosely
                    (void)isX;
                    return true;
                } catch (...) { return false; }
            };
            if (ss >> a) { kw(a, px, true); if (ss >> b) kw(b, py, false); }
            s.bgPosX = px; s.bgPosY = py;
        } else if (prop == "font-size") {
            int px = parseLengthInt(val, s.fontSize);
            if (px > 0) { s.fontSize = px; s.hasFontSize = true; }
        } else if (prop == "font-weight") {
            std::string v = toLower(trim(val));
            if (v == "bold" || v == "bolder") { s.bold = true; s.hasBold = true; }
            else if (v == "normal") { s.bold = false; s.hasBold = true; }
            else if (!v.empty() && std::isdigit((unsigned char)v[0])) {
                try {
                    int w = std::stoi(v);
                    s.bold = (w >= 600);
                    s.hasBold = true;
                } catch (...) {}
            }
        } else if (prop == "font" ) {
            // font: <size>/<line-height> <family> — minimal parse.
            std::istringstream ss(val);
            std::string tok;
            if (ss >> tok) {
                float px = 0;
                if (parseLengthPx(tok, s.fontSize, px) && px > 0) {
                    s.fontSize = (int)std::lround(px);
                    s.hasFontSize = true;
                }
                std::string fam;
                while (ss >> tok) fam += tok + " ";
                fam = trim(fam);
                if (!fam.empty()) {
                    s.fontFamily = lower(fam);
                    s.hasFontFamily = true;
                }
            }
        } else if (prop == "line-height") {
            std::string v = trim(val);
            // Unitless number = multiplier of font size.
            bool allNum = !v.empty();
            for (char c : v)
                if (!(std::isdigit((unsigned char)c) || c == '.')) { allNum = false; break; }
            if (allNum) {
                try {
                    s.lineHeight = std::stof(v) * s.fontSize;
                    s.hasLineHeight = true;
                } catch (...) {}
            } else {
                float px = 0;
                if (parseLengthPx(v, s.fontSize, px) && px > 0) {
                    s.lineHeight = px;
                    s.hasLineHeight = true;
                }
            }
        } else if (prop == "text-transform") {
            std::string v = toLower(trim(val));
            if (v == "uppercase" || v == "lowercase" || v == "capitalize") {
                s.textTransform = v;
                s.hasTextTransform = true;
            }
        } else if (prop == "white-space") {
            std::string v = toLower(trim(val));
            if (v == "pre" || v == "pre-wrap" || v == "pre-line") {
                s.pre = true;
                s.hasPre = true;
            }
        } else if (prop == "font-style") {
            std::string v = toLower(trim(val));
            if (v == "italic" || v == "oblique") { s.italic = true; s.hasItalic = true; }
            if (v == "normal") { s.italic = false; s.hasItalic = true; }
        } else if (prop == "text-decoration") {
            std::string v = toLower(trim(val));
            if (v.find("underline") != std::string::npos) {
                s.underline = true; s.hasUnderline = true;
            } else if (v == "none") {
                s.underline = false; s.hasUnderline = true;
            }
            // Explicit bit: diffBits() only records fields that CHANGED, so
            // "text-decoration:none" on the default (no underline) would
            // otherwise go unnoticed and the UA link default would re-add
            // the underline (e.g. the YouTube shim's Play button).
            s.declared |= B_UNDERLINE;
        } else if (prop == "font-family") {
            // Parse the first family name out of a possibly comma-separated list.
            // Supports quoted names like 'DejaVu Sans Mono' or "Helvetica".
            std::string f = trim(val);
            char quote = 0;
            if (!f.empty() && (f.front() == '"' || f.front() == '\'')) {
                quote = f.front();
                f = f.substr(1);
            }
            if (quote) {
                auto end = f.find(quote);
                if (end != std::string::npos) f = f.substr(0, end);
            } else {
                auto comma = f.find(',');
                if (comma != std::string::npos) f = f.substr(0, comma);
            }
            f = trim(f);
            std::transform(f.begin(), f.end(), f.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (!f.empty()) {
                s.fontFamily = f;
                s.hasFontFamily = true;
            }
        } else if (prop == "margin" || prop == "margin-top" || prop == "margin-right" ||
                   prop == "margin-bottom" || prop == "margin-left") {
            int px4[4] = {s.margin.top, s.margin.right, s.margin.bottom, s.margin.left};
            float pct4[4] = {s.marginPct.top, s.marginPct.right,
                             s.marginPct.bottom, s.marginPct.left};
            if (prop == "margin") {
                if (parseEdgeListEx(val, s.fontSize, px4, pct4,
                                    &s.marginAutoLeft, &s.marginAutoRight) > 0) {
                    s.hasMargin = true;
                }
            } else {
                // Single side: build a 1-token list then take the right slot.
                int one[4] = {0,0,0,0};
                float pct1[4] = {0,0,0,0};
                bool aL = false, aR = false;
                if (parseEdgeListEx(val, s.fontSize, one, pct1, &aL, &aR) > 0) {
                    int slot = prop == "margin-top" ? 0 :
                               prop == "margin-right" ? 1 :
                               prop == "margin-bottom" ? 2 : 3;
                    px4[slot] = one[slot];
                    pct4[slot] = pct1[slot];
                    if (slot == 1 && aR) s.marginAutoRight = true;
                    if (slot == 3 && aL) s.marginAutoLeft = true;
                    s.hasMargin = true;
                }
            }
            s.margin.set4(px4[0], px4[1], px4[2], px4[3]);
            s.marginPct = PctEdges{pct4[0], pct4[1], pct4[2], pct4[3]};
        } else if (prop == "padding" || prop == "padding-top" || prop == "padding-right" ||
                   prop == "padding-bottom" || prop == "padding-left") {
            int px4[4] = {s.padding.top, s.padding.right, s.padding.bottom, s.padding.left};
            float pct4[4] = {s.paddingPct.top, s.paddingPct.right,
                             s.paddingPct.bottom, s.paddingPct.left};
            if (prop == "padding") {
                if (parseEdgeListEx(val, s.fontSize, px4, pct4) > 0) s.hasPadding = true;
            } else {
                int one[4] = {0,0,0,0};
                float pct1[4] = {0,0,0,0};
                if (parseEdgeListEx(val, s.fontSize, one, pct1) > 0) {
                    int slot = prop == "padding-top" ? 0 :
                               prop == "padding-right" ? 1 :
                               prop == "padding-bottom" ? 2 : 3;
                    px4[slot] = one[slot];
                    pct4[slot] = pct1[slot];
                    s.hasPadding = true;
                }
            }
            s.padding.set4(px4[0], px4[1], px4[2], px4[3]);
            s.paddingPct = PctEdges{pct4[0], pct4[1], pct4[2], pct4[3]};
        } else if (prop == "border" || prop == "border-top" || prop == "border-right" ||
                   prop == "border-bottom" || prop == "border-left") {
            // Shorthand: <width> <style> <color> in any order. "none"/"hidden"
            // means no border at all.
            std::string v = toLower(trim(val));
            if (v == "none" || v == "hidden" || v == "0") {
                if (prop == "border") s.border.set1(0);
                else if (prop == "border-top") s.border.top = 0;
                else if (prop == "border-right") s.border.right = 0;
                else if (prop == "border-bottom") s.border.bottom = 0;
                else s.border.left = 0;
                s.hasBorder = true;
            } else {
                // Walk tokens: first parseable length = width; first
                // parseable color = color. Styles (solid, dashed, ...) ignored.
                float w = -1;
                SDL_Color col{};
                bool gotCol = false;
                std::istringstream ss(v);
                std::string tok;
                while (ss >> tok) {
                    if (w < 0) {
                        float px = 0;
                        std::string probe = tok;
                        size_t i = 0;
                        while (i < probe.size() && (std::isdigit((unsigned char)probe[i]) ||
                               probe[i] == '.')) ++i;
                        if (i > 0 && parseLengthPx(probe, s.fontSize, px)) {
                            w = px;
                            continue;
                        }
                        if (tok == "thin") { w = 1; continue; }
                        if (tok == "medium") { w = 2; continue; }
                        if (tok == "thick") { w = 4; continue; }
                    }
                    if (!gotCol) {
                        SDL_Color probe{};
                        bool has = false;
                        applyColor(tok, probe, has);
                        if (has) { col = probe; gotCol = true; continue; }
                    }
                }
                int wi = w >= 0 ? (int)std::lround(w) : 1;
                if (prop == "border") {
                    s.border.set1(wi);
                    if (gotCol) {
                        s.borderColor = s.borderTopColor = s.borderRightColor =
                            s.borderBottomColor = s.borderLeftColor = col;
                        s.hasBorderColor = true;
                    }
                } else if (prop == "border-top") {
                    s.border.top = wi; if (gotCol) s.borderTopColor = col;
                } else if (prop == "border-right") {
                    s.border.right = wi; if (gotCol) s.borderRightColor = col;
                } else if (prop == "border-bottom") {
                    s.border.bottom = wi; if (gotCol) s.borderBottomColor = col;
                } else {
                    s.border.left = wi; if (gotCol) s.borderLeftColor = col;
                }
                s.hasBorder = true;
            }
        } else if (prop == "border-width" || prop == "border-top-width" ||
                   prop == "border-right-width" || prop == "border-bottom-width" ||
                   prop == "border-left-width") {
            float px = 0;
            if (parseLengthPx(val, s.fontSize, px) && px >= 0) {
                int n = (int)std::lround(px);
                if (prop == "border-width") s.border.set1(n);
                else if (prop == "border-top-width") s.border.top = n;
                else if (prop == "border-right-width") s.border.right = n;
                else if (prop == "border-bottom-width") s.border.bottom = n;
                else s.border.left = n;
                s.hasBorder = true;
            }
        } else if (prop == "border-color" || prop == "border-top-color" ||
                   prop == "border-right-color" || prop == "border-bottom-color" ||
                   prop == "border-left-color") {
            if (prop == "border-color") {
                applyColor(val, s.borderColor, s.hasBorderColor);
                s.borderTopColor = s.borderRightColor =
                    s.borderBottomColor = s.borderLeftColor = s.borderColor;
            } else {
                SDL_Color c = s.borderColor;
                bool has = s.hasBorderColor;
                applyColor(val, c, has);
                if (prop == "border-top-color") s.borderTopColor = c;
                else if (prop == "border-right-color") s.borderRightColor = c;
                else if (prop == "border-bottom-color") s.borderBottomColor = c;
                else s.borderLeftColor = c;
                s.hasBorderColor = true;
            }
        } else if (prop == "border-radius") {
            // Rounded corners are not rasterized; ignore silently.
        } else if (prop == "text-align") {
            std::string v = toLower(trim(val));
            if (v == "left" || v == "center" || v == "right" ||
                v == "start" || v == "end" || v == "justify") {
                s.textAlign = v;
                s.hasTextAlign = true;
            }
        } else if (prop == "direction") {
            std::string v = toLower(trim(val));
            if (v == "ltr" || v == "rtl" || v == "auto") {
                s.direction = v;
                s.hasDirection = true;
                s.declared |= B_DIRECTION;
            }
        } else if (prop == "grid-template-columns") {
            s.gridTemplateColumns = trim(val);
            s.hasGridTemplateColumns = true;
            s.declared |= B_GRID_TPL;
        } else if (prop == "grid-template-rows") {
            s.gridTemplateRows = trim(val);
            s.hasGridTemplateRows = true;
            s.declared |= B_GRID_TPL;
        } else if (prop == "grid-template") {
            // Shorthand: "<rows> / <cols>" and optionally quoted area
            // strings before/after. Split on the top-level '/'.
            int depth = 0; size_t slash = std::string::npos;
            for (size_t i = 0; i < val.size(); ++i) {
                char c = val[i];
                if (c == '(') ++depth;
                else if (c == ')') --depth;
                else if (c == '/' && depth == 0) { slash = i; break; }
            }
            std::string first = trim(val.substr(0, slash == std::string::npos ? val.size() : slash));
            std::string rest  = slash == std::string::npos ? "" : trim(val.substr(slash + 1));
            auto hasQuotes = [](const std::string& v) {
                return v.find('\'') != std::string::npos || v.find('"') != std::string::npos;
            };
            std::string cols = rest, rows = first, areas;
            if (hasQuotes(first)) {
                areas = first;
                rows = "";
            } else if (hasQuotes(rest)) {
                size_t q = rest.find('\'');
                if (q == std::string::npos) q = rest.find('"');
                cols = trim(rest.substr(0, q));
                areas = trim(rest.substr(q));
            }
            if (!cols.empty() && cols != "none") {
                s.gridTemplateColumns = cols;
                s.hasGridTemplateColumns = true;
            }
            if (!rows.empty() && rows != "none") {
                s.gridTemplateRows = rows;
                s.hasGridTemplateRows = true;
            }
            if (!areas.empty()) {
                s.gridTemplateAreas = areas;
                s.hasGridTemplateAreas = true;
            }
            s.declared |= B_GRID_TPL;
        } else if (prop == "grid-template-areas") {
            s.gridTemplateAreas = trim(val);
            s.hasGridTemplateAreas = true;
            s.declared |= B_GRID_AREAS;
        } else if (prop == "grid-area") {
            s.gridArea = trim(val);
            s.hasGridArea = true;
            // Numeric forms also carry line placement:
            //   "row / col" starts, "row / col / row-end",
            //   "row / col / row-end / col-end" (optionally "span N").
            // Named-area values keep the raw string for grid placement.
            {
                std::vector<std::string> parts;
                int depth = 0;
                std::string cur;
                for (char ch : s.gridArea) {
                    if (ch == '(') ++depth;
                    else if (ch == ')') --depth;
                    if (ch == '/' && depth == 0) {
                        parts.push_back(trim(cur)); cur.clear();
                    } else cur += ch;
                }
                parts.push_back(trim(cur));
                bool numeric = !parts.empty() && parts.size() <= 4;
                for (auto& p : parts)
                    if (p.empty() ||
                        (p != "auto" && p.find("span") != 0 &&
                         p[0] != '-' && p[0] != '+' && !isdigit((unsigned char)p[0]))) {
                        numeric = false; break;
                    }
                if (numeric && parts.size() >= 2) {
                    auto line = [](const std::string& t, int& start,
                                   int& end, int& span) {
                        std::string low = t;
                        for (auto& ch : low)
                            ch = (char)tolower((unsigned char)ch);
                        if (low == "auto") return;
                        if (low.rfind("span", 0) == 0) {
                            try { span = std::stoi(trim(low.substr(4))); }
                            catch (...) { span = 1; }
                            return;
                        }
                        try {
                            int n = std::stoi(low);
                            if (n > 0) start = n;
                            else if (n < 0) end = n;   // from the end
                        } catch (...) {}
                    };
                    line(parts[0], s.gridRowStartLine, s.gridRowEndLine,
                         s.gridRowSpan);
                    line(parts[1], s.gridColStartLine, s.gridColEndLine,
                         s.gridColSpan);
                    if (parts.size() >= 3)
                        line(parts[2], s.gridRowStartLine, s.gridRowEndLine,
                             s.gridRowSpan);
                    if (parts.size() >= 4)
                        line(parts[3], s.gridColStartLine, s.gridColEndLine,
                             s.gridColSpan);
                    s.declared |= B_GRID_POS;
                }
            }
            s.declared |= B_GRID_AREA;
        } else if (prop == "grid-column" || prop == "grid-row") {
            bool isCol = (prop == "grid-column");
            int& startLine = isCol ? s.gridColStartLine : s.gridRowStartLine;
            int& endLine   = isCol ? s.gridColEndLine   : s.gridRowEndLine;
            int& span      = isCol ? s.gridColSpan      : s.gridRowSpan;
            std::string v = trim(val);
            std::vector<std::string> parts;
            {
                int depth = 0;
                std::string cur;
                for (char ch : v) {
                    if (ch == '(') ++depth;
                    else if (ch == ')') --depth;
                    if (ch == '/' && depth == 0) {
                        parts.push_back(trim(cur)); cur.clear();
                    } else cur += ch;
                }
                parts.push_back(trim(cur));
            }
            auto line = [](const std::string& t, int& st, int& en, int& sp) {
                std::string low = t;
                for (auto& ch : low)
                    ch = (char)tolower((unsigned char)ch);
                if (low.empty() || low == "auto") return;
                if (low.rfind("span", 0) == 0) {
                    try { sp = std::stoi(trim(low.substr(4))); }
                    catch (...) { sp = 1; }
                    if (sp < 1) sp = 1;
                    return;
                }
                try {
                    int n = std::stoi(low);
                    if (n > 0) st = n;
                    else if (n < 0) en = n;
                } catch (...) {}
            };
            if (parts.size() >= 1)
                line(parts[0], startLine, endLine, span);
            if (parts.size() >= 2)
                line(parts[1], startLine, endLine, span);
            if (isCol) s.hasGridColumn = true; else s.hasGridRow = true;
            s.declared |= B_GRID_POS;
        } else if (prop == "display") {
            std::string v = toLower(trim(val));
            // flex/flex-ish, grid and table-* map onto our simpler model.
            if (v == "inline" || v == "block" || v == "none" || v == "flex" ||
                v == "inline-flex" || v == "inline-block" || v == "grid" ||
                v == "inline-grid" || v == "list-item" || v == "table" || v == "table-cell" ||
                v == "table-row" || v == "table-row-group" ||
                v == "table-header-group" || v == "table-footer-group") {
                s.display = v;
                s.hasDisplay = true;
            }
        } else if (prop == "position") {
            std::string v = toLower(trim(val));
            if (v == "static" || v == "relative" || v == "absolute" || v == "fixed" ||
                v == "sticky") {
                if (v == "sticky") v = "relative";  // closest approximation
                s.position = v;
                s.hasPosition = true;
            }
        } else if (prop == "top" || prop == "right" || prop == "bottom" || prop == "left") {
            float px = 0, pct = 0;
            if (parseValueParts(val, s.fontSize, px, pct)) {
                if      (prop == "top")    { s.offTop = (int)std::lround(px); s.offTopPct = pct; s.hasOffTop = true; }
                else if (prop == "right")  { s.offRight = (int)std::lround(px); s.offRightPct = pct; s.hasOffRight = true; }
                else if (prop == "bottom") { s.offBottom = (int)std::lround(px); s.offBottomPct = pct; s.hasOffBottom = true; }
                else                       { s.offLeft = (int)std::lround(px); s.offLeftPct = pct; s.hasOffLeft = true; }
            }
        } else if (prop == "float") {
            std::string v = toLower(trim(val));
            if (v == "left" || v == "right" || v == "none") {
                s.cssFloat = v;
                s.hasFloat = true;
            }
        } else if (prop == "flex-direction") {
            std::string v = toLower(trim(val));
            if (v == "row" || v == "column" || v == "row-reverse" || v == "column-reverse") {
                if (v == "row-reverse") v = "row";
                if (v == "column-reverse") v = "column";
                s.flexDirection = v;
            }
        } else if (prop == "justify-content") {
            std::string v = toLower(trim(val));
            if (v == "flex-start" || v == "center" || v == "space-between" ||
                v == "space-around" || v == "space-evenly" || v == "flex-end")
                s.justifyContent = v;
        } else if (prop == "align-items") {
            std::string v = toLower(trim(val));
            if (v == "stretch" || v == "center" || v == "flex-start" || v == "flex-end" ||
                v == "baseline")
                s.alignItems = v;
        } else if (prop == "gap" || prop == "row-gap" || prop == "column-gap") {
            float px = 0;
            if (parseLengthPx(val, s.fontSize, px) && px >= 0) {
                s.flexGap = (int)std::lround(px);
                s.hasFlexGap = true;
            }
        } else if (prop == "box-sizing") {
            std::string v = toLower(trim(val));
            if (v == "border-box" || v == "content-box") {
                s.boxSizing = v;
                s.hasBoxSizing = true;
            }
        } else if (prop == "object-fit") {
            std::string v = toLower(trim(val));
            if (v == "fill" || v == "contain" || v == "cover") {
                s.objectFit = v;
                s.hasObjectFit = true;
            }
        } else if (prop == "opacity") {
            std::string v = trim(val);
            try {
                float f = std::stof(v);
                if (v.back() == '%') f /= 100.0f;
                s.opacity = std::max(0.0f, std::min(1.0f, f));
                s.hasOpacity = true;
            } catch (...) {}
        } else if (prop == "list-style" || prop == "list-style-type") {
            std::string v = toLower(trim(val));
            if (v.find("none") != std::string::npos) {
                s.listStyleType = "none";
                s.hasListStyle = true;
            }
        } else if (prop == "vertical-align") {
            std::string v = toLower(trim(val));
            if (v == "top" || v == "middle" || v == "bottom" || v == "baseline") {
                s.verticalAlign = v;
                s.hasVerticalAlign = true;
            }
        } else if (prop == "width") {
            float px = 0, pct = 0;
            if (trim(lower(val)) == "auto") { s.hasWidth = false; s.hasWidthPct = false; s.width = -1; s.widthPct = 0; }
            else if (parseValueParts(val, s.fontSize, px, pct)) {
                s.width = (int)std::lround(px);
                s.widthPct = pct;
                s.hasWidth = s.width >= 0;
                s.hasWidthPct = pct != 0;
            }
        } else if (prop == "height") {
            float px = 0, pct = 0;
            if (trim(lower(val)) == "auto") { s.hasHeight = false; s.hasHeightPct = false; s.height = -1; s.heightPct = 0; }
            else if (parseValueParts(val, s.fontSize, px, pct)) {
                s.height = (int)std::lround(px);
                s.heightPct = pct;
                s.hasHeight = s.height >= 0;
                s.hasHeightPct = pct != 0;
            }
        } else if (prop == "max-width") {
            float px = 0, pct = 0;
            if (trim(lower(val)) == "none") { s.hasMaxWidth = false; s.hasMaxWidthPct = false; s.maxWidth = -1; s.maxWidthPct = 0; }
            else if (parseValueParts(val, s.fontSize, px, pct)) {
                s.maxWidth = (int)std::lround(px);
                s.maxWidthPct = pct;
                s.hasMaxWidth = s.maxWidth >= 0;
                s.hasMaxWidthPct = pct != 0;
            }
        } else if (prop == "min-width") {
            float px = 0, pct = 0;
            if (parseValueParts(val, s.fontSize, px, pct)) {
                s.minWidth = (int)std::lround(px);
                s.minWidthPct = pct;
                s.hasMinWidth = s.minWidth >= 0;
                s.hasMinWidthPct = pct != 0;
            }
        } else if (prop == "max-height") {
            float px = 0, pct = 0;
            if (trim(lower(val)) == "none") { s.hasMaxHeight = false; s.maxHeight = -1; }
            else if (parseValueParts(val, s.fontSize, px, pct)) {
                s.maxHeight = (int)std::lround(px);
                s.hasMaxHeight = s.maxHeight >= 0;
            }
        } else if (prop == "min-height") {
            float px = 0, pct = 0;
            if (parseValueParts(val, s.fontSize, px, pct)) {
                s.minHeight = (int)std::lround(px);
                s.hasMinHeight = s.minHeight >= 0;
            }
        }

        // Record which properties this declaration actually changed so the
        // cascade (mergeDeclared / computeStyle) applies exactly those.
        s.declared |= diffBits(before, s);
    }

    return s;
}

// ---------------------------------------------------------------------------
// @media condition evaluation
// ---------------------------------------------------------------------------

// Evaluate one "(feature: value)" or media-type term.
static bool mediaFeatureMatches(const std::string& rawTerm, int vw) {
    std::string term = trim(rawTerm);
    if (term.empty()) return true;
    bool inParen = term.front() == '(';
    if (inParen && term.back() == ')') term = trim(term.substr(1, term.size() - 2));
    std::string t = lower(term);

    // Media types.
    if (t == "screen" || t == "all" || t == "only screen" || t == "tv") return true;
    if (t == "print" || t == "speech" || t == "tty" || t == "handheld") return false;

    auto numPx = [](const std::string& v) -> int {
        std::string s = trim(v);
        float mult = 1;
        if (s.size() >= 2 && s.substr(s.size()-2) == "em") { s = s.substr(0, s.size()-2); mult = 16; }
        else if (s.size() >= 3 && s.substr(s.size()-3) == "rem") { s = s.substr(0, s.size()-3); mult = 16; }
        try { return (int)std::lround(std::stof(s) * mult); } catch (...) { return -1; }
    };

    auto feature = [&](const std::string& name, const std::string& value,
                       bool& handled) -> bool {
        handled = true;
        if (name == "min-width")  { int n = numPx(value); return n < 0 ? true : vw >= n; }
        if (name == "max-width")  { int n = numPx(value); return n < 0 ? true : vw <= n; }
        if (name == "width")      { int n = numPx(value); return n < 0 ? true : vw == n; }
        if (name == "min-height") { return true; }   // height unknown → permissive
        if (name == "max-height") { return true; }
        if (name == "orientation") return lower(value) != "portrait";
        if (name == "hover")       return true;
        if (name == "pointer")     return true;
        if (name == "any-hover")   return true;
        if (name == "any-pointer") return true;
        if (name == "prefers-color-scheme") return lower(value) != "dark";
        if (name == "prefers-reduced-motion") return true;
        if (name == "display-mode") return true;
        if (name == "resolution")  return true;
        if (name == "-webkit-min-device-pixel-ratio") return true;
        if (name == "min-resolution") return true;
        if (name == "grid")        return true;
        if (name == "color")       return true;
        handled = false;
        return true;   // unknown features: don't block the sheet
    };

    size_t colon = t.find(':');
    if (colon != std::string::npos) {
        std::string name = trim(t.substr(0, colon));
        std::string value = trim(t.substr(colon + 1));
        bool handled = false;
        bool r = feature(name, value, handled);
        return r;
    }
    // Bare feature without value (e.g. "(hover)") → permissive.
    return true;
}

// Full condition: "screen and (min-width: 700px) and (max-width: 900px)",
// comma-separated lists act as OR; leading "not " inverts.
static bool mediaQueryMatches(const std::string& cond, int vw) {
    if (cond.empty()) return true;
    // OR over comma-separated queries.
    size_t start = 0;
    while (start <= cond.size()) {
        size_t comma = cond.find(',', start);
        std::string q = trim(cond.substr(start,
                comma == std::string::npos ? std::string::npos : comma - start));
        if (!q.empty()) {
            bool invert = false;
            std::string lq = lower(q);
            if (lq.compare(0, 4, "not ") == 0) { invert = true; q = trim(q.substr(4)); }
            // AND over " and "-separated terms.
            bool all = true;
            size_t p = 0;
            std::string lowerQ = lower(q);
            while (p <= q.size()) {
                size_t a = lowerQ.find(" and ", p);
                std::string term = trim(q.substr(p,
                        a == std::string::npos ? std::string::npos : a - p));
                if (!term.empty() && !mediaFeatureMatches(term, vw)) { all = false; break; }
                if (a == std::string::npos) break;
                p = a + 5;
            }
            bool res = invert ? !all : all;
            if (res) return true;   // OR: one matching query is enough
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

bool mediaConditionMatches(const std::string& cond) {
    return mediaQueryMatches(cond, mediaViewportWidth());
}

// ---------------------------------------------------------------------------
// parseCSS with at-rule support and specificity
// ---------------------------------------------------------------------------

// Compute specificity for a selector: ids*10000 + classes*100 + elements.
// Counts .class, [attr], :pseudo-class as classes; :pseudo-element as element.
static int selectorSpecificity(const std::string& sel) {
    int ids = 0, classes = 0, elems = 0;
    size_t i = 0;
    bool atTokenStart = true;
    while (i < sel.size()) {
        char c = sel[i];
        if (c == '#') { ++ids; ++i; while (i < sel.size() &&
            (std::isalnum((unsigned char)sel[i]) || sel[i]=='-' || sel[i]=='_')) ++i;
            atTokenStart = false; continue; }
        if (c == '.') { ++classes; ++i; while (i < sel.size() &&
            (std::isalnum((unsigned char)sel[i]) || sel[i]=='-' || sel[i]=='_')) ++i;
            atTokenStart = false; continue; }
        if (c == '[') { ++classes; ++i; while (i < sel.size() && sel[i] != ']') ++i; ++i;
            atTokenStart = false; continue; }
        if (c == ':') {
            bool doubleColon = (i + 1 < sel.size() && sel[i+1] == ':');
            if (doubleColon) { ++elems; i += 2; }
            else { ++classes; ++i; }
            while (i < sel.size() &&
                   (std::isalnum((unsigned char)sel[i]) || sel[i]=='-' || sel[i]=='_' ||
                    sel[i]=='(' )) { if (sel[i]=='(') { int d=1; ++i; while (i<sel.size()&&d){ if(sel[i]=='(')++d; else if(sel[i]==')')--d; ++i;} break; } ++i; }
            atTokenStart = false;
            continue;
        }
        if (std::isalpha((unsigned char)c) && atTokenStart) {
            ++elems;
            while (i < sel.size() &&
                   (std::isalnum((unsigned char)sel[i]) || sel[i]=='-' || sel[i]=='_')) ++i;
            atTokenStart = false;
            continue;
        }
        if (c == ' ' || c == '>' || c == '+' || c == '~' || c == ',') atTokenStart = true;
        ++i;
    }
    return ids * 10000 + classes * 100 + elems;
}

namespace {
// Skip a brace block starting at `openBrace` (which points at '{').
// Returns index just after the matching '}'.
size_t skipBraceBlock(const std::string& css, size_t openBrace) {
    int depth = 0;
    for (size_t i = openBrace; i < css.size(); ++i) {
        if (css[i] == '{') ++depth;
        else if (css[i] == '}') {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return css.size();
}
} // namespace

// Forward decl for mutual recursion.
static void parseCSSRange(const std::string& css, size_t start, size_t end,
                          const std::string& media,
                          std::vector<CSSRule>& out);

// ---------------------------------------------------------------------------
// Selector fast-match aids
// ---------------------------------------------------------------------------

// Defined further below with the selector-matching code.
static std::vector<std::string> tokenizeSelector(const std::string& sel);

// True when a selector scopes declarations to the document root, i.e.
// its custom properties can safely be treated as global defaults.
static bool ruleIsRootScoped(const std::string& sel) {
    auto toks = tokenizeSelector(sel);
    std::string compound;
    for (int i = (int)toks.size() - 1; i >= 0; --i) {
        const std::string& t = toks[i];
        if (t == " " || t == ">" || t == "+" || t == "~") continue;
        compound = t;
        break;
    }
    std::string c = lower(trim(compound));
    if (c == "*" || c == ":root" || c == ":host" || c == "html" ||
        c == "body") return true;
    // ":root ..." / "html ..." descendant forms (e.g. ":root { --x }"
    // with vendor noise) also count when the root compound is bare.
    return false;
}

// Split one compound selector ("div.foo#bar[title=a]:hover") into its
// pre-parsed parts so matching never allocates. See SimpleSel in style.h.
static SimpleSel parseSimpleSel(const std::string& compound) {
    SimpleSel ps;
    size_t i = 0;
    const size_t n = compound.size();
    auto isName = [](char c) {
        return std::isalnum((unsigned char)c) || c == '-' || c == '_';
    };
    // Leading type selector.
    if (i < n && (std::isalpha((unsigned char)compound[i]) || compound[i] == '_')) {
        size_t e = i;
        while (e < n && isName(compound[e])) ++e;
        ps.tag = lower(compound.substr(i, e - i));
        i = e;
    }
    while (i < n) {
        char c = compound[i];
        if (c == '#') {
            size_t e = ++i;
            while (e < n && isName(compound[e])) ++e;
            if (e > i) ps.id = compound.substr(i, e - i);
            i = e;
        } else if (c == '.') {
            size_t e = ++i;
            while (e < n && isName(compound[e])) ++e;
            if (e > i) ps.classes.push_back(compound.substr(i, e - i));
            i = e;
        } else if (c == '[') {
            size_t e = compound.find(']', i);
            if (e == std::string::npos) break;
            std::string body = compound.substr(i + 1, e - i - 1);
            AttrCheck ac;
            ac.op = 0;
            static const char* ops[] = {"~=", "^=", "$=", "*=", "|=", "=", nullptr};
            size_t opPos = std::string::npos;
            const char* opStr = nullptr;
            for (int k = 0; ops[k]; ++k) {
                size_t p = body.find(ops[k]);
                if (p != std::string::npos) { opPos = p; opStr = ops[k]; break; }
            }
            if (opStr) {
                ac.name = lower(trim(body.substr(0, opPos)));
                ac.op = opStr[0];
                ac.value = trim(body.substr(opPos + std::strlen(opStr)));
                if (ac.value.size() >= 2 &&
                    (ac.value.front() == '"' || ac.value.front() == '\''))
                    ac.value = ac.value.substr(1, ac.value.size() - 2);
            } else {
                ac.name = lower(trim(body));
            }
            ps.attrs.push_back(std::move(ac));
            i = e + 1;
        } else if (c == ':') {
            size_t e = i;
            while (e < n && compound[e] == ':') ++e;
            size_t ns = e;
            while (e < n && (std::isalnum((unsigned char)compound[e]) ||
                             compound[e] == '-')) ++e;
            std::string name = lower(compound.substr(ns, e - ns));
            std::string arg;
            if (e < n && compound[e] == '(') {
                int depth = 1;
                size_t as = ++e;
                while (e < n && depth > 0) {
                    if (compound[e] == '(') ++depth;
                    else if (compound[e] == ')') { --depth; if (depth == 0) break; }
                    ++e;
                }
                arg = compound.substr(as, e - as);
                if (e < n) ++e;   // closing ')'
            }
            if (!name.empty()) ps.pseudos.push_back({name, arg});
            i = e;
        } else {
            ++i;
        }
    }
    return ps;
}

// Extract a cheap pre-filter key from the selector's rightmost compound
// (the part that must match the node itself): its tag, class, or id.
// computeStyle uses it to skip the full combinator walk for rules that
// cannot possibly match, which turns stylesheet matching from
// O(nodes x rules x parse) into a hash of cheap string compares.
static SelectorKey computeSelectorKey(const std::string& sel) {
    SelectorKey k;
    auto toks = tokenizeSelector(sel);
    std::string compound;
    for (int i = (int)toks.size() - 1; i >= 0; --i) {
        const std::string& t = toks[i];
        if (t == " " || t == ">" || t == "+" || t == "~") continue;
        compound = t;
        break;
    }
    if (compound.empty()) return k;
    // Rightmost pseudo-elements (::before, ::placeholder, ::-webkit-*)
    // never render in this engine — drop them from candidacy entirely.
    if (compound.rfind("::", 0) == 0) { k.kind = 'n'; return k; }
    {
        static const char* kLegacyPseudoElems[] = {
            "before", "after", "first-line", "first-letter", "placeholder",
            "selection", "marker", "backdrop", "file-selector-button"
        };
        size_t c0 = compound.find(':');
        if (c0 == 0) {
            std::string nm = lower(compound.substr(1));
            size_t paren = nm.find('(');
            if (paren != std::string::npos) nm = nm.substr(0, paren);
            for (const char* pe : kLegacyPseudoElems)
                if (nm == pe) { k.kind = 'n'; return k; }
            // :is() / :not() / :where() with empty arguments never match.
            size_t op = compound.find('('), cp = compound.find(')');
            if (op != std::string::npos && cp == op + 1) {
                std::string fn = lower(compound.substr(1, op - 1));
                if (fn == "is" || fn == "not" || fn == "where") {
                    k.kind = 'n';
                    return k;
                }
            }
        }
    }
    // :root matches exactly the document root — key it so the other
    // N-thousand nodes never visit these rules.
    if (lower(compound).rfind(":root", 0) == 0) {
        k.kind = 'r';
        k.key = "root";
        return k;
    }
    // Interactive pseudo-classes on the rightmost compound only ever
    // apply while the user hovers/focuses — static layout must not pay
    // for them (primer alone ships ~1500 :hover/:focus selectors).
    {
        static const char* kInteractive[] = {
            "hover", "focus", "active", "visited", "target", "focus-visible",
            "focus-within", "enabled", "disabled", "checked", "indeterminate"
        };
        size_t cpos = compound.find(':');
        if (cpos != std::string::npos) {
            std::string pseudo = lower(compound.substr(cpos + 1));
            // strip a second colon (pseudo-element) and any argument
            size_t paren = pseudo.find('(');
            if (paren != std::string::npos) pseudo = pseudo.substr(0, paren);
            for (const char* ip : kInteractive) {
                if (pseudo == ip ||
                    pseudo.rfind(std::string(ip) + ":", 0) == 0) {
                    k.kind = 'h';
                    k.key = ip;
                    return k;
                }
            }
        }
    }
    // Drop pseudo-classes/elements: "a:hover" -> "a", "li.x::marker" -> "li.x".
    size_t colon = compound.find(':');
    std::string base = (colon == std::string::npos) ? compound
                                                    : compound.substr(0, colon);
    if (base.empty()) return k;

    auto isNameChar = [](char c) {
        return std::isalnum((unsigned char)c) || c == '-' || c == '_';
    };

    // Leading type selector: "div.foo" -> tag key.
    if (std::isalpha((unsigned char)base[0]) || base[0] == '_') {
        size_t e = 0;
        while (e < base.size() && isNameChar(base[e])) ++e;
        k.kind = 't';
        k.key = lower(base.substr(0, e));
        return k;
    }
    // #id — most selective.
    size_t hash = base.find('#');
    if (hash != std::string::npos) {
        size_t e = hash + 1;
        while (e < base.size() && isNameChar(base[e])) ++e;
        k.kind = 'i';
        k.key = base.substr(hash + 1, e - hash - 1);
        return k;
    }
    // .class
    size_t dot = base.find('.');
    if (dot != std::string::npos) {
        size_t e = dot + 1;
        while (e < base.size() && isNameChar(base[e])) ++e;
        k.kind = 'c';
        k.key = base.substr(dot + 1, e - dot - 1);
        return k;
    }
    // Attribute-only ("[data-x=v]", "[data-x][data-y]"): key by the
    // FIRST attribute name so these rules only get visited by nodes that
    // actually carry the attribute. Github/primer stylesheets ship tens
    // of thousands of such rules; as universal candidates they turned
    // every computeStyle call into a 15k-rule scan.
    size_t br = base.find('[');
    if (br != std::string::npos) {
        size_t e = br + 1;
        while (e < base.size() && (std::isalnum((unsigned char)base[e]) ||
                                   base[e] == '-' || base[e] == '_' ||
                                   base[e] == ':')) ++e;
        if (e > br + 1) {
            k.kind = 'a';
            k.key = lower(base.substr(br + 1, e - br - 1));
            return k;
        }
    }
    // "*", bare pseudo — candidate for every node.
    return k;
}

// Tokenize every selector once and derive its pre-filter key, so per-node
// matching never re-parses selector strings.
static void fillMatchAids(CSSRule& rule) {
    rule.selTokens.resize(rule.selectors.size());
    rule.selKeys.resize(rule.selectors.size());
    rule.selItems.resize(rule.selectors.size());
    for (size_t si = 0; si < rule.selectors.size(); ++si) {
        rule.selTokens[si] = tokenizeSelector(rule.selectors[si]);
        rule.selKeys[si] = computeSelectorKey(rule.selectors[si]);
        auto& items = rule.selItems[si];
        items.reserve(rule.selTokens[si].size());
        for (const auto& t : rule.selTokens[si]) {
            SelItem it;
            if (t == " " || t == ">" || t == "+" || t == "~") {
                it.isComb = true;
                it.comb = t[0];
            } else {
                it.simple = parseSimpleSel(t);
            }
            items.push_back(std::move(it));
        }
    }
}

static void parseCSSRange(const std::string& css, size_t start, size_t end,
                          const std::string& media,
                          std::vector<CSSRule>& out) {
    size_t i = start;
    while (i < end) {
        while (i < end && std::isspace((unsigned char)css[i])) i++;
        if (i >= end) break;

        if (css[i] == '@') {
            // At-rule.
            size_t wordEnd = i + 1;
            while (wordEnd < end && (std::isalnum((unsigned char)css[wordEnd]) ||
                                     css[wordEnd] == '-')) ++wordEnd;
            std::string at = lower(css.substr(i + 1, wordEnd - (i + 1)));
            size_t brace = css.find('{', wordEnd);
            size_t semi = css.find(';', wordEnd);
            if (at == "media" || at == "supports" || at == "document" ||
                at == "-moz-document" || at == "layer") {
                if (brace == std::string::npos || brace > end) break;
                size_t after = skipBraceBlock(css, brace);
                std::string cond = trim(css.substr(wordEnd, brace - wordEnd));
                std::string childMedia = media.empty()
                    ? cond : (media + " and " + cond);
                // @media: evaluate now if fully static, else defer to rule
                // records (we store the condition on the rules).
                if (at == "media") {
                    parseCSSRange(css, brace + 1, after - 1, childMedia, out);
                } else {
                    // @supports / @document: assume satisfied, recurse.
                    parseCSSRange(css, brace + 1, after - 1, media, out);
                }
                i = after;
            } else if (at == "keyframes" || at == "-webkit-keyframes" ||
                       at == "font-face" || at == "counter-style" ||
                       at == "property" || at == "font-feature-values") {
                if (brace == std::string::npos || brace > end) break;
                i = skipBraceBlock(css, brace);   // skip entirely
            } else {
                // @import/@charset/@namespace/unknown statement: skip to ';'
                // (or to '{' block for safety).
                if (semi != std::string::npos && (brace == std::string::npos || semi < brace)) {
                    i = semi + 1;
                } else if (brace != std::string::npos && brace < end) {
                    i = skipBraceBlock(css, brace);
                } else {
                    break;
                }
            }
            continue;
        }

        size_t brace = css.find('{', i);
        if (brace == std::string::npos || brace >= end) break;

        std::string selectorStr = trim(css.substr(i, brace - i));
        size_t close = css.find('}', brace);
        if (close == std::string::npos || close >= end) {
            // Unterminated rule: take the rest.
            close = end;
        }
        std::string body = css.substr(brace + 1, close - (brace + 1));
        i = close + 1;

        CSSRule rule;
        std::istringstream ss(selectorStr);
        std::string sel;
        while (std::getline(ss, sel, ',')) {
            sel = trim(sel);
            if (sel.empty()) continue;
            rule.selectors.push_back(sel);
            rule.specificities.push_back(selectorSpecificity(sel));
        }
        fillMatchAids(rule);

        // Custom properties: only rules scoped to the document root
        // (":root", "html", "*") register their vars globally. Rules like
        // "html.skin-theme-clientpref-night { --color-base: ... }" must
        // NOT pollute the table when the element doesn't match — var()
        // then falls back to the light/default values.
        bool registersVars = false;
        for (const auto& rs : rule.selectors) {
            if (ruleIsRootScoped(rs)) { registersVars = true; break; }
        }
        // Media-gated blocks (e.g. "@media (prefers-color-scheme: dark)
        // { :root { --dark-vars } }") must not register when the
        // condition doesn't hold — the dark theme vars would otherwise
        // override the light defaults everywhere.
        if (registersVars && !media.empty() &&
            !mediaConditionMatches(media)) {
            registersVars = false;
        }
        if (registersVars) {
            size_t bp = 0;
            while (bp < body.size()) {
                size_t bsemi = body.find(';', bp);
                if (bsemi == std::string::npos) bsemi = body.size();
                std::string bdecl = body.substr(bp, bsemi - bp);
                bp = bsemi + 1;
                size_t bcolon = bdecl.find(':');
                if (bcolon == std::string::npos) continue;
                std::string bprop = lower(trim(bdecl.substr(0, bcolon)));
                if (bprop.size() >= 2 && bprop[0] == '-' && bprop[1] == '-') {
                    std::string bval = stripImportant(bdecl.substr(bcolon + 1));
                    bval = substituteVars(bval);
                    if (!bval.empty()) setCustomProperty(bprop, bval);
                }
            }
        }

        Style s;
        std::istringstream ds(body);
        std::string decl;
        while (std::getline(ds, decl, ';')) {
            s = applyStyle(s, decl);
        }
        rule.style = s;
        rule.media = media;

        if (!rule.selectors.empty()) out.push_back(std::move(rule));
    }
}

// Stable sort by specificity (ascending) so later rules with equal
// specificity keep winning, while more specific rules always win.
static void sortRulesBySpecificity(std::vector<CSSRule>& rules) {
    std::stable_sort(rules.begin(), rules.end(),
                     [](const CSSRule& a, const CSSRule& b) {
                         int amax = 0, bmax = 0;
                         for (int s : a.specificities) amax = std::max(amax, s);
                         for (int s : b.specificities) bmax = std::max(bmax, s);
                         return amax < bmax;
                     });
}

std::vector<CSSRule> parseCSS(const std::string& css) {
    std::vector<CSSRule> rules;
    parseCSSRange(css, 0, css.size(), "", rules);
    sortRulesBySpecificity(rules);
    ++g_parseGeneration;
    return rules;
}

uint64_t parseCSSGeneration() { return g_parseGeneration; }

// ---------------------------------------------------------------------------
// Selector matching (with attribute selectors and structural pseudo-classes)
// ---------------------------------------------------------------------------

// Element children only (text nodes don't count for :first-child etc).
static size_t elementIndexAmongSiblings(const std::shared_ptr<Node>& node) {
    auto p = node->parent.lock();
    if (!p) return 0;
    size_t idx = 0;
    for (auto& c : p->children) {
        if (c.get() == node.get()) break;
        if (c->tag != "text") ++idx;
    }
    return idx;   // 0-based index among element siblings
}

static size_t elementCountAmongSiblings(const std::shared_ptr<Node>& node) {
    auto p = node->parent.lock();
    if (!p) return 1;
    size_t n = 0;
    for (auto& c : p->children)
        if (c->tag != "text") ++n;
    return std::max<size_t>(n, 1);
}

// Evaluate one structural pseudo-class (name without colon) against node.
static bool matchPseudoClass(const std::string& name,
                             const std::string& arg,
                             const std::shared_ptr<Node>& node) {
    std::string n = lower(trim(name));
    if (n == "first-child") return elementIndexAmongSiblings(node) == 0;
    if (n == "last-child")  return elementIndexAmongSiblings(node) + 1 ==
                                   elementCountAmongSiblings(node);
    if (n == "only-child")  return elementCountAmongSiblings(node) == 1;
    if (n == "empty") {
        for (auto& c : node->children) {
            if (c->tag != "text") return false;
            if (!trim(c->text).empty()) return false;
        }
        return true;
    }
    if (n == "root") return node->tag == "html";
    if (n == "nth-child" || n == "nth-last-child") {
        size_t idx = elementIndexAmongSiblings(node);
        size_t count = elementCountAmongSiblings(node);
        if (n == "nth-last-child") idx = count - 1 - idx;   // from the end
        std::string a = lower(trim(arg));
        if (a.empty()) return false;
        if (a == "odd")  return idx % 2 == 0;
        if (a == "even") return idx % 2 == 1;
        // Patterns: "5", "2n", "2n+1", "-n+3", "n"
        size_t npos = a.find('n');
        if (npos == std::string::npos) {
            try { return idx + 1 == (size_t)std::stoi(a); } catch (...) { return false; }
        }
        int A = 1;
        std::string aPart = trim(a.substr(0, npos));
        if (aPart.empty() || aPart == "+") A = 1;
        else if (aPart == "-") A = -1;
        else { try { A = std::stoi(aPart); } catch (...) { A = 1; } }
        int B = 0;
        if (npos + 1 < a.size()) {
            std::string bPart = trim(a.substr(npos + 1));
            if (!bPart.empty()) {
                try { B = std::stoi(bPart); } catch (...) { B = 0; }
            }
        }
        // idx+1 = A*k + B for some k >= 0
        int target = (int)idx + 1 - B;
        if (A == 0) return target == 0;
        if ((target % A) != 0) return false;
        return target / A >= 0;
    }
    // Unknown pseudo (e.g. :focus, :checked, :active, :visited) — don't match
    // so rules behave like browsers do for non-applicable states.
    return false;
}

// Match attribute selector "[attr]", "[attr=v]", "[attr^=v]", "[attr$=v]",
// "[attr*=v]", "[attr~=v]", "[attr|=v]".
static bool matchAttrSelector(const std::string& attrSel,
                              const std::map<std::string, std::string>& attrs) {
    std::string body = trim(attrSel);
    if (!body.empty() && body.front() == '[') body = trim(body.substr(1));
    if (!body.empty() && body.back() == ']') body = trim(body.substr(0, body.size() - 1));
    // operators: ~= ^= $= *= |= =
    size_t opPos = std::string::npos;
    std::string op;
    static const char* ops[] = {"~=", "^=", "$=", "*=", "|=", "=", nullptr};
    for (int k = 0; ops[k]; ++k) {
        size_t p = body.find(ops[k]);
        if (p != std::string::npos) { op = ops[k]; opPos = p; break; }
    }
    if (op.empty()) {
        // bare [attr] presence check
        std::string name = lower(trim(body));
        return attrs.count(name) > 0;
    }
    std::string name = lower(trim(body.substr(0, opPos)));
    std::string val = trim(body.substr(opPos + op.size()));
    if (val.size() >= 2 && (val.front() == '"' || val.front() == '\''))
        val = val.substr(1, val.size() - 2);
    auto it = attrs.find(name);
    if (it == attrs.end()) return false;
    const std::string& actual = it->second;
    if (op == "=")  return actual == val;
    if (op == "^=") return actual.size() >= val.size() &&
                           actual.compare(0, val.size(), val) == 0;
    if (op == "$=") return actual.size() >= val.size() &&
                           actual.compare(actual.size() - val.size(), val.size(), val) == 0;
    if (op == "*=") return actual.find(val) != std::string::npos;
    if (op == "~=") {
        std::istringstream ss(actual);
        std::string tok;
        while (ss >> tok) if (tok == val) return true;
        return false;
    }
    if (op == "|=") return actual == val ||
                           (actual.size() > val.size() &&
                            actual.compare(0, val.size(), val) == 0 &&
                            actual[val.size()] == '-');
    return false;
}

bool matchesSelector(const std::string& selector,
                     const std::string& tag,
                     const std::map<std::string, std::string>& attrs) {
    std::string sel = lower(trim(selector));
    std::string t = lower(tag);

    // Split off attribute selectors and treat them separately: they can
    // appear anywhere in the compound selector.
    size_t pos = 0;
    bool attrOk = true;
    std::string rest;
    while (pos < sel.size()) {
        if (sel[pos] == '[') {
            size_t close = sel.find(']', pos);
            if (close == std::string::npos) { attrOk = false; break; }
            if (!matchAttrSelector(sel.substr(pos, close - pos + 1), attrs))
                attrOk = false;
            pos = close + 1;
        } else {
            rest += sel[pos];
            ++pos;
        }
    }
    if (!attrOk) return false;

    sel = trim(rest);
    size_t dot  = sel.find('.');
    size_t hash = sel.find('#');

    std::string selTag = sel.substr(0, std::min({sel.size(), dot, hash}));
    std::string selClass, selId;

    if (dot != std::string::npos) {
        selClass = sel.substr(dot + 1);
        // Stop at any of " .#" — a class name is a single identifier.
        size_t sp = selClass.find_first_of(" .#");
        if (sp != std::string::npos) selClass = selClass.substr(0, sp);
    }
    if (hash != std::string::npos) {
        selId = sel.substr(hash + 1);
        size_t sp = selId.find_first_of(" .#");
        if (sp != std::string::npos) selId = selId.substr(0, sp);
    }

    if (!selTag.empty() && selTag != "*" && selTag != t) return false;

    if (!selClass.empty()) {
        auto it = attrs.find("class");
        if (it == attrs.end()) return false;
        // Manual whole-token scan — an istringstream per call showed up
        // as a hotspot when matching attribute/universal selectors
        // against every node.
        const std::string& cl = it->second;
        bool found = false;
        size_t p = 0;
        while (p < cl.size() && !found) {
            size_t e = cl.find_first_of(" \t\r\n", p);
            if (e == std::string::npos) e = cl.size();
            if (e - p == selClass.size() &&
                strncasecmp(cl.c_str() + p, selClass.c_str(),
                            selClass.size()) == 0) {
                found = true;
            }
            p = e + 1;
        }
        if (!found) return false;
    }

    if (!selId.empty()) {
        auto it = attrs.find("id");
        if (it == attrs.end() || it->second != selId) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Compound selector matching (with combinators)
// ---------------------------------------------------------------------------

// Test a single "simple selector" (e.g. "p", ".foo", "div.bar#x") against
// a node. No combinators.
static bool matchSimple(const std::string& selector,
                        const std::shared_ptr<Node>& node) {
    if (!node) return false;

    // Structural pseudo-classes and :not() — evaluate against the node
    // before the plain tag/class/id match.
    size_t colon = selector.find(':');
    if (colon != std::string::npos) {
        std::string base = trim(selector.substr(0, colon));
        std::string pseudo = selector.substr(colon + 1);

        // There may be several pseudo-classes chained ("a:first-child:hover").
        while (!pseudo.empty()) {
            size_t nextColon = std::string::npos;
            int depth = 0;
            for (size_t k = 0; k < pseudo.size(); ++k) {
                if (pseudo[k] == '(') ++depth;
                else if (pseudo[k] == ')') --depth;
                else if (pseudo[k] == ':' && depth == 0) { nextColon = k; break; }
            }
            std::string one = trim(pseudo.substr(0,
                    nextColon == std::string::npos ? std::string::npos : nextColon));
            pseudo = nextColon == std::string::npos ? "" : pseudo.substr(nextColon + 1);

            std::string pname = one, parg;
            size_t paren = one.find('(');
            if (paren != std::string::npos && one.back() == ')') {
                pname = trim(one.substr(0, paren));
                parg = trim(one.substr(paren + 1, one.size() - paren - 2));
            }
            std::string pl = lower(pname);
            if (pl == "hover") {
                // handled by the Hover variant; treat as non-matching here
                return false;
            }
            // Selector LIST aware matching for :not() / :is() / :where().
            // ":not(a,b)" must be true only when NONE of the alternatives
            // match — comparing the raw list as one selector string made
            // ":not(faceplate-x,faceplate-y)" always-true, which let
            // Reddit's ":not(:defined):not(faceplate-…){visibility:hidden}"
            // gate blank the whole page.
            auto matchList = [&](const std::string& list) {
                size_t start = 0;
                while (start <= list.size()) {
                    size_t comma = list.find(',', start);
                    std::string one = trim(list.substr(start,
                        comma == std::string::npos
                            ? std::string::npos : comma - start));
                    if (!one.empty() && matchSimple(one, node)) return true;
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                return false;
            };
            if (pl == "defined") {
                // Every element we render is a standard element — custom
                // elements only differ after their JS registers them,
                // which mirrors the "scripts ran" end state.
                return true;
            }
            if (pl == "not") {
                if (matchList(parg)) return false;
                continue;
            }
            if (pl == "is" || pl == "where") {
                if (!matchList(parg)) return false;
                continue;
            }
            if (pl == "first-of-type" || pl == "last-of-type" ||
                pl == "nth-of-type" || pl == "only-of-type") {
                // Approximate: treat like the child variants.
                std::string alt = pl.substr(0, pl.size() - 9);  // strip "-of-type"
                if (!matchPseudoClass(alt, parg, node)) return false;
                continue;
            }
            if (!matchPseudoClass(pl, parg, node)) return false;
        }
        return matchesSelector(base, node->tag, node->attrs);
    }

    return matchesSelector(selector, node->tag, node->attrs);
}

// Split "a > b  c" into ["a", ">", "b", " ", "c"]. Whitespace between
// tokens means descendant, ">" means child, "+" means adjacent sibling.
static std::vector<std::string> tokenizeSelector(const std::string& sel) {
    std::vector<std::string> toks;
    std::string cur;
    int parenDepth = 0;
    for (size_t i = 0; i < sel.size(); ++i) {
        char c = sel[i];
        if (c == '(') { ++parenDepth; cur += c; continue; }
        if (c == ')') { --parenDepth; cur += c; continue; }
        if (parenDepth > 0) { cur += c; continue; }
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            // Collapse runs of whitespace into one " " descendant token.
            while (i + 1 < sel.size() &&
                   (sel[i+1] == ' ' || sel[i+1] == '\t' || sel[i+1] == '\n')) ++i;
            // Only emit the descendant token if there's a real selector
            // on both sides (i.e., this isn't a leading/trailing space).
            if (!toks.empty() && toks.back() != ">" && toks.back() != "+" &&
                toks.back() != " " && toks.back() != "~" &&
                i + 1 < sel.size()) {
                toks.push_back(" ");
            }
        } else if (c == '>') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            if (!toks.empty() && toks.back() == " ") toks.pop_back();
            toks.push_back(">");
        } else if (c == '+') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            if (!toks.empty() && toks.back() == " ") toks.pop_back();
            toks.push_back("+");
        } else if (c == '~') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            if (!toks.empty() && toks.back() == " ") toks.pop_back();
            toks.push_back("~");
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

// Recursive evaluation: walk the token list from the right (the rightmost
// simple selector must match `node`; combinators to its left walk up the
// tree).
static bool matchFrom(const std::vector<std::string>& toks,
                      size_t pos,
                      const std::shared_ptr<Node>& node) {
    if (!node) return false;
    if (toks.empty()) return false;

    // The rightmost simple selector must match `node`.
    const std::string& right = toks[pos];
    if (!matchSimple(right, node)) return false;

    if (pos == 0) return true;

    // Walk to the left.
    const std::string& comb = toks[pos - 1];
    if (comb == " ") {
        // Descendant: any ancestor of `node` must match the selector to the left.
        auto p = node->parent.lock();
        while (p) {
            if (pos >= 2 && matchFrom(toks, pos - 2, p)) return true;
            p = p->parent.lock();
        }
        return false;
    }
    if (comb == ">") {
        // Child: the direct parent must match.
        auto p = node->parent.lock();
        if (!p) return false;
        if (pos >= 2) return matchFrom(toks, pos - 2, p);
        return matchSimple(toks[0], p);
    }
    if (comb == "+") {
        // Adjacent sibling: the immediately preceding *element* sibling.
        auto p = node->parent.lock();
        if (!p) return false;
        std::shared_ptr<Node> prevSibling;
        for (auto& c : p->children) {
            if (c.get() == node.get()) break;
            if (c->tag != "text") prevSibling = c;
        }
        if (!prevSibling) return false;
        if (pos >= 2) return matchFrom(toks, pos - 2, prevSibling);
        return matchSimple(toks[0], prevSibling);
    }
    if (comb == "~") {
        // General sibling: any preceding element sibling matches.
        auto p = node->parent.lock();
        if (!p) return false;
        for (auto& c : p->children) {
            if (c.get() == node.get()) break;
            if (c->tag == "text") continue;
            bool ok = (pos >= 2) ? matchFrom(toks, pos - 2, c) : matchSimple(toks[0], c);
            if (ok) return true;
        }
        return false;
    }
    // Unknown combinator — be conservative.
    return false;
}

bool matchesSelectorNode(const std::string& selector,
                         const std::shared_ptr<Node>& node) {
    std::string s = trim(selector);
    if (s.empty()) return false;
    auto toks = tokenizeSelector(s);
    if (toks.empty()) return false;
    return matchFrom(toks, toks.size() - 1, node);
}

// Test a single "simple selector" (e.g. "p", ".foo", "div.bar#x:hover")
// against a node, with hover awareness. If `:hover` is present, the
// node must equal `hovered` to match.
static bool matchSimpleHover(const std::string& selector,
                              const std::shared_ptr<Node>& node,
                              const std::shared_ptr<Node>& hovered) {
    if (!node) return false;

    size_t colon = selector.find(':');
    if (colon == std::string::npos)
        return matchesSelector(selector, node->tag, node->attrs);

    std::string base = trim(selector.substr(0, colon));
    std::string pseudo = selector.substr(colon + 1);
    while (!pseudo.empty()) {
        size_t nextColon = std::string::npos;
        int depth = 0;
        for (size_t k = 0; k < pseudo.size(); ++k) {
            if (pseudo[k] == '(') ++depth;
            else if (pseudo[k] == ')') --depth;
            else if (pseudo[k] == ':' && depth == 0) { nextColon = k; break; }
        }
        std::string one = trim(pseudo.substr(0,
                nextColon == std::string::npos ? std::string::npos : nextColon));
        pseudo = nextColon == std::string::npos ? "" : pseudo.substr(nextColon + 1);

        std::string pname = one, parg;
        size_t paren = one.find('(');
        if (paren != std::string::npos && one.back() == ')') {
            pname = trim(one.substr(0, paren));
            parg = trim(one.substr(paren + 1, one.size() - paren - 2));
        }
        std::string pl = lower(pname);
        if (pl == "hover") {
            if (node != hovered) return false;
            continue;
        }
        if (pl == "not") {
            // For :not inside hover-aware matching, use the non-hover matcher.
            if (matchSimple(parg, node)) return false;
            continue;
        }
        if (pl == "is" || pl == "where") {
            if (!matchSimple(parg, node)) return false;
            continue;
        }
        if (!matchPseudoClass(pl, parg, node)) return false;
    }
    return matchesSelector(base, node->tag, node->attrs);
}

static bool matchFromHover(const std::vector<std::string>& toks,
                            size_t pos,
                            const std::shared_ptr<Node>& node,
                            const std::shared_ptr<Node>& hovered) {
    if (!node) return false;
    if (toks.empty()) return false;
    const std::string& right = toks[pos];
    if (!matchSimpleHover(right, node, hovered)) return false;
    if (pos == 0) return true;
    const std::string& comb = toks[pos - 1];
    if (comb == " ") {
        auto p = node->parent.lock();
        while (p) {
            if (pos >= 2 && matchFromHover(toks, pos - 2, p, hovered)) return true;
            p = p->parent.lock();
        }
        return false;
    }
    if (comb == ">") {
        auto p = node->parent.lock();
        if (!p) return false;
        if (pos >= 2) return matchFromHover(toks, pos - 2, p, hovered);
        return matchSimpleHover(toks[0], p, hovered);
    }
    if (comb == "+") {
        auto p = node->parent.lock();
        if (!p) return false;
        std::shared_ptr<Node> prevSibling;
        for (auto& c : p->children) {
            if (c.get() == node.get()) break;
            if (c->tag != "text") prevSibling = c;
        }
        if (!prevSibling) return false;
        if (pos >= 2) return matchFromHover(toks, pos - 2, prevSibling, hovered);
        return matchSimpleHover(toks[0], prevSibling, hovered);
    }
    if (comb == "~") {
        auto p = node->parent.lock();
        if (!p) return false;
        for (auto& c : p->children) {
            if (c.get() == node.get()) break;
            if (c->tag == "text") continue;
            bool ok = (pos >= 2) ? matchFromHover(toks, pos - 2, c, hovered)
                                 : matchSimpleHover(toks[0], c, hovered);
            if (ok) return true;
        }
        return false;
    }
    return false;
}

bool matchesSelectorNodeHover(const std::string& selector,
                                const std::shared_ptr<Node>& node,
                                const std::shared_ptr<Node>& hovered) {
    std::string s = trim(selector);
    if (s.empty()) return false;
    auto toks = tokenizeSelector(s);
    if (toks.empty()) return false;
    return matchesSelectorTokens(toks, node, hovered);
}

bool matchesSelectorTokens(const std::vector<std::string>& toks,
                           const std::shared_ptr<Node>& node,
                           const std::shared_ptr<Node>& hovered) {
    if (toks.empty()) return false;
    return matchFromHover(toks, toks.size() - 1, node, hovered);
}

// ---------------------------------------------------------------------------
// Pre-parsed selector matching (allocation-free hot path)
// ---------------------------------------------------------------------------

// Ancestor-fingerprint Bloom filters. Wikipedia-style sheets are full of
// selectors like ".foo .bar *" whose rightmost compound matches half the
// tree; the expensive part is walking every ancestor for the left part.
// Each node caches a 256-bit filter of all its ancestors' tag/class/id
// part keys, so a compound that references e.g. tag "table" fails
// instantly when no ancestor is a table. False positives are fine — the
// full walk still decides.
static const int kAncMaskBits = 256;
struct AncMask { uint64_t m[kAncMaskBits / 64] = {0, 0, 0, 0}; };
static std::unordered_map<const void*, AncMask> g_ancMask;

static uint64_t fpHashMix(uint64_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t partFp(char kind, const std::string& v) {
    uint64_t h = 1469598103934665603ull;
    h = fpHashMix(h, &kind, 1);
    h = fpHashMix(h, v.data(), v.size());
    return h;
}

static void ancMaskInsert(AncMask& a, uint64_t fp) {
    int idx = (int)(fp % kAncMaskBits);
    a.m[idx >> 6] |= 1ull << (idx & 63);
}

static bool ancMaskMayContain(const AncMask& a, uint64_t fp) {
    int idx = (int)(fp % kAncMaskBits);
    return (a.m[idx >> 6] >> (idx & 63)) & 1ull;
}

// Bloom filter covering every ancestor's tag/class/id parts.
static const AncMask& ancestorMaskOf(const std::shared_ptr<Node>& node) {
    auto res = g_ancMask.try_emplace(node.get());
    AncMask& mine = res.first->second;
    auto parent = node->parent.lock();
    if (!parent) return mine;   // root: empty filter
    if (!res.second) return mine;   // cached
    const AncMask& pm = ancestorMaskOf(parent);
    for (int i = 0; i < 4; ++i) mine.m[i] = pm.m[i];
    ancMaskInsert(mine, partFp('t', parent->tag));
    auto cit = parent->attrs.find("class");
    if (cit != parent->attrs.end() && !cit->second.empty()) {
        const std::string& cl = cit->second;
        size_t p = 0;
        while (p < cl.size()) {
            size_t e = cl.find_first_of(" \t\r\n", p);
            if (e == std::string::npos) e = cl.size();
            if (e > p) ancMaskInsert(mine, partFp('c', cl.substr(p, e - p)));
            p = e + 1;
        }
    }
    auto iit = parent->attrs.find("id");
    if (iit != parent->attrs.end())
        ancMaskInsert(mine, partFp('i', iit->second));
    return mine;
}

void clearSelectorAncCache() { g_ancMask.clear(); }

// Sound over-approximation: every compound except the one matching the
// node itself must have all its tag/class/id parts present among the
// ancestors. Only sound for " "/" >" chains — for "+" / "~" the left
// compounds match SIBLINGS, which the ancestor filter knows nothing
// about (this used to silently drop e.g. Wikipedia's
// ".cdx-button--icon-only span + span{clip:…}" sr-only rule). Selectors
// with sibling combinators skip the pruning and take the full walk.
static bool selectorPlausibleFor(const std::vector<SelItem>& items,
                                 const std::shared_ptr<Node>& node) {
    if (items.size() <= 1) return true;
    for (size_t i = 1; i < items.size(); ++i)
        if (items[i].isComb &&
            (items[i].comb == '+' || items[i].comb == '~')) return true;
    const AncMask& anc = ancestorMaskOf(node);
    for (size_t i = 0; i + 1 < items.size(); ++i) {
        if (items[i].isComb) continue;
        const SimpleSel& ps = items[i].simple;
        if (!ps.tag.empty() && ps.tag != "*" &&
            !ancMaskMayContain(anc, partFp('t', ps.tag))) return false;
        for (const auto& cl : ps.classes)
            if (!ancMaskMayContain(anc, partFp('c', cl))) return false;
        if (!ps.id.empty() &&
            !ancMaskMayContain(anc, partFp('i', ps.id))) return false;
    }
    return true;
}

// Whole-token, case-insensitive class check without allocations.
static bool classListContains(const std::string& cl, const std::string& cls) {
    size_t p = 0;
    while (p < cl.size()) {
        size_t e = cl.find_first_of(" \t\r\n", p);
        if (e == std::string::npos) e = cl.size();
        if (e - p == cls.size() &&
            strncasecmp(cl.c_str() + p, cls.c_str(), cls.size()) == 0)
            return true;
        p = e + 1;
    }
    return false;
}

// Match one pre-parsed compound against a node.
static bool matchSimpleParsed(const SimpleSel& ps,
                              const std::shared_ptr<Node>& node,
                              const std::shared_ptr<Node>& hovered) {
    if (!node) return false;
    for (const auto& p : ps.pseudos) {
        if (p.name == "hover") {
            if (node != hovered) return false;
        } else if (p.name == "not") {
            if (matchSimple(p.arg, node)) return false;
        } else if (p.name == "is" || p.name == "where") {
            if (!matchSimple(p.arg, node)) return false;
        } else if (p.name == "first-of-type" || p.name == "last-of-type" ||
                   p.name == "nth-of-type" || p.name == "only-of-type") {
            // Approximate: treat like the child variants (matches the
            // string-based matcher's behavior).
            std::string alt = p.name.substr(0, p.name.size() - 9);
            if (!matchPseudoClass(alt, p.arg, node)) return false;
        } else {
            if (!matchPseudoClass(p.name, p.arg, node)) return false;
        }
    }
    if (!ps.tag.empty() && ps.tag != "*" && ps.tag != node->tag) return false;
    if (!ps.id.empty()) {
        auto it = node->attrs.find("id");
        if (it == node->attrs.end() || it->second != ps.id) return false;
    }
    if (!ps.classes.empty()) {
        auto it = node->attrs.find("class");
        if (it == node->attrs.end()) return false;
        for (const auto& cl : ps.classes)
            if (!classListContains(it->second, cl)) return false;
    }
    for (const auto& ac : ps.attrs) {
        auto it = node->attrs.find(ac.name);
        if (it == node->attrs.end()) return false;
        if (ac.op == 0) continue;               // bare [attr] presence
        const std::string& actual = it->second;
        const std::string& v = ac.value;
        switch (ac.op) {
            case '=': {
                bool eq = actual.size() == v.size() &&
                          strncasecmp(actual.c_str(), v.c_str(), v.size()) == 0;
                if (!eq) return false;
                break;
            }
            case '^':
                if (actual.size() < v.size() ||
                    strncasecmp(actual.c_str(), v.c_str(), v.size()) != 0)
                    return false;
                break;
            case '$':
                if (actual.size() < v.size() ||
                    strncasecmp(actual.c_str() + actual.size() - v.size(),
                                v.c_str(), v.size()) != 0)
                    return false;
                break;
            case '*':
                if (actual.size() < v.size()) return false;
                {
                    bool found = false;
                    for (size_t p = 0; p + v.size() <= actual.size(); ++p) {
                        if (strncasecmp(actual.c_str() + p, v.c_str(),
                                        v.size()) == 0) { found = true; break; }
                    }
                    if (!found) return false;
                }
                break;
            case '~':
                if (!classListContains(actual, v)) return false;
                break;
            case '|':
                if (!(actual == v ||
                      (actual.size() > v.size() &&
                       strncasecmp(actual.c_str(), v.c_str(), v.size()) == 0 &&
                       actual[v.size()] == '-')))
                    return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

// Recursive right-to-left evaluation over pre-parsed items.
static bool matchFromItems(const std::vector<SelItem>& items,
                           size_t pos,
                           const std::shared_ptr<Node>& node,
                           const std::shared_ptr<Node>& hovered) {
    if (!node || items.empty()) return false;
    const SelItem& it = items[pos];
    if (it.isComb) return false;   // malformed sequence
    if (!matchSimpleParsed(it.simple, node, hovered)) return false;
    if (pos == 0) return true;

    char comb = items[pos - 1].comb;   // pos-1 is a combinator item
    if (comb == ' ') {
        auto p = node->parent.lock();
        while (p) {
            if (pos >= 2 && matchFromItems(items, pos - 2, p, hovered)) return true;
            p = p->parent.lock();
        }
        return false;
    }
    if (comb == '>') {
        auto p = node->parent.lock();
        if (!p) return false;
        if (pos >= 2) return matchFromItems(items, pos - 2, p, hovered);
        return matchSimpleParsed(items[0].simple, p, hovered);
    }
    if (comb == '+') {
        auto p = node->parent.lock();
        if (!p) return false;
        std::shared_ptr<Node> prevSibling;
        for (auto& c : p->children) {
            if (c.get() == node.get()) break;
            if (c->tag != "text") prevSibling = c;
        }
        if (!prevSibling) return false;
        if (pos >= 2) return matchFromItems(items, pos - 2, prevSibling, hovered);
        return matchSimpleParsed(items[0].simple, prevSibling, hovered);
    }
    if (comb == '~') {
        auto p = node->parent.lock();
        if (!p) return false;
        for (auto& c : p->children) {
            if (c.get() == node.get()) break;
            if (c->tag == "text") continue;
            bool ok = (pos >= 2)
                    ? matchFromItems(items, pos - 2, c, hovered)
                    : matchSimpleParsed(items[0].simple, c, hovered);
            if (ok) return true;
        }
        return false;
    }
    return false;
}

bool matchesSelectorItems(const std::vector<SelItem>& items,
                          const std::shared_ptr<Node>& node,
                          const std::shared_ptr<Node>& hovered) {
    if (items.empty()) return false;
    // Prune multi-compound selectors whose left-hand parts cannot match
    // any ancestor (cheap Bloom filter over ancestor tag/class/id keys).
    if (items.size() > 1 && !selectorPlausibleFor(items, node)) return false;
    return matchFromItems(items, items.size() - 1, node, hovered);
}

} // namespace browser
